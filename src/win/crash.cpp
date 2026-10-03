// VOSStudio Native: crash reports and recovery. On an unhandled exception the handler writes, in this order, a marker
// (pending.txt), a readable report with the stack, a minidump and a recovery copy of the open project, all under
// %LOCALAPPDATA%\VOSStudio. The next start offers Recover, Show Report or Discard. Nothing is sent anywhere.
#include "app.h"

#include <dbghelp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <exception>

namespace vs {
namespace win {

namespace {

App* g_app = nullptr;
wchar_t g_crashDir[MAX_PATH] = {0};
wchar_t g_recoveryDir[MAX_PATH] = {0};
volatile LONG g_inCrash = 0;
const char* kVersionText = kAppVersion;

std::wstring localDir(const wchar_t* sub) {
  wchar_t p[MAX_PATH] = {0};
  std::wstring d;
  if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p) == S_OK) d = std::wstring(p) + L"\\VOSStudio";
  else d = widen(appDataDir());
  CreateDirectoryW(d.c_str(), nullptr);
  d += L"\\";
  d += sub;
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

// plain Win32 file writes: the C++ runtime may be in a bad state
void writeRaw(const std::wstring& path, const string& text, bool append = false) {
  HANDLE h = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, 0, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  WriteFile(h, text.data(), DWORD(text.size()), &w, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
}

const char* codeName(DWORD c) {
  switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "Access violation";
    case EXCEPTION_STACK_OVERFLOW: return "Stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "Integer division by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "Illegal instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "Array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "In-page error";
    case EXCEPTION_PRIV_INSTRUCTION: return "Privileged instruction";
    case 0xE06D7363: return "C++ exception";
    case 0xE0000001: return "Unhandled C++ exception (terminate)";
    default: return "Exception";
  }
}

string hex64(uint64_t v) {
  char b[24];
  snprintf(b, sizeof b, "0x%016llx", (unsigned long long)v);
  return b;
}

string moduleOf(uint64_t addr, uint64_t* base) {
  HMODULE m = nullptr;
  *base = 0;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)addr, &m) || !m) return "?";
  wchar_t p[MAX_PATH] = {0};
  GetModuleFileNameW(m, p, MAX_PATH);
  std::wstring s(p);
  size_t k = s.find_last_of(L"\\/");
  *base = uint64_t(uintptr_t(m));
  return narrow(k == std::wstring::npos ? s : s.substr(k + 1));
}

string stackText(CONTEXT* ctx0) {
  string out;
  if (!ctx0) {
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
      uint64_t base = 0;
      string mod = moduleOf(uint64_t(uintptr_t(frames[i])), &base);
      out += "  #" + std::to_string(i) + "  " + mod + " + " + hex64(uint64_t(uintptr_t(frames[i])) - base) + "\r\n";
    }
    return out;
  }
  CONTEXT ctx = *ctx0;
  HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
  bool sym = SymInitialize(proc, nullptr, TRUE);
  STACKFRAME64 f{};
#ifdef _M_X64
  DWORD mach = IMAGE_FILE_MACHINE_AMD64;
  f.AddrPC.Offset = ctx.Rip; f.AddrFrame.Offset = ctx.Rbp; f.AddrStack.Offset = ctx.Rsp;
#else
  DWORD mach = IMAGE_FILE_MACHINE_AMD64;
#if defined(__x86_64__)
  f.AddrPC.Offset = ctx.Rip; f.AddrFrame.Offset = ctx.Rbp; f.AddrStack.Offset = ctx.Rsp;
#endif
#endif
  f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 48; i++) {
    if (!StackWalk64(mach, proc, thr, &f, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    if (!f.AddrPC.Offset) break;
    uint64_t base = 0;
    string mod = moduleOf(f.AddrPC.Offset, &base);
    out += "  #" + std::to_string(i) + "  " + mod + " + " + hex64(f.AddrPC.Offset - base);
    if (sym) {
      alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
      SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(buf);
      si->SizeOfStruct = sizeof(SYMBOL_INFO);
      si->MaxNameLen = 255;
      DWORD64 disp = 0;
      if (SymFromAddr(proc, f.AddrPC.Offset, &disp, si)) out += "  " + string(si->Name) + " + " + std::to_string(disp);
    }
    out += "\r\n";
  }
  if (sym) SymCleanup(proc);
  return out;
}

string stampNow(bool file) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  char b[40];
  if (file) snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  else snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return b;
}

void handleCrash(EXCEPTION_POINTERS* ep, const string& what) {
  if (InterlockedExchange(&g_inCrash, 1)) return;  // a crash inside the handler: give up quietly
  string id = "crash-" + stampNow(true);
  std::wstring dir(g_crashDir), rdir(g_recoveryDir);
  std::wstring report = dir + L"\\" + widen(id) + L".txt", dump = dir + L"\\" + widen(id) + L".dmp";
  string projPath;
  bool haveWork = false;
  App* a = g_app;
  if (a && a->P) {
    projPath = a->P->path;  // captured before anything else touches the project
    haveWork = !a->P->corpus.recs.empty() || a->P->net.n() > 0;
  }
  // 1. marker: the next start finds it even if everything below fails
  string marker = "report=" + narrow(report) + "\r\nproject=" + projPath + "\r\nwhen=" + stampNow(false) + "\r\n";
  writeRaw(dir + L"\\pending.txt", marker);
  // 2. readable report
  string r = "VOSStudio " + string(kVersionText) + " closed unexpectedly\r\n" + "Time: " + stampNow(false) + "\r\n";
  if (ep && ep->ExceptionRecord) {
    auto* er = ep->ExceptionRecord;
    uint64_t base = 0;
    string mod = moduleOf(uint64_t(uintptr_t(er->ExceptionAddress)), &base);
    char code[16];
    snprintf(code, sizeof code, "0x%08lx", (unsigned long)er->ExceptionCode);
    r += string("Exception: ") + codeName(er->ExceptionCode) + " (" + code + ") at " + mod + " + " + hex64(uint64_t(uintptr_t(er->ExceptionAddress)) - base) + "\r\n";
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
      r += string("  ") + (er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing") + " address " + hex64(er->ExceptionInformation[1]) + "\r\n";
  }
  if (!what.empty()) r += "Message: " + what + "\r\n";
  if (a && a->P) {
    const Project& P = *a->P;
    r += "\r\nState\r\n";
    r += "  Page " + std::to_string(int(a->page)) + ", view " + std::to_string(int(a->view)) + "\r\n";
    r += "  Records " + std::to_string(P.corpus.recs.size()) + ", map " + std::to_string(P.net.n()) + " items / " + std::to_string(P.net.m()) + " links / " + std::to_string(P.net.nClusters) + " clusters\r\n";
    r += "  Analysis type " + std::to_string(int(P.spec.type)) + ", unit " + std::to_string(int(P.spec.unit)) + ", min " + std::to_string(P.spec.min) + "\r\n";
    r += string("  Background task: ") + (a->busy() ? a->jobLabel : string("none")) + "\r\n";
    r += string("  Agent: ") + (a->agent.active ? "running, step " + std::to_string(a->agent.steps) : string("idle")) + "\r\n";
    r += "  Project file: " + (projPath.empty() ? string("(not saved)") : projPath) + "\r\n";
  }
  r += "\r\nStack\r\n" + stackText(ep ? ep->ContextRecord : nullptr);
  writeRaw(report, r);
  // 3. minidump
  HANDLE h = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mi{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo), ep ? &mi : nullptr, nullptr, nullptr);
    CloseHandle(h);
  }
  // 4. recovery copy of the open work (may fail if the crash damaged the project)
  if (haveWork && a) {
    std::wstring rec = rdir + L"\\recovery.vosproj";
    DeleteFileW(rec.c_str());
    string err;
    if (a->P->save(narrow(rec), &err)) writeRaw(dir + L"\\pending.txt", "recovery=" + narrow(rec) + "\r\n", true);
  }
  if (a && !a->scriptMode)
    MessageBoxW(a->hwnd, L"VOSStudio closed unexpectedly. A report was saved, and your work will be offered for recovery at the next start.", L"VOSStudio", MB_OK | MB_ICONERROR);
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
  handleCrash(ep, "");
  return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
  string what = "std::terminate";
  try {
    if (auto e = std::current_exception()) std::rethrow_exception(e);
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
    what = "unknown exception";
  }
  handleCrash(nullptr, what);
  TerminateProcess(GetCurrentProcess(), 3);
}

}  // namespace

void crashInstall(App* app) {
  g_app = app;
  wcsncpy(g_crashDir, localDir(L"crashes").c_str(), MAX_PATH - 1);
  wcsncpy(g_recoveryDir, localDir(L"recovery").c_str(), MAX_PATH - 1);
  SetUnhandledExceptionFilter(crashFilter);
  std::set_terminate(onTerminate);
  ULONG stack = 64 * 1024;  // room to write the report after a stack overflow
  SetThreadStackGuarantee(&stack);
}

// called once at start: a pending.txt from the last session opens the recovery dialog
void App::crashCheck() {
  std::wstring pend = std::wstring(g_crashDir) + L"\\pending.txt";
  bool ok = false;
  string t = readFileU(narrow(pend), &ok);
  if (!ok) return;
  crash = CrashInfo();
  for (auto& l : split(t, '\n')) {
    string x = trim(l);
    size_t k = x.find('=');
    if (k == string::npos) continue;
    string key = x.substr(0, k), v = x.substr(k + 1);
    if (key == "report") crash.report = v;
    else if (key == "project") crash.project = v;
    else if (key == "when") crash.when = v;
    else if (key == "recovery") crash.recovery = v;
  }
  if (!crash.recovery.empty() && GetFileAttributesW(widen(crash.recovery).c_str()) == INVALID_FILE_ATTRIBUTES) crash.recovery.clear();
  crash.pending = true;
  crashDialog = true;
  ui.openModal("crash");
}

static void crashResolve(App& a, bool deleteRecovery) {
  DeleteFileW((std::wstring(g_crashDir) + L"\\pending.txt").c_str());
  if (deleteRecovery && !a.crash.recovery.empty()) DeleteFileW(widen(a.crash.recovery).c_str());
  a.crash.pending = false;
  a.crashDialog = false;
  a.ui.closeModal();
}

void App::drawCrashDialog() {
  if (!crashDialog) return;
  if (!ui.isModalOpen("crash")) ui.openModal("crash");
  ui.beginModalLayer();
  float s = ui.s;
  Rect full{0, 0, float(g.W), float(g.H)};
  ui.fill(full, Color(0, 0, 0, ui.dark ? 0.40f : 0.16f));
  bool canRecover = !crash.recovery.empty();
  float w = std::min(480 * s, float(g.W) - 60 * s);
  string msg = canRecover ? "Your work was saved when it happened" + (crash.when.empty() ? string() : " (" + crash.when + ")") + ". Recover it to continue where you left off."
                          : "The open work could not be saved. A report describes what happened.";
  if (canRecover && !crash.project.empty()) msg += " Saving afterwards writes to " + fileName(crash.project) + ".";
  float msgW = w - 96 * s;
  float msgH = ui.textWrap({0, 0, msgW, 1000}, msg, 12.5f * s, ui.c.textDim, 400, false);
  float h = 50 * s + msgH + 14 * s + 16 * s + 72 * s;
  Rect d{std::round((g.W - w) / 2), std::round((g.H - h) / 2 - 30 * s), w, h};
  ui.shadow(d, 12 * s, 28 * s);
  ui.fill(d, ui.c.panel2, 12 * s);
  ui.stroke(d, ui.c.border, 12 * s);
  ui.circle(d.x + 40 * s, d.y + 42 * s, 17 * s, ui.c.warn.withA(ui.dark ? 0.22f : 0.14f));
  ui.icon("warn", d.x + 40 * s, d.y + 42 * s, 17 * s, ui.c.warn, 1.8f);
  float x = d.x + 72 * s, cw = d.r() - 24 * s - x;
  ui.text({x, d.y + 22 * s, cw, 22 * s}, "VOSStudio closed unexpectedly", 15 * s, ui.c.text, AL_LEFT, 650);
  ui.textWrap({x, d.y + 50 * s, cw, msgH + 4}, msg, 12.5f * s, ui.c.textDim);
  ui.text({x, d.y + 50 * s + msgH + 14 * s, cw, 16 * s}, "The report stays on this computer. Nothing is sent.", 11.5f * s, ui.c.textFaint);
  float by = d.b() - 52 * s, bx = d.r() - 24 * s;
  auto btn = [&](const string& label, int kind, bool en = true) {
    float bw = ui.textW(label, 12.5f * s, 500) + 30 * s;
    bx -= bw;
    Rect b{bx, by, bw, 32 * s};
    bx -= 8 * s;
    return ui.button(b, label, kind, "", en);
  };
  if (canRecover && btn("Recover", BTN_PRIMARY)) {
    string rec = crash.recovery, proj = crash.project;
    crashResolve(*this, false);
    ui.endModalLayer();
    showStart = false;
    cmdOpenProject(rec);
    if (P) {
      P->path = proj;  // Save goes to the original file (or asks for a name)
      P->dirty = true;
    }
    DeleteFileW(widen(rec).c_str());
    ui.toast("Work recovered", proj.empty() ? string("Save the project to keep it.") : "Save to update " + fileName(proj) + ".", 1, 5);
    return;
  }
  if (btn("Discard", canRecover ? BTN_NORMAL : BTN_PRIMARY)) { crashResolve(*this, true); ui.endModalLayer(); return; }
  if (!crash.report.empty() && btn("Show Report", BTN_GHOST)) {
    std::wstring args = L"/select,\"" + widen(crash.report) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  }
  ui.endModalLayer();
}

// script command "crashtest": exercises the handler
void crashTest() {
  volatile int* p = nullptr;
  *p = 42;
}

}  // namespace win
}  // namespace vs
