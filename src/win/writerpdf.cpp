// VOSStudio Native — compiling the visual Writer document to LaTeX and previewing the resulting PDF.
// Compilation is explicit and disables TeX shell escape; page rendering shares the Reader's single PDFium worker.
#include "writerpdf.h"
#include "reader.h"
#include "platform.h"
#include "tectonic.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

#include "../core/doc.h"

namespace vs {
namespace win {
namespace {

struct CompileResult {
  string pdfPath;
  string error;
};

bool createDirectory(const std::wstring& path) {
  if (CreateDirectoryW(path.c_str(), nullptr)) return true;
  if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

string previewRoot() {
  std::vector<wchar_t> buf(32768, 0);
  DWORD n = GetTempPathW(DWORD(buf.size()), buf.data());
  const bool useTemp = n && n < buf.size();
  std::wstring base = useTemp ? std::wstring(buf.data(), n) : widen(appDataDir());
  while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
  if (base.empty()) return "";
  std::wstring parent = useTemp ? base + L"\\VOSStudio" : base;
  std::wstring root = parent + L"\\WriterPreview";
  if (!createDirectory(parent) || !createDirectory(root)) return "";
  return narrow(root);
}

string newBuildDirectory() {
  static std::atomic<uint64_t> sequence{0};
  string root = previewRoot();
  if (root.empty()) return "";
  string leaf = "build-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence);
  string path = root + "\\" + leaf;
  if (!createDirectory(widen(path))) return "";
  return path;
}

bool fileExists(const string& path) {
  DWORD attr = GetFileAttributesW(widen(path).c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

string cleanLogTail(const string& path) {
  bool ok = false;
  string log = readFileU(path, &ok);
  if (!ok || log.empty()) return "";
  if (log.size() > 2400) log.erase(0, log.size() - 2400);
  string clean;
  clean.reserve(log.size());
  for (unsigned char c : log) {
    if (c == '\r') continue;
    if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) clean += char(c);
    else if (c >= 127) clean += '?';
  }
  while (!clean.empty() && (clean.back() == '\n' || clean.back() == ' ')) clean.pop_back();
  return clean;
}

bool runTectonic(const std::wstring& exe, const string& workDir, Job& job, string& error) {
  const std::wstring cwd = widen(workDir);
  // Tectonic's V1 CLI runs the TeX engine and one rerun in a single process; --untrusted disables insecure features.
  std::wstring cmd = L"\"" + exe + L"\" --untrusted --keep-logs --reruns 1 --outdir \"" + cwd + L"\" document.tex";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
    error = "Tectonic could not be started (Windows error " + std::to_string(GetLastError()) + ").";
    return false;
  }
  CloseHandle(pi.hThread);
  constexpr ULONGLONG timeoutMs = 240000;
  const ULONGLONG started = GetTickCount64();
  bool timedOut = false, cancelled = false;
  DWORD wait = WAIT_TIMEOUT;
  while (wait == WAIT_TIMEOUT) {
    const ULONGLONG elapsedMs = GetTickCount64() - started;
    if (job.cancel) { cancelled = true; TerminateProcess(pi.hProcess, 1); }
    if (!cancelled && elapsedMs >= timeoutMs) { timedOut = true; TerminateProcess(pi.hProcess, 1); }
    wait = WaitForSingleObject(pi.hProcess, 100);
    if (wait == WAIT_TIMEOUT) job.progress = 0.08 + 0.80 * std::min(1.0, double(elapsedMs) / double(timeoutMs));
  }
  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  if (cancelled) { error = "Compilation cancelled."; return false; }
  if (timedOut) { error = "Tectonic exceeded the four-minute build limit."; return false; }
  if (wait != WAIT_OBJECT_0 || exitCode != 0) {
    error = "Tectonic stopped with an error.";
    string detail = cleanLogTail(workDir + "\\document.log");
    if (!detail.empty()) error += "\n\n" + detail;
    return false;
  }
  return true;
}

void useBitmap(WriterPdfPreview& state, int page) {
  state.bitmapLru.erase(std::remove(state.bitmapLru.begin(), state.bitmapLru.end(), page), state.bitmapLru.end());
  state.bitmapLru.push_back(page);
  while (state.bitmapLru.size() > 8) {
    int old = state.bitmapLru.front();
    state.bitmapLru.erase(state.bitmapLru.begin());
    if (old >= 0 && size_t(old) < state.pages.size()) state.pages[size_t(old)].bitmap.reset();
  }
}

void forgetBitmap(WriterPdfPreview& state, int page) {
  state.bitmapLru.erase(std::remove(state.bitmapLru.begin(), state.bitmapLru.end(), page), state.bitmapLru.end());
}

int previewBitmapWidth(float displayW, float pageW, float pageH) {
  int target = clampv(int(std::lround(displayW * 1.35f / 64.f)) * 64, 320, 1200);
  if (pageH > 0 && pageW > 0) target = std::min(target, std::max(240, int(1600.f * pageW / pageH)));
  return target;
}

string firstLine(const string& s) {
  size_t n = s.find_first_of("\r\n");
  return trim(n == string::npos ? s : s.substr(0, n));
}

}  // namespace

void App::writerCompileLatex() {
  if (writerPdfCompileBusy_) return;
  if (busy()) { ui.toast("Please wait", jobLabel + " is still running.", 2); return; }
  if (wdoc.empty()) { ui.toast("Nothing to compile", "Add content to the document first.", 2); return; }

  writerSetPdfPreview(true);
  writerPdfCompileBusy_ = true;
  writerPdfCompileError_.clear();
  const uint64_t version = wed.version;
  const uint64_t generation = writerPdfGeneration_;
  auto snapshot = std::make_shared<Document>(wdoc);
  auto result = std::make_shared<CompileResult>();
  bool started = startJob("Compiling LaTeX preview", [snapshot, result](Job& job) {
    try {
      if (job.cancel) { result->error = "Compilation cancelled."; return; }
      job.progress = 0.02;
      LatexBundle bundle = docToLatex(*snapshot, "figures");
      if (bundle.source.empty()) { result->error = "The LaTeX source could not be generated."; return; }

      string workDir = newBuildDirectory();
      if (workDir.empty()) { result->error = "A temporary preview folder could not be created."; return; }
      if (!writeFileU(workDir + "\\document.tex", bundle.source)) { result->error = "The LaTeX source could not be written."; return; }
      if (!bundle.bibliography.empty() && !writeFileU(workDir + "\\document.bib", bundle.bibliography)) {
        result->error = "The bibliography file could not be written."; return;
      }
      for (const auto& asset : bundle.assets) {
        if (job.cancel) { result->error = "Compilation cancelled."; return; }
        if (asset.first.empty() || asset.first.find("..") != string::npos || asset.first[0] == '/' || asset.first[0] == '\\') {
          result->error = "The generated figure asset path is not safe."; return;
        }
        string rel = replaceAll(asset.first, "/", "\\");
        size_t slash = rel.find_last_of('\\');
        if (slash != string::npos && !createDirectory(widen(workDir + "\\" + rel.substr(0, slash)))) {
          result->error = "The figure folder could not be created."; return;
        }
        if (!writeFileU(workDir + "\\" + rel, asset.second)) { result->error = "A figure asset could not be written."; return; }
      }
      if (job.cancel) { result->error = "Compilation cancelled."; return; }

      string engineError;
      std::wstring exe = tectonicPath(&engineError);
      if (exe.empty()) {
        result->error = engineError.empty() ? "The bundled Tectonic engine is unavailable." : engineError;
        return;
      }
      job.progress = 0.08;
      string err;
      if (!runTectonic(exe, workDir, job, err)) { result->error = err; return; }
      string pdfPath = workDir + "\\document.pdf";
      if (!fileExists(pdfPath)) { result->error = "The compiler finished without producing a PDF."; return; }
      result->pdfPath = pdfPath;
      job.progress = 1;
    } catch (const std::exception& e) {
      result->error = string("The LaTeX preview failed: ") + e.what();
    } catch (...) {
      result->error = "The LaTeX preview failed unexpectedly.";
    }
  }, [this, result, version, generation]() {
    writerPdfCompileBusy_ = false;
    if (generation != writerPdfGeneration_) { needFrame = true; return; }  // a new/opened document superseded this snapshot
    if (result->error.empty() && !result->pdfPath.empty()) {
      writerPdfPath_ = result->pdfPath;
      writerPdfCompiledVersion_ = version;
      writerPdfCompileError_.clear();
      writerPreviewLoad(writerPdfPath_);
      ui.toast("PDF ready", "The Writer LaTeX preview is up to date.", 1, 3);
    } else {
      writerPdfCompileError_ = result->error.empty() ? "The preview could not be compiled." : result->error;
      ui.toast("LaTeX compile failed", truncate(writerPdfCompileError_, 900), 3, 8);
    }
    needFrame = true;
  });
  if (!started) writerPdfCompileBusy_ = false;
}

void App::writerSetPdfPreview(bool on) {
  if (on && !writerOpen) openWriter();
  if (writerPdfPreviewOpen_ == on) return;
  if (on) {
    writerPaneBeforePdfPreview_ = writerPane;
    writerPane = 0;
    writerPdfPreviewOpen_ = true;
    writerFitOnce_ = true;
    if (!writerPdfPath_.empty()) writerPreviewLoad(writerPdfPath_);
  } else {
    writerPdfPreviewOpen_ = false;
    if (writerPane == 0) writerPane = writerPaneBeforePdfPreview_;
    writerFitOnce_ = true;
  }
  needFrame = true;
}

bool App::writerOpenPdfSeparately() {
  if (writerPdfPath_.empty() || !fileExists(writerPdfPath_)) {
    ui.toast("No compiled PDF", "Compile the document first.", 2, 3);
    return false;
  }
  openUrl(writerPdfPath_);
  return true;
}

void App::writerPreviewLoad(const string& path) {
  if (path.empty()) return;
  if (!writerPdfPreview_) writerPdfPreview_ = std::make_shared<WriterPdfPreview>();
  auto state = writerPdfPreview_;
  if (state->path == path && (state->loading || state->ready)) return;

  readerEnsureEngine();  // the preview uses the same PDFium worker as Read; PDFium itself is not thread-safe
  if (!rd || !rd->workerStarted) {
    state->failed = true;
    state->loading = false;
    state->ready = false;
    state->error = "The PDF engine could not be started.";
    needFrame = true;
    return;
  }

  const uint32_t gen = ++state->generation;
  const int oldId = state->docId;
  if (oldId >= 0) rd->worker.submit(-10, [oldId](pdf::Worker& worker) { worker.closeDoc(oldId); });
  state->docId = rd->worker.newDocId();
  const int id = state->docId;
  state->path = path;
  state->loading = true;
  state->ready = false;
  state->failed = false;
  state->error.clear();
  state->info = pdf::DocInfo();
  state->pages.clear();
  state->pageTop.clear();
  state->pageBottom.clear();
  state->pageW.clear();
  state->pageH.clear();
  state->bitmapLru.clear();
  state->layoutW = state->layoutScale = -1;
  std::weak_ptr<WriterPdfPreview> weak = state;

  rd->worker.submit(0, [this, weak, gen, id, path](pdf::Worker& worker) {
    if (!worker.hasDoc(id)) worker.doc(id);
    pdf::Document& doc = worker.doc(id);
    string error;
    const bool ok = doc.open(path, &error);
    pdf::DocInfo info;
    if (ok) info = doc.info();
    else {
      if (error.empty()) error = "This PDF could not be opened by the PDF engine.";
      worker.closeDoc(id);
    }
    worker.toUi([this, weak, gen, id, ok, error, info = std::move(info)]() mutable {
      auto state = weak.lock();
      if (!state || state->generation != gen || state->docId != id) return;
      state->loading = false;
      state->ready = ok;
      state->failed = !ok;
      state->error = error;
      state->info = std::move(info);
      state->pages.clear();
      if (ok) {
        state->pages.resize(size_t(std::max(0, state->info.pages)));
        for (size_t i = 0; i < state->pages.size() && i < state->info.sizes.size(); i++) {
          state->pages[i].w = std::max(1.f, state->info.sizes[i][0]);
          state->pages[i].h = std::max(1.f, state->info.sizes[i][1]);
        }
      } else state->docId = -1;
      state->layoutW = state->layoutScale = -1;
      needFrame = true;
    });
  });
  needFrame = true;
}

void App::writerPreviewClear() {
  ++writerPdfGeneration_;
  if (writerPdfPreview_) {
    ++writerPdfPreview_->generation;
    int id = writerPdfPreview_->docId;
    if (id >= 0 && rd && rd->workerStarted) rd->worker.submit(-10, [id](pdf::Worker& worker) { worker.closeDoc(id); });
    writerPdfPreview_->docId = -1;
    writerPdfPreview_->loading = writerPdfPreview_->ready = writerPdfPreview_->failed = false;
    writerPdfPreview_->path.clear();
    writerPdfPreview_->error.clear();
    writerPdfPreview_->info = pdf::DocInfo();
    writerPdfPreview_->pages.clear();
    writerPdfPreview_->pageTop.clear();
    writerPdfPreview_->pageBottom.clear();
    writerPdfPreview_->pageW.clear();
    writerPdfPreview_->pageH.clear();
    writerPdfPreview_->bitmapLru.clear();
    writerPdfPreview_->layoutW = writerPdfPreview_->layoutScale = -1;
  }
  writerPdfPath_.clear();
  writerPdfCompileError_.clear();
  writerPdfCompiledVersion_ = ~0ull;
  needFrame = true;
}

void App::writerPreviewShutdown() {
  writerPreviewClear();
  writerPdfPreview_.reset();
  writerPdfPreviewOpen_ = false;
}

void App::writerDrawPdfPreview(const Rect& r) {
  const float s = ui.s;
  ui.fill(r, ui.c.panel);
  ui.line(r.x + 0.5f, r.y, r.x + 0.5f, r.b(), ui.c.border);
  const float headH = 34 * s;
  Rect header{r.x + 12 * s, r.y, std::max(0.f, r.w - 58 * s), headH};
  Rect open{r.r() - 36 * s, r.y + 4 * s, 28 * s, 26 * s};
  const bool havePdf = !writerPdfPath_.empty();
  if (ui.iconButton(open, "external", "Open the compiled PDF in the system viewer", false, havePdf)) writerOpenPdfSeparately();

  WriterPdfPreview* state = writerPdfPreview_.get();
  string title = "PDF preview";
  if (state && state->ready) title += " \xC2\xB7 " + std::to_string(state->pages.size()) + " pages";
  bool stale = havePdf && writerPdfCompiledVersion_ != wed.version;
  const float tagW = stale ? std::min(76 * s, std::max(0.f, header.w * 0.38f)) : 0;
  Rect titleR{header.x, header.y, std::max(0.f, header.w - tagW - (tagW ? 5 * s : 0)), header.h};
  ui.text(titleR, title, 12.5f * s, ui.c.text, AL_LEFT, 600);
  if (stale && tagW > 20 * s) {
    Rect tag{titleR.r() + 5 * s, header.y + 8 * s, tagW - 5 * s, 18 * s};
    ui.text(tag, "Out of date", 10.5f * s, ui.c.warn, AL_RIGHT, 500);
  }
  ui.line(r.x, r.y + headH, r.r(), r.y + headH, ui.c.border);

  Rect content{r.x, r.y + headH + 1, r.w, std::max(0.f, r.h - headH - 1)};
  ui.fill(content, ui.c.bg);
  float bannerH = 0;
  if (!writerPdfCompileError_.empty()) {
    bannerH = 32 * s;
    Rect banner{content.x + 8 * s, content.y + 6 * s, std::max(0.f, content.w - 16 * s), 24 * s};
    ui.fill(banner, ui.c.warn.withA(0.12f), 4 * s);
    ui.text({banner.x + 8 * s, banner.y, std::max(0.f, banner.w - 16 * s), banner.h}, firstLine(writerPdfCompileError_), 10.5f * s, ui.c.warn);
  }
  Rect scroll{content.x, content.y + bannerH, content.w, std::max(0.f, content.h - bannerH)};

  if (!havePdf) {
    string msg = writerPdfCompileBusy_ ? "Compiling LaTeX\xE2\x80\xA6" : "Compile the document to build a PDF preview.";
    ui.text({content.x + 20 * s, content.y + content.h * 0.36f, std::max(0.f, content.w - 40 * s), 36 * s}, msg, 12 * s, ui.c.textDim, AL_CENTER);
    return;
  }
  if (!state || state->loading) {
    ui.animating = true;
    ui.text({scroll.x + 12 * s, scroll.y + 22 * s, std::max(0.f, scroll.w - 24 * s), 28 * s}, "Opening PDF preview\xE2\x80\xa6", 12 * s, ui.c.textDim, AL_CENTER);
    return;
  }
  if (state->failed) {
    ui.textWrap({scroll.x + 16 * s, scroll.y + 20 * s, std::max(0.f, scroll.w - 32 * s), std::max(0.f, std::min(120 * s, scroll.h - 20 * s))},
                state->error.empty() ? "The PDF preview could not be opened." : state->error, 12 * s, ui.c.textDim, 400);
    return;
  }
  if (!state->ready || state->pages.empty()) {
    ui.text({scroll.x + 12 * s, scroll.y + 22 * s, std::max(0.f, scroll.w - 24 * s), 28 * s}, "No pages in this PDF.", 12 * s, ui.c.textDim, AL_CENTER);
    return;
  }

  const float gap = 14 * s, pad = 14 * s;
  const float maxPageW = std::max(40.f, scroll.w - 34 * s);
  const bool rebuildLayout = state->pageTop.size() != state->pages.size() || std::fabs(state->layoutW - maxPageW) > 0.75f || std::fabs(state->layoutScale - s) > 0.001f;
  if (rebuildLayout) {
    state->pageTop.assign(state->pages.size(), pad);
    state->pageBottom.assign(state->pages.size(), pad);
    state->pageW.assign(state->pages.size(), maxPageW);
    state->pageH.assign(state->pages.size(), 1.f);
    float y = pad;
    for (size_t i = 0; i < state->pages.size(); i++) {
      WriterPdfPagePreview& p = state->pages[i];
      float ptW = std::max(1.f, p.w), ptH = std::max(1.f, p.h);
      float scale = maxPageW / ptW;
      float dw = ptW * scale, dh = ptH * scale;
      state->pageTop[i] = y;
      state->pageW[i] = dw;
      state->pageH[i] = dh;
      state->pageBottom[i] = y + dh;
      y += dh + gap;
      int targetW = previewBitmapWidth(dw, ptW, ptH);
      if (p.wantedW != targetW) {
        forgetBitmap(*state, int(i));
        p.bitmap.reset();
        p.wantedW = targetW;
        p.requestToken++;
        p.requested = false;
        p.failed = false;
      }
    }
    state->contentH = state->pages.empty() ? 0 : state->pageBottom.back() + pad;
    state->layoutW = maxPageW;
    state->layoutScale = s;
  }

  ui.beginScroll("writerpdfpreview:" + std::to_string(state->generation), scroll);
  const float scrollY = ui.scrollY();
  const float overscan = scroll.h * 0.35f;
  float scanTop = std::max(0.f, scrollY - overscan);
  auto firstIt = std::lower_bound(state->pageBottom.begin(), state->pageBottom.end(), scanTop);
  size_t first = size_t(firstIt - state->pageBottom.begin());
  for (size_t i = first; i < state->pages.size() && state->pageTop[i] < scrollY + scroll.h + overscan; i++) {
    WriterPdfPagePreview& p = state->pages[i];
    Rect page{scroll.x + (scroll.w - state->pageW[i]) / 2, scroll.y + state->pageTop[i] - scrollY, state->pageW[i], state->pageH[i]};
    ui.fill(page, Color::hex(0xFFFFFF), 2 * s);
    ui.stroke(page, ui.c.border, 2 * s, 1.f);

    if (p.bitmap) {
      useBitmap(*state, int(i));
      ui.dc()->DrawBitmap(p.bitmap.get(), D2D1::RectF(page.x, page.y, page.r(), page.b()), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else if (p.failed) {
      ui.text({page.x + 10 * s, page.y + 10 * s, std::max(0.f, page.w - 20 * s), 24 * s}, "Page preview unavailable", 11 * s, ui.c.textDim, AL_CENTER);
    } else {
      if (!p.requested) {
        p.requested = true;
        const uint32_t token = ++p.requestToken;
        const int docId = state->docId;
        const uint32_t gen = state->generation;
        const int targetW = p.wantedW;
        const int targetH = clampv(int(std::lround(double(targetW) * p.h / std::max(1.f, p.w))), 1, 1600);
        const double renderScale = double(targetW) / std::max(1.f, p.w);
        std::weak_ptr<WriterPdfPreview> weak = writerPdfPreview_;
        readerEnsureEngine();
        if (rd && rd->workerStarted) {
          rd->worker.submit(5, [this, weak, gen, token, docId, pageIndex = int(i), targetW, targetH, renderScale](pdf::Worker& worker) {
            auto bitmap = std::make_shared<pdf::Bitmap>();
            bool ok = worker.hasDoc(docId) && worker.doc(docId).render(pageIndex, renderScale, 0, 0, targetW, targetH, *bitmap);
            worker.toUi([this, weak, gen, token, pageIndex, targetW, ok, bitmap]() {
              auto state = weak.lock();
              if (!state || state->generation != gen || pageIndex < 0 || size_t(pageIndex) >= state->pages.size()) return;
              WriterPdfPagePreview& p = state->pages[size_t(pageIndex)];
              if (p.requestToken != token || p.wantedW != targetW) return;
              p.requested = false;
              if (!ok || bitmap->w <= 0 || bitmap->h <= 0 || !g.dc) {
                p.failed = true;
                needFrame = true;
                return;
              }
              D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
              if (FAILED(g.dc->CreateBitmap(D2D1::SizeU(UINT32(bitmap->w), UINT32(bitmap->h)), bitmap->bgra.data(), UINT32(bitmap->w * 4), bp, p.bitmap.put()))) {
                p.failed = true;
                needFrame = true;
                return;
              }
              p.failed = false;
              useBitmap(*state, pageIndex);
              needFrame = true;
            });
          });
        } else {
          p.requested = false;
          p.failed = true;
        }
      }
      if (!p.failed) ui.text({page.x + 10 * s, page.y + 10 * s, std::max(0.f, page.w - 20 * s), 24 * s}, "Rendering\xE2\x80\xa6", 11 * s, ui.c.textDim, AL_CENTER);
    }
  }
  ui.endScroll(state->contentH);
}

}  // namespace win
}  // namespace vs
