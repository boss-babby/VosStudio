// Printing from the PDF reader.
//
// An in-app card asks what to print (all pages, this page, a range; with or without the project's annotations,
// including freehand drawings; fitted to the paper), then the system's printer dialog chooses the printer, the paper and the copies,
// then a background job prints: every page is rendered by the engine on its own thread at the printer's resolution
// (300 dpi at most, turned as the reader shows it), the marks are burnt in (pdf::burnMarks), and the page goes to the
// printer as a bitmap. The job opens its own copy of the document on the engine thread, so the reader can close,
// change documents or release its memory while a long print runs. The reader stays fluid: one page renders at a
// time, between the reader's own tiles.
#include "reader.h"

#include <commdlg.h>

#include <cmath>
#include <future>

namespace vs {
namespace win {

namespace {

// "1-3, 7, 10-" -> page indices (0-based) within n pages, in the order given; false with a message on a bad range
bool rdParseRange(const string& txt, int n, vector<int>& out, string& err) {
  out.clear();
  for (string part : split(txt, ',')) {
    part = trim(part);
    if (part.empty()) continue;
    size_t dash = part.find('-');
    int a = 0, b = 0;
    if (dash == string::npos) { a = b = atoi(part.c_str()); }
    else {
      string l = trim(part.substr(0, dash)), r = trim(part.substr(dash + 1));
      a = l.empty() ? 1 : atoi(l.c_str());
      b = r.empty() ? n : atoi(r.c_str());
    }
    if (a < 1 || b < 1 || a > n || b > n) { err = "There is no page " + std::to_string(a < 1 || a > n ? a : b) + " (the document has " + plural(n, "page") + ")."; return false; }
    if (a <= b) for (int i = a; i <= b; i++) out.push_back(i - 1);
    else for (int i = a; i >= b; i--) out.push_back(i - 1);
  }
  if (out.empty()) { err = "Type the pages to print, for example 1-3, 7."; return false; }
  return true;
}

// the marks of one page as the engine burns them: kind, colour, boxes (unrotated page points)
vector<pdf::Mark> rdMarksOf(const PdfItem& it, int page) {
  vector<pdf::Mark> out;
  for (const PdfAnnot& a : it.annots) {
    if (a.page != page) continue;
    pdf::Mark m;
    m.kind = a.kind;
    m.rgb = a.color & 0xFFFFFFu;
    m.quads = a.quads;
    m.strokes = a.strokes;
    m.strokeWidth = a.strokeWidth;
    m.rect = a.rect;
    out.push_back(std::move(m));
  }
  return out;
}

// a 32-bit BGRA bitmap as a 24-bit top-down DIB (a quarter less to spool)
void rdBlit(HDC hdc, int dx, int dy, int dw, int dh, const pdf::Bitmap& bm) {
  const int stride = (bm.w * 3 + 3) & ~3;
  vector<uint8_t> rgb(size_t(stride) * size_t(bm.h));
  for (int y = 0; y < bm.h; y++) {
    const uint8_t* src = bm.bgra.data() + size_t(y) * size_t(bm.w) * 4;
    uint8_t* dst = rgb.data() + size_t(y) * size_t(stride);
    for (int x = 0; x < bm.w; x++, src += 4, dst += 3) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = bm.w;
  bi.bmiHeader.biHeight = -bm.h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(hdc, HALFTONE);
  SetBrushOrgEx(hdc, 0, 0, nullptr);
  StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, bm.w, bm.h, rgb.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

struct PrintCtx {  // shared between the job thread and the engine thread
  std::unique_ptr<pdf::Document> doc;  // created, used and destroyed on the engine thread only
  string path, password, error;
  pdf::DocInfo info;
};

}  // namespace

// ------------------------------------------------------------------ the card
void App::readerPrintDialog() {
  if (!rd || !rd->doc.ready || !readerOpen) { ui.toast("Print", "Open a PDF in the reader first.", 2, 3); return; }
  if (busy()) { ui.toast("Please wait", jobLabel + " is still running.", 2, 3); return; }
  ReaderState& R = *rd;
  PdfItem* item = readerItem();
  R.printPages = 0;
  R.printRange = std::to_string(readerCurrentPage() + 1);
  R.printMarks = item && item->annotCount() > 0;
  ui.focus = 0;
  ui.openPopup("rdprint");
  needFrame = true;
}

void App::drawReaderPrintCard() {
  const float s = ui.s;
  ui.overlay([this, s]() {
    if (!rd || !readerOpen) { ui.closePopup(); return; }
    ReaderState& R = *rd;
    PdfItem* item = readerItem();
    if (!item || !R.doc.ready) { ui.closePopup(); return; }
    const int n = int(R.doc.pages.size());
    const int nMarks = item->annotCount();
    const float w = std::min(400 * s, float(g.W) - 32 * s), pad = 16 * s, rowH = 26 * s;
    const bool rangeRow = R.printPages == 2;
    const float h = pad + 24 * s + 18 * s + 10 * s + rowH + (rangeRow ? 34 * s : 0) + 10 * s + 3 * rowH + 6 * s + (R.rot ? 18 * s : 0) + 12 * s + 32 * s + pad;
    const Rect area = R.pageArea.w > 0 ? R.pageArea : Rect{0, 0, float(g.W), float(g.H)};
    Rect r{area.x + (area.w - w) / 2, area.y + std::max(16 * s, (area.h - h) / 2 - 20 * s), w, h};
    r.x = clampv(r.x, 8 * s, std::max(8 * s, float(g.W) - w - 8 * s));
    r.y = clampv(r.y, 8 * s, std::max(8 * s, float(g.H) - h - 8 * s));
    ui.shadow(r, 12 * s, 24 * s);
    ui.fill(r, ui.c.panel2, 10 * s);
    ui.stroke(r, ui.c.border, 10 * s);
    ui.popupRect(r);
    float x = r.x + pad, y = r.y + pad;
    ui.icon("print", x + 10 * s, y + 11 * s, 20 * s, ui.c.text, 1.6f);
    ui.text({x + 30 * s, y, w - 2 * pad - 30 * s, 24 * s}, "Print", 15 * s, ui.c.text, AL_LEFT, 600);
    y += 24 * s;
    string title = item->title.empty() ? PdfLibrary::fileTitle(item->path) : item->title;
    ui.text({x, y, w - 2 * pad, 18 * s}, truncate(title, 70) + "  \xC2\xB7  " + plural(n, "page"), 11.5f * s, ui.c.textDim);
    y += 18 * s + 10 * s;
    // pages
    ui.text({x, y, 60 * s, rowH}, "Pages", 12.5f * s, ui.c.text);
    vector<string> opts = {"All", "This page", "Pages\xE2\x80\xA6"};
    ui.segmented({x + 64 * s, y, w - 2 * pad - 64 * s, rowH}, opts, R.printPages, "rdprintpages");
    y += rowH;
    if (R.printPages == 2) {
      y += 6 * s;
      bool sub = false;
      ui.textInput({x + 64 * s, y, w - 2 * pad - 64 * s, 28 * s}, "rdprintrange", R.printRange, "for example 1-3, 7, 12-", &sub);
      y += 28 * s;
    }
    y += 10 * s;
    if (nMarks > 0) ui.checkbox({x, y, w - 2 * pad, rowH}, "Print project annotations (" + plural(nMarks, "mark") + ")", R.printMarks);
    else { bool off = false; ui.checkbox({x, y, w - 2 * pad, rowH}, "Print project annotations (none on this paper)", off); }
    y += rowH;
    ui.checkbox({x, y, w - 2 * pad, rowH}, "Fit each page to the printable area", R.printFit);
    y += rowH;
    ui.checkbox({x, y, w - 2 * pad, rowH}, "Turn landscape pages to fit the paper", R.printAutoRotate);
    y += rowH + 6 * s;
    if (R.rot) { ui.text({x, y, w - 2 * pad, 18 * s}, "The pages print turned " + std::to_string(R.rot * 90) + "\xC2\xB0, as the reader shows them.", 11.5f * s, ui.c.textDim); y += 18 * s; }
    y += 12 * s;
    const float bw = 96 * s;
    if (ui.button({r.r() - pad - bw, y, bw, 30 * s}, "Print\xE2\x80\xA6", BTN_PRIMARY, "print")) {
      vector<int> pages;
      string err;
      if (R.printPages == 0) for (int i = 0; i < n; i++) pages.push_back(i);
      else if (R.printPages == 1) pages.push_back(clampv(readerCurrentPage(), 0, std::max(0, n - 1)));
      else if (!rdParseRange(R.printRange, n, pages, err)) { ui.toast("Print", err, 2, 4); return; }
      ui.closePopup();
      const bool marks = R.printMarks && nMarks > 0, fit = R.printFit, autoRot = R.printAutoRotate;
      // the system's dialog runs outside the frame (it is modal and pumps its own messages)
      post([this, pages, marks, fit, autoRot]() {
        PRINTDLGW pd{};
        pd.lStructSize = sizeof pd;
        pd.hwndOwner = hwnd;
        pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE | PD_HIDEPRINTTOFILE;
        pd.nCopies = 1;
        const BOOL ok = PrintDlgW(&pd);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        if (!ok) {
          const DWORD e = CommDlgExtendedError();
          if (e) ui.toast("Print", "The printer dialog could not open (error " + std::to_string(e) + "). Is a printer installed?", 2, 6);
          needFrame = true;
          return;
        }
        if (!pd.hDC) { ui.toast("Print", "The printer could not be opened.", 2, 4); return; }
        readerPrint(pd.hDC, 0, 0, pages, marks, fit, autoRot, std::max(1, int(pd.nCopies)));
      });
    }
    if (ui.button({r.r() - pad - bw - 8 * s - 80 * s, y, 80 * s, 30 * s}, "Cancel", BTN_GHOST)) ui.closePopup();
  });
}

// ------------------------------------------------------------------ the job
void App::readerPrint(HDC hdc, int, int, const vector<int>& pagesIn, bool marks, bool fit, bool autoRotate, int copies) {
  if (!rd || pagesIn.empty()) { DeleteDC(hdc); return; }
  ReaderState& R = *rd;
  PdfItem* item = readerItem();
  if (!item) { DeleteDC(hdc); return; }
  readerEnsureEngine();
  auto ctx = std::make_shared<PrintCtx>();
  ctx->path = item->path;
  ctx->password = R.password;
  const int rot = R.rot;
  const vector<int> pages = pagesIn;
  std::map<int, vector<pdf::Mark>> marksByPage;
  if (marks) for (int pg : pages) if (!marksByPage.count(pg)) marksByPage[pg] = rdMarksOf(*item, pg);
  const string docName = item->title.empty() ? fileName(item->path) : item->title;
  pdf::Worker* worker = &R.worker;
  readerPrinting = true;
  const int resX = std::max(72, int(GetDeviceCaps(hdc, LOGPIXELSX))), resY = std::max(72, int(GetDeviceCaps(hdc, LOGPIXELSY)));
  const int areaW = std::max(1, int(GetDeviceCaps(hdc, HORZRES))), areaH = std::max(1, int(GetDeviceCaps(hdc, VERTRES)));
  auto run = [this, hdc, ctx, pages, marksByPage, fit, autoRotate, copies, rot, docName, worker, resX, resY, areaW, areaH](Job& job) {
    // one engine task at a time, awaited here; the reader's tiles interleave
    auto onWorker = [worker](int prio, std::function<void(pdf::Worker&)> fn) {
      if (!worker->running()) return;  // the application is closing
      auto done = std::make_shared<std::promise<void>>();
      auto fut = done->get_future();
      worker->submit(prio, [fn, done](pdf::Worker& wk) { try { fn(wk); } catch (...) {} done->set_value(); });
      fut.wait();  // a task dropped by a stopping engine breaks the promise, which also ends the wait
    };
    struct Cleanup {  // the private document goes on the engine thread, the printer DC here
      std::function<void(int, std::function<void(pdf::Worker&)>)> onWorker;
      std::shared_ptr<PrintCtx> ctx;
      HDC hdc;
      bool docStarted = false, aborted = false;
      ~Cleanup() {
        if (docStarted) { if (aborted) AbortDoc(hdc); else EndDoc(hdc); }
        DeleteDC(hdc);
        onWorker(0, [ctx = ctx](pdf::Worker&) { ctx->doc.reset(); });
      }
    } cleanup{onWorker, ctx, hdc};
    onWorker(0, [ctx](pdf::Worker&) {
      ctx->doc = std::make_unique<pdf::Document>();
      if (!ctx->doc->open(ctx->path, &ctx->error, ctx->password)) { if (ctx->error.empty()) ctx->error = "The file could not be opened."; ctx->doc.reset(); }
      else {
        ctx->info = ctx->doc->info();
        // Burn the project's current marks exactly once below. The PDF may contain stale copies from a prior explicit
        // save, so strip only our /NM namespace from this private print copy; other viewers' annotations remain.
        for (int pg = 0; pg < ctx->info.pages; pg++) ctx->doc->removeAnnots(pg, [](const pdf::FileAnnot& a) { return startsWith(a.name, "vs:"); });
      }
    });
    if (!ctx->doc) { job.error = "The PDF could not be opened for printing: " + ctx->error; return; }
    DOCINFOW di{};
    di.cbSize = sizeof di;
    const std::wstring wname = widen(docName);
    di.lpszDocName = wname.c_str();
    if (StartDocW(hdc, &di) <= 0) { job.error = "The printer refused the document (StartDoc failed)."; return; }
    cleanup.docStarted = true;
    const int total = int(pages.size()) * std::max(1, copies);
    int done = 0;
    for (int copy = 0; copy < std::max(1, copies); copy++) {
      for (int pg : pages) {
        if (job.cancel) { cleanup.aborted = true; post([this]() { ui.toast("Printing cancelled", "", 1, 2); }); return; }
        if (pg < 0 || size_t(pg) >= ctx->info.sizes.size()) continue;
        const int shown = pg + 1, of = int(pages.size());
        post([this, shown, of, copy, copies]() { jobLabel = "Printing page " + std::to_string(shown) + (of > 1 ? " of " + std::to_string(of) : "") + (copies > 1 ? ", copy " + std::to_string(copy + 1) : ""); needFrame = true; });
        // the page as the reader shows it, turned once more when a landscape page meets portrait paper (or the reverse)
        const float pw = ctx->info.sizes[size_t(pg)][0], ph = ctx->info.sizes[size_t(pg)][1];
        int prot = rot;
        float dw = (prot & 1) ? ph : pw, dh = (prot & 1) ? pw : ph;
        if (autoRotate && (dw > dh) != (areaW > areaH)) { prot = (prot + 1) & 3; std::swap(dw, dh); }
        // the printed size: fitted into the printable area, or actual size (centred, clipped when larger)
        const double pxX = resX / 72.0, pxY = resY / 72.0;
        double sf = 1;
        if (fit) sf = std::min(double(areaW) / (dw * pxX), double(areaH) / (dh * pxY));
        const int outW = std::max(1, int(std::lround(dw * pxX * sf))), outH = std::max(1, int(std::lround(dh * pxY * sf)));
        const int ox = (areaW - outW) / 2, oy = (areaH - outH) / 2;
        // rendered at the printer's resolution up to 300 dpi, and at most ~30 megapixels (posters)
        double dpi = std::min(300.0, double(std::max(resX, resY))) * sf;
        double scale = dpi / 72.0;
        if (dw * scale * dh * scale > 30e6) scale = std::sqrt(30e6 / (double(dw) * double(dh)));
        const int bw = std::max(1, int(std::ceil(dw * scale))), bh = std::max(1, int(std::ceil(dh * scale)));
        auto bm = std::make_shared<pdf::Bitmap>();
        auto pageMarks = marksByPage.count(pg) ? marksByPage.at(pg) : vector<pdf::Mark>();
        bool ok = false;
        onWorker(15, [&](pdf::Worker&) {
          if (!ctx->doc) return;
          ok = ctx->doc->render(pg, scale, 0, 0, bw, bh, *bm, true, false, prot);
          if (ok && !pageMarks.empty()) pdf::burnMarks(*bm, scale, prot, pw, ph, 0, 0, pageMarks);
          if (pages.size() > 2) ctx->doc->dropPages();  // a long print keeps no page objects around
        });
        if (!ok) { job.error = "Page " + std::to_string(shown) + " could not be rendered."; cleanup.aborted = true; return; }
        if (StartPage(hdc) <= 0) { job.error = "The printer refused page " + std::to_string(shown) + "."; cleanup.aborted = true; return; }
        rdBlit(hdc, ox, oy, outW, outH, *bm);
        bm->bgra.clear();
        bm->bgra.shrink_to_fit();
        if (EndPage(hdc) <= 0) { job.error = "The printer stopped at page " + std::to_string(shown) + "."; cleanup.aborted = true; return; }
        done++;
        job.progress = double(done) / std::max(1, total);
      }
    }
    const int printed = done;
    post([this, printed, copies]() { ui.toast("Sent to the printer", plural(printed / std::max(1, copies), "page") + (copies > 1 ? " \xC3\x97 " + std::to_string(copies) : ""), 1, 4); });
  };
  if (!startJob("Printing", run, [this]() { readerPrinting = false; })) { readerPrinting = false; DeleteDC(hdc); }
}

}  // namespace win
}  // namespace vs
