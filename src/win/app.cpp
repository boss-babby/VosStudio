// VOSStudio Native — application shell
#if !defined(WINVER) || WINVER < 0x0A00
#undef WINVER
#define WINVER 0x0A00  // the application requires Windows 10; WM_TOUCH carries native multi-contact input
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "app.h"

#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "../core/oa.h"
#include "../core/records.h"
#include "../core/semantic.h"
#include "../core/world.h"

namespace vs {
namespace win {

App* gApp = nullptr;

namespace {
const char* kPageTitles[PG_COUNT] = {"Data", "Build", "Look", "Analyse", "Trends", "Actors", "Read", "Write", "Publish", "Assistant"};
const char* kPageSubs[PG_COUNT] = {"Import, merge and clean bibliographic records", "Choose what to map: type, unit, counting and thresholds",
                                   "Colours, sizes, labels, links and density", "Clusters, items, network statistics and stability",
                                   "Growth, bursts, thematic evolution and comparisons", "Authors, sources, countries, organisations and laws",
                                   "Read the papers: attach PDFs, highlight and code passages, take notes, keep the reading status",
                                   "Write the report: pages, figures, tables, citations; export as PDF, Word or HTML",
                                   "Publication figures, exports and project bundles", "AI research assistant for your data and map"};
const char* kPageIcons[PG_COUNT] = {"data", "network", "look", "analyse", "trends", "actors", "book", "writer", "publish", "sparkle"};
enum class RailAction { Page, Map, Papers, Review, Write };
struct RailItem { const char* id; const char* label; const char* hint; const char* icon; RailAction action; int page; };
const RailItem kVisualizationNav[] = {
    {"data", "Data", "Import, merge and clean records", "data", RailAction::Page, PG_DATA},
    {"build", "Build", "Choose what to map and build a network", "network", RailAction::Page, PG_BUILD},
    {"map", "Map", "Explore the shared network and its visual views", "map", RailAction::Map, PG_NONE},
    {"style", "Style", "Colours, sizes, labels, links and density", "look", RailAction::Page, PG_LOOK},
    {"analyse", "Analyse", "Clusters, items, network statistics and stability", "analyse", RailAction::Page, PG_ANALYSE},
    {"trends", "Trends", "Growth, bursts, thematic evolution and comparisons", "trends", RailAction::Page, PG_TRENDS},
    {"actors", "Actors", "Authors, sources, countries and organisations", "actors", RailAction::Page, PG_ACTORS},
    {"publish", "Figures", "Compose and export publication figures", "publish", RailAction::Page, PG_PUBLISH},
};
const RailItem kBibliographyNav[] = {
    {"papers", "Papers", "Search and manage the project bibliography", "book", RailAction::Papers, PG_NONE},
    {"review", "Review", "Reading queue, statuses, notes and PDF annotations", "check", RailAction::Review, PG_READ},
    {"write", "Write", "Write and cite from the shared bibliography", "writer", RailAction::Write, PG_WRITER},
};
struct ViewDef { ViewKind v; const char* label; const char* icon; };
const ViewDef kViews[] = {{ViewKind::Network, "Network", "network"}, {ViewKind::Overlay, "Overlay", "clock"}, {ViewKind::Density, "Density", "density"},
                          {ViewKind::Timeline, "Timeline", "trends"}, {ViewKind::Matrix, "Matrix", "grid"}, {ViewKind::Geo, "Geo", "globe"},
                          {ViewKind::ThreeD, "3D", "cube"}};
double tlS0 = 0, tlS1 = 1, tlX0 = 0, tlX1 = 1;  // timeline: score range → world x range
vector<Rect> chromeRects;                        // canvas overlays that swallow mouse input
bool pendingShot = false;
string pendingShotPath;
bool pendingViewCrop = false;
double lastDt = 0.016;

float easeInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }

bool isPromotedTouchMouseMessage(UINT msg) {
  return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_NCMOUSEMOVE && msg <= WM_NCMBUTTONDBLCLK);
}
bool isPromotedTouchMouse() {
  const ULONG_PTR extra = ULONG_PTR(GetMessageExtraInfo());
  return (extra & ULONG_PTR(0xFFFFFF00u)) == ULONG_PTR(0xFF515700u) && (extra & ULONG_PTR(0x80u)) != 0;
}
}  // namespace

double App::lastDtSec() const { return lastDt; }

// =====================================================================================
// init / message loop
// =====================================================================================
static LRESULT CALLBACK StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (gApp) return gApp->wndProc(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}

static float windowDpiScale(HWND h) {
  typedef UINT(WINAPI * GetDpiForWindow_t)(HWND);
  static auto fn = (GetDpiForWindow_t)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
  if (fn && h) { UINT d = fn(h); if (d) return float(d) / 96.f; }
  HDC dc = GetDC(nullptr);
  int d = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return d > 0 ? float(d) / 96.f : 1.f;
}

bool App::init(HINSTANCE hi, int show, const string& scriptPath, const vector<string>& openPaths) {
  gApp = this;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  {
    typedef BOOL(WINAPI * SetCtx_t)(HANDLE);
    auto fn = (SetCtx_t)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!fn || !fn((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
  }
  settings.load();
  linkScope = settings.j["linkScope"].boolean(true);
  Workspace initialWorkspace = settings.j["workspace"].integer(WS_VISUALIZATION) == WS_BIBLIOGRAPHY ? WS_BIBLIOGRAPHY : WS_VISUALIZATION;
  int savedVisualPage = settings.j["workspaceVisualPage"].integer(PG_DATA);
  if (savedVisualPage < PG_NONE || savedVisualPage > PG_PUBLISH || savedVisualPage == PG_READ || savedVisualPage == PG_WRITER) savedVisualPage = PG_DATA;
  lastVisualizationPage = savedVisualPage;
  page = lastVisualizationPage;
  int savedBiblioRoute = clampv(settings.j["workspaceBibliographyRoute"].integer(BR_PAPERS), int(BR_PAPERS), int(BR_WRITE));
  lastBibliographyRoute = BibliographyRoute(savedBiblioRoute);
  railExpanded = settings.j["railExpanded"].boolean(true);
  workspace = WS_VISUALIZATION;  // setWorkspace(initialWorkspace) applies the saved route after Project exists
  setAppVersion(kAppVersion);
  crashInstall(this);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = StaticWndProc;
  wc.hInstance = hi;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(hi, L"APPICON");
  if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wc.lpszClassName = L"VOSStudioNative";
  RegisterClassExW(&wc);
  float s0 = windowDpiScale(nullptr);
  int W = int(1480 * s0), H = int(920 * s0);
  RECT wa;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  {  // respect "Show animations in Windows" (Settings › Accessibility › Visual effects)
    BOOL anim = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0)) reduceMotion = !anim;
  }
  W = std::min(W, int(wa.right - wa.left) - 40);
  H = std::min(H, int(wa.bottom - wa.top) - 40);
  if (!scriptPath.empty()) { W = 1600; H = 960; }
  hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"VOSStudio", WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - W) / 2,
                         wa.top + (wa.bottom - wa.top - H) / 2, W, H, nullptr, nullptr, hi, nullptr);
  if (!hwnd) return false;
  touchRegistered_ = RegisterTouchWindow(hwnd, 0) != FALSE;
  {  // one unified window: the client area covers the caption; DWM keeps the shadow, snapping and (Windows 11) rounded corners
    MARGINS m{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof corner);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  string err;
  if (!g.init(hwnd, &err)) {
    MessageBoxW(hwnd, widen("Direct3D 11 / Direct2D could not be initialised.\n\n" + err).c_str(), L"VOSStudio", MB_ICONERROR);
    return false;
  }
  if (!nv.init(g, &err)) gpuError = err;
  ui.init(g, hwnd);
  registerHelp();
  ui.s = windowDpiScale(hwnd);
  leftW = 344 * ui.s;
  rightW = 300 * ui.s;
  bool dark = settings.j.has("dark") ? settings.j["dark"].boolean(true) : true;
  canvasCacheOn = settings.j["canvasCache"].boolean(true);
  writerWordSources_ = settings.j["writerWordSources"].boolean(true);
  splitLoad();
  ui.textCacheOn = settings.j["textCache"].boolean(true);
  P = std::make_unique<Project>();
  setTheme(dark);
  styleCommitted = styleToJson(P->style);
  buildCommands();
  if (initialWorkspace != WS_VISUALIZATION) setWorkspace(initialWorkspace);
  if (!scriptPath.empty()) {
    bool ok = false;
    string t = readFileU(scriptPath, &ok);
    if (ok) { for (auto& l : split(t, '\n')) { string x = trim(l); if (!x.empty() && x[0] != '#') script.push_back(x); } }
    scriptMode = true;
  }
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);
  showStart = !scriptMode && openPaths.empty();
  crashCheck();
  if (!openPaths.empty()) dropFiles(openPaths);
  if (!gpuError.empty()) ui.toast("GPU renderer", gpuError, 3, 10);
  return true;
}

// Message loop tuned for low idle cost:
//  * nothing changes -> block in the message queue (no timer), 0 % CPU and GPU;
//  * a text field has focus -> wake twice a second for the caret;
//  * a background job runs -> ~10 frames per second for the progress bar (the job gets the CPU);
//  * minimised or fully covered -> no rendering at all; finished jobs are still handled.
int App::run() {
  MSG msg;
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return int(msg.wParam);
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (job) job->notify = hwnd;
    const double rdDue = readerIdleTick();  // a closed reader frees its document after a while
    bool iconic = IsIconic(hwnd) != 0;
    if ((iconic || occluded) && !scriptMode) {
      pumpWork();
      if (occluded && !iconic && g.presentTest()) { occluded = false; needFrame = true; continue; }
      MsgWaitForMultipleObjects(0, nullptr, FALSE, occluded ? 250 : (busy() ? 500 : INFINITE), QS_ALLINPUT);
      continue;
    }
    bool smooth = ui.animating || animT0 >= 0 || camTargetZoom > 0 || scriptMode || !uiQueue.empty() || !ui.toasts.empty() || (job && job->finished) ||
                  themeAnim.active || themeAnim.pending;
    if (needFrame || smooth) {
      needFrame = false;
      liveOverlayFrame_ = false;
      frame();
    } else if (live.open && (live.dirty || live.animFps > 0)) {
      // only the Live assistant's overlay moves: redraw it alone, at its own modest rate, over the cached window
      double period = 1.0 / (live.dirty ? std::max(30, live.animFps) : std::max(1, live.animFps));
      double due = liveFrameT_ + period - nowSeconds();
      if (due > 0.0015) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(std::ceil(due * 1000)), QS_ALLINPUT);
      } else {
        liveOverlayFrame_ = liveCached_;
        frame();
      }
    } else if (busy()) {
      if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_TIMEOUT) needFrame = true;
    } else {
      DWORD wait = ui.wantsCaret ? 530 : (perfHud ? 500 : INFINITE);
      if (rdDue >= 0) wait = std::min(wait, DWORD(rdDue * 1000) + 50);
      if (ui.wakeAt > 0) {
        double ms = std::max(0.0, (ui.wakeAt - nowSeconds()) * 1000.0);
        wait = std::min(wait, DWORD(std::ceil(std::min<double>(ms, double(INFINITE - 1)))));
      }
      if (MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT) == WAIT_TIMEOUT) needFrame = true;
    }
  }
}

// posted work and finished jobs (also runs while minimised, without rendering)
uint64_t App::chartSig() const {
  uint64_t h = 1469598103934665603ull;
  auto mixIn = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
  mixIn(uint64_t(reinterpret_cast<uintptr_t>(P.get())));
  if (P) {
    mixIn(P->corpusVersion);
    mixIn(uint64_t(P->net.n()));
    mixIn(uint64_t(P->net.links.size()));
    mixIn(uint64_t(P->net.quality * 1e9));
    mixIn(uint64_t(P->corpus.recs.size()));
  }
  mixIn(uint64_t(page));
  return h;
}

void App::pumpWork() {
  {
    std::deque<std::function<void()>> q;
    { std::lock_guard<std::mutex> l(qmu); q.swap(uiQueue); }
    for (auto& f : q) f();
  }
  if (job && job->finished) {
    job->join();
    string err = job->error;
    auto done = job->onDone;
    string lbl = jobLabel;
    job.reset();
    lastJobError = err;
    if (!err.empty()) ui.toast(lbl + " failed", err, 3, 7);
    else if (done) done();
    jobSeq++;
    needFrame = true;
  }
}

void App::touchPoint(DWORD id, POINT p, bool isDown, bool isUp) {
  const double now = nowSeconds();
  input.touch = true;
  lastInputT = now;
  bool pinchStarted = false;

  if (isDown) {
    const bool first = touchPoints_.empty();
    touchPoints_[id] = p;
    if (first) {
      touchPinching_ = false;
      touchPinchDistance_ = 0;
      touchTapCandidate_ = true;
      touchTapMoved_ = false;
      touchTapStart_ = p;
      const float doubleTapRadius = 14 * ui.s;
      if (touchLastTapT_ > 0 && now - touchLastTapT_ <= 0.48 &&
          std::hypot(float(p.x - touchLastTap_.x), float(p.y - touchLastTap_.y)) <= doubleTapRadius) input.dbl = true;
      input.mx = float(p.x);
      input.my = float(p.y);
      input.down[0] = true;
      input.pressed[0] = true;
      input.released[0] = false;
    } else if (!touchPinching_ && touchPoints_.size() >= 2) {
      // A second contact cancels a pending click or one-finger drag; the pair becomes a pinch gesture.
      touchPinching_ = true;
      touchPinchDistance_ = 0;
      touchTapCandidate_ = false;
      touchTapMoved_ = true;
      input.down[0] = false;
      input.pressed[0] = false;
      input.released[0] = false;
      input.touchCancel = true;
      pinchStarted = true;
    }
  } else {
    auto it = touchPoints_.find(id);
    if (it == touchPoints_.end()) return;
    it->second = p;
  }

  auto syncPinch = [&]() {
    if (touchPoints_.size() < 2) return;
    auto a = touchPoints_.begin();
    auto b = std::next(a);
    const float dx = float(a->second.x - b->second.x), dy = float(a->second.y - b->second.y);
    const float distance = std::hypot(dx, dy);
    input.mx = (float(a->second.x) + float(b->second.x)) * 0.5f;
    input.my = (float(a->second.y) + float(b->second.y)) * 0.5f;
    if (!pinchStarted && touchPinchDistance_ > 1.f && distance > 1.f) {
      const float delta = std::log(distance / touchPinchDistance_) / std::log(1.2f);
      if (std::isfinite(delta)) input.pinch += delta;
    }
    touchPinchDistance_ = distance;
  };

  if (touchPinching_) syncPinch();
  else if (!isDown && touchPoints_.size() == 1) {
    const POINT q = touchPoints_.begin()->second;
    input.mx = float(q.x);
    input.my = float(q.y);
    input.down[0] = true;
    const float moveThreshold = 8 * ui.s;
    if (touchTapCandidate_ && std::hypot(float(q.x - touchTapStart_.x), float(q.y - touchTapStart_.y)) >= moveThreshold) touchTapMoved_ = true;
  }

  if (isUp) {
    auto it = touchPoints_.find(id);
    if (it != touchPoints_.end()) {
      if (!touchPinching_ && touchPoints_.size() == 1) {
        input.mx = float(p.x);
        input.my = float(p.y);
        input.down[0] = false;
        input.released[0] = true;
        if (touchTapCandidate_ && !touchTapMoved_) {
          touchLastTap_ = p;
          touchLastTapT_ = now;
        }
        touchTapCandidate_ = false;
      } else {
        // Finish the pinch at the last two-contact midpoint. A remaining finger never turns into a new click.
        if (touchPinching_ && touchPoints_.size() >= 2) syncPinch();
        input.down[0] = false;
        input.released[0] = false;
        touchPinchDistance_ = 0;
      }
      touchPoints_.erase(it);
      if (touchPoints_.empty()) {
        touchPinching_ = false;
        touchPinchDistance_ = 0;
        touchTapCandidate_ = false;
      }
    }
  }
  needFrame = true;
}

LRESULT App::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  // WM_TOUCH is handled below; ignore its compatibility mouse stream to avoid double activation.
  if (touchRegistered_ && isPromotedTouchMouseMessage(msg) && isPromotedTouchMouse()) return 0;
  switch (msg) {
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) {
        // give memory back while minimised: GPU transient allocations and the process working set
        if (g.dev) g.trim();
        SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
      } else if (g.dev) { occluded = false; g.resize(LOWORD(lp), HIWORD(lp)); needFrame = true; frame(); }
      return 0;
    case WM_ACTIVATEAPP:
      if (wp) { occluded = false; needFrame = true; }
      else if (!touchPoints_.empty()) {
        touchPoints_.clear();
        touchPinching_ = false;
        touchPinchDistance_ = 0;
        touchTapCandidate_ = false;
        input.down[0] = false;
        input.released[0] = true;
        input.touchCancel = true;
        needFrame = true;
      }
      return 0;
    case WM_NCCALCSIZE:
      if (wp) {
        // the whole window is client area; when maximised keep it on the monitor's work area (taskbar stays visible)
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
        if (IsZoomed(h)) {
          MONITORINFO mi{};
          mi.cbSize = sizeof mi;
          if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) p->rgrc[0] = mi.rcWork;
        }
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(h, &pt);
      RECT rc;
      GetClientRect(h, &rc);
      if (!IsZoomed(h)) {
        int b = std::max(4, int(6 * ui.s));
        bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bo = pt.y >= rc.bottom - b;
        int cb = b * 3;  // generous corners
        bool lc = pt.x < cb, rcn = pt.x >= rc.right - cb, tc = pt.y < cb, bc = pt.y >= rc.bottom - cb;
        if ((t && lc) || (l && tc)) return HTTOPLEFT;
        if ((t && rcn) || (r && tc)) return HTTOPRIGHT;
        if ((bo && lc) || (l && bc)) return HTBOTTOMLEFT;
        if ((bo && rcn) || (r && bc)) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (bo) return HTBOTTOM;
      }
      float x = float(pt.x), y = float(pt.y);
      Rect mb = captionBtn(1);
      if (x >= mb.x && x < mb.r() && y >= mb.y && y < mb.b()) return HTMAXBUTTON;  // Windows 11 snap layouts on hover
      float capH = topR.h > 0 ? topR.h : 52 * ui.s;
      if (y < capH && !ui.widgetAt(x, y)) return HTCAPTION;
      return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(h, &pt);
      const float x = float(pt.x), y = float(pt.y);
      const bool moved = x != input.mx || y != input.my;
      input.mx = x;
      input.my = y;
      if (moved) {
        lastInputT = nowSeconds();
        if (pointerMoveNeedsFrame(x, y)) needFrame = true;
      }
      TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE | TME_NONCLIENT, h, 0};
      TrackMouseEvent(&tme);
      break;
    }
    case WM_NCMOUSELEAVE:
      input.mx = input.my = -1;
      maxBtnDown = false;
      needFrame = true;
      break;
    case WM_NCLBUTTONDOWN:
      if (wp == HTMAXBUTTON) { maxBtnDown = true; needFrame = true; return 0; }
      break;
    case WM_NCLBUTTONUP:
      if (wp == HTMAXBUTTON) {
        if (maxBtnDown) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
        maxBtnDown = false;
        needFrame = true;
        return 0;
      }
      maxBtnDown = false;
      break;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize.x = LONG(960 * ui.s);
      mm->ptMinTrackSize.y = LONG(600 * ui.s);
      return 0;
    }
    case WM_DPICHANGED: {
      ui.s = HIWORD(wp) / 96.f;
      leftW = 344 * ui.s;
      rightW = 300 * ui.s;
      RECT* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      needFrame = true;
      return 0;
    }
    case WM_MOUSEMOVE: {
      const float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
      const bool moved = x != input.mx || y != input.my;
      input.mx = x;
      input.my = y;
      if (moved) {
        lastInputT = nowSeconds();
        if (pointerMoveNeedsFrame(x, y)) needFrame = true;
      }
      TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE:
      input.mx = input.my = -1;
      needFrame = true;
      return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: {
      int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? 0 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? 1 : 2;
      input.down[b] = input.pressed[b] = true;
      if (msg == WM_LBUTTONDBLCLK) input.dbl = true;
      input.mx = float(GET_X_LPARAM(lp));
      input.my = float(GET_Y_LPARAM(lp));
      SetCapture(h);
      needFrame = true;
      return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
      int b = msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2;
      input.down[b] = false;
      input.released[b] = true;
      if (!input.down[0] && !input.down[1] && !input.down[2]) ReleaseCapture();
      needFrame = true;
      return 0;
    }
    case WM_MOUSEWHEEL: {
      input.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / 120.f;
      needFrame = true;
      return 0;
    }
    case WM_MOUSEHWHEEL: {
      input.hwheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / 120.f;
      needFrame = true;
      return 0;
    }
    case WM_CHAR: {
      static wchar_t hi = 0;
      wchar_t c = wchar_t(wp);
      if (c >= 0xD800 && c <= 0xDBFF) { hi = c; return 0; }
      char32_t cp = c;
      if (c >= 0xDC00 && c <= 0xDFFF && hi) { cp = 0x10000 + ((char32_t(hi) - 0xD800) << 10) + (char32_t(c) - 0xDC00); hi = 0; }
      if (cp >= 32 && cp != 127) input.chars.push_back(cp);
      needFrame = true;
      return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
      input.keys.push_back(int(wp));
      needFrame = true;
      if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;
      break;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        LPCTSTR c = IDC_ARROW;
        if (ui.cursor == "hand") c = IDC_HAND;
        else if (ui.cursor == "ibeam") c = IDC_IBEAM;
        else if (ui.cursor == "move") c = IDC_SIZEALL;
        else if (ui.cursor == "cross") c = IDC_CROSS;
        else if (ui.cursor == "wait") c = IDC_APPSTARTING;
        else if (ui.cursor == "ew") c = IDC_SIZEWE;
        else if (ui.cursor == "help") c = IDC_HELP;
        SetCursor(LoadCursor(nullptr, c));
        return TRUE;
      }
      break;
    case WM_TOUCH: {
      const UINT count = LOWORD(wp);
      TOUCHINPUT points[256]{};  // Win32 touch frames are bounded well below this fixed stack buffer.
      if (count > 256 || !GetTouchInputInfo(reinterpret_cast<HTOUCHINPUT>(lp), count, points, sizeof(TOUCHINPUT)))
        return DefWindowProcW(h, msg, wp, lp);
      for (UINT i = 0; i < count; ++i) {
        const TOUCHINPUT& ti = points[i];
        POINT pt{LONG(ti.x / 100), LONG(ti.y / 100)};  // TOUCHINPUT coordinates are in hundredths of a physical pixel
        ScreenToClient(h, &pt);
        const bool down = (ti.dwFlags & TOUCHEVENTF_DOWN) != 0;
        const bool up = (ti.dwFlags & TOUCHEVENTF_UP) != 0;
        const bool moved = (ti.dwFlags & TOUCHEVENTF_MOVE) != 0;
        if (down || up || moved) touchPoint(ti.dwID, pt, down, up);
      }
      CloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lp));
      return 0;
    }
    case WM_DROPFILES: {
      HDROP d = reinterpret_cast<HDROP>(wp);
      UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
      vector<string> files;
      for (UINT i = 0; i < n; i++) {
        wchar_t buf[MAX_PATH * 2];
        DragQueryFileW(d, i, buf, MAX_PATH * 2);
        files.push_back(narrow(buf));
      }
      DragFinish(d);
      dropFiles(files);
      needFrame = true;
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      EndPaint(h, &ps);
      needFrame = true;
      return 0;
    }
    case WM_CLOSE:
      if (live.open && P && (hasCorpus() || hasMap()) && !scriptMode) liveSaveToChat();
      if (P && P->dirty && (hasCorpus() || hasMap() || P->library.hasData() || !wdoc.empty()) && !scriptMode) {
        int r = MessageBoxW(h, L"Save changes to this project before closing?", L"VOSStudio", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES) { cmdSaveProject(false); if (P->dirty) return 0; }
      }
      if (job) { job->cancel = true; job->join(); }
      liveDisconnect(false);
      readerStorePosition();
      writerPreviewShutdown();
      readerShutdown();
      settings.j.set("dark", ui.dark);
      settings.j.set("workspace", int(workspace));
      settings.j.set("workspaceVisualPage", lastVisualizationPage);
      settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
      settings.j.set("railExpanded", railExpanded);
      settings.save();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (touchRegistered_) { UnregisterTouchWindow(h); touchRegistered_ = false; }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// =====================================================================================
// frame
// =====================================================================================
void App::setTheme(bool dark) {
  settings.j.set("dark", dark);
  auto& a = themeAnim;
  auto startIcon = [&](bool wasDark) {  // the button's icon turns from the one theme's to the other's
    a.iconFrom = wasDark ? "sun" : "moon";
    a.iconTo = dark ? "sun" : "moon";
    a.iconT0 = nowSeconds();
  };
  if (a.pending) { if (dark != a.pendingDark) { startIcon(a.pendingDark); a.pendingDark = dark; } return; }
  if (a.active) {
    if (dark == a.toDark) return;
    // switched again mid-way: the circle turns round from where it is (the kept picture is the right one, since the
    // way back leads to the theme it shows); the speed stays the same, so the leg takes its share of the full time
    startIcon(a.toDark);
    a.toDark = dark;
    a.r0 = a.r;
    a.r1 = dark ? a.rmax : 0;
    a.t0 = nowSeconds();
    a.dur = themeRevealSeconds() * clampv(std::fabs(a.r1 - a.r0) / std::max(1.f, a.rmax), 0.15f, 1.f);
    needFrame = true;
    return;
  }
  if (dark == ui.dark) return;
  if (!themeRevealAllowed()) { applyTheme(dark); applyFrameTheme(); return; }
  startIcon(ui.dark);
  a.pending = true;
  a.pendingDark = dark;
  needFrame = true;  // this frame ends in the old theme and is kept; the reveal starts with the next one (drawThemeReveal)
}

void App::applyTheme(bool dark) {
  ui.setTheme(dark);
  P->style.darkTheme = dark;
  styleDirty = true;
  figSig.clear();
  settings.j.set("dark", dark);
}

double App::themeRevealSeconds() const { return clampv(settings.j["themeRevealMs"].num(750) / 1000.0, 0.1, 3.0); }

bool App::themeRevealAllowed() const {
  if (scriptMode || frameNo == 0 || !g.backTex || g.W <= 0 || g.H <= 0) return false;
  if (!settings.j["themeReveal"].boolean(true)) return false;
  BOOL anim = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);  // Windows: Accessibility -> Visual effects -> Animation effects
  return anim != FALSE;
}

// The sun / moon button: background as any icon button, the icon of the theme one would switch to; while a switch is
// under way the old icon turns and shrinks away as the new one turns in.
void App::drawThemeButton(const Rect& r, bool hovered, bool held, const UiColors& col) {
  float s = ui.s;
  if (hovered) ui.fill(r, col.hover, 6 * s);
  if (held) ui.fill(r, col.active, 6 * s);
  Color fg = hovered ? col.text : col.textDim;
  float cx = r.x + r.w / 2, cy = r.y + r.h / 2, size = std::min(17 * s, r.h * 0.58f);
  auto& a = themeAnim;
  bool targetDark = themeTarget();
  string to = targetDark ? "sun" : "moon";
  const double morphS = 0.45;
  float p = a.iconT0 < 0 ? 1.f : float(clampv((ui.time - a.iconT0) / morphS, 0.0, 1.0));
  if (p >= 1 || a.iconFrom.empty() || a.iconFrom == a.iconTo) { ui.icon(to, cx, cy, size, fg, 1.6f); return; }
  float e = easeInOut(p);
  auto turn = [&](const string& name, float deg, float sc, float alpha) {
    if (sc < 0.03f || alpha < 0.03f) return;
    D2D1_MATRIX_3X2_F old;
    ui.dc()->GetTransform(&old);
    D2D1_POINT_2F c = D2D1::Point2F(cx, cy);
    ui.dc()->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, c) * D2D1::Matrix3x2F::Rotation(deg, c) * old);
    ui.icon(name, cx, cy, size, fg.withA(fg.a * alpha), 1.6f);
    ui.dc()->SetTransform(old);
  };
  turn(a.iconFrom, 90 * e, 1 - e, 1 - e);
  turn(a.iconTo, -90 * (1 - e), e, e);
  ui.animating = true;
}

void App::drawThemeReveal(double t) {
  auto& a = themeAnim;
  if (a.pending) {  // this frame is complete in the old theme: keep it, switch the colours, and start
    a.pending = false;
    bool toDark = a.pendingDark;
    bool kept = g.copyCapture(themeCopy_) && themeCopy_.w == g.W && themeCopy_.h == g.H;
    a.oldDark = ui.dark;
    applyTheme(toDark);
    if (!kept) { applyFrameTheme(); needFrame = true; return; }
    float s = ui.s;
    if (a.btn.w > 0) { a.ox = a.btn.x + a.btn.w / 2; a.oy = a.btn.y + a.btn.h / 2; }
    else { a.ox = float(g.W) - 6 * s - captionW() - 34 * s * 2.5f; a.oy = 26 * s; }  // where the button sits when the top bar is not shown
    float W = float(g.W), H = float(g.H);
    a.rmax = std::hypot(std::max(a.ox, W - a.ox), std::max(a.oy, H - a.oy)) + 2;
    a.toDark = toDark;
    a.r0 = toDark ? 0 : a.rmax;  // dark grows outward from the button; light closes in from the edges onto it
    a.r1 = toDark ? a.rmax : 0;
    a.r = a.r0;
    a.t0 = t;
    a.dur = themeRevealSeconds();
    a.active = true;
    needFrame = true;
    return;  // the frame on screen is the old theme; the first step follows at once
  }
  if (!a.active) return;
  auto finish = [&]() {
    if (a.toDark != ui.dark) applyTheme(a.toDark);  // a run that turned round ends in the theme of the kept picture
    applyFrameTheme();
    a.active = false;
    g.copyDrop(themeCopy_);
    needFrame = true;
  };
  if (!themeCopy_.valid || themeCopy_.w != g.W || themeCopy_.h != g.H) { finish(); return; }  // resized meanwhile: nothing to show
  float k = float(clampv((t - a.t0) / std::max(0.05, a.dur), 0.0, 1.0));
  a.r = a.r0 + (a.r1 - a.r0) * easeInOut(k);
  float r = std::max(0.f, a.r);
  auto* dc = g.dc.get();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  // The circle is the dark region. The kept picture goes inside it when the picture is the dark one (light closing in
  // on a dark window, or its reversal) and outside it otherwise (dark spreading over a light window).
  Com<ID2D1EllipseGeometry> ell;
  Com<ID2D1PathGeometry> outside;
  ID2D1Geometry* mask = nullptr;
  if (SUCCEEDED(g.d2f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(a.ox, a.oy), r, r), ell.put()))) {
    if (a.oldDark) mask = ell.get();
    else {
      Com<ID2D1RectangleGeometry> rg;
      Com<ID2D1GeometrySink> sink;
      if (SUCCEEDED(g.d2f->CreateRectangleGeometry(D2D1::RectF(0, 0, float(g.W), float(g.H)), rg.put())) && SUCCEEDED(g.d2f->CreatePathGeometry(outside.put())) &&
          SUCCEEDED(outside->Open(sink.put()))) {
        rg->CombineWithGeometry(ell.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.25f, sink.get());
        sink->Close();
        mask = outside.get();
      }
    }
  }
  if (!mask) { finish(); return; }
  D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), mask, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
  dc->PushLayer(&lp, nullptr);
  g.copyRestore(themeCopy_);
  if (a.btn.w > 0) {  // the button in the old theme, its icon turning as well (it lies under the picture until the circle passes it)
    UiColors oc = Ui::palette(a.oldDark);
    ui.fill(a.btn, oc.rail);
    drawThemeButton(a.btn, a.btnHover, false, oc);
  }
  dc->PopLayer();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  if (k >= 1) finish();
  else needFrame = true;
}

void App::applyFrameTheme() {
  if (!hwnd) return;
  BOOL d = ui.dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, 20, &d, sizeof d);  // DWMWA_USE_IMMERSIVE_DARK_MODE: system menu, borders and snap previews follow the theme
}

void App::layout() {
  float s = ui.s;
  float W = float(g.W), H = float(g.H);
  topR = {0, 0, W, 52 * s};
  statusR = {0, H - 24 * s, W, 24 * s};
  // Expanded labels are useful on wide desktop windows; at tighter widths the rail returns to the compact icon strip.
  float navW = railExpanded && W >= 1400 * s ? 192 * s : 64 * s;
  railR = {0, topR.b(), navW, statusR.y - topR.b()};
  float lw = std::round(leftW * leftEase());
  leftR = {railR.r(), topR.b(), lw, railR.h};
  float rw = std::round(rightW * rightEase());
  rightR = {W - rw, topR.b(), rw, railR.h};
  canvasR = {leftR.r(), topR.b(), rightR.x - leftR.r(), railR.h};
  mainR = canvasR;
  if (splitActive()) { int mp = splitMapPane(); canvasR = mp >= 0 ? splitRect(mp) : mainR; }  // the map lives in its pane
}

bool App::startJob(const string& label, std::function<void(Job&)> fn, std::function<void()> done) {
  if (busy()) { ui.toast("Please wait", jobLabel + " is still running.", 2); return false; }
  job = std::make_unique<Job>();
  job->title = label;
  job->onDone = std::move(done);
  jobLabel = label;
  job->start(std::move(fn));
  needFrame = true;
  return true;
}

// Cheap canvas-cache guard. Map-sized inputs are protected by explicit NetView dirty bits and storage generations;
// this hash covers only scalar/style state so a cache hit never walks every label, position, attribute or flag.
namespace {
struct SigHash {
  uint64_t h = 0x243F6A8885A308D3ull;
  void word(uint64_t w) { h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
  void add(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    while (n >= 8) { uint64_t w; memcpy(&w, b, 8); word(w); b += 8; n -= 8; }
    if (n) { uint64_t w = 0; memcpy(&w, b, n); word(w | (uint64_t(n) << 56)); }
  }
  template <class T> void v(const T& x) { static_assert(std::is_trivially_copyable<T>::value, "pod"); add(&x, sizeof x); }
  void str(const string& x) { word(x.size()); add(x.data(), x.size()); }
  void col(const Color& c) { v(c.r); v(c.g); v(c.b); v(c.a); }
};
}  // namespace

void App::setCanvasCache(bool on) {
  canvasCacheOn = on;
  settings.j.set("canvasCache", on);
  settings.save();
  if (!on) g.copyDrop(g.canvasCopy);
  needFrame = true;
}

void App::setTextCache(bool on) {
  ui.textCacheOn = on;
  settings.j.set("textCache", on);
  settings.save();
  if (!on) ui.textCacheClear();
  needFrame = true;
}

namespace {
double processWorkingSetMB() {
  struct Pmc { DWORD cb, pageFaults; SIZE_T peakWs, ws, qppp, qpp, qpnpp, qnpp, pf, ppf; };  // PROCESS_MEMORY_COUNTERS
  using Fn = BOOL(WINAPI*)(HANDLE, Pmc*, DWORD);
  static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
  if (!fn) return 0;
  Pmc m{};
  m.cb = sizeof m;
  if (!fn(GetCurrentProcess(), &m, sizeof m)) return 0;
  return double(m.ws) / (1024.0 * 1024.0);
}
}  // namespace

// bookkeeping for the performance overlay: one call per presented frame
void App::perfFrameDone(double t0, bool overlayOnly, bool canvasFromCopy) {
  if (!perfHud) return;
  double now = nowSeconds();
  double ms = (now - t0) * 1000;
  perf_.frames++;
  perf_.ms += ms;
  perf_.msMax = std::max(perf_.msMax, ms);
  if (overlayOnly) perf_.overlay++;
  else if (canvasFromCopy) perf_.canvasHit++;
  else perf_.canvasMiss++;
  perf_.textHit += ui.textHits;
  perf_.textMiss += ui.textMisses;
  if (perfT0_ <= 0) perfT0_ = now;
  if (now - perfT0_ < 0.5) return;
  perfShown_ = perf_;
  perfShown_.sec = now - perfT0_;
  perfShown_.ms = perf_.frames ? perf_.ms / perf_.frames : 0;
  perf_ = PerfWindow{};
  perfT0_ = now;
  FILETIME c, e, k, u;
  if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
    uint64_t ticks = ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
    if (perfCpuT_ > 0) perfCpu_ = double(ticks - perfCpuTicks_) / 1e7 / std::max(1e-3, now - perfCpuT_) * 100.0;
    perfCpuTicks_ = ticks;
    perfCpuT_ = now;
  }
  perfMemMB_ = processWorkingSetMB();
}

void App::drawPerfHud() {
  float s = ui.s;
  const PerfWindow& w = perfShown_;
  double sec = w.sec > 0 ? w.sec : 1;
  vector<string> lines;
  lines.push_back(string("Performance  ·  VOSStudio ") + kAppVersion + "  ·  " + g.adapter);
  lines.push_back("frames/s " + fmtNum(w.frames / sec, 1) + "   frame " + fmtNum(w.ms, 2) + " ms avg, " + fmtNum(w.msMax, 1) + " ms max");
  lines.push_back("map layer: rendered " + std::to_string(w.canvasMiss) + " · from cache " + std::to_string(w.canvasHit) + " · overlay-only " + std::to_string(w.overlay) +
                  (canvasCacheOn ? "   (canvas cache on)" : "   (canvas cache OFF)"));
  int tt = w.textHit + w.textMiss;
  lines.push_back("text layouts: " + (tt ? fmtNum(100.0 * w.textHit / tt, 0) : string("-")) + " % reused · " + fmtInt(int(ui.textCacheSize())) + " cached" +
                  (ui.textCacheOn ? "   (text cache on)" : "   (text cache OFF)"));
  lines.push_back("items " + fmtInt(P->net.n()) + " · links " + fmtInt(P->net.m()) + " · labels " + fmtInt(nv.labelsShown) + " · charts " + fmtInt(static_cast<long long>(chartCache.size())));
  lines.push_back("CPU " + fmtNum(perfCpu_, 1) + " % of one core · memory " + fmtNum(perfMemMB_, 0) + " MB · " + (busy() ? "job running" : "idle"));
  float fs = 11.5f * s, lh = 16 * s, pad = 10 * s;
  float bw = 0;
  for (auto& l : lines) bw = std::max(bw, ui.textW(l, fs, 400, true));
  Rect area = canvasR.w > 100 * s ? canvasR : Rect{0, topR.b(), float(g.W), float(g.H) - topR.b()};
  Rect r{area.r() - bw - 2 * pad - 12 * s, area.b() - lines.size() * lh - 2 * pad - 12 * s, bw + 2 * pad, lines.size() * lh + 2 * pad};
  ui.shadow(r, 8 * s);
  ui.fill(r, ui.dark ? Color{0.08f, 0.09f, 0.11f, 0.92f} : Color{1, 1, 1, 0.94f}, 8 * s);
  ui.stroke(r, ui.c.border, 8 * s);
  for (size_t i = 0; i < lines.size(); i++)
    ui.text({r.x + pad, r.y + pad + i * lh, bw, lh}, lines[i], fs, i == 0 ? ui.c.text : ui.c.textDim, AL_LEFT, i == 0 ? 600 : 400, true);
}

uint64_t App::canvasSignature(const Theme& th) const {
  SigHash h;
  const Network& N = P->net;
  const ViewStyle& S = P->style;
  h.v(reinterpret_cast<uintptr_t>(P.get())); h.v(reinterpret_cast<uintptr_t>(&N)); h.v(reinterpret_cast<uintptr_t>(&S));
  h.v(P->corpusVersion);
  // view, camera, viewport, theme
  h.v(int(view)); h.v(int(nv.kind)); h.v(int(nv.geoTint)); h.v(nv.hideMarks); h.v(nv.hoverLink); h.v(showLabels); h.v(ui.dark);
  h.v(nv.cam.x); h.v(nv.cam.y); h.v(nv.cam.zoom); h.v(nv.cam.yaw); h.v(nv.cam.pitch); h.v(nv.cam.dist3); h.v(nv.fitZoom());
  h.v(nv.vp.x); h.v(nv.vp.y); h.v(nv.vp.w); h.v(nv.vp.h); h.v(nv.uiScale); h.v(g.W); h.v(g.H);
  h.col(th.bg); h.col(th.fg); h.col(th.muted); h.col(th.halo); h.col(th.grid); h.col(th.panel); h.v(th.light);
  // style (fixed-size fields plus the user override table's identity/size; actual changes set nodesDirty via setData)
  h.str(S.look); h.str(S.palette); h.v(S.clusterOverride.size());
  h.v(S.sizing); h.v(S.sizeVar); h.v(S.linkVar); h.v(S.flat); h.v(S.nodeOpacity); h.v(S.border); h.v(int(S.borderCol)); h.col(S.borderCustom);
  h.v(S.baseSize); h.v(S.maxSize); h.v(S.scale); h.v(S.labelVar); h.v(int(S.colorBy)); h.col(S.single); h.str(S.scheme); h.v(S.scoreMin); h.v(S.scoreMax);
  h.v(int(S.labelPlace)); h.v(S.labelWeight); h.v(S.labelSize); h.v(S.maxLen); h.v(S.maxLabels); h.v(S.truncate); h.v(S.labelHalo); h.v(S.labelByCluster);
  h.v(S.linksVisible); h.v(int(S.linkColor)); h.col(S.linkSingle); h.v(S.linkOpacity); h.v(S.curvature); h.v(S.linkWidth); h.v(S.linkMaxW); h.v(int(S.linkGeom));
  h.v(S.maxLines); h.v(S.minStrength); h.v(S.kernel); h.v(S.densityAlpha); h.str(S.densityScheme); h.v(S.densityFull); h.v(S.kernelAuto); h.v(S.densityByCluster);
  h.v(int(S.backdrop)); h.v(S.darkTheme); h.v(S.hulls); h.v(S.clusterNames); h.v(S.bundle); h.v(S.cvd); h.v(S.zScale);
  // Network, positions and flags are invalidated explicitly through NetView's dirty bits. Hash only their stable
  // storage identity and shape here; traversing labels/attributes/positions on every cached frame erased the gain.
  h.v(N.n()); h.v(N.links.size()); h.v(N.nClusters); h.v(N.weightIdx); h.v(N.scoreIdx); h.v(N.scoreNames.size());
  h.v(reinterpret_cast<uintptr_t>(N.nodes.data())); h.v(reinterpret_cast<uintptr_t>(N.links.data()));
  h.v(reinterpret_cast<uintptr_t>(N.clusterNames.data())); h.v(N.clusterNames.size());
  h.v(reinterpret_cast<uintptr_t>(nv.pos.data())); h.v(nv.pos.size());
  h.v(reinterpret_cast<uintptr_t>(nv.flags.data())); h.v(nv.flags.size());
  h.v(P->bundles.P); h.v(reinterpret_cast<uintptr_t>(P->bundles.pts.data())); h.v(P->bundles.pts.size());
  h.v(hover); h.v(clusterFilter); h.v(selection.size()); h.v(searchHits.size());
  h.v(liveSignature());  // the linked views of the split panes
  // the geo background layer
  if (view == ViewKind::Geo) {
    h.v(geoMode()); h.v(geoFill); h.v(geoBasemap); h.v(geoArcs); h.v(geoLayerKind); h.v(geoDenStyle); h.v(geoDenMeasure); h.v(geoHover); h.v(geoSel);
    h.v(geoModeA); h.v(geoStats.size()); h.v(geoNodeCountry.size()); h.v(geoPairs.size()); h.str(geoDenBmpSig); h.v(geoDocsTotal); h.v(geoDocsMax);
    h.v(geoYearLo); h.v(geoYearHi); h.v(geoWorld.size()); h.v(geoGeom.size()); h.v(geoDenW); h.v(geoDenH);
  }
  return h.h ? h.h : 1;
}

void App::frame() {
  double t = nowSeconds();
  lastDt = clampv(t - lastFrameT, 0.001, 0.1);
  if (lastFrameT > 0) fps = fps * 0.9 + 0.1 / lastDt;
  lastFrameT = t;
  frameNo++;
  bool overlay = liveOverlayFrame_ && liveCached_ && live.open && !showStart && !figZoomOpen && !scriptMode && !paletteOpen && !crashDialog && !ui.anyModal() && !ui.anyPopup() && !job;
  liveOverlayFrame_ = false;
  pumpWork();
  if (P && !overlay) scopeActive();  // keep the linked-selection scope current for the geo view and the caches
  // input snapshot
  Input in = input;
  in.ctrl = GetKeyState(VK_CONTROL) < 0;
  if (!in.keys.empty() || !in.chars.empty() || in.wheel != 0 || in.hwheel != 0 || in.pinch != 0 || in.touch || in.pressed[0] || in.pressed[1] || in.pressed[2] || in.down[0] || in.down[1] || in.down[2] || in.mx != inputMx || in.my != inputMy) lastInputT = t;
  inputMx = in.mx;
  inputMy = in.my;
  in.shift = GetKeyState(VK_SHIFT) < 0;
  in.alt = GetKeyState(VK_MENU) < 0;
  for (int b = 0; b < 3; b++) { input.pressed[b] = input.released[b] = false; }
  input.dbl = false;
  input.wheel = 0;
  input.hwheel = 0;
  input.pinch = 0;
  input.touch = false;
  input.touchCancel = false;
  input.chars.clear();
  input.keys.clear();

  ui.beginFrame(in, t, overlay);
  {
    bool ev = in.pressed[0] || in.pressed[1] || in.pressed[2] || in.released[0] || in.released[1] || in.released[2] || in.down[0] || in.down[1] ||
              in.wheel != 0 || in.hwheel != 0 || in.pinch != 0 || in.touch || !in.chars.empty() || !in.keys.empty() || busy() || scriptMode || animT0 >= 0;
    if (ev) chartFresh = 3;
    else if (chartFresh > 0) chartFresh--;
    if (frameNo % 600 == 0) {  // drop scenes of charts that are no longer shown
      for (auto it = chartCache.begin(); it != chartCache.end();) it = frameNo - it->second.frame > 600 ? chartCache.erase(it) : std::next(it);
    }
  }
  themeAnim.btn = {0, 0, 0, 0};  // set by drawTopBar when the top bar is shown
  if (scriptMode) stepScript();
  aiPump();
  agentPump();
  livePump();
  live.dirty = false;  // consumed by this frame
  live.animFps = 0;    // the overlay asks again while it draws
  if (overlay && !needFrame && !ui.animating && !job && !agent.active && !live.toolRunning && liveCached_ && frameOverlay(t)) return;
  if (overlay) ui.fullFrameAfterAll();
  chartHoverRegions_.clear();
  pageCharts.swap(frameCharts);  // charts drawn in the previous frame
  frameCharts.clear();
  stepPanelAnim();
  layout();
  if (live.open && !showStart && !figZoomOpen) ui.block(liveHitRect());  // the Live AI pop-up owns its area; everything under it ignores the mouse
  if (paletteOpen) ui.block({0, 0, float(g.W), float(g.H)});  // the command palette is on top of everything (drawPalette unblocks its own widgets)
  updateWindowTitle();
  handleShortcuts();

  // sync renderer
  if (styleDirty) {
    nv.setData(hasMap() ? &P->net : nullptr, &P->style, &P->bundles);
    if (view == ViewKind::Timeline) posDirty = true;  // timeline x positions can depend on the selected score data
    for (auto& L : livePane_) {
      L.dataDirty = true;
      if (L.kind == ViewKind::Timeline) L.posDirty = true;
    }
    styleDirty = false;
    figSig.clear();
  }
  if (posDirty && hasMap()) {
    if (animT0 < 0) { nv.pos = targetPositions(view); nv.positionsChanged(); }
    nv.nodesDirty = nv.linksDirty = true;
    for (auto& L : livePane_) L.posDirty = true;
    posDirty = false;
  }
  // view transition
  if (animT0 >= 0 && hasMap()) {
    float k = float(clampv((t - animT0) / animDur, 0.0, 1.0));
    float e = easeInOut(k);
    if (animFrom.size() == nv.pos.size() && animTo.size() == nv.pos.size()) {
      for (size_t i = 0; i < nv.pos.size(); i++)
        for (int d = 0; d < 3; d++) nv.pos[i][d] = animFrom[i][d] + (animTo[i][d] - animFrom[i][d]) * e;
    }
    if (k >= 1) { animT0 = -1; if (animTo.size() == nv.pos.size()) nv.pos = animTo; }
    nv.positionsChanged();
    nv.nodesDirty = nv.linksDirty = true;
  }
  // smooth camera
  if (camTargetZoom > 0) {
    double a = 1 - std::exp(-lastDt * 16);
    double lz = std::log(nv.cam.zoom) + (std::log(camTargetZoom) - std::log(nv.cam.zoom)) * a;
    nv.cam.zoom = std::exp(lz);
    nv.cam.x += (camTargetX - nv.cam.x) * a;
    nv.cam.y += (camTargetY - nv.cam.y) * a;
    if (std::fabs(nv.cam.zoom / camTargetZoom - 1) < 0.002 && std::fabs(nv.cam.x - camTargetX) * nv.cam.zoom < 0.3 && std::fabs(nv.cam.y - camTargetY) * nv.cam.zoom < 0.3) {
      nv.cam.zoom = camTargetZoom; nv.cam.x = camTargetX; nv.cam.y = camTargetY; camTargetZoom = -1;
    }
  }
  // keep the same framing when the canvas is resized (side panel opened/closed, window resized)
  if (hasMap() && nv.vp.w > 50 && nv.vp.h > 50 && canvasR.w > 50 && canvasR.h > 50 && view != ViewKind::ThreeD &&
      (std::fabs(nv.vp.w - canvasR.w) > 0.5f || std::fabs(nv.vp.h - canvasR.h) > 0.5f)) {
    Camera keep = nv.cam;
    nv.fit();
    double f0 = nv.cam.zoom;
    nv.vp = canvasR;
    nv.fit();
    double f1 = nv.cam.zoom;
    nv.cam = keep;
    if (f0 > 0) { nv.cam.zoom *= f1 / f0; if (camTargetZoom > 0) camTargetZoom *= f1 / f0; }
  }
  nv.vp = canvasR;
  nv.uiScale = ui.s;
  nv.kind = view;
  const bool mapHoverCanvas = hasMap() && workspace != WS_BIBLIOGRAPHY && view != ViewKind::Matrix && !mainChartOpen && !(papersOpen && hasCorpus()) &&
                              !writerOpen && !readerOpen && page != PG_AI && !(splitActive() && splitMapPane() < 0);
  // Resolve map hover before flags and the canvas signature. A target transition already scheduled this frame;
  // computing it here lets the node overlay paint immediately instead of forcing a redundant follow-up frame.
  if (mapHoverCanvas && drag == DR_NONE) {
    bool overChrome = false;
    for (const Rect& r : chromeRects) if (r.has(in.mx, in.my)) { overChrome = true; break; }
    bool inside = canvasR.has(in.mx, in.my) && !overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen;
    bool free = ui.active == 0 && drag != DR_MINIMAP;
    hover = inside && free ? nv.hitNode(in.mx, in.my) : -1;
  }
  if (hasMap()) updateFlags();
  liveSync();  // the linked views of the split panes follow the data, the positions and the flags

  Theme th = canvasTheme(P->style);
  bool netCanvas = hasMap() && workspace != WS_BIBLIOGRAPHY && view != ViewKind::Matrix && !mainChartOpen && !(papersOpen && hasCorpus()) && !writerOpen && !readerOpen && page != PG_AI && !(splitActive() && splitMapPane() < 0);
  if (netCanvas && view == ViewKind::Geo) {
    const int mode = geoMode();
    if (mode <= 0) geoHover = -1;
    else {
      geoEnsureWorld();
      bool overChrome = false;
      for (const Rect& r : chromeRects) if (r.has(in.mx, in.my)) { overChrome = true; break; }
      bool inside = canvasR.has(in.mx, in.my) && !overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen;
      bool free = ui.active == 0 && drag != DR_MINIMAP;
      bool mouseFree = inside && free && hover < 0 && (drag == DR_NONE || drag == DR_PAN);
      geoHover = mouseFree ? geoHit(in.mx, in.my) : -1;
    }
  }
  const bool liveOn = liveAny();
  {  // Geo layers on a country network: overlay colours, or density underneath with the marks hidden
    bool g1 = hasMap() && view == ViewKind::Geo && geoMode() == 1;
    ViewKind tint = g1 && geoLayerKind == 1 ? ViewKind::Overlay : ViewKind::Geo;
    bool hide = g1 && geoLayerKind == 2;
    if (tint != nv.geoTint || hide != nv.hideMarks) { nv.geoTint = tint; nv.hideMarks = hide; nv.nodesDirty = nv.linksDirty = true; }
  }
  // The canvas layer: rendered again only when one of its inputs changed (hover, selection, camera, positions, style
  // ... all in the signature; a press, wheel or drag on the canvas renders too, because it moves things during this
  // very frame); otherwise the GPU copy of the last render comes back and only the chrome is drawn.
  bool cacheable = (netCanvas || liveOn) && canvasCacheOn && !scriptMode && pendingViewFmt.empty() && animT0 < 0 && camTargetZoom <= 0;
  uint64_t csig = cacheable ? canvasSignature(th) : 0;
  bool fromCopy = cacheable && !nv.nodesDirty && !nv.linksDirty && liveClean() && csig == canvasSig_ && g.canvasCopy.valid && g.canvasCopy.w == g.W && g.canvasCopy.h == g.H &&
                  !(canvasR.has(in.mx, in.my) && (in.down[0] || in.down[1] || in.down[2] || in.wheel != 0 || drag != DR_NONE));
  bool restored = false;
  if (fromCopy) {
    g.beginD2D();
    restored = g.copyRestore(g.canvasCopy);
    if (!restored) { g.endD2D(); g.canvasCopy.valid = false; }
  }
  if (restored) canvasHits++;
  else {
    canvasMisses++;
    if (netCanvas && view == ViewKind::Geo && geoMode() > 0) {
      if (!nv.backgroundPainter) nv.backgroundPainter = [this](ID2D1DeviceContext* dc) { geoPaint(dc, nv); };
      nv.drawBackgroundLayer();
    } else nv.backgroundPainter = nullptr;
    if (liveOn) liveBackground();
    g.beginGpu(netCanvas || liveOn ? th.bg : ui.c.bg);
    if (netCanvas) nv.renderGpu(th.bg);
    if (liveOn) liveRender(th);
    g.endGpu();
    if (!pendingViewFmt.empty()) doViewExport();
    g.beginD2D();
    if (netCanvas) nv.drawOverlay(g.dc.get(), th, showLabels, P->style.maxLabels);
    if (liveOn) liveOverlay(th);
    if (cacheable && g.copyCapture(g.canvasCopy)) canvasSig_ = csig;
    else g.canvasCopy.valid = false;
  }
  bool figOn = figZoomOpen && hasMap();
  if (!figOn) figZoomOpen = false;
  bool figOpening = figOn && figZoomT0 >= 0 && !scriptMode && ui.time - figZoomT0 < 0.22;  // app stays visible under the fade-in
  if (showStart && (hasCorpus() || hasMap())) showStart = false;
  if (showStart && !figOn) {
    drawStart();
    drawPalette();
    drawSettings();
  } else if (!figOn || figOpening) {
    drawCanvas();
    drawTopBar();
    drawRail();
    drawLeft();
    drawInspector();
    drawStatus();
    drawFetchPopups();  // Get PDF: the confirm card, the run's summary, a record's card (when open)
    if (perfHud) drawPerfHud();
    // keep a picture of the window without the overlay: while only the overlay moves, the next frames draw just it
    liveCached_ = live.open && !scriptMode && !paletteOpen && !crashDialog && !ui.anyModal() && !ui.anyPopup() && ui.toasts.empty() && !ui.tipShowing() &&
                  animT0 < 0 && camTargetZoom <= 0 && !job && uiQueue.empty() && !themeAnim.active && !themeAnim.pending && g.copyCapture(g.overlayCopy);
    drawLive();
    drawPalette();
    drawSettings();
  }
  if (figOn) drawFigureZoom();
  drawCrashDialog();
  ui.beginModalLayer();  // window buttons stay usable while a dialog is open
  drawCaption();
  ui.endModalLayer();
  ui.endFrame();
  pointerHoverTarget_ = pointerHoverIdAt(ui.in.mx, ui.in.my);
  drawThemeReveal(t);  // over everything, tooltips included: the old picture through the circle
  g.endD2D();
  if (pendingShot) {
    pendingShot = false;
    vector<uint8_t> px;
    int w = 0, h = 0;
    if (g.readback(px, w, h)) {
      int x0 = int(canvasR.x), y0 = int(canvasR.y), cw = int(canvasR.w), ch = int(canvasR.h);
      bool cropOk = pendingViewCrop && cw > 0 && ch > 0 && x0 >= 0 && y0 >= 0 && x0 + cw <= w && y0 + ch <= h;
      if (cropOk) {
        vector<uint8_t> crop(static_cast<size_t>(cw) * size_t(ch) * 4);
        for (int y = 0; y < ch; y++) memcpy(&crop[size_t(y) * size_t(cw) * 4], &px[(size_t(y0 + y) * size_t(w) + size_t(x0)) * 4], size_t(cw) * 4);
        px.swap(crop); w = cw; h = ch;
      }
      if (pendingShotPath == "live-vision") liveSendPicture(px, w, h, cropOk);  // a still picture for the Live assistant (never a video stream)
      else if (pendingShotPath == "clipboard") setClipboardImage(hwnd, w, h, px.data());
      else if (g.savePngWic(pendingShotPath, w, h, px.data(), 96 * ui.s)) { if (!scriptMode && pendingViewCrop) ui.toast("View exported", fileName(pendingShotPath), 1); }
    } else if (pendingShotPath == "live-vision") {
      liveSendPicture({}, 0, 0, false);
    }
    pendingViewCrop = false;
  }
  if (!g.present(!scriptMode)) occluded = true;
  perfFrameDone(t, false, restored);
  if (ui.animating) needFrame = true;
  // an event handled this frame may have changed state after the layers were drawn (a click selects a node after the
  // map was rendered): one more frame settles it
  if (in.pressed[0] || in.pressed[1] || in.pressed[2] || in.released[0] || in.released[1] || in.released[2] || in.dbl || in.wheel != 0 || in.hwheel != 0 || in.pinch != 0 || in.touch || !in.chars.empty() || !in.keys.empty()) needFrame = true;
  if (showStart || figZoomOpen) liveCached_ = false;
  liveFrameT_ = t;
}

// The overlay-only frame: the last full frame comes back from the GPU copy, then only the Live assistant's panel or
// orb (and the caption buttons, which lie outside the copy's coverage of the overlay) are drawn on top. Everything
// that could have changed underneath forces a full frame instead (input events, jobs, tools, other animations).
bool App::frameOverlay(double t) {
  if (!g.beginD2D()) return false;
  if (!g.copyRestore(g.overlayCopy)) {
    liveCached_ = false;
    g.endD2D();
    needFrame = true;
    return true;  // nothing was presented; the full frame follows at once
  }
  drawLive();
  ui.beginModalLayer();
  drawCaption();
  ui.endModalLayer();
  ui.endFrame();
  pointerHoverTarget_ = pointerHoverIdAt(ui.in.mx, ui.in.my);
  g.endD2D();
  if (!g.present(!scriptMode)) occluded = true;
  perfFrameDone(t, true, true);
  if (ui.animating || shotPending()) needFrame = true;  // a picture (camera button) is taken by a full frame
  liveFrameT_ = t;
  return true;
}

// =====================================================================================
// shortcuts & commands
// =====================================================================================
void App::handleShortcuts() {
  const Input& in = ui.in;
  bool edit = ui.editing();
  if (ui.anyModal()) {  // a dialog is open: only Esc (closes an open list first, then the dialog)
    for (int k : in.keys)
      if (k == VK_ESCAPE && !ui.anyPopup()) ui.closeModal();
    return;
  }
  if (readerOpen && !paletteOpen && !edit) {  // the reader owns the keyboard (reader.cpp); the application keys stay global
    for (int k : in.keys) {
      if (k == VK_F1) { paletteOpen = true; paletteQuery.clear(); continue; }
      if (in.ctrl && !in.alt) {
        if (k == 'O') { if (in.shift) cmdOpenProject(); else cmdOpenFiles(); continue; }
        if (k == 'S') { cmdSaveProject(in.shift); continue; }
        if (k == 'K' && !in.shift) { paletteOpen = !paletteOpen; paletteQuery.clear(); paletteSel = 0; ui.focus = 0; continue; }  // Ctrl+P prints here (reader.cpp)
        if (k == 'J' && !in.shift) { setPage(page == PG_AI ? PG_NONE : PG_AI); continue; }
        if (k == 'L' && in.shift) { if (live.open && live.mini) liveSetMini(false); else if (live.open) liveClose(); else liveOpen(false); continue; }
        if (k == VK_OEM_COMMA) { openSettings(); continue; }
      }
      readerKey(k);
    }
    return;
  }
  if (writerOpen && !paletteOpen && !edit) {  // the writer owns the keyboard (its own map in writer.cpp); a few application keys stay global
    for (int k : in.keys) {
      if (k == VK_F1) { paletteOpen = true; paletteQuery.clear(); continue; }
      if (k == VK_ESCAPE) {
        if (figZoomOpen) figZoomOpen = false;
        else if (writerPopupPrev_) writerPopupPrev_ = false;  // Esc closed a menu or dialog of the writer
        else if (wed.hasSelection()) wed.setCaret(wed.caret);
        else closeWriter();
        continue;
      }
      if (in.ctrl && !in.alt) {
        if (k == 'O') { if (in.shift) cmdOpenProject(); else cmdOpenFiles(); continue; }
        if (k == 'S') { cmdSaveProject(in.shift); continue; }
        if (k == 'P' && !in.shift) { paletteOpen = !paletteOpen; paletteQuery.clear(); paletteSel = 0; ui.focus = 0; continue; }
        if (k == 'J' && !in.shift) { setPage(page == PG_AI ? PG_NONE : PG_AI); continue; }
        if (k == 'L' && in.shift) { if (live.open && live.mini) liveSetMini(false); else if (live.open) liveClose(); else liveOpen(false); continue; }
        if (k == VK_OEM_COMMA) { openSettings(); continue; }
      }
      writerKey(k);
    }
    writerTyped();
    return;
  }
  for (int k : in.keys) {
    if (in.ctrl) {
      switch (k) {
        case 'O': if (in.shift) cmdOpenProject(); else cmdOpenFiles(); break;
        case 'S': cmdSaveProject(in.shift); break;
        case 'Z': if (!edit) { if (in.shift) redo(); else undo(); } break;
        case 'Y': if (!edit) redo(); break;
        case 'B': if (workspace == WS_VISUALIZATION) cmdBuild(); break;
        case 'J': setPage(page == PG_AI ? PG_NONE : PG_AI); break;
        case 'E': if (workspace == WS_VISUALIZATION && hasMap()) { if (in.shift) cmdExportCurrentView(""); else setPage(PG_PUBLISH); } break;
        case 'K': case 'P': paletteOpen = !paletteOpen; paletteQuery.clear(); paletteSel = 0; ui.focus = 0; break;
        case 'F': if (workspace == WS_BIBLIOGRAPHY) { openPapers(); ui.focus = ui.id("ti:papers:filter"); } else ui.focus = ui.id("ti:search"); break;
        case 'N': cmdNewProject(); break;
        case 'T': if (hasCorpus() || workspace == WS_BIBLIOGRAPHY) {
if (papersOpen) { papersOpen = false; if (workspace == WS_BIBLIOGRAPHY) { lastBibliographyRoute = BR_REVIEW; papersStatusFilter = -1; papersPdfFilter = 1; papersBuiltKey.clear(); page = PG_READ; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); } }
          else openPapers();
        } break;
        case 'C': if (!edit && in.shift) cmdCopyFigure(); break;
        case 'L': if (in.shift) { if (live.open && live.mini) liveSetMini(false); else if (live.open) liveClose(); else liveOpen(false); } else if (workspace == WS_VISUALIZATION && hasMap()) cmdRelayout(); break;
        case 'W': if (in.shift) openWriter(); break;
        case '2': if (in.shift && workspace == WS_VISUALIZATION && hasMap()) setSplit(splitMode == 1 ? 0 : 1); break;  // Ctrl+Shift+2 / 4: split view
        case '4': if (in.shift && workspace == WS_VISUALIZATION && hasMap()) setSplit(splitMode == 3 ? 0 : 3); break;
        case VK_OEM_COMMA: openSettings(); break;
      }
      continue;
    }
    if (k == VK_ESCAPE) {
      if (figZoomOpen) figZoomOpen = false;
      else if (paletteOpen) paletteOpen = false;
      else if (docPreview >= 0) {
        if (!docBackStack.empty()) { docPreview = docBackStack.back(); docBackStack.pop_back(); }
        else docPreview = -1;
      }
      else if (papersOpen) { papersOpen = false; if (workspace == WS_BIBLIOGRAPHY) { lastBibliographyRoute = BR_REVIEW; papersStatusFilter = -1; papersPdfFilter = 1; papersBuiltKey.clear(); page = PG_READ; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); } }
      else if (workspace == WS_VISUALIZATION && geoSel >= 0 && view == ViewKind::Geo) geoSel = -1;
      else if (workspace == WS_VISUALIZATION && mainChartOpen) mainChartOpen = false;
      else if (workspace == WS_VISUALIZATION && splitMax >= 0) splitMax = -1;
      else if (workspace == WS_VISUALIZATION && (!selection.empty() || clusterFilter >= 0)) { clearSelection(); clusterFilter = -1; }
      else if (workspace == WS_VISUALIZATION && !search.empty()) { search.clear(); searchHits.clear(); invalidateFlags(); }
      continue;
    }
    if (k == VK_F1) { paletteOpen = true; paletteQuery.clear(); continue; }
    if (edit || paletteOpen) continue;
    if (papersOpen && hasCorpus() && papersKey(k)) continue;
    if (figZoomOpen) {
      if (k == '0' || k == VK_NUMPAD0) { figZoomK = 1; figZoomX = figZoomY = 0; }
      else if (k == VK_OEM_PLUS || k == VK_ADD) figZoomK = clampv(figZoomK * 1.25, 0.25, 40.0);
      else if (k == VK_OEM_MINUS || k == VK_SUBTRACT) figZoomK = clampv(figZoomK / 1.25, 0.25, 40.0);
      continue;
    }
    if (workspace == WS_VISUALIZATION && k >= '1' && k <= '7' && hasMap()) setView(kViews[k - '1'].v);
    else if (workspace == WS_VISUALIZATION && k == 'F' && hasMap()) { mainChartOpen = false; nv.fit(); camTargetZoom = -1; }
    else if (workspace == WS_VISUALIZATION && k == 'L') showLabels = !showLabels;
    else if (workspace == WS_VISUALIZATION && k == 'H' && hasMap()) { P->style.hulls = !P->style.hulls; styleDirty = true; }
    else if (workspace == WS_VISUALIZATION && k == 'N' && hasMap()) { P->style.clusterNames = !P->style.clusterNames; styleDirty = true; }
    else if (workspace == WS_VISUALIZATION && k == 'M') showMinimap = !showMinimap;
    else if (workspace == WS_VISUALIZATION && k == 'G') showLegend = !showLegend;
    else if (workspace == WS_VISUALIZATION && k == 'I') inspectorOpen = !inspectorOpen;
    else if (workspace == WS_VISUALIZATION && k == VK_OEM_2 /* / */) ui.focus = ui.id("ti:search");
    else if (workspace == WS_VISUALIZATION && (k == VK_OEM_PLUS || k == VK_ADD)) { if (hasMap()) { camTargetZoom = (camTargetZoom > 0 ? camTargetZoom : nv.cam.zoom) * 1.3; camTargetX = nv.cam.x; camTargetY = nv.cam.y; } }
    else if (workspace == WS_VISUALIZATION && (k == VK_OEM_MINUS || k == VK_SUBTRACT)) { if (hasMap()) { camTargetZoom = (camTargetZoom > 0 ? camTargetZoom : nv.cam.zoom) / 1.3; camTargetX = nv.cam.x; camTargetY = nv.cam.y; } }
    else if (workspace == WS_VISUALIZATION && k == VK_TAB && !searchHits.empty()) { searchIdx = (searchIdx + (in.shift ? -1 : 1) + int(searchHits.size())) % int(searchHits.size()); focusOn(searchHits[size_t(searchIdx)]); }
  }
}

namespace {
// Sections of the command palette when it is browsed without a query (in this order).
const char* const kPaletteGroups[] = {"File", "Map", "Views", "Pages", "Display", "Export", "Analyse", "AI", "Live AI", "Looks", "Tools", "Help", "Items", "Other"};
const char* paletteGroupFor(const string& id) {
  static const std::map<string, const char*> byId = {
      {"open", "File"}, {"openproj", "File"}, {"save", "File"}, {"saveas", "File"}, {"new", "File"}, {"sample", "File"}, {"sample2", "File"}, {"papers", "Views"}, {"writer", "Views"}, {"writerpreview", "Export"}, {"writercompile", "Export"}, {"writeropenpdf", "Export"}, {"writerpdf", "Export"}, {"writerdocx", "Export"}, {"writerhtml", "Export"}, {"writerlatex", "Export"},
      {"build", "Map"}, {"relayout", "Map"}, {"recluster", "Map"}, {"bundle", "Map"}, {"fit", "Map"}, {"maphist", "Map"}, {"undo", "Map"}, {"redo", "Map"}, {"vosload", "Map"}, {"vosexport", "Map"},
      {"labels", "Display"}, {"legend", "Display"}, {"hulls", "Display"}, {"names", "Display"}, {"theme", "Display"}, {"settings", "Display"},
      {"svg", "Export"}, {"pdf", "Export"}, {"png", "Export"}, {"copyfig", "Export"}, {"viewany", "Export"}, {"figall", "Export"}, {"viewsvg", "Export"}, {"viewpdf", "Export"}, {"viewpng", "Export"},
      {"recwos", "Export"}, {"recris", "Export"}, {"reccsv", "Export"}, {"items", "Export"}, {"methods", "Export"},
      {"trtopics", "Analyse"}, {"rpys", "Analyse"}, {"topdocs", "Analyse"}, {"clyears", "Analyse"}, {"topdoc", "Analyse"},
      {"live", "Live AI"}, {"live:talk", "Live AI"}, {"live:orb", "Live AI"}, {"livevad", "Live AI"},
      {"perf", "Tools"}, {"textcache", "Tools"}, {"canvascache", "Tools"}, {"cite", "Help"}};
  auto it = byId.find(id);
  if (it != byId.end()) return it->second;
  if (id.rfind("view", 0) == 0) return "Views";
  if (id.rfind("ai:", 0) == 0) return "AI";
  if (id.rfind("page", 0) == 0) return "Pages";
  if (id.rfind("rd", 0) == 0) return "Read";
  if (id.rfind("look:", 0) == 0) return "Looks";
  return "Other";
}
}  // namespace

void App::cmdCite() {
  string apa = softwareReference(false), bib = softwareReference(true);
  setClipboardText(hwnd, apa + "\n\n" + bib + "\n");
  ui.toast("Copied", "APA and BibTeX references to VOSStudio Native " + string(kAppVersion) + " are on the clipboard. The methods paragraph carries the same reference.", 1, 5);
}

void App::buildCommands() {
  commands.clear();
  auto add = [&](const string& id, const string& label, const string& hint, const string& sc, const string& ic, std::function<void()> fn) { commands.push_back({id, label, hint, sc, ic, fn, ""}); };
  add("open", "Open bibliographic files…", "WoS, Scopus, RIS, BibTeX, OpenAlex JSON", "Ctrl+O", "folder", [this] { cmdOpenFiles(); });
  add("openproj", "Open project…", ".vosproj bundle", "Ctrl+Shift+O", "file", [this] { cmdOpenProject(); });
  add("save", "Save project", "Self-contained bundle with records and map", "Ctrl+S", "save", [this] { cmdSaveProject(false); });
  add("saveas", "Save project as…", "", "Ctrl+Shift+S", "save", [this] { cmdSaveProject(true); });
  add("new", "New project", "Start over", "Ctrl+N", "file", [this] { cmdNewProject(); });
  add("papers", "Papers table", "Every record as a sortable, filterable list", "Ctrl+T", "table", [this] { if (hasCorpus() || workspace == WS_BIBLIOGRAPHY) openPapers(); else ui.toast("No records", "Load records first.", 2); });
  add("writer", "Writer (document editor)", "Write and edit visually; export PDF, Word, HTML or an advanced LaTeX source bundle", "Ctrl+Shift+W", "writer", [this] { if (writerOpen) closeWriter(); else openWriter(); });
  add("writerpreview", "Writer: side-by-side PDF preview", "Show or hide the latest compiled PDF beside the document", "", "eye", [this] { writerSetPdfPreview(!writerPdfPreviewOpen_); });
  add("writercompile", "Writer: compile LaTeX preview", "Compile or recompile a PDF from the visual document", "", "play", [this] { writerCompileLatex(); });
  add("writeropenpdf", "Writer: open compiled PDF separately", "Open the latest compiled PDF in the system viewer", "", "external", [this] { writerOpenPdfSeparately(); });
  add("split1", "Split view: two panes side by side", "The map and a chart, or two charts, next to each other", "Ctrl+Shift+2", "table", [this] { if (!hasMap()) ui.toast("Split view", "Build a map first.", 2); else setSplit(splitMode == 1 ? 0 : 1); });
  add("split2", "Split view: two panes, one above the other", "", "", "table", [this] { if (!hasMap()) ui.toast("Split view", "Build a map first.", 2); else setSplit(splitMode == 2 ? 0 : 2); });
  add("split3", "Split view: four panes", "Four charts of the loaded data at once (one may be the live map)", "Ctrl+Shift+4", "grid", [this] { if (!hasMap()) ui.toast("Split view", "Build a map first.", 2); else setSplit(splitMode == 3 ? 0 : 3); });
  add("split0", "Split view: single pane", "Back to the map alone", "", "fit", [this] { setSplit(0); });
  add("writerpdf", "Writer: export as PDF\xE2\x80\xA6", "The document as a print-ready PDF", "", "download", [this] { writerExport("pdf"); });
  add("writerdocx", "Writer: export as Word\xE2\x80\xA6", "The document as an editable .docx", "", "download", [this] { writerExport("docx"); });
  add("writerhtml", "Writer: export as web page\xE2\x80\xA6", "The document as one self-contained .html file", "", "download", [this] { writerExport("html"); });
  add("writerlatex", "Writer: export LaTeX source bundle\xE2\x80\xA6", "Advanced source export with companion BibTeX and vector figure PDFs", "", "download", [this] { writerExport("latex"); });
  add("sample", "Load sample data (Web of Science)", "140 records, 2016–2024", "", "sparkle", [this] { cmdSample(false); });
  add("sample2", "Load sample data (Scopus)", "90 records", "", "sparkle", [this] { cmdSample(true); });
  add("build", "Build map", "Run the analysis with the current builder settings", "Ctrl+B", "play", [this] { cmdBuild(); });
  add("relayout", "Re-run layout", "VOS mapping technique", "Ctrl+L", "refresh", [this] { cmdRelayout(); });
  add("recluster", "Re-run clustering", "Leiden algorithm", "", "shuffle", [this] { cmdRecluster(); });
  add("bundle", "Compute edge bundling", "Force-directed edge bundling (FDEB)", "", "bundle", [this] { cmdBundles(); });
  add("vosload", "Import VOSviewer map/network…", "", "", "network", [this] { cmdLoadVosviewer(); });
  add("vosexport", "Export VOSviewer map + network files…", "", "", "download", [this] { cmdExportVosviewer(); });
  add("svg", "Export figure as SVG…", "Vector, editable in Illustrator/Inkscape", "", "download", [this] { cmdExportFigure("svg"); });
  add("pdf", "Export figure as PDF…", "Vector, print-ready", "", "download", [this] { cmdExportFigure("pdf"); });
  add("png", "Export figure as PNG…", "Raster at the figure DPI", "", "download", [this] { cmdExportFigure("png"); });
  add("copyfig", "Copy figure to clipboard", "PNG at 200 dpi", "Ctrl+Shift+C", "copy", [this] { cmdCopyFigure(); });
  add("viewany", "Export current view…", "Any view (Network, Overlay, Density, Timeline, Matrix, Geo, 3D) as SVG, PDF or PNG", "Ctrl+Shift+E", "download", [this] { cmdExportCurrentView(""); });
  add("figall", "Publication figure with all views", "Network, overlay, density, timeline, geo, 3D and matrix panels", "", "publish", [this] {
    auto& F = P->fig; F.panelNetwork = F.panelOverlay = F.panelDensity = F.panelTimeline = F.panel3D = F.panelMatrix = true; F.panelGeo = geoAvailable(); page = PG_PUBLISH; });
  add("viewsvg", "Export current view as SVG…", "Exactly what you see: vector, sphere-shaded nodes, basemap", "", "download", [this] { cmdExportCurrentView("svg"); });
  add("viewpdf", "Export current view as PDF…", "Exactly what you see, as vector PDF", "", "download", [this] { cmdExportCurrentView("pdf"); });
  add("viewpng", "Export current view as PNG…", "Exactly what you see, at twice the screen resolution", "", "download", [this] { cmdExportCurrentView("png"); });
  add("legend", "Toggle cluster legend", "Show or hide the legend card on the canvas", "G", "legend", [this] { showLegend = !showLegend; });
  add("recwos", "Export records (Web of Science format)…", "Readable by VOSviewer, bibliometrix, CiteSpace", "", "download", [this] { cmdExportRecords(false, "wos"); });
  add("recris", "Export records (RIS)…", "", "", "download", [this] { cmdExportRecords(false, "ris"); });
  add("recbib", "Export records (BibTeX)…", "", "", "download", [this] { cmdExportRecords(false, "bib"); });
  add("reccsv", "Export records (CSV)…", "Scopus column names", "", "download", [this] { cmdExportRecords(false, "csv"); });
  add("items", "Export items table (CSV)…", "", "", "table", [this] { cmdExportItemsCsv(); });
  add("methods", "Copy methods paragraph", "Reproducible description of the map", "", "copy", [this] { if (hasMap()) { setClipboardText(hwnd, P->methods()); ui.toast("Copied", "Methods paragraph copied to the clipboard.", 1); } });
  add("theme", "Toggle light / dark theme", "", "", "sun", [this] { setTheme(!themeTarget()); });
  add("settings", "Settings", "AI provider, OpenAlex API key, appearance", "Ctrl+,", "settings", [this] { settingsWanted = true; });
  add("live", live.open && live.mini ? "Live AI: open the panel" : live.open ? "Close Live AI" : "Live AI", "Talk or type with the assistant while it works the app for you", "Ctrl+Shift+L", "live", [this] { if (live.open && live.mini) liveSetMini(false); else if (live.open) liveClose(); else liveOpen(false); });
  add("live:talk", "Live AI: start talking", "Voice session with spoken replies (Gemini Live)", "", "mic", [this] { liveOpen(true); liveSetMini(false); });
  add("live:orb", "Live AI: voice orb", "A small circle that listens and answers by voice; right-click it for the panel", "", "orb", [this] { liveOpen(true); liveSetMini(true); });
  add("labels", "Toggle labels", "", "L", "tag", [this] { showLabels = !showLabels; });
  add("livevad", "Live AI: toggle speech detection in the app", "On (recommended): the app tells the model when you start and stop talking. Off: the server's own voice detection, which can leave questions unanswered", "", "mic", [this] { runCommand("livevad"); });
  add("perf", "Toggle performance overlay", "Frames per second, frame time, cache hit rates, CPU and memory (for comparing versions and settings)", "", "activity", [this] { perfHud = !perfHud; perf_ = PerfWindow{}; perfShown_ = PerfWindow{}; perfT0_ = 0; perfCpuT_ = 0; needFrame = true; });
  add("textcache", "Toggle text cache", "Reuse DirectWrite text layouts between frames (off: lay every text out again on every frame)", "", "type", [this] { setTextCache(!ui.textCacheOn); ui.toast(ui.textCacheOn ? "Text cache on" : "Text cache off", ui.textCacheOn ? "Text layouts are kept between frames." : "Every text is laid out again on every frame.", 1); });
  add("canvascache", "Toggle canvas cache", "Reuse the last render of the map while nothing on it changed (off: render every frame)", "", "layers", [this] { setCanvasCache(!canvasCacheOn); ui.toast(canvasCacheOn ? "Canvas cache on" : "Canvas cache off", canvasCacheOn ? "The map is rendered again only when something on it changes." : "The map is rendered on every frame.", 1); });
  add("hulls", "Toggle cluster hulls", "", "H", "hull", [this] { P->style.hulls = !P->style.hulls; styleDirty = true; });
  add("names", "Toggle cluster names", "", "N", "type", [this] { P->style.clusterNames = !P->style.clusterNames; styleDirty = true; });
  add("fit", "Fit map to window", "", "F", "fit", [this] { if (hasMap()) nv.fit(); });
  add("undo", "Undo", "", "Ctrl+Z", "undo", [this] { undo(); });
  add("redo", "Redo", "", "Ctrl+Y", "redo", [this] { redo(); });
  add("trtopics", "Trend topics", "Median year and interquartile range of frequent terms", "", "trends", [this] { page = PG_TRENDS; trTab = 5; });
  add("rpys", "Reference publication year spectroscopy (RPYS)", "Historical roots: peaks in the years of cited references", "", "trends", [this] { page = PG_TRENDS; trTab = 6; });
  add("topdocs", "Most cited documents & historiograph", "Global and local citations, direct-citation history", "", "file", [this] { page = PG_ACTORS; acTab = 5; });
  add("clyears", "Clusters over time", "Documents per year for each cluster of the map", "", "analyse", [this] { page = PG_ANALYSE; anaTab = 0; });
  add("maphist", "Switch map\xE2\x80\xA6", "Maps built in this session", "", "history", [this] { if (hasMap()) ui.openPopup("maphist"); });
  add("topdoc", "Preview the most cited document", "", "", "file", [this] {
    int r = -1; long best = -1;
    for (size_t i = 0; i < P->corpus.recs.size(); i++) if (P->corpus.recs[i].cites > best) { best = P->corpus.recs[i].cites; r = int(i); }
    openDoc(r);
  });
  for (int v = 0; v < 7; v++) add("view" + std::to_string(v), string("View: ") + kViews[v].label, "", std::to_string(v + 1), kViews[v].icon, [this, v] { setView(kViews[v].v); });
  for (auto& ti : ai::tasks()) {
    if (ti.task == ai::Task::Chat) { add("ai:chat", "Ask the AI assistant", "Open the assistant", "Ctrl+J", "sparkle", [this] { setPage(PG_AI); }); continue; }
    ai::Task t = ti.task;
    add(string("ai:") + ti.id, string("AI: ") + ti.title, ti.sub, "", ti.icon, [this, t] { if (t == ai::Task::Document && aiDoc < 0) aiDoc = docPreview; aiRun(t); });
  }
  for (int p = 0; p < PG_COUNT; p++) add("page" + std::to_string(p), string("Go to ") + kPageTitles[p], kPageSubs[p], "", kPageIcons[p], [this, p] { setPage(p); });
  for (auto& lk : lookPresets()) { string id = lk.id; add("look:" + id, string("Look: ") + lk.label, lk.sub, "", "look", [this, id] { applyLook(P->style, id); P->style.darkTheme = ui.dark; styleDirty = true; if (hasMap()) { nv.fit(); camTargetZoom = -1; } }); }
  add("cite", "Cite VOSStudio\xE2\x80\xA6", "Copy a reference for your paper (APA and BibTeX)", "", "copy", [this] { cmdCite(); });
  add("rdattach", "Attach PDFs\xE2\x80\xA6", "Add the papers' PDF files to the reading library (linked to the records by DOI or title)", "", "book", [this] { readerAttachDialog(-1); });
  add("rdnotes", "Export reading notes (Markdown)", "Every paper's status, notes, highlights and codes as one Markdown file", "", "download", [this] { readerExportNotes(""); });
  add("rdcoding", "Export the coding matrix (CSV)", "One row per paper: status, rating, tags and the passages coded Aim, Method, Finding, Theory, Gap, Quote, Question", "", "table", [this] { readerExportCoding(); });
  for (auto& c : commands) c.group = paletteGroupFor(c.id);
}

void App::setWorkspace(Workspace target) {
  if (target == workspace) return;

  // Remember each workspace's route before changing the shared shell. The map, paper selection, writer document,
  // and reader document remain in memory; only the active surface is switched.
  if (workspace == WS_VISUALIZATION) {
    if (page == PG_NONE) lastVisualizationPage = PG_NONE;
    else if (page != PG_AI && page != PG_READ && page != PG_WRITER && page >= PG_DATA && page <= PG_PUBLISH) lastVisualizationPage = page;
    lastVisualizationChartOpen = mainChartOpen;
  } else if (page != PG_AI) {
    if (writerOpen || page == PG_WRITER) lastBibliographyRoute = BR_WRITE;
    else if (readerOpen) lastBibliographyRoute = BR_REVIEW;
    else if (!papersOpen && page == PG_READ) lastBibliographyRoute = BR_REVIEW;
    // Papers and Review share the persistent PG_READ library sidebar and papers canvas; keep the explicit route.
  }

  if (workspace == WS_BIBLIOGRAPHY && readerOpen) {
    readerStorePosition();
    if (PdfItem* item = readerItem()) bibliographyReaderItem = item->id;
    bibliographyReaderWasOpen = !bibliographyReaderItem.empty();
  }
  if (readerOpen) closeReader();
  writerOpen = false;  // wdoc, caret, zoom and scroll are retained for the next visit
  ui.focus = 0;
  ui.closePopup();
  inspectorOpen = false;  // the inspector is a Visualization tool, not a Bibliography destination
  papersOpen = false;
  figZoomOpen = false;
  workspace = target;
  settings.j.set("workspace", int(workspace));

  if (workspace == WS_VISUALIZATION) {
    page = lastVisualizationPage;
    mainChartOpen = lastVisualizationChartOpen;
  } else {
    mainChartOpen = false;  // Bibliography has no network/map destination of its own
    switch (lastBibliographyRoute) {
      case BR_PAPERS:
        page = PG_READ;
        papersOpen = true;
        break;
      case BR_REVIEW:
        page = PG_READ;
        papersOpen = !bibliographyReaderWasOpen;
        if (bibliographyReaderWasOpen && !bibliographyReaderItem.empty()) {
          openReader(bibliographyReaderItem);
          bibliographyReaderWasOpen = false;
        }
        break;
      case BR_WRITE:
        page = PG_WRITER;
        openWriter();
        break;
    }
  }
  settings.j.set("workspaceVisualPage", lastVisualizationPage);
  settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  settings.j.set("railExpanded", railExpanded);
  needFrame = true;
}

void App::activateWorkspaceNav(const string& id) {
  if (workspace == WS_VISUALIZATION) {
    if (id == "map") { showVisualizationMap(); return; }
    for (const RailItem& item : kVisualizationNav) {
      if (id != item.id) continue;
      setPage(item.action == RailAction::Page ? item.page : PG_NONE);
      return;
    }
    return;
  }

  if (id == "papers") { openPapers("", -1, true, false); return; }
  if (id == "review") {
    applyPaperView(-1, 1, papersCollectionFilter, papersTagFilter, papersFilter, BR_REVIEW);
    ui.focus = 0;
    return;
  }
  if (id == "write") { openWriter(); return; }
}

void App::showVisualizationMap() {
  if (workspace != WS_VISUALIZATION) setWorkspace(WS_VISUALIZATION);
  if (readerOpen) closeReader();
  writerOpen = false;
  papersOpen = false;
  mainChartOpen = false;
  figZoomOpen = false;
  page = hasMap() ? PG_NONE : PG_BUILD;
  lastVisualizationPage = page;
  lastVisualizationChartOpen = false;
  ui.focus = 0;
  needFrame = true;
}

void App::showPapersOnMap() {
  if (!hasMap()) { ui.toast("No map yet", "Build a Visualization map before locating these papers.", 2, 4); return; }

  vector<int> targetRecords;
  if (papersSelectedCount() > 0) {
    for (size_t i = 0; i < papersSel.size(); i++) if (papersSel[i]) targetRecords.push_back(int(i));
  } else if (papersCursor >= 0) targetRecords.push_back(papersCursor);
  if (targetRecords.empty()) { ui.toast("Select papers", "Select one or more rows, or focus a paper, before showing them on the map.", 2, 3); return; }

  std::unordered_set<int> wanted(targetRecords.begin(), targetRecords.end());
  vector<int> nodes;
  nodes.reserve(std::min<size_t>(P->net.nodes.size(), targetRecords.size() * 4));
  for (size_t i = 0; i < P->net.nodes.size(); i++) {
    const Node& nd = P->net.nodes[i];
    bool hit = false;
    for (int rec : nd.recs) if (wanted.count(rec)) { hit = true; break; }
    if (hit) nodes.push_back(int(i));
  }
  if (nodes.empty()) { ui.toast("No matching map items", "The selected papers are not represented by items in the current map.", 2, 4); return; }

  if (workspace != WS_VISUALIZATION) setWorkspace(WS_VISUALIZATION);
  papersOpen = false;
  writerOpen = false;
  mainChartOpen = false;
  page = PG_NONE;
  lastVisualizationPage = PG_NONE;
  lastVisualizationChartOpen = false;
  settings.j.set("workspaceVisualPage", int(lastVisualizationPage));
  selection = nodes;
  invalidateFlags();
  closeDoc();
  scopeChanged();
  inspectorOpen = true;
  nv.nodesDirty = nv.linksDirty = true;
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  for (int i : nodes) if (size_t(i) < nv.pos.size()) {
    x0 = std::min(x0, double(nv.pos[size_t(i)][0])); x1 = std::max(x1, double(nv.pos[size_t(i)][0]));
    y0 = std::min(y0, double(nv.pos[size_t(i)][1])); y1 = std::max(y1, double(nv.pos[size_t(i)][1]));
  }
  if (x0 <= x1 && view != ViewKind::ThreeD && view != ViewKind::Matrix) {
    camTargetX = (x0 + x1) * 0.5;
    camTargetY = (y0 + y1) * 0.5;
    double w = std::max(1.0, x1 - x0), h = std::max(1.0, y1 - y0);
    camTargetZoom = clampv(std::min((canvasR.w - 120 * ui.s) / w, (canvasR.h - 120 * ui.s) / h), nv.fitZoom() * 0.7, nv.fitZoom() * 8.0);
  }
  ui.toast("Showing papers on the map", plural(long(nodes.size()), P->net.unitNoun), 1, 3);
  needFrame = true;
}

void App::setPage(int p) {
  Workspace beforeWorkspace = workspace;
  if ((p == PG_READ || p == PG_WRITER) && workspace != WS_BIBLIOGRAPHY) setWorkspace(WS_BIBLIOGRAPHY);
  else if (p >= PG_DATA && p <= PG_PUBLISH && p != PG_READ && p != PG_WRITER && workspace != WS_VISUALIZATION) setWorkspace(WS_VISUALIZATION);
  bool switchedWorkspace = beforeWorkspace != workspace;
  if (p == PG_WRITER) {  // the Write page opens the writer in the main area; a second click only folds its side panel
    if (!switchedWorkspace && page == PG_WRITER && !readerOpen) { page = PG_NONE; return; }
    if (!writerOpen) openWriter();  // captures the Bibliography return route before it changes the page
    else { page = PG_WRITER; lastBibliographyRoute = BR_WRITE; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); }
    return;
  }
  if (p == PG_READ) {  // the persistent Library sidebar stays beside the reader or attached-PDF table
    lastBibliographyRoute = BR_REVIEW;
    papersStatusFilter = -1;
    papersPdfFilter = 1;
    if (readerOpen) { bibliographyReaderReturnRoute = BR_REVIEW; bibliographyReaderReturnToPapers = true; }
    papersBuiltKey.clear();
    settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
    if (writerOpen) writerOpen = false;
    page = PG_READ;
    papersOpen = !readerOpen;
    mainChartOpen = false;
    needFrame = true;
    return;
  }
  if (readerOpen && p != PG_NONE) closeReader();  // any other stage leaves the reader (its position is kept)
  page = (page == p) ? PG_NONE : p;
  if (p == PG_NONE) page = PG_NONE;
  if (workspace == WS_VISUALIZATION && p != PG_AI) lastVisualizationPage = page;
  else if (workspace == WS_BIBLIOGRAPHY && (writerOpen || page == PG_WRITER)) lastBibliographyRoute = BR_WRITE;
  settings.j.set("workspaceVisualPage", lastVisualizationPage);
  settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
}

void App::pushUndo(const string& what) {
  if (!hasMap()) return;
  UndoEntry e;
  e.what = what;
  e.snap = P->snapshot(what);
  e.style = styleCommitted;
  e.n = P->net.n();
  undoStack.push_back(std::move(e));
  if (undoStack.size() > 60) undoStack.erase(undoStack.begin());
  redoStack.clear();
}

void App::commitStyleIfChanged() {
  if (ui.in.down[0]) return;  // wait until the drag ends
  Json cur = styleToJson(P->style);
  string a = cur.dump(), b = styleCommitted.dump();
  if (a == b) return;
  if (hasMap()) {
    UndoEntry e;
    e.what = "Look";
    e.snap = P->snapshot("Look");
    e.style = styleCommitted;
    e.n = P->net.n();
    undoStack.push_back(std::move(e));
    redoStack.clear();
  }
  styleCommitted = cur;
  P->dirty = true;
}

void App::undo() {
  if (undoStack.empty()) { ui.toast("Nothing to undo", "", 0, 1.5); return; }
  UndoEntry e = undoStack.back();
  undoStack.pop_back();
  if (e.n != P->net.n()) { undoStack.clear(); ui.toast("Undo unavailable", "The map was rebuilt since this change.", 2); return; }
  UndoEntry r;
  r.what = e.what; r.snap = P->snapshot(e.what); r.style = styleToJson(P->style); r.n = P->net.n();
  redoStack.push_back(r);
  P->restore(e.snap);
  styleFromJson(P->style, e.style);
  P->style.darkTheme = ui.dark;
  styleCommitted = styleToJson(P->style);
  P->bundles = Bundles();
  invalidateFlags();
  styleDirty = posDirty = true;
  cinfoValid = false;
  ui.toast("Undo", e.what, 0, 1.5);
}

void App::redo() {
  if (redoStack.empty()) return;
  UndoEntry e = redoStack.back();
  redoStack.pop_back();
  if (e.n != P->net.n()) { redoStack.clear(); return; }
  UndoEntry u;
  u.what = e.what; u.snap = P->snapshot(e.what); u.style = styleToJson(P->style); u.n = P->net.n();
  undoStack.push_back(u);
  P->restore(e.snap);
  styleFromJson(P->style, e.style);
  P->style.darkTheme = ui.dark;
  styleCommitted = styleToJson(P->style);
  invalidateFlags();
  styleDirty = posDirty = true;
  cinfoValid = false;
  ui.toast("Redo", e.what, 0, 1.5);
}

// ------------------------------------------------------------------ data commands
static const vector<FileFilter> kBibFilters = {{"Supported files", "*.txt;*.csv;*.tsv;*.ris;*.bib;*.json;*.nbib;*.vosproj"},
                                               {"All files", "*.*"}};

void App::cmdOpenFiles() {
  auto files = openFileDialog(hwnd, "Open", kBibFilters, true);
  if (!files.empty()) { showStart = false; dropFiles(files); }
}
void App::cmdOpenAny() { cmdOpenFiles(); }

bool App::dropFiles(const vector<string>& files) {
  vector<string> bib, vos, pdfs;
  for (auto& f : files) if (lower(fileExt(f)) == "pdf") pdfs.push_back(f);
  if (!pdfs.empty()) {  // PDFs join the reading library (linked to a record when the DOI or title matches); one PDF opens
    int rec = (papersOpen || docPreview >= 0) && docPreview >= 0 ? docPreview : -1;
    readerAttach(pdfs, rec, pdfs.size() == 1 && files.size() == 1);
    if (pdfs.size() == files.size()) return true;
  }
  for (auto& f : files) {
    string ext = fileExt(f);
    if (lower(ext) == "pdf") continue;
    if (ext == "vosproj") { cmdOpenProject(f); return true; }
    bool ok = false;
    string head = readFileU(f, &ok).substr(0, 4096);
    if (!ok) { ui.toast("Cannot read file", f, 3); continue; }
    BibFormat bf = detectFormat(head, f);
    if (bf == BibFormat::VOSviewer) vos.push_back(f);
    else if (bf == BibFormat::Bundle) { cmdOpenProject(f); return true; }
    else bib.push_back(f);
  }
  if (!vos.empty()) {
    string mapT, netT;
    for (auto& f : vos) {
      string t = readFileU(f);
      string first = lower(t.substr(0, t.find('\n')));
      if (contains(first, "label") || contains(first, "id\t") || contains(first, "x\t")) mapT = t; else netT = t;
    }
    string err;
    if (P->loadVosviewer(mapT, netT, &err)) { afterMapChanged(); ui.toast("VOSviewer map loaded", plural(P->net.n(), "item") + " · " + plural(P->net.m(), "link"), 1); }
    else ui.toast("Could not load VOSviewer files", err, 3, 7);
  }
  if (!bib.empty()) cmdAddFiles(bib);
  return true;
}

void App::cmdAddFiles(const vector<string>& files) {
  struct Parsed { string name; BibFormat f; vector<Record> recs; string err; };
  auto out = std::make_shared<vector<Parsed>>();
  startJob("Importing " + plural(long(files.size()), "file"), [files, out](Job& j) {
    for (size_t i = 0; i < files.size(); i++) {
      Parsed p;
      p.name = files[i];
      bool ok = false;
      string text = readFileU(files[i], &ok);
      if (!ok) p.err = "cannot read";
      else parseBibText(files[i], text, p.f, p.recs, &p.err);
      out->push_back(std::move(p));
      j.progress = double(i + 1) / files.size();
      if (j.cancel) break;
    }
  }, [this, out] {
    int added = 0, before = int(P->corpus.recs.size()), idConflicts = 0;
    for (auto& p : *out) {
      if (p.recs.empty()) { ui.toast("Skipped " + fileName(p.name), p.err, 2, 7); continue; }
      added += P->addRecords(p.name, p.f, p.recs);
      idConflicts += P->lastRecordIdConflicts;
    }
    if (added > 0 || int(P->corpus.recs.size()) != before) {
      int duplicateGroups = 0;
      for (const RecordDuplicateGroup& group : recordDuplicateGroups(P->corpus.recs)) {
        bool pending = false;
        for (int index : group.indices) if (index >= 0 && size_t(index) < P->corpus.recs.size() && !P->corpus.recs[size_t(index)].duplicateReviewed) pending = true;
        if (pending) duplicateGroups++;
      }
      string importDetail = plural(added, "record") + " added" + (duplicateGroups ? " · " + plural(duplicateGroups, "duplicate group") + " waiting for review" : "") + " · " + fmtInt(long(P->corpus.recs.size())) + " total";
      if (idConflicts) importDetail += " · " + plural(idConflicts, "reused local ID") + " assigned new IDs to keep records distinct";
      ui.toast("Records imported", importDetail, 1, 6);
      pageStateReset();
      if (workspace == WS_BIBLIOGRAPHY) {
        lastBibliographyRoute = BR_PAPERS;
        page = PG_NONE;
        papersOpen = true;
        settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
      } else if (!hasMap()) page = PG_BUILD;
    }
  });
}

void App::cmdSample(bool scopus) {
  int n = P->addSample(scopus);
  pageStateReset();
  ui.toast("Sample loaded", plural(n, "record") + (scopus ? " (Scopus CSV)" : " (Web of Science)") + "", 1, 5);
  if (workspace == WS_BIBLIOGRAPHY) { lastBibliographyRoute = BR_PAPERS; page = PG_READ; papersOpen = true; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); }
  else if (!hasMap()) page = PG_BUILD;
}

void App::pageStateReset() {
  variantsScanned = false; variants.clear();
  burstsValid = evoValid = cmpValid = tfValid = false;
  actorsUnit = -1; sensSig.clear(); cinfoValid = false; stabValid = false;
  cmpA0 = cmpA1 = cmpB0 = cmpB1 = 0;
  pdiffValid = false; mpVer = 0; sweepValid = false;  // 1.8
  // 1.2: corpus-dependent caches; the map history restarts from the current map
  ttValid = rpValid = cyValid = collabValid = false;
  prodUnit = -1;
  cprof = CountryProfile();
  citeIdx = CitationIndex();
  closeDoc();
  mapHistory.clear();
  mapCur = -1;
  if (hasMap()) rememberMap();
}

void App::cmdBuild() {
  if (!hasCorpus()) { ui.toast("No data yet", "Open a bibliographic file or load the sample first.", 2); setPage(PG_DATA); return; }
  auto res = std::make_shared<Network>();
  auto rep = std::make_shared<BuildReport>();
  Project* p = P.get();
  startJob("Building map", [p, res, rep](Job& j) {
    *res = p->computeNetwork(*rep, [&j](double x) { j.progress = x; }, &j.cancel);
  }, [this, res, rep] {
    P->commitBuild(std::move(*res), *rep);
    undoStack.clear(); redoStack.clear();
    afterMapChanged();
    const auto& R = P->last;
    ui.toast("Map built", plural(R.n, P->net.unitNoun) + " · " + plural(R.m, "link") + " · " + plural(R.clusters, "cluster") + " · Q " + fmtFixed(R.Q, 3) + " · " + fmtNum(R.ms, 0) + " ms", 1, 5);
  });
}

void App::afterMapChanged(bool fit) {
  compareClose(); cmpThA = cmpThB = 0;
  geoSig_.clear();  // country classification is keyed to this map's node labels, even when vector storage was reused
  selection.clear(); hover = -1; clusterFilter = -1; searchHits.clear();
  invalidateFlags();
  pos3dValid = false; pos3d.clear();
  cinfoValid = false; stabValid = false; figSig.clear();
  mainChartOpen = false;
  papersOpen = workspace == WS_BIBLIOGRAPHY && !writerOpen && !readerOpen && lastBibliographyRoute != BR_WRITE;  // maps never take over Bibliography
  pdiffValid = false; sweepValid = false; diffRestore.active = false;  // 1.8
  if (geoAfterBuild) { geoAfterBuild = false; if (geoAvailable()) view = ViewKind::Geo; }
  if (view == ViewKind::Geo && !geoAvailable()) view = ViewKind::Network;
  geoSel = -1; geoHover = -1;
  geoFitBounds(view);
  if (P->net.scoreIdx < 0 && !P->net.scoreNames.empty()) P->net.scoreIdx = 0;
  nv.pos.clear();
  nv.flags.clear();
  nv.setData(hasMap() ? &P->net : nullptr, &P->style, &P->bundles);
  for (auto& L : livePane_) { L.dataDirty = true; L.posDirty = true; L.fitted = false; }
  animT0 = -1;
  nv.pos = targetPositions(view);
  nv.positionsChanged();
  nv.nodesDirty = nv.linksDirty = true;
  nv.kind = view;
  nv.vp = canvasR;
  if (fit) { nv.fit(); camTargetZoom = -1; }
  styleCommitted = styleToJson(P->style);
  if (view == ViewKind::ThreeD) start3DLayout();
  welcome = false;
  cyValid = false;
  if (hasMap()) rememberMap();
}

void App::cmdRelayout() {
  if (!hasMap()) return;
  pushUndo("Layout");
  auto copy = std::make_shared<Network>(P->net);
  BuildParams bp = P->params;
  bp.layout.seed++;
  P->params.layout.seed = bp.layout.seed;
  startJob("Running VOS layout", [copy, bp](Job& j) {
    normalise(*copy, bp.norm);
    vosLayout(*copy, bp.layout, [&j](double x) { j.progress = x; }, &j.cancel);
  }, [this, copy] {
    if (copy->n() != P->net.n()) return;
    for (int i = 0; i < P->net.n(); i++) { P->net.nodes[size_t(i)].x = copy->nodes[size_t(i)].x; P->net.nodes[size_t(i)].y = copy->nodes[size_t(i)].y; }
    P->finishPositions();
    P->bundles = Bundles();
    P->dirty = true;
    pos3dValid = false;
    setView(view, true);
    ui.toast("Layout updated", "Seed " + std::to_string(P->params.layout.seed), 1, 2.5);
  });
}

void App::cmdRecluster() {
  if (!hasMap()) return;
  pushUndo("Clustering");
  P->recluster();
  invalidateFlags();
  cinfoValid = false;
  styleDirty = true;
  ui.toast("Clusters updated", plural(P->net.nClusters, "cluster") + " · Q " + fmtFixed(P->last.Q, 3), 1, 3);
}

void App::cmdBundles() {
  if (!hasMap()) return;
  auto copy = std::make_shared<Network>(P->net);
  auto out = std::make_shared<Bundles>();
  startJob("Bundling edges", [copy, out](Job& j) { *out = fdeb(*copy, 0.6, 5, 60, [&j](double x) { j.progress = x; }); }, [this, out, copy] {
    if (copy->n() != P->net.n()) return;
    P->bundles = std::move(*out);
    P->style.bundle = true;
    styleDirty = true;
    ui.toast("Edges bundled", "Visible in Network, Overlay and Density views and in figures.", 1, 3);
  });
}

// ------------------------------------------------------------------ project
void App::cmdSaveProject(bool saveAs) {
  if (!hasCorpus() && !hasMap() && wdoc.empty() && !P->library.hasData()) { ui.toast("Nothing to save", "Load data, add papers or write something first.", 2); return; }
  if (live.open) liveSaveToChat();  // the voice conversation so far goes into the project with the Assistant's chat
  string path = P->path;
  if (saveAs || path.empty()) {
    path = saveFileDialog(hwnd, "Save project", {{"VOSStudio project", "*.vosproj"}}, exportName("vosproj"), "vosproj");
    if (path.empty()) return;
  }
  string err;
  mapsToProject();
  writerToProject();
  if (P->save(path, &err)) {
    P->path = path;
    P->dirty = false;
    settings.addRecent(path);
    settings.save();
    ui.toast("Project saved", fileName(path), 1, 2.5);
  } else ui.toast("Save failed", err, 3);
}

void App::cmdOpenProject(const string& path0) {
  string path = path0;
  if (path.empty()) {
    auto v = openFileDialog(hwnd, "Open project", {{"VOSStudio project", "*.vosproj"}, {"All files", "*.*"}}, false);
    if (v.empty()) return;
    path = v[0];
  }
  if (busy()) { ui.toast("Please wait", jobLabel, 2); return; }
  auto np = std::make_unique<Project>();
  string err;
  if (!np->open(path, &err)) { ui.toast("Could not open project", err, 3, 7); return; }
  np->path = path;
  np->dirty = false;
  readerStorePosition();
  readerProjectChanged();
  P = std::move(np);
  P->style.darkTheme = ui.dark;
  settings.addRecent(path);
  settings.save();
  undoStack.clear(); redoStack.clear();
  papersStatusFilter = papersPdfFilter = -1;
  papersCollectionFilter.clear(); papersTagFilter.clear(); papersFilter.clear();
  papersNewCollectionOpen = papersNewViewOpen = false;
  papersNewCollectionName.clear(); papersNewViewName.clear(); papersNewTagName.clear(); papersBuiltKey.clear();
  pageStateReset();
  mapsFromProject();
  writerFromProject();
  afterMapChanged();
  if (workspace == WS_BIBLIOGRAPHY && !writerOpen && !readerOpen) { lastBibliographyRoute = BR_PAPERS; page = PG_READ; papersOpen = true; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); }
  ui.toast("Project opened", fileName(path) + " · " + plural(long(P->corpus.recs.size()), "record") + " · " + plural(P->net.n(), "item"), 1, 4);
  livingValid = false; livingRecs.clear(); living = Placement();
  livingOnOpen();  // 1.8: check the saved search for new papers when the project asks for it
}

void App::cmdNewProject() {
  if (busy()) return;
  if (P->dirty && (hasMap() || hasCorpus() || !wdoc.empty() || P->library.hasData()) && !scriptMode) {
    int r = MessageBoxW(hwnd, L"Discard the current project?", L"VOSStudio", MB_OKCANCEL | MB_ICONQUESTION);
    if (r != IDOK) return;
  }
  newProjectNow();
}

void App::newProjectNow() {
  closeDoc();
  papersOpen = false;
  mainChartOpen = false;
  figZoomOpen = false;
  writerReset();
  readerProjectChanged();
  P = std::make_unique<Project>();
  P->style.darkTheme = ui.dark;
  undoStack.clear(); redoStack.clear();
  papersStatusFilter = papersPdfFilter = -1;
  papersCollectionFilter.clear(); papersTagFilter.clear(); papersFilter.clear();
  papersNewCollectionOpen = papersNewViewOpen = false;
  papersNewCollectionName.clear(); papersNewViewName.clear(); papersNewTagName.clear(); papersBuiltKey.clear();
  pageStateReset();
  afterMapChanged();
  welcome = true;
  if (workspace == WS_BIBLIOGRAPHY) { lastBibliographyRoute = BR_PAPERS; page = PG_NONE; papersOpen = true; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); }
  else { lastVisualizationPage = PG_DATA; page = PG_DATA; }
}

string App::exportName(const string& ext) const {
  string base = P->path.empty() ? "" : fileName(P->path);
  if (!base.empty()) { size_t d = base.rfind('.'); if (d != string::npos) base = base.substr(0, d); }
  if (base.empty()) base = hasMap() && P->mapSource == "analysis" ? string(typeInfo(P->spec.type).id) + "-" + unitId(P->spec.unit) + "-map" : "vosstudio-map";
  return base + "." + ext;
}

// ------------------------------------------------------------------ exports
namespace {
bool hybridizeForExport(Gfx& gfx, const FigureSpec& spec, Scene& scene, string* err) {
  if (!spec.hybridExport) return true;
  Scene hybrid;
  if (!makeHybridScene(gfx, scene, spec.dpi, hybrid, err)) return false;
  scene = std::move(hybrid);
  return true;
}

bool hybridizeForExport(Gfx& gfx, const FigureSpec& spec, vector<Scene>& scenes, string* err) {
  if (!spec.hybridExport) return true;
  for (Scene& scene : scenes) if (!hybridizeForExport(gfx, spec, scene, err)) return false;
  return true;
}
}  // namespace

void App::cmdExportFigure(const string& fmt, const string& path0) {
  if (!hasMap()) { ui.toast("No map", "Build a map first.", 2); return; }
  string path = path0;
  if (path.empty()) {
    FileFilter f = fmt == "svg" ? FileFilter{"SVG vector image", "*.svg"} : fmt == "pdf" ? FileFilter{"PDF document", "*.pdf"} : FileFilter{"PNG image", "*.png"};
    path = saveFileDialog(hwnd, "Export figure", {f}, exportName(fmt), fmt);
    if (path.empty()) return;
  }
  ViewStyle st = P->style;
  auto panelDefs = figurePanelDefs();
  Scene sc = buildFigure(P->net, st, P->fig, &P->bundles, P->methodsShort(), &panelDefs);
  bool ok = false;
  string err;
  string title = P->fig.title.empty() ? "VOSStudio figure" : P->fig.title;
  int nPages = 1;
  if (fmt == "svg") {
    ok = hybridizeForExport(g, P->fig, sc, &err) && writeFileU(path, toSVG(sc, P->fig.serif));
  }
  else if (fmt == "pdf" && P->fig.pdfPages && figurePanels(P->fig).size() > 1) {
    auto pages = buildFigurePages(P->net, st, P->fig, &P->bundles, P->methodsShort(), &panelDefs);
    nPages = int(pages.size());
    ok = hybridizeForExport(g, P->fig, pages, &err) && writeFileU(path, toPDF(pages, title));
  }
  else if (fmt == "pdf") {
    ok = hybridizeForExport(g, P->fig, sc, &err) && writeFileU(path, toPDF(sc, title));
  }
  else ok = exportScenePNG(g, sc, P->fig.dpi, path, &err, P->fig.transparent);
  if (!ok) { ui.toast("Export failed", err.empty() ? "Could not write " + path : err, 3); return; }
  if (!scriptMode) ui.toast("Figure exported", fileName(path) + (nPages > 1 ? " · " + std::to_string(nPages) + " pages" : "") + " · " + fmtNum(P->fig.wmm, 0) + "×" + fmtNum(P->fig.hmm, 0) + " mm" + (sc.warnings.empty() ? "" : " · " + plural(long(sc.warnings.size()), "warning")), 1, 4);
}

void App::cmdCopyFigure() {
  if (!hasMap()) return;
  auto panelDefs = figurePanelDefs();
  Scene sc = buildFigure(P->net, P->style, P->fig, &P->bundles, P->methodsShort(), &panelDefs);
  vector<uint8_t> px;
  int w, h;
  if (renderSceneBGRA(g, sc, 200, px, w, h) && setClipboardImage(hwnd, w, h, px.data())) ui.toast("Copied", "Figure copied to the clipboard (" + std::to_string(w) + "×" + std::to_string(h) + " px).", 1);
  else ui.toast("Copy failed", "", 3);
}

void App::cmdExportView(const string& path0) {
  if (!hasMap()) return;
  string path = path0;
  if (path.empty()) { path = saveFileDialog(hwnd, "Export current view", {{"PNG image", "*.png"}}, exportName("png"), "png"); if (path.empty()) return; }
  pendingShot = true;
  pendingViewCrop = true;
  pendingShotPath = path;
}

// ------------------------------------------------------------------ WYSIWYG view export
// The current canvas as vector primitives: basemap (Geo), hulls, links, sphere-shaded nodes, labels, timeline axis
// and legend, at the on-screen size (1 logical pixel = 0.75 pt). Density maps are embedded as a raster layer.
Scene App::captureView(bool forRaster) const {
  (void)forRaster;
  Scene sc;
  if (!hasMap()) return sc;
  float s = ui.s;
  double k = 0.75 / s;
  if (view == ViewKind::Matrix) { ChartTheme ct = chartTheme(false); ct.transparent = P->fig.transparent; return chartDensityMatrix(P->net, clusterColors(), canvasR.w * k, canvasR.h * k, ct); }
  Theme th = canvasTheme(P->style);
  sc.W = nv.vp.w * k;
  sc.H = nv.vp.h * k;
  if (!P->fig.transparent) { Prim bg; bg.type = Prim::Rect; bg.group = "background"; bg.w = float(sc.W); bg.h = float(sc.H); bg.fill = true; bg.fillC = th.bg; sc.items.push_back(bg); }
  if (view == ViewKind::Geo) geoPrims(sc, k, true);
  if (view == ViewKind::Density && viewGrabW > 0 && !viewGrab.empty()) {
    Prim im; im.type = Prim::Image; im.group = "density"; im.w = float(sc.W); im.h = float(sc.H);
    im.imgW = viewGrabW; im.imgH = viewGrabH; im.rgba = viewGrab;
    sc.items.push_back(std::move(im));
  }
  nv.captureInto(sc, th, k, showLabels);
  auto T = [&](float x, float y, const string& t, float px, Color c, int anchor, bool bold) {
    Prim p; p.type = Prim::Text; p.group = "legend"; p.x = x; p.y = y; p.text = t; p.size = float(px * k); p.fill = true; p.fillC = c; p.anchor = anchor; p.bold = bold; return p;
  };
  auto R = [&](float x, float y, float w, float h, Color f, Color st, const string& grp) {
    Prim p; p.type = Prim::Rect; p.group = grp; p.x = x; p.y = y; p.w = w; p.h = h; p.fill = true; p.fillC = f;
    if (st.a > 0) { p.stroke = true; p.strokeC = st; p.sw = float(0.75); }
    return p;
  };
  Color fg = th.fg, dim = th.muted;
  // timeline axis
  if (view == ViewKind::Timeline && tlS1 > tlS0) {
    float y = float((nv.vp.h - 34 * s) * k);
    sc.items.push_back(R(0, float(y - 8 * s * k), float(sc.W), float(42 * s * k), th.bg.withA(0.85f), Color(0, 0, 0, 0), "axis"));
    Prim ax; ax.type = Prim::Path; ax.group = "axis"; ax.stroke = true; ax.strokeC = dim.withA(0.5f); ax.sw = 0.75f;
    ax.d = {{'M', float(20 * s * k), y, 0, 0}, {'L', float(sc.W - 20 * s * k), y, 0, 0}};
    sc.items.push_back(ax);
    int y0 = int(std::floor(tlS0)), y1 = int(std::ceil(tlS1));
    int step = std::max(1, (y1 - y0) / 10);
    for (int yr = y0; yr <= y1; yr += step) {
      double wx = tlX0 + (yr - tlS0) / (tlS1 - tlS0) * (tlX1 - tlX0);
      float sx, sy;
      nv.worldToScreen(wx, 0, 0, sx, sy);
      if (sx < nv.vp.x + 10 * s || sx > nv.vp.r() - 10 * s) continue;
      float x = float((sx - nv.vp.x) * k);
      Prim t; t.type = Prim::Path; t.group = "axis"; t.stroke = true; t.strokeC = dim; t.sw = 0.75f;
      t.d = {{'M', x, float(y - 4 * s * k), 0, 0}, {'L', x, float(y + 4 * s * k), 0, 0}};
      sc.items.push_back(t);
      Prim gl = t; gl.strokeC = dim.withA(0.08f); gl.d = {{'M', x, 0, 0, 0}, {'L', x, float(y - 8 * s * k), 0, 0}};
      sc.items.push_back(gl);
      Prim lab = T(x, float(y + 18 * s * k), std::to_string(yr), 11 * s, dim, 1, true);
      lab.group = "axis";
      sc.items.push_back(lab);
    }
  }
  // legend (as on screen, top-left)
  if (showLegend) {
    Color cardBg = th.light ? Color(1, 1, 1, 0.92f) : Color(0.09f, 0.1f, 0.12f, 0.9f);
    Color cardBd = th.light ? Color(0, 0, 0, 0.1f) : Color(1, 1, 1, 0.1f);
    float x = float(14 * s * k), y = float(14 * s * k), u = float(s * k);
    const Network& N = P->net;
    const int gm = view == ViewKind::Geo ? geoMode() : 0;
    bool scoreMode = view == ViewKind::Overlay || view == ViewKind::Timeline || P->style.colorBy == ColorBy::Score || (gm == 1 && geoLayerKind == 1);
    auto card = [&](float w, float h) { Prim c = R(x, y, w, h, cardBg, cardBd, "legend"); sc.items.push_back(c); };
    auto bar = [&](float bx, float by, float bw, float bh, std::function<Color(double)> col) {
      int K = 48;
      for (int q = 0; q < K; q++) sc.items.push_back(R(bx + bw * q / K, by, bw / K + 0.3f, bh, col((q + 0.5) / K), Color(0, 0, 0, 0), "legend"));
    };
    if (gm > 0 && geoLayerKind == 2) {  // Geo density layer
      card(236 * u, 62 * u);
      sc.items.push_back(T(x + 12 * u, y + 19 * u, "Density of " + (gm == 1 ? pluralWord(N.unitNoun) : string("documents")), 11.5f * s, fg, 0, true));
      bar(x + 12 * u, y + 28 * u, 212 * u, 9 * u, [&](double t) { return simulateCvd(cmapAt(P->style.densityScheme, 0.1 + 0.9 * std::pow(t, 0.6)), P->style.cvd); });
      sc.items.push_back(T(x + 12 * u, y + 50 * u, "Low", 10.5f * s, dim, 0, false));
      sc.items.push_back(T(x + 224 * u, y + 50 * u, "High", 10.5f * s, dim, 2, false));
    } else if (gm == 2 && geoLayerKind == 1) {  // Geo overview: average year
      card(250 * u, 76 * u);
      sc.items.push_back(T(x + 12 * u, y + 19 * u, "Average publication year", 11.5f * s, fg, 0, true));
      Color land = geoLand(th);
      bar(x + 12 * u, y + 28 * u, 226 * u, 9 * u, [&](double t) { return land.mix(simulateCvd(cmapAt(P->style.scheme, t), P->style.cvd), 0.88f); });
      sc.items.push_back(T(x + 12 * u, y + 50 * u, fmtFixed(geoYearLo, 1), 10.5f * s, dim, 0, false));
      sc.items.push_back(T(x + 238 * u, y + 50 * u, fmtFixed(geoYearHi, 1), 10.5f * s, dim, 2, false));
      sc.items.push_back(T(x + 12 * u, y + 67 * u, "5th to 95th percentile", 10.5f * s, dim, 0, false));
    } else if (gm == 2) {
      card(250 * u, 76 * u);
      sc.items.push_back(T(x + 12 * u, y + 19 * u, "Documents per country", 11.5f * s, fg, 0, true));
      Color land = geoLand(th);
      bar(x + 12 * u, y + 28 * u, 226 * u, 9 * u, [&](double t) { return land.mix(cmapAt(P->style.scheme, 0.12 + 0.88 * t), float(0.45 + 0.5 * t)); });
      sc.items.push_back(T(x + 12 * u, y + 50 * u, "1", 10.5f * s, dim, 0, false));
      sc.items.push_back(T(x + 238 * u, y + 50 * u, fmtInt(geoDocsMax), 10.5f * s, dim, 2, false));
      sc.items.push_back(T(x + 12 * u, y + 67 * u, "log scale", 10.5f * s, dim, 0, false));
    } else if (scoreMode && N.scoreIdx >= 0) {
      card(236 * u, 62 * u);
      string sname = N.scoreIdx < int(N.scoreNames.size()) ? N.scoreNames[size_t(N.scoreIdx)] : "Score";
      sc.items.push_back(T(x + 12 * u, y + 19 * u, sname, 11.5f * s, fg, 0, true));
      bar(x + 12 * u, y + 28 * u, 212 * u, 9 * u, [&](double t) { return simulateCvd(cmapAt(P->style.scheme, t), P->style.cvd); });
      const Encoder& e = nv.enc();
      bool yr = contains(lower(sname), "year");
      sc.items.push_back(T(x + 12 * u, y + 50 * u, yr ? fmtFixed(e.sMin, 1) : fmtNum(e.sMin, 2), 10.5f * s, dim, 0, false));
      sc.items.push_back(T(x + 224 * u, y + 50 * u, yr ? fmtFixed(e.sMax, 1) : fmtNum(e.sMax, 2), 10.5f * s, dim, 2, false));
    } else if (N.nClusters > 0 && view != ViewKind::Density) {
      int shown = std::min(N.nClusters, 12);
      card(250 * u, 34 * u + shown * 24 * u + (N.nClusters > shown ? 18 * u : 0));
      sc.items.push_back(T(x + 12 * u, y + 20 * u, "Clusters", 11.5f * s, fg, 0, true));
      std::vector<int> sizes(size_t(N.nClusters), 0);
      for (auto& nd : N.nodes) if (nd.cluster >= 0 && nd.cluster < N.nClusters) sizes[size_t(nd.cluster)]++;
      auto cols = clusterColors();
      for (int c = 0; c < shown; c++) {
        float ry = y + 30 * u + c * 24 * u;
        Prim dot; dot.type = Prim::Circle; dot.group = "legend"; dot.x = x + 17 * u; dot.y = ry + 11 * u; dot.r = 5.5f * u; dot.fill = true; dot.fillC = cols[size_t(c) % cols.size()];
        dot.sphere = !P->style.flat;
        sc.items.push_back(dot);
        bool dimmed = clusterFilter >= 0 && clusterFilter != c;
        // truncate like the on-screen card so the name never runs into the count
        string nm = N.clusterName(c);
        const double maxW = 190.0 * u, fs = 11.5 * s * k;
        if (textWidth(nm, fs, false) > maxW) {
          string u8 = nm;
          while (!u8.empty() && textWidth(u8 + "\xE2\x80\xA6", fs, false) > maxW) {
            u8.pop_back();
            while (!u8.empty() && (u8.back() & 0xC0) == 0x80) u8.pop_back();  // keep UTF-8 whole
            if (!u8.empty() && (static_cast<unsigned char>(u8.back()) & 0xC0) == 0xC0) u8.pop_back();
          }
          nm = trim(u8) + "\xE2\x80\xA6";
        }
        sc.items.push_back(T(x + 30 * u, ry + 15 * u, nm, 11.5f * s, dimmed ? dim : fg, 0, false));
        sc.items.push_back(T(x + 238 * u, ry + 15 * u, std::to_string(sizes[size_t(c)]), 11 * s, dim, 2, false));
      }
      if (N.nClusters > shown) sc.items.push_back(T(x + 12 * u, y + 34 * u + shown * 24 * u + 12 * u, "+ " + std::to_string(N.nClusters - shown) + " more", 10.5f * s, dim, 0, false));
    }
  }
  return sc;
}

string App::viewName(ViewKind v) {
  for (auto& vd : kViews) if (vd.v == v) return vd.label;
  return "Network";
}

// Exports exactly what the canvas shows, for every view (Network, Overlay, Density, Timeline, Matrix, Geo, 3D).
// fmt "" asks for the format in the save dialog (from the chosen file type / extension).
void App::cmdExportCurrentView(const string& fmt0, const string& path0) {
  if (!hasMap()) { ui.toast("Export view", "Build or open a map first.", 2); return; }
  string fmt = fmt0, path = path0;
  string base = exportName("x");
  base = base.substr(0, base.size() - 2) + "-" + lower(viewName(view) == "3D" ? string("3d") : viewName(view));
  if (path.empty()) {
    vector<FileFilter> fl;
    if (fmt.empty() || fmt == "svg") fl.push_back({"SVG vector image", "*.svg"});
    if (fmt.empty() || fmt == "pdf") fl.push_back({"PDF document", "*.pdf"});
    if (fmt.empty() || fmt == "png") fl.push_back({"PNG image", "*.png"});
    string def = fmt.empty() ? "svg" : fmt;
    path = saveFileDialog(hwnd, "Export the " + viewName(view) + " view", fl, base + "." + def, def);
    if (path.empty()) return;
  }
  if (fmt.empty()) {
    string ext = lower(path.substr(path.find_last_of('.') == string::npos ? path.size() : path.find_last_of('.') + 1));
    fmt = ext == "pdf" || ext == "png" ? ext : "svg";
  }
  pendingViewFmt = fmt;
  pendingViewPath = path;
  needFrame = true;
}

// runs inside the frame right after the GPU pass (so density maps can be read back from the canvas)
void App::doViewExport() {
  string fmt = pendingViewFmt, path = pendingViewPath;
  pendingViewFmt.clear();
  viewGrab.clear(); viewGrabW = viewGrabH = 0;
  if (view == ViewKind::Density) {
    vector<uint8_t> px;
    int w = 0, h = 0;
    if (g.readback(px, w, h)) {
      int x0 = int(canvasR.x), y0 = int(canvasR.y), cw = std::min(int(canvasR.w), w - x0), ch = std::min(int(canvasR.h), h - y0);
      if (cw > 0 && ch > 0) {
        viewGrab.resize(size_t(cw) * size_t(ch) * 4);
        for (int y = 0; y < ch; y++)
          for (int x = 0; x < cw; x++) {
            const uint8_t* q = &px[(size_t(y0 + y) * size_t(w) + size_t(x0 + x)) * 4];
            uint8_t* o = &viewGrab[(size_t(y) * size_t(cw) + size_t(x)) * 4];
            o[0] = q[2]; o[1] = q[1]; o[2] = q[0]; o[3] = 255;
          }
        viewGrabW = cw; viewGrabH = ch;
      }
    }
  }
  Scene sc = captureView(fmt == "png");
  bool ok = false;
  string err;
  if (fmt == "svg") ok = hybridizeForExport(g, P->fig, sc, &err) && writeFileU(path, toSVG(sc, P->fig.serif));
  else if (fmt == "pdf") ok = hybridizeForExport(g, P->fig, sc, &err) && writeFileU(path, toPDF(sc, "VOSStudio view"));
  else ok = exportScenePNG(g, sc, 192 * ui.s, path, &err, P->fig.transparent);
  viewGrab.clear();
  viewGrabW = viewGrabH = 0;
  if (!scriptMode) {
    if (ok) ui.toast("View exported", fileName(path) + "  \xC2\xB7  " + fmtNum(sc.W / MM2PT, 0) + " \xC3\x97 " + fmtNum(sc.H / MM2PT, 0) + " mm, " + fmtInt(long(sc.items.size())) + " objects", 1, 4);
    else ui.toast("Export failed", err.empty() ? "Could not write " + fileName(path) : err, 3);
  }
  lastViewExport = ok ? int(sc.items.size()) : -1;
}

ChartTheme App::chartTheme(bool forExport) const {
  if (forExport) { ChartTheme t = ChartTheme::make(true); t.font = 9; t.transparent = P->fig.transparent; return t; }
  ChartTheme t = ChartTheme::make(!ui.dark);
  t.bg = ui.c.card;
  t.fg = ui.c.text;
  t.muted = ui.c.textDim;
  t.accent = ui.c.accent;
  t.font = 10.5f;
  return t;
}

void App::cmdExportChart(const ChartDef& def, const string& fmt) {
  if (!def.valid()) return;
  string name = lower(replaceAll(def.title, " ", "-"));
  name = replaceAll(replaceAll(name, "/", "-"), ":", "");
  FileFilter f = fmt == "svg" ? FileFilter{"SVG vector image", "*.svg"} : fmt == "pdf" ? FileFilter{"PDF document", "*.pdf"} : FileFilter{"PNG image", "*.png"};
  string path = saveFileDialog(hwnd, "Export chart", {f}, name + "." + fmt, fmt);
  if (path.empty()) return;
  Scene sc = def.make(500, 320, chartTheme(true));
  bool ok = false;
  string err;
  if (fmt == "svg") ok = hybridizeForExport(g, P->fig, sc, &err) && writeFileU(path, toSVG(sc));
  else if (fmt == "pdf") ok = hybridizeForExport(g, P->fig, sc, &err) && writeFileU(path, toPDF(sc, def.title));
  else ok = exportScenePNG(g, sc, 300, path, &err, P->fig.transparent);
  if (ok) ui.toast("Chart exported", fileName(path), 1);
  else ui.toast("Export failed", err, 3);
}

void App::cmdExportChartsPdf(const vector<ChartDef>& defs0, const string& path0) {
  vector<ChartDef> defs;
  for (auto& d : defs0) if (d.valid()) defs.push_back(d);
  if (defs.empty()) { ui.toast("No charts", "Open a page with charts first.", 2); return; }
  string path = path0;
  if (path.empty()) {
    string name = page >= 0 && page < PG_COUNT ? lower(kPageTitles[page]) : "";
    path = saveFileDialog(hwnd, "Export charts", {{"PDF document", "*.pdf"}}, (name.empty() ? string("charts") : name + "-charts") + ".pdf", "pdf");
    if (path.empty()) return;
  }
  vector<Scene> pages;
  ChartTheme t = chartTheme(true);
  for (auto& d : defs) pages.push_back(d.make(500, 320, t));
  string err;
  bool ok = hybridizeForExport(g, P->fig, pages, &err) && writeFileU(path, toPDF(pages, "VOSStudio charts"));
  if (ok) { if (!scriptMode) ui.toast("Charts exported", fileName(path) + " · " + plural(long(pages.size()), "page"), 1); }
  else ui.toast("Export failed", err.empty() ? "Could not write " + fileName(path) : err, 3);
}

void App::cmdLoadVosviewer() {
  auto files = openFileDialog(hwnd, "Select VOSviewer map and/or network file(s)", {{"VOSviewer files", "*.txt"}, {"All files", "*.*"}}, true);
  if (files.empty()) return;
  string mapT, netT;
  for (auto& f : files) {
    string t = readFileU(f);
    string first = lower(t.substr(0, t.find('\n')));
    if (contains(first, "label") || contains(first, "x\t") || contains(first, "id\t")) mapT = t; else netT = t;
  }
  string err;
  if (P->loadVosviewer(mapT, netT, &err)) { afterMapChanged(); ui.toast("VOSviewer map loaded", plural(P->net.n(), "item"), 1); }
  else ui.toast("Could not load", err, 3, 7);
}

void App::cmdExportVosviewer() {
  if (!hasMap()) return;
  string path = saveFileDialog(hwnd, "Export VOSviewer map file (the network file is written next to it)", {{"VOSviewer map", "*.txt"}}, "map.txt", "txt");
  if (path.empty()) return;
  string netPath = path.substr(0, path.size() - 4) + "_network.txt";
  bool ok = writeFileU(path, writeVosMap(P->net)) && writeFileU(netPath, writeVosNetwork(P->net));
  if (ok) ui.toast("VOSviewer files written", fileName(path) + " + " + fileName(netPath), 1, 4);
  else ui.toast("Export failed", "", 3);
}

void App::cmdExportItemsCsv() {
  if (!hasMap()) return;
  string path = saveFileDialog(hwnd, "Export items", {{"CSV (UTF-8)", "*.csv"}}, "items.csv", "csv");
  if (path.empty()) return;
  const Network& N = P->net;
  const ItemMetrics& M = P->itemMetricsCached();
  auto q = [](const string& s) { return "\"" + replaceAll(s, "\"", "\"\"") + "\""; };
  string o = "\xEF\xBB\xBFid,label,cluster,cluster_name,x,y";
  for (auto& w : N.weightNames) o += "," + q(w);
  for (auto& sname : N.scoreNames) o += "," + q(sname);
  o += ",degree,betweenness,participation,role\n";
  for (int i = 0; i < N.n(); i++) {
    const Node& nd = N.nodes[size_t(i)];
    o += q(nd.id) + "," + q(nd.label) + "," + std::to_string(nd.cluster + 1) + "," + q(N.clusterName(nd.cluster)) + "," + fmtFixed(nd.x, 3) + "," + fmtFixed(nd.y, 3);
    for (size_t k = 0; k < N.weightNames.size(); k++) o += "," + (k < nd.w.size() ? fmtNum(nd.w[k], 3) : string());
    for (size_t k = 0; k < N.scoreNames.size(); k++) o += "," + (k < nd.sc.size() && std::isfinite(nd.sc[k]) ? fmtNum(nd.sc[k], 3) : string());
    o += "," + fmtNum(M.degree[size_t(i)], 0) + "," + fmtNum(M.betweenness[size_t(i)], 4) + "," + fmtNum(M.participation[size_t(i)], 3) + "," + q(M.role[size_t(i)]) + "\n";
  }
  if (writeFileU(path, o)) ui.toast("Items exported", fileName(path) + " · " + plural(N.n(), "row"), 1);
}

// OpenAlex query terms: separated by ';' or new lines (a term may contain spaces, quotes and AND/OR/NOT)
vector<string> openAlexTerms(const string& q) {
  vector<string> out;
  for (auto& t : splitAny(q, ";\n")) {
    string u = trim(t);
    if (!u.empty() && std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
  }
  return out;
}

// OpenAlex: search → pages of works → records; the most-cited references are resolved into WoS-style strings.
// Several terms: "Any" runs one search per term and merges the results (duplicates removed by OpenAlex id);
// "All" combines the terms with AND in a single search.
void App::cmdOpenAlex() {
  const int purpose = oaPurpose;  // 1: check the saved search for new papers (living map)
  oaPurpose = 0;
  bool semantic = oaKind == 1;
  vector<string> semQueries;
  if (semantic) {
    for (auto& sq : oaSemQueries) { string t = trim(sq); if (!t.empty()) semQueries.push_back(t.size() > 1900 ? t.substr(0, 1900) : t); }
    if (semQueries.empty()) { ui.toast("Semantic search", "Describe the topic in a sentence or two.", 2); return; }
  }
  string q = semantic ? join(semQueries, "\n") : trim(oaQuery);
  if (q.empty()) { ui.toast("OpenAlex", "Type one or more search terms (separate them with ;) or paste DOIs.", 2); return; }
  int from = toInt(oaFrom, 0), to = toInt(oaTo, 0), mx = clampv(toInt(oaMax, 500), 10, 10000);
  auto recs = std::make_shared<vector<Record>>();
  auto raw = std::make_shared<Json>(Json::array());
  auto counts = std::make_shared<vector<OaTermCount>>();
  auto resolved = std::make_shared<int>(0);
  auto enc = [](const string& s) {
    string o;
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
      else { char b[4]; snprintf(b, 4, "%%%02X", c); o += b; }
    }
    return o;
  };
  string key = openAlexKey(), mail = trim(settings.j["openalexEmail"].str());
  const string auth = !key.empty() ? "&api_key=" + enc(key) : "&mailto=" + enc(mail.empty() ? string("vosstudio@example.org") : mail);
  int strategy = oaSemStrategy;
  double minSim = oaStrict == 0 ? 0.05 : oaStrict == 2 ? 0.18 : 0.10;
  auto semInfo = std::make_shared<OaSemInfo>();
  vector<string> dois;
  if (!semantic)
  for (auto& tok : splitAny(q, " ,;\n\t")) if (startsWith(lower(tok), "10.") || contains(lower(tok), "doi.org/10.")) dois.push_back(normDoi(tok));
  vector<string> terms = openAlexTerms(q);
  if (terms.size() > 1 && oaMode == 1) {
    string all;
    for (auto& t : terms) all += (all.empty() ? "" : " AND ") + string("(") + t + ")";
    terms = {all};
  }
  int field = oaField;
  startJob("Fetching from OpenAlex", [=](Job& j) {
    const string sel = "id,doi,display_name,publication_year,authorships,primary_location,keywords,concepts,topics,abstract_inverted_index,cited_by_count,type,language,biblio,referenced_works";
    Json all = Json::array();
    auto get = [&](const string& url) {
      int st = 0;
      string err;
      string body = httpGet(url, &st, &err);
      if (st == 401 || st == 403) throw std::runtime_error("OpenAlex did not accept the API key. Check it in Settings.");
      if (st == 429) throw std::runtime_error("OpenAlex rate limit or daily allowance reached. Try again later.");
      if (st != 200) throw std::runtime_error("OpenAlex request failed (" + (err.empty() ? "HTTP " + std::to_string(st) : err) + ")");
      string e;
      Json js = Json::parse(body, &e);
      if (!e.empty()) throw std::runtime_error("Invalid JSON from OpenAlex");
      return js;
    };
    if (!dois.empty()) {
      for (size_t i = 0; i < dois.size(); i += 50) {
        string f;
        for (size_t k = i; k < std::min(dois.size(), i + 50); k++) f += (k > i ? "|" : "") + string("https://doi.org/") + dois[k];
        Json js = get("https://api.openalex.org/works?filter=doi:" + enc(f) + "&per-page=50&select=" + sel + auth);
        for (auto& w : js["results"].a) all.push(w);
        j.progress = 0.6 * double(i + 50) / dois.size();
        if (j.cancel) throw std::runtime_error("Cancelled.");
      }
    } else if (semantic) {
      // ---- semantic search: OpenAlex embeds each query and returns the closest works (at most 50 per query,
      // one request per second)
      string filt;
      if (from) filt += "from_publication_date:" + std::to_string(from) + "-01-01";
      if (to) filt += string(filt.empty() ? "" : ",") + "to_publication_date:" + std::to_string(to) + "-12-31";
      std::unordered_map<string, size_t> byId;
      double lastCall = -10;
      auto throttle = [&]() {
        double wait = lastCall + 1.05 - nowSeconds();
        if (wait > 0) Sleep(DWORD(wait * 1000));
        lastCall = nowSeconds();
      };
      auto addWork = [&](const Json& w, const string& tag) {
        string id = w["id"].str();
        auto it = byId.find(id);
        if (it != byId.end()) { for (auto& kv : all.a[it->second].o) if (kv.first == "vosstudio_matched_terms") kv.second.push(tag); return false; }
        byId[id] = all.a.size();
        Json ww = w;
        Json mt = Json::array();
        mt.push(tag);
        ww.set("vosstudio_matched_terms", mt);
        all.push(ww);
        return true;
      };
      double semShare = strategy == 1 ? 0.15 : 0.6;
      for (size_t qi = 0; qi < semQueries.size(); qi++) {
        throttle();
        string url = "https://api.openalex.org/works?search.semantic=" + enc(semQueries[qi]) + "&per-page=50&select=" + sel + auth;
        if (!filt.empty()) url += "&filter=" + enc(filt);
        Json js = get(url);
        string tag = semQueries[qi].size() > 60 ? semQueries[qi].substr(0, 57) + "..." : semQueries[qi];
        int got = 0, added = 0;
        for (auto& w : js["results"].a) { got++; if (addWork(w, tag)) added++; }
        counts->push_back({tag, got, got, added});
        j.progress = semShare * double(qi + 1) / double(semQueries.size());
        if (j.cancel) throw std::runtime_error("Cancelled.");
      }
      if (strategy == 1 && !all.a.empty()) {
        // ---- expand: references of the seeds and works citing them, then keep what reads like the seeds
        size_t nSeeds = all.a.size();
        int cap = std::max(300, mx);  // per direction
        std::map<string, int> refFreq;
        for (size_t i = 0; i < nSeeds; i++) for (auto& r : all.a[i]["referenced_works"].a) refFreq[r.str()]++;
        vector<std::pair<string, int>> refs(refFreq.begin(), refFreq.end());
        std::stable_sort(refs.begin(), refs.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (int(refs.size()) > cap) refs.resize(size_t(cap));
        for (size_t i = 0; i < refs.size(); i += 50) {
          string f;
          for (size_t k = i; k < std::min(refs.size(), i + 50); k++) { const string& id = refs[k].first; size_t sl = id.rfind('/'); f += (k > i ? "|" : "") + (sl == string::npos ? id : id.substr(sl + 1)); }
          string ff = "openalex:" + f + (filt.empty() ? "" : "," + filt);
          Json js = get("https://api.openalex.org/works?filter=" + enc(ff) + "&per-page=50&select=" + sel + auth);
          for (auto& w : js["results"].a) addWork(w, "reference of a seed");
          j.progress = 0.15 + 0.2 * double(i + 50) / double(std::max<size_t>(1, refs.size()));
          if (j.cancel) throw std::runtime_error("Cancelled.");
        }
        int citing = 0;
        for (size_t i = 0; i < nSeeds && citing < cap; i += 50) {
          string f;
          for (size_t k = i; k < std::min(nSeeds, i + 50); k++) { string id = all.a[k]["id"].str(); size_t sl = id.rfind('/'); f += (k > i ? "|" : "") + (sl == string::npos ? id : id.substr(sl + 1)); }
          string ff = "cites:" + f + (filt.empty() ? "" : "," + filt);
          string cursor = "*";
          while (!cursor.empty() && citing < cap) {
            Json js = get("https://api.openalex.org/works?filter=" + enc(ff) + "&sort=cited_by_count:desc&per-page=200&cursor=" + enc(cursor) + "&select=" + sel + auth);
            for (auto& w : js["results"].a) { if (citing >= cap) break; citing++; addWork(w, "cites a seed"); }
            cursor = js["meta"]["next_cursor"].str();
            j.progress = 0.35 + 0.2 * double(citing) / double(cap);
            if (js["results"].a.empty() || j.cancel) break;
          }
          if (j.cancel) throw std::runtime_error("Cancelled.");
        }
        // ---- re-rank by TF-IDF similarity to the seeds (title, abstract, keywords)
        vector<Record> cand;
        vector<char> isSeed;
        cand.reserve(all.a.size());
        for (size_t i = 0; i < all.a.size(); i++) {
          const Json& w = all.a[i];
          Record r;
          r.title = w["display_name"].str();
          const Json& inv = w["abstract_inverted_index"];
          if (inv.t == Json::Obj) {
            vector<std::pair<int, string>> pos;
            for (auto& kv : inv.o) for (auto& pp : kv.second.a) pos.push_back({int(pp.integer()), kv.first});
            std::sort(pos.begin(), pos.end());
            for (auto& pw : pos) { r.abstract_ += pw.second; r.abstract_ += ' '; }
          }
          for (auto& kw : w["keywords"].a) r.keywords.push_back(kw["display_name"].str());
          cand.push_back(std::move(r));
          isSeed.push_back(i < nSeeds ? 1 : 0);
        }
        RerankResult rr = rerankCandidates(cand, isSeed, std::max(mx, int(nSeeds)), minSim);
        Json kept = Json::array();
        for (int k : rr.keep) {
          Json w = all.a[size_t(k)];
          w.set("vosstudio_similarity", std::round(rr.score[size_t(k)] * 1000) / 1000);
          w.set("vosstudio_seed", bool(isSeed[size_t(k)]));
          kept.push(std::move(w));
        }
        semInfo->seeds = int(nSeeds);
        semInfo->cands = int(all.a.size());
        semInfo->kept = int(kept.a.size());
        semInfo->cutoff = rr.cutoff;
        semInfo->valid = true;
        all = std::move(kept);
        j.progress = 0.6;
      }
    } else {
      std::unordered_map<string, size_t> byId;  // OpenAlex id -> index in `all`
      for (size_t ti = 0; ti < terms.size(); ti++) {
        const string& term = terms[ti];
        string filt;
        if (from) filt += "from_publication_date:" + std::to_string(from) + "-01-01";
        if (to) filt += string(filt.empty() ? "" : ",") + "to_publication_date:" + std::to_string(to) + "-12-31";
        string searchParam;
        if (field == 0) searchParam = "&search=" + enc(term);
        else filt += string(filt.empty() ? "" : ",") + (field == 1 ? "title_and_abstract.search:" : "title.search:") + replaceAll(term, ",", " ");
        string cursor = "*";
        int total = 0, got = 0, added = 0;
        while (!cursor.empty() && got < mx) {
          string url = "https://api.openalex.org/works?per-page=200&cursor=" + enc(cursor) + searchParam + "&select=" + sel + auth;
          if (!filt.empty()) url += "&filter=" + enc(filt);
          Json js = get(url);
          total = js["meta"]["count"].integer();
          for (auto& w : js["results"].a) {
            if (got >= mx) break;
            got++;
            string id = w["id"].str();
            auto it = byId.find(id);
            if (it == byId.end()) {
              byId[id] = all.a.size();
              Json ww = w;
              Json mt = Json::array();
              mt.push(term);
              ww.set("vosstudio_matched_terms", mt);
              all.push(ww);
              added++;
            } else {
              for (auto& kv : all.a[it->second].o) if (kv.first == "vosstudio_matched_terms") kv.second.push(term);
            }
          }
          cursor = js["meta"]["next_cursor"].str();
          j.progress = 0.6 * (double(ti) + double(got) / std::max(1, std::min(mx, total))) / double(terms.size());
          if (js["results"].a.empty() || j.cancel) break;
        }
        counts->push_back({term, total, got, added});
        if (j.cancel) throw std::runtime_error("Cancelled.");
      }
    }
    if (all.a.empty() && purpose != 1) throw std::runtime_error("No works matched.");
    // resolve the most frequently cited references (≥2 citing works, top 400)
    std::map<string, int> refCount;
    for (auto& w : all.a) for (auto& r : w["referenced_works"].a) refCount[r.str()]++;
    vector<std::pair<string, int>> top(refCount.begin(), refCount.end());
    top.erase(std::remove_if(top.begin(), top.end(), [](auto& p) { return p.second < 2; }), top.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (top.size() > 400) top.resize(400);
    std::map<string, string> label;
    for (size_t i = 0; i < top.size(); i += 50) {
      string f;
      for (size_t k = i; k < std::min(top.size(), i + 50); k++) { string id = top[k].first; size_t sl = id.rfind('/'); f += (k > i ? "|" : "") + (sl == string::npos ? id : id.substr(sl + 1)); }
      try {
        Json js = get("https://api.openalex.org/works?filter=openalex:" + enc(f) + "&per-page=50&select=id,publication_year,authorships,primary_location,display_name" + auth);
        for (auto& r : js["results"].a) {
          string a = r["authorships"][0]["author"]["display_name"].str("Anonymous");
          auto parts = split(a, ' ');
          string sur = parts.empty() ? a : parts.back();
          string ini = parts.size() > 1 ? string(1, parts[0][0]) : "";
          string src = upper(r["primary_location"]["source"]["display_name"].str(r["display_name"].str()));
          src = replaceAll(src, ",", " ");
          if (src.size() > 80) src = src.substr(0, 80);
          label[r["id"].str()] = sur + " " + ini + ", " + std::to_string(r["publication_year"].integer()) + ", " + src;
        }
      } catch (...) {}
      j.progress = 0.6 + 0.35 * double(i + 50) / std::max<size_t>(1, top.size());
      if (j.cancel) break;
    }
    *resolved = int(label.size());
    parseOpenAlex(all, *recs);
    for (auto& r : *recs) {
      vector<string> keep;
      for (auto& ref : r.refs) {
        string id = ref.substr(ref.find(':') == string::npos ? 0 : ref.find(':') + 1);
        auto it = label.find(id);
        if (it != label.end()) keep.push_back(it->second);
      }
      r.refs = keep;
    }
    *raw = std::move(all);
  }, [this, recs, raw, counts, resolved, q, dois, semantic, semInfo, purpose] {
    if (purpose == 1) { livingDone(*recs); return; }
    if (!oaAppend) { P->clearCorpus(); }
    int n = int(recs->size());
    oaLastRecs = *recs;
    oaLastRaw = raw;
    oaCounts = *counts;
    string label = dois.empty() ? q : std::to_string(dois.size()) + " DOIs";
    if (semantic) label = "semantic: " + label;
    oaSemInfo = *semInfo;
    oaLastLabel = label;
    P->addRecords("OpenAlex: " + replaceAll(label, "\n", "; "), BibFormat::OpenAlex, *recs);
    if (dois.empty()) livingRemember(label);
    livingValid = false;
    pageStateReset();
    string per;
    if (counts->size() > 1) {
      int dupl = 0;
      for (auto& c : *counts) dupl += c.fetched - c.added;
      per = " \xC2\xB7 " + std::to_string(counts->size()) + " terms merged, " + plural(dupl, "duplicate") + " removed";
    }
    if (semInfo->valid)
      per = " \xC2\xB7 " + std::to_string(semInfo->seeds) + " search results expanded to " + fmtInt(semInfo->cands) + " related works, " + fmtInt(semInfo->kept) + " kept";
    ui.toast("OpenAlex", plural(n, "work") + " imported \xC2\xB7 " + fmtInt(*resolved) + " cited references resolved" + per, 1, 6);
    if (!hasMap() && !agent.active) page = PG_BUILD;  // an agent run keeps the Assistant in front
  });
}

// export the last OpenAlex fetch (or all loaded records): WoS text / RIS / Scopus-style CSV / raw OpenAlex JSON
void App::cmdExportRecords(bool lastFetch, const string& fmt) {
  const vector<Record>& recs = lastFetch ? oaLastRecs : P->corpus.recs;
  if (recs.empty()) { ui.toast("Export", "No records to export.", 2); return; }
  string base = lastFetch ? "openalex" : "records";
  string path;
  string body;
  if (fmt == "json") {
    if (!oaLastRaw) { ui.toast("Export", "Raw OpenAlex JSON is only kept for the most recent fetch.", 2); return; }
    path = saveFileDialog(hwnd, "Export OpenAlex works (JSON)", {{"JSON", "*.json"}}, base + ".json", "json");
    if (path.empty()) return;
    Json root = Json::object();
    root.set("query", oaLastLabel);
    root.set("source", "OpenAlex API via VOSStudio");
    root.set("count", int(oaLastRaw->a.size()));
    root.set("results", *oaLastRaw);
    body = root.dump(1);
  } else if (fmt == "wos") {
    path = saveFileDialog(hwnd, "Export records (Web of Science tagged text)", {{"Web of Science plain text", "*.txt"}}, base + "_wos.txt", "txt");
    if (path.empty()) return;
    body = writeRecords(recs, RecordExport::WoS);
  } else if (fmt == "ris") {
    path = saveFileDialog(hwnd, "Export records (RIS)", {{"RIS", "*.ris"}}, base + ".ris", "ris");
    if (path.empty()) return;
    body = writeRecords(recs, RecordExport::RIS);
  } else if (fmt == "bib") {
    path = saveFileDialog(hwnd, "Export records (BibTeX)", {{"BibTeX", "*.bib"}}, base + ".bib", "bib");
    if (path.empty()) return;
    body = writeRecords(recs, RecordExport::BibTeX);
  } else {
    path = saveFileDialog(hwnd, "Export records (CSV)", {{"CSV (Scopus columns)", "*.csv"}}, base + ".csv", "csv");
    if (path.empty()) return;
    body = "\xEF\xBB\xBF" + writeRecords(recs, RecordExport::CSV);
  }
  if (writeFileU(path, body)) ui.toast("Exported", plural(long(recs.size()), "record") + " \xE2\x86\x92 " + fileName(path), 1);
  else ui.toast("Export failed", "Could not write " + fileName(path), 3);
}

// =====================================================================================
// view positions, selection, search
// =====================================================================================
vector<std::array<float, 3>> App::targetPositions(ViewKind v) {
  const Network& N = P->net;
  vector<std::array<float, 3>> p(static_cast<size_t>(N.n()));
  for (int i = 0; i < N.n(); i++) p[size_t(i)] = {float(N.nodes[size_t(i)].x), float(N.nodes[size_t(i)].y), 0.f};
  if (v == ViewKind::Timeline) {
    int si = N.scoreIndex("Avg. pub. year");
    if (si < 0) si = N.scoreIdx;
    double s0 = 1e18, s1 = -1e18, x0 = 1e18, x1 = -1e18;
    for (int i = 0; i < N.n(); i++) {
      x0 = std::min(x0, N.nodes[size_t(i)].x); x1 = std::max(x1, N.nodes[size_t(i)].x);
      double sv = si >= 0 && size_t(si) < N.nodes[size_t(i)].sc.size() ? N.nodes[size_t(i)].sc[size_t(si)] : NAN;
      if (std::isfinite(sv)) { s0 = std::min(s0, sv); s1 = std::max(s1, sv); }
    }
    if (s1 > s0) {
      double span = std::max(200.0, (x1 - x0) * 1.25);
      tlS0 = s0; tlS1 = s1; tlX0 = -span / 2; tlX1 = span / 2;
      for (int i = 0; i < N.n(); i++) {
        double sv = si >= 0 && size_t(si) < N.nodes[size_t(i)].sc.size() ? N.nodes[size_t(i)].sc[size_t(si)] : NAN;
        if (std::isfinite(sv)) p[size_t(i)][0] = float(tlX0 + (sv - s0) / (s1 - s0) * span);
      }
    }
  } else if (v == ViewKind::Geo) {
    geoUpdate();
    for (int i = 0; i < N.n() && i < int(geoNodeCountry.size()); i++) {
      int c = geoNodeCountry[size_t(i)];
      if (c < 0) continue;
      float x, y;
      geoProject(worldCountry(c).labelLon, worldCountry(c).labelLat, x, y);
      p[size_t(i)] = {x, y, 0.f};
    }
  } else if (v == ViewKind::ThreeD) {
    if (pos3dValid && pos3d.size() == p.size()) {
      for (size_t i = 0; i < p.size(); i++) p[i] = {pos3d[i][0], pos3d[i][1], pos3d[i][2] * P->style.zScale};
    }
  }
  return p;
}

void App::start3DLayout() {
  if (!hasMap() || pos3dValid) return;
  auto copy = std::make_shared<Network>(P->net);
  BuildParams bp = P->params;
  if (busy()) return;
  startJob("3D layout", [copy, bp](Job& j) {
    LayoutOpts o = bp.layout;
    o.threeD = true;
    normalise(*copy, bp.norm);
    vosLayout(*copy, o, [&j](double x) { j.progress = x; }, &j.cancel);
    worldScale(*copy);
  }, [this, copy] {
    if (copy->n() != P->net.n()) return;
    pos3d.resize(size_t(copy->n()));
    for (int i = 0; i < copy->n(); i++) pos3d[size_t(i)] = {float(copy->nodes[size_t(i)].x), float(copy->nodes[size_t(i)].y), float(copy->nodes[size_t(i)].z)};
    pos3dValid = true;
    if (view == ViewKind::ThreeD) setView(ViewKind::ThreeD, true);
  });
}

void App::setView(ViewKind v, bool animate) {
  if (!hasMap()) { view = v; return; }
  if (v == ViewKind::Geo && !geoAvailable()) { ui.toast("Geo view", "No country information: the records need affiliations with countries (or build a map of countries).", 2, 5); return; }
  geoFitBounds(v);
  if (v != ViewKind::Geo) geoSel = -1;
  mainChartOpen = false;
  papersOpen = false;  // choosing a map view shows the map
  ViewKind old = view;
  view = v;
  nv.kind = v;
  if (old != v) nv.visibilityChanged();
  if (v == ViewKind::ThreeD && !pos3dValid) start3DLayout();
  if (v == ViewKind::Matrix) { animT0 = -1; camTargetZoom = -1; return; }
  auto target = targetPositions(v);
  bool geoSwitch = (old == ViewKind::Geo) != (v == ViewKind::Geo);
  bool samePositions = nv.pos.size() == target.size();
  if (samePositions) {
    for (size_t i = 0; i < target.size() && samePositions; i++)
      for (int d = 0; d < 3; d++) if (nv.pos[i][d] != target[i][d]) { samePositions = false; break; }
  }
  if (animate && nv.pos.size() == target.size() && old != ViewKind::Matrix) {
    if (!samePositions) {
      animFrom = nv.pos;
      animTo = target;
      animT0 = nowSeconds();
    } else animT0 = -1;  // camera can still glide, but don't animate/invalidate unchanged positions
    // camera: fit the destination layout and glide there
    auto save = nv.pos;
    nv.pos = target;
    Camera c0 = nv.cam;
    nv.fit();
    Camera c1 = nv.cam;
    nv.cam = c0;
    nv.pos = save;
    if (v == ViewKind::ThreeD || old == ViewKind::ThreeD) { nv.cam = c1; camTargetZoom = -1; }
    else { camTargetZoom = c1.zoom; camTargetX = c1.x; camTargetY = c1.y; }
    if (geoSwitch) { nv.cam = c1; camTargetZoom = -1; }
  } else {
    animT0 = -1;  // cancel an earlier transition when this target already matches or animation was disabled
    nv.pos = target;
    nv.fit();
    camTargetZoom = -1;
  }
  nv.positionsChanged();
  nv.nodesDirty = nv.linksDirty = true;
}

// The flags of a view depend on selection/search/pin/filter revisions, the hovered item and the active view.
// Map-sized node content changes invalidate explicitly (map rebuild, recluster, pin edit or geo-index rebuild).
void App::invalidateFlags() const {
  ++flagsInputVersion_;
  flagsSig_ = 0;
}

uint64_t App::flagsSignature(ViewKind vk, int hoverIdx) const {
  const Network& N = P->net;
  int gm = vk == ViewKind::Geo ? geoMode() : 0;  // geoUpdate may bump the input revision
  SigHash h;
  h.v(reinterpret_cast<uintptr_t>(P.get())); h.v(N.n()); h.v(N.links.size());
  h.v(reinterpret_cast<uintptr_t>(N.nodes.data())); h.v(reinterpret_cast<uintptr_t>(N.links.data())); h.v(N.nClusters);
  h.v(hoverIdx); h.v(clusterFilter); h.v(int(vk)); h.v(flagsInputVersion_);
  h.v(selection.size()); h.v(reinterpret_cast<uintptr_t>(selection.data()));
  h.v(searchHits.size()); h.v(reinterpret_cast<uintptr_t>(searchHits.data()));
  h.v(gm); h.v(reinterpret_cast<uintptr_t>(geoNodeCountry.data())); h.v(geoNodeCountry.size());
  if (vk == ViewKind::Timeline) {
    int si = N.scoreIndex("Avg. pub. year");
    if (si < 0) si = N.scoreIdx;
    h.v(si);
  }
  return h.h ? h.h : 1;
}

void App::computeFlags(ViewKind vk, int hoverIdx, vector<uint32_t>& f) const {
  const Network& N = P->net;
  size_t n = size_t(N.n());
  f.assign(n, 0);
  std::unordered_set<int> sel(selection.begin(), selection.end());
  std::unordered_set<int> nb;
  if (!sel.empty())
    for (auto& l : N.links) {
      if (sel.count(l.a)) nb.insert(l.b);
      if (sel.count(l.b)) nb.insert(l.a);
    }
  std::unordered_set<int> hits(searchHits.begin(), searchHits.end());
  int si = N.scoreIndex("Avg. pub. year");
  int gmode = vk == ViewKind::Geo ? geoMode() : 0;
  if (si < 0) si = N.scoreIdx;
  for (size_t i = 0; i < n; i++) {
    uint32_t x = 0;
    if (sel.count(int(i))) x |= NF_SELECTED;
    else if (nb.count(int(i))) x |= NF_NEIGHBOUR;
    if (int(i) == hoverIdx) x |= NF_HOVER;
    if (hits.count(int(i))) x |= NF_MATCH;
    if (N.nodes[i].pinned) x |= NF_PINNED;
    bool dim = false;
    if (!sel.empty() && !(x & (NF_SELECTED | NF_NEIGHBOUR))) dim = true;
    if (clusterFilter >= 0 && N.nodes[i].cluster != clusterFilter && !(x & NF_SELECTED)) dim = true;
    if (!hits.empty() && sel.empty() && !(x & NF_MATCH)) dim = true;
    if (dim) x |= NF_DIM;
    if (gmode == 2 || (gmode == 1 && (i >= geoNodeCountry.size() || geoNodeCountry[i] < 0))) x |= NF_HIDDEN;
    if (vk == ViewKind::Timeline && !(si >= 0 && size_t(si) < N.nodes[i].sc.size() && std::isfinite(N.nodes[i].sc[size_t(si)]))) x |= NF_HIDDEN;
    f[i] = x;
  }
}

void App::updateFlags() {
  const size_t n = size_t(P->net.n());
  const int hv = hover >= 0 ? hover : liveHover();
  uint64_t h = flagsSignature(view, hv);
  if (h == flagsSig_ && nv.flags.size() == n) return;  // nothing that feeds the flags changed (this runs every frame)
  flagsSig_ = h;
  vector<uint32_t> f;
  computeFlags(view, hv, f);
  if (f != nv.flags) {
    if (view == ViewKind::Geo || view == ViewKind::Timeline) nv.visibilityChanged();
    nv.flags.swap(f); nv.nodesDirty = nv.linksDirty = true;
  }
}

void App::selectNode(int i, bool add) {
  if (i < 0) return;
  bool changed = false;
  if (add) {
    auto it = std::find(selection.begin(), selection.end(), i);
    if (it != selection.end()) { selection.erase(it); changed = true; }
    else { selection.push_back(i); changed = true; }
  } else if (selection.size() != 1 || selection[0] != i) { selection = {i}; changed = true; }
  if (changed) invalidateFlags();
  if (!inspectorOpen) inspectorOpen = true;
  closeDoc();  // a node selection replaces an open document preview
}

void App::clearSelection() {
  if (!selection.empty()) { selection.clear(); invalidateFlags(); }
  closeDoc();
}

void App::focusOn(int i) {
  if (!hasMap() || i < 0 || i >= P->net.n()) return;
  selectNode(i);
  if (view == ViewKind::ThreeD || view == ViewKind::Matrix) return;
  camTargetX = nv.pos[size_t(i)][0];
  camTargetY = nv.pos[size_t(i)][1];
  camTargetZoom = std::max(nv.cam.zoom, nv.fitZoom() * 2.4);
}

void App::focusCluster(int c) {
  if (!hasMap()) return;
  clusterFilter = c;
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  for (int i = 0; i < P->net.n(); i++) {
    if (P->net.nodes[size_t(i)].cluster != c) continue;
    x0 = std::min(x0, double(nv.pos[size_t(i)][0])); x1 = std::max(x1, double(nv.pos[size_t(i)][0]));
    y0 = std::min(y0, double(nv.pos[size_t(i)][1])); y1 = std::max(y1, double(nv.pos[size_t(i)][1]));
  }
  if (x0 > x1 || view == ViewKind::ThreeD) return;
  camTargetX = (x0 + x1) / 2;
  camTargetY = (y0 + y1) / 2;
  camTargetZoom = clampv(std::min((canvasR.w - 220 * ui.s) / std::max(1.0, x1 - x0), (canvasR.h - 160 * ui.s) / std::max(1.0, y1 - y0)), nv.fitZoom() * 0.8, nv.fitZoom() * 8);
}

// =====================================================================================
// linked selection: the highlighted cluster / selected items scope the analyses
// =====================================================================================
bool App::scopeActive() {
  auto drop = [&]() {
    if (scopeKey_.empty() && scopeRecs_.empty()) return;
    scopeKey_.clear(); scopeRecs_.clear(); scopeLabel_.clear(); scopeC_ = Corpus();
    scopeVer_++;
    scopeChanged();
  };
  if (!linkScope || !P || !hasMap() || !hasCorpus() || (clusterFilter < 0 && selection.empty())) { drop(); return false; }
  string key = std::to_string(reinterpret_cast<uintptr_t>(P.get())) + "|" + std::to_string(P->corpusVersion) + "|" + P->builtSig + "|" + std::to_string(P->net.n()) + "|c" + std::to_string(clusterFilter);
  if (clusterFilter < 0) { key += "|s"; for (int i : selection) key += "," + std::to_string(i); }
  if (key == scopeKey_) return !scopeRecs_.empty();
  scopeKey_ = key;
  scopeRecs_.clear();
  scopeLabel_.clear();
  scopeC_ = Corpus();
  const Network& N = P->net;
  size_t total = P->corpus.recs.size();
  vector<char> mark(total, 0);
  bool anyLinks = false;
  if (clusterFilter >= 0) {
    for (auto& nd : N.nodes) {
      if (nd.cluster != clusterFilter) continue;
      if (!nd.recs.empty()) anyLinks = true;
      for (int r : nd.recs) if (r >= 0 && size_t(r) < total) mark[size_t(r)] = 1;
    }
    string nm = N.clusterName(clusterFilter);
    scopeLabel_ = "Cluster " + std::to_string(clusterFilter + 1) + (nm.empty() || startsWith(lower(nm), "cluster ") ? string() : ": " + nm);
  } else {
    for (int i : selection) {
      if (i < 0 || i >= N.n()) continue;
      if (!N.nodes[size_t(i)].recs.empty()) anyLinks = true;
      for (int r : N.nodes[size_t(i)].recs) if (r >= 0 && size_t(r) < total) mark[size_t(r)] = 1;
    }
    scopeLabel_ = selection.size() == 1 && selection[0] >= 0 && selection[0] < N.n() ? "\xE2\x80\x9C" + N.nodes[size_t(selection[0])].label + "\xE2\x80\x9D" : plural(long(selection.size()), "selected item");
  }
  for (size_t r = 0; r < total; r++) if (mark[r]) scopeRecs_.push_back(int(r));
  if (!anyLinks || scopeRecs_.empty() || scopeRecs_.size() == total) {
    // nothing to narrow (no record links, or the scope covers everything): the pages show the whole corpus
    if (!anyLinks) scopeLabel_ += " (this map has no record links)";
    scopeRecs_.clear();
  } else {
    scopeC_.files = P->corpus.files;
    scopeC_.format = P->corpus.format;
    scopeC_.recs.reserve(scopeRecs_.size());
    for (int r : scopeRecs_) scopeC_.recs.push_back(P->corpus.recs[size_t(r)]);
  }
  scopeVer_++;
  scopeChanged();
  return !scopeRecs_.empty();
}

void App::scopeChanged() {
  burstsValid = evoValid = cmpValid = tfValid = collabValid = rpValid = ttValid = false;
  actorsUnit = -1;
  prodUnit = -1;
  papersBuiltKey.clear();
  needFrame = true;
}

void App::clearScope() {
  clusterFilter = -1;
  if (!selection.empty()) clearSelection();
  needFrame = true;
}

void App::drawScopeBanner(Lay& L) {
  float s = ui.s;
  bool any = hasMap() && (clusterFilter >= 0 || !selection.empty());
  if (!any || !hasCorpus()) return;
  bool active = scopeActive();
  Rect r = L.row(30 * s);
  Color c = active ? ui.c.accent : ui.c.textDim;
  ui.fill(r, c.withA(active ? 0.10f : 0.06f), 7 * s);
  ui.stroke(r, c.withA(0.35f), 7 * s);
  ui.icon("link", r.x + 16 * s, r.y + r.h / 2, 14 * s, c);
  string txt;
  if (!linkScope) txt = "Linked selection is off";
  else if (active) txt = scopeLabel_ + " \xC2\xB7 " + fmtInt(long(scopeRecs_.size())) + " of " + fmtInt(long(P->corpus.recs.size())) + " records";
  else txt = scopeLabel_.empty() ? string("Selection covers all records") : scopeLabel_ + " \xC2\xB7 all records";
  float bw = 56 * s;
  ui.pushId("scopebar");
  if (ui.button({r.r() - bw - 4 * s, r.y + 4 * s, bw, r.h - 8 * s}, linkScope ? "Unlink" : "Link", BTN_GHOST)) {
    linkScope = !linkScope;
    settings.j.set("linkScope", linkScope);
    settings.save();
    scopeActive();
  }
  ui.tip(linkScope ? "Stop scoping the charts, papers and geo view to the selection (setting)" : "Scope the charts, papers and geo view to the highlighted cluster or selected items");
  if (ui.button({r.r() - 2 * bw - 8 * s, r.y + 4 * s, bw, r.h - 8 * s}, "Clear", BTN_GHOST)) clearScope();
  ui.tip("Clear the cluster highlight and the selection");
  ui.popId();
  ui.text({r.x + 30 * s, r.y, r.w - 2 * bw - 42 * s, r.h}, txt, 11.5f * s, active ? ui.c.text : ui.c.textDim, AL_LEFT, active ? 600 : 400);
  L.space(4 * s);
}

// =====================================================================================
// compare mode: periods, source files or thresholds; side by side or as a difference map
// =====================================================================================
bool App::compareMasks(vector<char>& inA, vector<char>& inB, string& titleA, string& titleB, string* err) {
  if (!hasCorpus()) { if (err) *err = "Import records first."; return false; }
  const Corpus& C = P->corpus;
  auto per = [](int x, int y) { return x == y ? std::to_string(x) : std::to_string(x) + "\xE2\x80\x93" + std::to_string(y); };
  if (cmpKind == 1) {
    if (C.files.size() < 2) { if (err) *err = "Comparing sources needs at least two imported files."; return false; }
    if (!C.provenanceKnown()) { if (err) *err = "This project does not record which file each record came from (import the files again)."; return false; }
    cmpFileA = clampv(cmpFileA, 0, int(C.files.size()) - 1);
    cmpFileB = clampv(cmpFileB, 0, int(C.files.size()) - 1);
    if (cmpFileA == cmpFileB) { if (err) *err = "Choose two different files."; return false; }
    inA = fileMask(C, size_t(cmpFileA));
    inB = fileMask(C, size_t(cmpFileB));
    titleA = "A: " + fileName(C.files[size_t(cmpFileA)].name);
    titleB = "B: " + fileName(C.files[size_t(cmpFileB)].name);
    return true;
  }
  if (!cmpA0) suggestPeriods(C, cmpA0, cmpA1, cmpB0, cmpB1);
  inA = yearMask(C, cmpA0, cmpA1);
  inB = yearMask(C, cmpB0, cmpB1);
  titleA = "A: " + per(std::min(cmpA0, cmpA1), std::max(cmpA0, cmpA1));
  titleB = "B: " + per(std::min(cmpB0, cmpB1), std::max(cmpB0, cmpB1));
  return true;
}

bool App::compareSides(vector<double>& a, vector<double>& b, double& nA, double& nB, string& titleA, string& titleB, string* err) {
  if (!hasMap()) { if (err) *err = "Build a map first."; return false; }
  const Network& N = P->net;
  a.assign(size_t(N.n()), 0);
  b.assign(size_t(N.n()), 0);
  if (cmpKind == 2) {
    double lo = 1e18;
    for (int i = 0; i < N.n(); i++) lo = std::min(lo, N.weight(i));
    double tA = cmpThA > 0 ? cmpThA : lo, tB = cmpThB > 0 ? cmpThB : lo;
    int kA = 0, kB = 0;
    for (int i = 0; i < N.n(); i++) {
      double w = N.weight(i);
      if (w >= tA) { a[size_t(i)] = w; kA++; }
      if (w >= tB) { b[size_t(i)] = w; kB++; }
    }
    if (!kA || !kB) { if (err) *err = "No item reaches one of the thresholds."; return false; }
    nA = nB = 0;
    string wn = N.weightIdx >= 0 && N.weightIdx < int(N.weightNames.size()) ? lower(N.weightNames[size_t(N.weightIdx)]) : string("weight");
    titleA = "A: " + wn + " \xE2\x89\xA5 " + fmtNum(tA, tA == std::floor(tA) ? 0 : 1) + " \xC2\xB7 " + plural(kA, "item");
    titleB = "B: " + wn + " \xE2\x89\xA5 " + fmtNum(tB, tB == std::floor(tB) ? 0 : 1) + " \xC2\xB7 " + plural(kB, "item");
    return true;
  }
  vector<char> inA, inB;
  if (!compareMasks(inA, inB, titleA, titleB, err)) return false;
  PeriodDiff d = subsetDiff(N, P->corpus, inA, inB);
  if (!d.ok) { if (err) *err = d.error; return false; }
  for (size_t i = 0; i < d.items.size() && i < a.size(); i++) { a[i] = d.items[i].a; b[i] = d.items[i].b; }
  nA = d.nA;
  nB = d.nB;
  titleA += " \xC2\xB7 " + plural(d.nA, "document");
  titleB += " \xC2\xB7 " + plural(d.nB, "document");
  return true;
}

Scene App::compareScene(double w, double h, const ChartTheme* th) {
  vector<double> a, b;
  double nA = 0, nB = 0;
  string ta, tb, err;
  if (!compareSides(a, b, nA, nB, ta, tb, &err)) {
    Scene sc;
    sc.W = w; sc.H = h;
    Prim p;
    p.type = Prim::Text; p.text = err; p.size = 12; p.x = float(w / 2); p.y = float(h / 2); p.anchor = 1; p.fillC = th ? th->fg : Color(0.3f, 0.3f, 0.3f, 1); p.fill = true;
    sc.items.push_back(p);
    return sc;
  }
  FigureSpec sp = P->fig;
  sp.theme = th && !th->light ? FigTheme::Slide : FigTheme::Print;
  sp.title.clear();
  return compareSideBySide(P->net, P->style, sp, &P->bundles, P->methodsShort(), a, b, nA, nB, ta, tb, w, h);
}

string App::compareSummary() {
  vector<double> a, b;
  double nA = 0, nB = 0;
  string ta, tb, err;
  if (!compareSides(a, b, nA, nB, ta, tb, &err)) return err;
  int onlyA = 0, onlyB = 0, both = 0;
  for (size_t i = 0; i < a.size(); i++) { if (a[i] > 0 && b[i] > 0) both++; else if (a[i] > 0) onlyA++; else if (b[i] > 0) onlyB++; }
  return ta + " | " + tb + ": " + std::to_string(both) + " items on both sides, " + std::to_string(onlyA) + " only in A, " + std::to_string(onlyB) + " only in B";
}

bool App::compareShowSideBySide(string* err) {
  vector<double> a, b;
  double nA = 0, nB = 0;
  string ta, tb;
  if (!compareSides(a, b, nA, nB, ta, tb, err)) return false;
  ChartDef def;
  def.title = "Compare \xC2\xB7 " + ta + " vs " + tb;
  def.make = [this](double w, double h, const ChartTheme& t) { return compareScene(w, h, &t); };
  mainChart = def;
  mainChartOpen = true;
  cmpSideOpen = true;
  mainChartSummary = "a side-by-side comparison of the map (" + compareSummary() + ")";
  papersOpen = false;
  figZoomOpen = false;
  chartCache.erase("#main|" + mainChart.title);
  needFrame = true;
  return true;
}

void App::compareClose() {
  if (!cmpSideOpen) return;
  cmpSideOpen = false;
  if (mainChartOpen && startsWith(mainChart.title, "Compare \xC2\xB7 ")) { mainChartOpen = false; mainChart = ChartDef(); mainChartSummary.clear(); }
  needFrame = true;
}

void App::runSearch() {
  searchHits.clear();
  invalidateFlags();
  searchIdx = 0;
  string q = trim(search);
  if (q.empty() || !hasMap()) return;
  vector<std::pair<int, int>> sc;  // (rank, i)
  for (int i = 0; i < P->net.n(); i++) {
    const string& l = P->net.nodes[size_t(i)].label;
    if (iequals(l, q)) sc.push_back({0, i});
    else if (startsWith(lower(l), lower(q))) sc.push_back({1, i});
    else if (icontains(l, q)) sc.push_back({2, i});
  }
  std::stable_sort(sc.begin(), sc.end(), [&](auto& a, auto& b) { return a.first != b.first ? a.first < b.first : P->net.weight(a.second) > P->net.weight(b.second); });
  for (auto& p : sc) searchHits.push_back(p.second);
}

vector<Color> App::clusterColors() const {
  vector<Color> v;
  if (!hasMap()) return v;
  for (int c = 0; c < std::max(1, P->net.nClusters); c++) v.push_back(nv.enc().clusterColor(c));
  return v;
}

// =====================================================================================
// chrome: top bar, rail, left panel, status bar
// =====================================================================================
void App::drawCaption() {
  float s = ui.s;
  bool zoomed = IsZoomed(hwnd) != 0;
  const char* icons[3] = {"win-min", zoomed ? "win-restore" : "win-max", "win-close"};
  const char* tips[3] = {"Minimise", zoomed ? "Restore Down" : "Maximise", "Close"};
  for (int i = 0; i < 3; i++) {
    Rect r = captionBtn(i);
    bool hov = false, held = false, clicked = false;
    if (i == 1) {  // handled by the system (HTMAXBUTTON) so snap layouts work; the app only paints it
      hov = input.mx >= r.x && input.mx < r.r() && input.my >= r.y && input.my < r.b();
      held = maxBtnDown && hov;
      ui.addHitRect(r);
    } else {
      clicked = ui.behave(ui.id(string("caption:") + icons[i]), r, &hov, &held);
    }
    Color fg = ui.c.text.withA(0.85f);
    if (i == 2 && (hov || held)) {
      ui.fill(r, held ? Color::hex(0x94281c) : Color::hex(0xc42b1c));
      fg = Color::hex(0xffffff);
    } else if (hov || held) {
      ui.fill(r, held ? ui.c.hover.withA(std::min(1.f, ui.c.hover.a * 1.8f)) : ui.c.hover);
      fg = ui.c.text;
    }
    ui.icon(icons[i], r.x + r.w / 2, r.y + r.h / 2, 20 * s, fg, 1.05f);  // 10 px glyph, 1 px line (icons live on a 24-unit grid)
    if (i != 1) ui.tipFor(ui.id(string("caption:") + icons[i]), tips[i]);
    if (clicked) {
      if (i == 0) ShowWindow(hwnd, SW_MINIMIZE);
      else PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
  }
}

void App::drawTopBar() {
  float s = ui.s;
  ui.fill(topR, ui.c.rail);
  ui.line(0, topR.b() - 0.5f, topR.w, topR.b() - 0.5f, ui.c.border);
  // left: file menu, save, undo/redo, map switcher (the product name lives in the window title)
  float brandR = 0;
  drawTopLeft(brandR);
  // Network views and node search belong to Visualization; Bibliography keeps its own paper-library navigation.
  const bool visualizationWorkspace = workspace == WS_VISUALIZATION;
  const float searchFull = 190 * s, searchMin = 100 * s;
  const float chipW = visualizationWorkspace ? clampv(topR.w * 0.15f, (topR.w < 1150 * s ? 120 : 150) * s, 240 * s) : 0;
  Rect seg{0, topR.y + 11 * s, 0, 30 * s};
  if (visualizationWorkspace) {
    float rightFull = 6 * 34 * s + 14 * s + searchFull + 8 * s + chipW + captionW() + 6 * s;
    float rightMin = 6 * 34 * s + 14 * s + searchMin + 8 * s + chipW + captionW() + 6 * s;
  vector<float> ws;
  float segFull = 0;
  for (auto& vd : kViews) { float w = ui.textW(vd.label, 12.5f * s, 600) + 42 * s; ws.push_back(w); segFull += w; }
  const float segIcons = 7 * 38 * s, segDrop = 46 * s;
  // centred on the window (not the canvas), so opening or closing a side panel never moves it;
  // full labels only when they fit centred between the left tools and the right-hand block
  float mid = topR.w / 2;
  auto halfFor = [&](float right) { return std::min(mid - brandR, mid - right) - 12 * s; };
  int segMode = 0;  // 0 labels, 1 icons, 2 drop-down
  if (segFull / 2 + 2 * s <= halfFor(rightFull)) segMode = 0;
  else if (segIcons / 2 + 2 * s <= halfFor(rightMin)) segMode = 1;
  else if (brandR + 8 * s + segIcons + 4 * s + 8 * s <= topR.w - rightMin) segMode = 1;  // icons, left of centre
  else segMode = 2;
  bool compact = segMode == 1;
  float segW = segMode == 0 ? segFull : segMode == 1 ? segIcons : segDrop;
  if (compact) for (auto& w : ws) w = 38 * s;
  float right = segMode == 0 ? rightFull : rightMin;
  float cx = clampv(std::round(mid - (segW + 4 * s) / 2), brandR + 8 * s, std::max(brandR + 8 * s, topR.w - right - segW - 4 * s - 8 * s));
  seg = {cx, topR.y + 11 * s, segW + 4 * s, 30 * s};
  ui.fill(seg, ui.dark ? Color(1, 1, 1, 0.07f) : Color(0, 0, 0, 0.055f), 7 * s);
  if (segMode == 2) {  // one button: the current view's icon and a chevron; the seven views in a menu
    int cur = 0;
    for (int i = 0; i < 7; i++) if (kViews[i].v == view) cur = i;
    Rect r{seg.x + 2 * s, seg.y + 2 * s, segDrop, seg.h - 4 * s};
    uint64_t idv = ui.id("view:menu");
    bool hov = false;
    bool open = ui.isPopupOpen("views");
    if (ui.behave(idv, r, &hov) && hasMap()) { if (open) ui.closePopup(); else ui.openPopup("views"); }
    if (hov || open) ui.fill(r, ui.c.hover, 5.5f * s);
    Color fg = hasMap() ? ui.c.text : ui.c.textFaint;
    ui.icon(kViews[cur].icon, r.x + 15 * s, r.y + r.h / 2, 16 * s, hasMap() ? ui.c.accent : fg, 1.7f);
    ui.icon("chev-down", r.x + 33 * s, r.y + r.h / 2 + 1 * s, 11 * s, ui.c.textFaint, 2.f);
    ui.tipFor(idv, string(kViews[cur].label) + " view \xE2\x80\x94 click for the other views  (1\xE2\x80\x93" "7)");
    if (open && hasMap()) {
      ui.overlay([this, r, s]() {
        float rowH = 32 * s;
        Rect pr{r.x, r.b() + 6 * s, 230 * s, 8 * s + 7 * rowH};
        ui.shadow(pr, 8 * s);
        ui.fill(pr, ui.c.panel2, 8 * s);
        ui.stroke(pr, ui.c.border, 8 * s);
        ui.popupRect(pr);
        float yy = pr.y + 4 * s;
        for (int i = 0; i < 7; i++) {
          Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, rowH - 2 * s};
          bool selv = view == kViews[i].v;
          bool enabled = kViews[i].v != ViewKind::Geo || geoAvailable();
          if (ui.listRow(rr, string("vm") + std::to_string(i), selv) && enabled) { ui.closePopup(); setView(kViews[i].v); return; }
          Color fg2 = enabled ? ui.c.text : ui.c.textFaint;
          ui.icon(kViews[i].icon, rr.x + 16 * s, rr.y + rr.h / 2, 15 * s, selv ? ui.c.accent : (enabled ? ui.c.textDim : ui.c.textFaint), 1.6f);
          ui.text({rr.x + 34 * s, rr.y, rr.w - 70 * s, rr.h}, string(kViews[i].label) + (enabled ? "" : "  (needs countries)"), 12.5f * s, fg2, AL_LEFT, selv ? 600 : 400);
          ui.text({rr.r() - 34 * s, rr.y, 26 * s, rr.h}, std::to_string(i + 1), 11 * s, ui.c.textFaint, AL_RIGHT);
          yy += rowH;
        }
      });
    }
  }
  float x = seg.x + 2 * s;
  for (int i = 0; segMode < 2 && i < 7; i++) {
    Rect r{x, seg.y + 2 * s, ws[size_t(i)], seg.h - 4 * s};
    bool selv = view == kViews[i].v && hasMap();
    bool hov = false;
    bool enabled = hasMap() && (kViews[i].v != ViewKind::Geo || geoAvailable());
    uint64_t idv = ui.id(string("view:") + kViews[i].label);
    bool clicked = enabled && ui.behave(idv, r, &hov);
    if (selv) {
      ui.fill({r.x, r.y + 0.8f * s, r.w, r.h}, Color(0, 0, 0, ui.dark ? 0.3f : 0.07f), 5.5f * s);
      ui.fill(r, ui.dark ? Color::hex(0x636366) : Color::hex(0xffffff), 5.5f * s);
    } else if (hov && enabled) ui.fill(r, ui.c.hover, 5.5f * s);
    Color fg = selv ? ui.c.text : (enabled ? ui.c.text.withA(0.78f) : ui.c.textFaint);
    if (compact) ui.icon(kViews[i].icon, r.x + r.w / 2, r.y + r.h / 2, 16 * s, fg, 1.7f);
    else {
      ui.icon(kViews[i].icon, r.x + 16 * s, r.y + r.h / 2, 14 * s, selv ? ui.c.accent : fg, 1.6f);
      ui.text({r.x + 27 * s, r.y, r.w - 30 * s, r.h}, kViews[i].label, 12 * s, fg, AL_LEFT, selv ? 600 : 400);
    }
    ui.tipFor(idv, string(kViews[i].label) + " view  (" + std::to_string(i + 1) + ")" + (kViews[i].v == ViewKind::Geo && !enabled && hasMap() ? ": needs records with country affiliations" : ""));
    if (clicked) setView(kViews[i].v);
    x += ws[size_t(i)];
  }
  } else {
    string route = lastBibliographyRoute == BR_PAPERS ? "Papers" : lastBibliographyRoute == BR_REVIEW ? "Review" : "Write";
    float labelW = topR.w - brandR - 240 * s;
    if (labelW > 60 * s) ui.text({brandR + 12 * s, topR.y + 8 * s, std::min(180 * s, labelW), 34 * s}, "Bibliography · " + route, 12.5f * s, ui.c.textDim, AL_LEFT, 550);
  }
  // right: actions shared by both workspaces; map search/view controls are only drawn in Visualization
  float rx = topR.w - 6 * s - captionW();
  auto ib = [&](const string& icon, const string& tip, bool toggled, bool enabled) {
    rx -= 34 * s;
    return ui.iconButton({rx, topR.y + 12 * s, 30 * s, 28 * s}, icon, tip, toggled, enabled);
  };
  if (visualizationWorkspace && ib("layers", "Inspector  (I)", inspectorOpen && canInspect(), canInspect())) inspectorOpen = !inspectorOpen;
  string papersTip = workspace == WS_BIBLIOGRAPHY ? (papersOpen ? "Return to Bibliography home  (Ctrl+T)" : "Open the papers library  (Ctrl+T)")
                                                    : (papersOpen ? "Return to Visualization  (Ctrl+T)" : "Open the papers library  (Ctrl+T)");
  if (ib("table", papersTip, papersOpen, hasCorpus() || workspace == WS_BIBLIOGRAPHY)) {
    if (papersOpen) {
      papersOpen = false;
      if (workspace == WS_BIBLIOGRAPHY) { lastBibliographyRoute = BR_REVIEW; papersStatusFilter = -1; papersPdfFilter = 1; papersBuiltKey.clear(); page = PG_READ; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); }
    } else openPapers();
  }
  if (ib("writer", writerOpen ? "Back to Bibliography  (Esc)" : wdoc.empty() ? "Open the visual-first writer  (Ctrl+Shift+W)" : "Writer: " + writerSummary() + "  (Ctrl+Shift+W)", writerOpen && !readerOpen, true)) { if (writerOpen) closeWriter(); else openWriter(); }
  if (ib("live", live.open && live.mini ? "Live AI: open the panel again  (Ctrl+Shift+L)" : live.open ? "Close Live AI  (Ctrl+Shift+L)" : "Live AI: talk with the assistant while you work  (Ctrl+Shift+L)", live.open, true)) { if (live.open && live.mini) liveSetMini(false); else if (live.open) liveClose(); else liveOpen(false); }
  {  // sun / moon: drawn by hand, its icon morphs while the theme reveal runs (see drawThemeButton)
    rx -= 34 * s;
    Rect tr{rx, topR.y + 12 * s, 30 * s, 28 * s};
    uint64_t idv = ui.id("ib:theme");
    bool hov = false, held = false;
    bool clicked = ui.behave(idv, tr, &hov, &held);
    drawThemeButton(tr, hov, held, ui.c);
    bool targetDark = themeTarget();
    ui.tipFor(idv, targetDark ? "Light theme" : "Dark theme");
    themeAnim.btn = tr;
    themeAnim.btnHover = hov;
    if (clicked) setTheme(!targetDark);
  }
  if (ib("command", "Command palette  (Ctrl+K)", paletteOpen, true)) { paletteOpen = !paletteOpen; paletteQuery.clear(); paletteSel = 0; }
  if (visualizationWorkspace) {
  rx -= 8 * s;
  float sw = std::min(250 * s, std::max(searchMin, rx - chipW - 8 * s - (seg.r() + 16 * s)));
  Rect sr{rx - sw, topR.y + 12 * s, sw, 28 * s};
  drawMapChip({sr.x - 8 * s - chipW, topR.y + 10 * s, chipW, 32 * s});
  bool submitted = false;
  string before = search;
  ui.textInput(sr, "search", search, hasMap() ? "Search" : "Search", &submitted, "search");
  if (search != before) { runSearch(); if (!searchHits.empty()) { searchIdx = 0; } }
  if (submitted && !searchHits.empty()) { focusOn(searchHits[size_t(searchIdx)]); searchIdx = (searchIdx + 1) % int(searchHits.size()); }
  if (!search.empty()) {
    string cnt = searchHits.empty() ? "0" : std::to_string(searchIdx + 1 <= int(searchHits.size()) ? std::max(1, searchIdx) : 1) + "/" + std::to_string(searchHits.size());
    if (!searchHits.empty()) cnt = std::to_string(searchHits.size()) + " found";
    ui.text({sr.r() - 90 * s, sr.y, 84 * s, sr.h}, cnt, 11 * s, searchHits.empty() ? ui.c.danger : ui.c.textDim, AL_RIGHT);
  }
  // live suggestions under the search box
  if (ui.focus == ui.id("search") && !searchHits.empty()) {
    ui.overlay([this, sr, s]() {
      int n = std::min<int>(8, int(searchHits.size()));
      Rect pr{sr.x, sr.b() + 4 * s, sr.w, n * 30 * s + 8 * s};
      ui.shadow(pr, 8 * s);
      ui.fill(pr, ui.c.panel2, 8 * s);
      ui.stroke(pr, ui.c.border, 8 * s);
      ui.popupRect(pr);
      auto cols = clusterColors();
      for (int k = 0; k < n; k++) {
        int i = searchHits[size_t(k)];
        Rect rr{pr.x + 4 * s, pr.y + 4 * s + k * 30 * s, pr.w - 8 * s, 28 * s};
        if (ui.listRow(rr, "sugg" + std::to_string(i), false)) { focusOn(i); ui.focus = 0; }
        const Node& nd = P->net.nodes[size_t(i)];
        ui.circle(rr.x + 12 * s, rr.y + rr.h / 2, 5 * s, cols.empty() ? ui.c.accent : cols[size_t(std::max(0, nd.cluster)) % cols.size()]);
        ui.text({rr.x + 24 * s, rr.y, rr.w - 80 * s, rr.h}, nd.label, 12.5f * s, ui.c.text);
        ui.text({rr.r() - 60 * s, rr.y, 54 * s, rr.h}, fmtNum(P->net.weight(i), 0), 11 * s, ui.c.textFaint, AL_RIGHT);
      }
    });
  }
  }
}

void App::drawRail() {
  const float s = ui.s;
  const bool expanded = railR.w > 100 * s;
  ui.fill(railR, ui.c.rail);
  ui.line(railR.r() - 0.5f, railR.y, railR.r() - 0.5f, railR.b(), ui.c.border);

  float y = railR.y + 6 * s;
  Rect head{railR.x + 10 * s, y, railR.w - 20 * s, 22 * s};
  if (expanded) ui.text(head, "WORKSPACE", 10 * s, ui.c.textFaint, AL_LEFT, 700);
  Rect modeR{railR.r() - 30 * s, y - 2 * s, 24 * s, 24 * s};
  uint64_t modeId = ui.id("rail:expand");
  bool modeHover = false;
  bool modeClick = ui.behave(modeId, modeR, &modeHover);
  if (modeHover) ui.fill(modeR, ui.c.hover, 5 * s);
  ui.icon(railExpanded ? "chev-left" : "chev-right", modeR.x + modeR.w / 2, modeR.y + modeR.h / 2, 13 * s, modeHover ? ui.c.text : ui.c.textDim, 1.8f);
  ui.tipFor(modeId, railExpanded ? "Collapse navigation" : "Expand navigation when the window is wide enough");
  if (modeClick) { railExpanded = !railExpanded; settings.j.set("railExpanded", railExpanded); needFrame = true; }
  y += 24 * s;

  auto workspaceButton = [&](Workspace ws, const string& label, const string& icon, float yy) {
    Rect r{railR.x + 6 * s, yy, railR.w - 12 * s, 28 * s};
    uint64_t idv = ui.id(string("rail:workspace:") + std::to_string(int(ws)));
    bool hov = false;
    bool clicked = ui.behave(idv, r, &hov);
    bool selected = workspace == ws;
    if (selected) ui.fill(r, ui.c.active, 6 * s);
    else if (hov) ui.fill(r, ui.c.hover, 6 * s);
    Color fg = selected ? ui.c.accent : (hov ? ui.c.text : ui.c.textDim);
    ui.icon(icon, expanded ? r.x + 16 * s : r.x + r.w / 2, r.y + r.h / 2, 15 * s, fg, 1.7f);
    if (expanded) ui.text({r.x + 31 * s, r.y, r.w - 36 * s, r.h}, label, 12.5f * s, selected ? ui.c.text : fg, AL_LEFT, selected ? 600 : 450);
    ui.tipFor(idv, string("Switch to the ") + label + " workspace");
    if (clicked && workspace != ws) setWorkspace(ws);
  };
  workspaceButton(WS_VISUALIZATION, "Visualization", "network", y);
  y += 30 * s;
  workspaceButton(WS_BIBLIOGRAPHY, "Bibliography", "book", y);
  y += 34 * s;
  ui.line(railR.x + 8 * s, y, railR.r() - 8 * s, y, ui.c.border);
  y += 7 * s;

  if (workspace == WS_BIBLIOGRAPHY) {
    Rect add{railR.x + 6 * s, y, railR.w - 12 * s, 32 * s};
    uint64_t addId = ui.id("rail:addpapers");
    bool hov = false;
    bool clicked = ui.behave(addId, add, &hov);
    if (hov) ui.fill(add, ui.c.hover, 6 * s);
    ui.icon("plus", expanded ? add.x + 16 * s : add.x + add.w / 2, add.y + add.h / 2, 15 * s, hov ? ui.c.accent : ui.c.textDim, 1.8f);
    if (expanded) ui.text({add.x + 31 * s, add.y, add.w - 36 * s, add.h}, "Add papers", 12.5f * s, hov ? ui.c.text : ui.c.textDim, AL_LEFT, 500);
    ui.tipFor(addId, "Import bibliographic files or attach PDFs (Ctrl+O)");
    if (clicked) cmdOpenFiles();
    y += 36 * s;
  }

  const RailItem* items = workspace == WS_VISUALIZATION ? kVisualizationNav : kBibliographyNav;
  const size_t count = workspace == WS_VISUALIZATION ? sizeof(kVisualizationNav) / sizeof(kVisualizationNav[0]) : sizeof(kBibliographyNav) / sizeof(kBibliographyNav[0]);
  const float bottomH = 78 * s;
  Rect list{railR.x, y, railR.w, std::max(0.f, railR.b() - bottomH - y)};
  ui.beginScroll(workspace == WS_VISUALIZATION ? "nav:visualization" : "nav:bibliography", list);
  float sy = ui.scrollY();
  float rowH = expanded ? 42 * s : 48 * s;
  for (size_t i = 0; i < count; i++) {
    const RailItem& item = items[i];
    Rect r{railR.x + 6 * s, list.y + 4 * s + float(i) * rowH - sy, railR.w - 12 * s, rowH - 4 * s};
    uint64_t idv = ui.id(string("rail:item:") + (workspace == WS_VISUALIZATION ? "v:" : "b:") + item.id);
    bool hov = false;
    bool clicked = ui.behave(idv, r, &hov);
    bool selected = false;
    if (workspace == WS_VISUALIZATION) {
      selected = item.action == RailAction::Map ? (page == PG_NONE && !papersOpen && !readerOpen && !writerOpen && !mainChartOpen)
                                                : (item.action == RailAction::Page && page == item.page);
    } else {
      selected = (item.action == RailAction::Papers && lastBibliographyRoute == BR_PAPERS) ||
                 (item.action == RailAction::Review && lastBibliographyRoute == BR_REVIEW) ||
                 (item.action == RailAction::Write && lastBibliographyRoute == BR_WRITE);
    }
    if (selected) ui.fill(r, ui.c.active, 7 * s);
    else if (hov) ui.fill(r, ui.c.hover, 7 * s);
    if (selected) ui.fill({r.x, r.y + 7 * s, 3 * s, r.h - 14 * s}, ui.c.accent, 1.5f * s);
    Color fg = selected ? ui.c.accent : (hov ? ui.c.text : ui.c.textDim);
    ui.icon(item.icon, expanded ? r.x + 18 * s : r.x + r.w / 2, r.y + r.h / 2, 17 * s, fg, 1.7f);
    if (expanded) ui.text({r.x + 36 * s, r.y, r.w - 44 * s, r.h}, item.label, 12.5f * s, selected ? ui.c.text : fg, AL_LEFT, selected ? 600 : 450);
    bool hint = item.action == RailAction::Page && ((item.page == PG_DATA && !hasCorpus()) || (item.page == PG_BUILD && hasCorpus() && !hasMap()));
    if (hint) ui.circle(r.r() - 9 * s, r.y + 10 * s, 3 * s, ui.c.accent);
    ui.tipFor(idv, item.hint);
    if (clicked) activateWorkspaceNav(item.id);
  }
  ui.endScroll(8 * s + float(count) * rowH);

  auto bottomButton = [&](const string& id, const string& icon, const string& label, const string& tip, float yy, std::function<void()> action, bool selected = false) {
    Rect r{railR.x + 6 * s, yy, railR.w - 12 * s, 32 * s};
    uint64_t idv = ui.id("rail:bottom:" + id);
    bool hov = false;
    bool clicked = ui.behave(idv, r, &hov);
    if (selected) ui.fill(r, ui.c.active, 6 * s);
    else if (hov) ui.fill(r, ui.c.hover, 6 * s);
    Color fg = selected ? ui.c.accent : (hov ? ui.c.text : ui.c.textDim);
    ui.icon(icon, expanded ? r.x + 16 * s : r.x + r.w / 2, r.y + r.h / 2, 15 * s, fg, 1.7f);
    if (expanded) ui.text({r.x + 31 * s, r.y, r.w - 36 * s, r.h}, label, 12 * s, selected ? ui.c.text : fg, AL_LEFT, selected ? 600 : 450);
    ui.tipFor(idv, tip);
    if (clicked) action();
  };
  float by = railR.b() - bottomH + 2 * s;
  bottomButton("assistant", "sparkle", "Assistant", "Assistant workspace tool  (Ctrl+J)", by, [this] { setPage(page == PG_AI ? PG_NONE : PG_AI); }, page == PG_AI);
  by += 34 * s;
  bottomButton("commands", "command", "Commands", "Keyboard shortcuts and command palette  (F1 / Ctrl+K)", by, [this] { paletteOpen = true; paletteQuery.clear(); paletteSel = 0; });
}

void App::sectionTitle(Lay& L, const string& t, const string& help, const string& status) {
  float s = ui.s;
  L.space(6 * s);
  Rect r = L.row(18 * s);
  const string& T = t;
  ui.text(r, T, 12 * s, ui.c.textDim, AL_LEFT, 600);
  float tw = ui.textW(T, 12 * s, 600);
  if (!help.empty()) ui.help(r.x + tw + 6 * s, r.y + r.h / 2, help);
  if (!status.empty()) ui.text({r.x + tw + 30 * s, r.y, r.w - tw - 30 * s, r.h}, status, 11 * s, ui.c.textFaint, AL_RIGHT);
}

void App::buildBar(const Rect& r) {
  float s = ui.s;
  ui.pushId("buildbar");
  Rect b{r.x, r.y + 14 * s, r.w - 2 * (30 * s + 6 * s), 28 * s};
  if (ui.button(b, busy() ? "Building\xE2\x80\xA6" : (hasMap() ? "Rebuild Map" : "Build Map"), BTN_PRIMARY, "", !busy())) cmdBuild();
  ui.tip("Build the map with the settings below  (Ctrl+B)");
  Rect i1{b.r() + 6 * s, b.y, 30 * s, 28 * s}, i2{i1.r() + 6 * s, b.y, 30 * s, 28 * s};
  if (ui.iconButton(i1, "refresh", "Re-run layout (new positions, same clusters)", false, hasMap() && !busy())) cmdRelayout();
  if (ui.iconButton(i2, "shuffle", "Re-cluster (same positions)", false, hasMap() && !busy())) cmdRecluster();
  if (hasMap() && P->mapSource == "analysis") {
    const BuildReport& R = P->last;
    string line = fmtInt(R.n) + " items \xC2\xB7 " + fmtInt(R.m) + " links \xC2\xB7 " + std::to_string(R.clusters) + " clusters \xC2\xB7 Q " + fmtFixed(R.Q, 3);
    ui.text({r.x, b.b() + 6 * s, r.w, 18 * s}, line, 11 * s, ui.c.textFaint);
    ui.help(r.x + ui.textW(line, 11 * s) + 6 * s, b.b() + 15 * s, "Layout " + fmtNum(R.layoutMs, 0) + " ms \xC2\xB7 clustering " + fmtNum(R.clusterMs, 0) + " ms.\nQ is the modularity of the clustering (higher = more separated clusters).");
  }
  ui.popId();
}

void App::emptyHint(Lay& L, const string& icon, const string& title, const string& msg) {
  float s = ui.s;
  float h = ui.textWrap({0, 0, L.w - 40 * s, 1000}, msg, 12 * s, ui.c.textDim, 400, false);
  L.space(18 * s);
  Rect r = L.row(64 * s + h);
  ui.icon(icon, r.x + r.w / 2, r.y + 14 * s, 22 * s, ui.c.textFaint, 1.5f);
  ui.text({r.x, r.y + 32 * s, r.w, 20 * s}, title, 13 * s, ui.c.text, AL_CENTER, 600);
  float tw = std::min(L.w - 40 * s, ui.textW(msg, 12 * s) + 4);
  ui.textWrap({r.x + (r.w - tw) / 2, r.y + 54 * s, tw, h + 10}, msg, 12 * s, ui.c.textDim);
}

// The panel slides: its content is laid out at full width and clipped to the visible part, so
// nothing reflows (and no chart is re-made) while it moves.
void App::drawLeft() {
  if (leftR.w <= 0.5f) return;
  bool closing = page < 0;
  if (closing && leftShownPage < 0) return;
  Rect real = leftR;
  float fullW = std::round(leftW);
  bool sliding = real.w < fullW - 0.5f;
  leftR = {real.r() - fullW, real.y, fullW, real.h};
  int saved = page;
  if (closing) page = leftShownPage;
  if (sliding) ui.pushClip(real);
  drawLeftBody();
  if (sliding) ui.popClip();
  if (closing && page == leftShownPage) page = saved;  // the body did not navigate elsewhere
  leftR = real;
}

void App::drawLeftBody() {
  if (page < 0 || leftR.w <= 0) return;
  float s = ui.s;
  ui.fill(leftR, ui.c.panel);
  ui.line(leftR.r() - 0.5f, leftR.y, leftR.r() - 0.5f, leftR.b(), ui.c.border);
  // header
  Rect hr{leftR.x, leftR.y, leftR.w, 46 * s};
  string panelTitle = workspace == WS_BIBLIOGRAPHY && page == PG_READ ? "Library" : kPageTitles[page];
  ui.text({hr.x + 16 * s, hr.y, hr.w - 60 * s, hr.h}, panelTitle, 15 * s, ui.c.text, AL_LEFT, 600);
  if (ui.iconButton({hr.r() - 36 * s, hr.y + 10 * s, 26 * s, 26 * s}, "chev-left", "Hide panel")) page = PG_NONE;
  ui.line(hr.x + 16 * s, hr.b() - 0.5f, hr.r() - 16 * s, hr.b() - 0.5f, ui.c.border);
  if (page < 0) return;
  Rect body{leftR.x, hr.b(), leftR.w, leftR.h - hr.h};
  if (page == PG_BUILD && hasCorpus()) {
    // pinned action bar: Build is always reachable without scrolling past the candidate list
    float bh = hasMap() && P->mapSource == "analysis" ? 80 * s : 56 * s;
    Rect bar{leftR.x, hr.b(), leftR.w - 1, bh};
    ui.fill(bar, ui.c.panel);
    buildBar(inset(bar, 16 * s, 0));
    ui.line(bar.x, bar.b() - 0.5f, bar.r(), bar.b() - 0.5f, ui.c.border);
    body = {leftR.x, bar.b(), leftR.w, leftR.b() - bar.b()};
  }
  ui.beginScroll(string("page") + std::to_string(page), body);
  Lay L{body.x + 16 * s, body.y + 12 * s - ui.scrollY(), body.w - 32 * s, 8 * s};
  float y0 = L.y;
  ui.pushId(kPageTitles[page]);
  switch (page) {
    case PG_DATA: pageData(L); break;
    case PG_BUILD: pageBuild(L); break;
    case PG_LOOK: pageLook(L); break;
    case PG_ANALYSE: pageAnalyse(L); break;
    case PG_TRENDS: pageTrends(L); break;
    case PG_ACTORS: pageActors(L); break;
    case PG_PUBLISH: pagePublish(L); break;
    case PG_READ: pageRead(L); break;
    case PG_WRITER: pageWriter(L); break;
    case PG_AI: pageAssistant(L); break;
  }
  ui.popId();
  ui.endScroll(L.y - y0 + 40 * s);
  // panel resize handle
  Rect grip{leftR.r() - 3 * s, leftR.y, 6 * s, leftR.h};
  static bool resizing = false;
  bool hov = leftT >= 1 && page >= 0 && grip.has(ui.in.mx, ui.in.my);
  if (hov || resizing) ui.cursor = "ew";
  if (hov && ui.in.pressed[0] && ui.active == 0) resizing = true;
  if (resizing) { leftW = clampv(ui.in.mx - leftR.x, 300 * s, 620 * s); if (!ui.in.down[0]) resizing = false; figSig.clear(); }
  commitStyleIfChanged();
}

void App::drawStatus() {
  float s = ui.s;
  ui.fill(statusR, ui.c.rail);
  ui.line(0, statusR.y + 0.5f, statusR.w, statusR.y + 0.5f, ui.c.border);
  string left;
  if (hasCorpus()) left += plural(long(P->corpus.recs.size()), "record");
  if (hasMap()) {
    left += (left.empty() ? "" : "   \xC2\xB7   ") + plural(P->net.n(), P->net.unitNoun) + "   \xC2\xB7   " + plural(P->net.m(), "link") + "   \xC2\xB7   " + plural(P->net.nClusters, "cluster");
    if (P->last.Q > 0) left += "   \xC2\xB7   Q " + fmtFixed(P->last.Q, 3);
    if (!selection.empty()) left += "   \xC2\xB7   " + std::to_string(selection.size()) + " selected";
  }

  float lx = 12 * s;
  if (hasCorpus() || hasMap()) {  // the project name (the window has no system caption to carry it)
    string nm = P->path.empty() ? string("Untitled") : fileName(P->path);
    if (nm.size() > 8 && nm.substr(nm.size() - 8) == ".vosproj") nm.resize(nm.size() - 8);
    nm = truncate(nm, 40);
    ui.text({lx, statusR.y, 320 * s, statusR.h}, nm, 11 * s, ui.c.text.withA(0.8f), AL_LEFT, 600);
    lx += ui.textW(nm, 11 * s, 600) + 6 * s;
    if (P->dirty) { ui.text({lx, statusR.y, 80 * s, statusR.h}, "Edited", 11 * s, ui.c.textFaint); lx += ui.textW("Edited", 11 * s) + 6 * s; }
    lx += 12 * s;
  }
  ui.text({lx, statusR.y, statusR.w * 0.62f - lx, statusR.h}, left, 11 * s, ui.c.textDim);
  string right;
  if (hasMap() && view != ViewKind::ThreeD && view != ViewKind::Matrix) right = fmtNum(nv.cam.zoom / std::max(1e-9, nv.fitZoom()) * 100, 0) + "%";
  ui.text({statusR.w * 0.45f, statusR.y, statusR.w * 0.55f - 12 * s, statusR.h}, right, 11 * s, ui.c.textFaint, AL_RIGHT);
  if (busy()) {
    float w = 280 * s;
    Rect pr{statusR.w / 2 - w / 2, statusR.y + 5 * s, w, statusR.h - 10 * s};
    ui.text({pr.x + 10 * s, pr.y, 150 * s, pr.h}, jobLabel, 11 * s, ui.c.textDim, AL_LEFT, 400);
    ui.progress({pr.x + 160 * s, pr.y + pr.h / 2 - 2 * s, 90 * s, 4 * s}, job->progress, ui.c.accent);
    if (ui.iconButton({pr.r() - 22 * s, pr.y, 18 * s, pr.h}, "x", "Cancel")) job->cancel = true;
  }
}

// =====================================================================================
// canvas
// =====================================================================================
void App::drawBibliographyHome(const Rect& r) {
  const float s = ui.s;
  ui.fill(r, ui.c.bg);
  const float cw = std::min(660 * s, std::max(280 * s, r.w - 48 * s));
  const float x = r.x + (r.w - cw) / 2;
  const float y = r.y + r.h * 0.32f;
  ui.icon("book", x + cw / 2, y - 28 * s, 30 * s, ui.c.accent, 1.6f);
  ui.text({x, y, cw, 34 * s}, "Bibliography", 22 * s, ui.c.text, AL_CENTER, 650);
  string summary = hasCorpus() ? plural(long(P->corpus.recs.size()), "paper") + " in your library" : "Manage papers, PDFs, notes and citations in one place.";
  if (hasCorpus() && P && !P->library.items.empty()) summary += "   ·   " + plural(long(P->library.items.size()), "PDF");
  ui.text({x + 12 * s, y + 38 * s, cw - 24 * s, 22 * s}, summary, 13 * s, ui.c.textDim, AL_CENTER);

  const float bw = std::min(190 * s, (cw - 24 * s) / 3);
  const float gap = 10 * s;
  const float bx = x + (cw - (3 * bw + 2 * gap)) / 2;
  const float by = y + 82 * s;
  if (ui.button({bx, by, bw, 34 * s}, "Papers", BTN_PRIMARY, "table")) openPapers("", -1, true, false);
  if (ui.button({bx + bw + gap, by, bw, 34 * s}, "Add papers\xE2\x80\xA6", BTN_NORMAL, "plus")) cmdOpenFiles();
  if (ui.button({bx + 2 * (bw + gap), by, bw, 34 * s}, "Review PDFs", BTN_NORMAL, "book")) activateWorkspaceNav("review");
  ui.text({x + 24 * s, by + 52 * s, cw - 48 * s, 44 * s},
          "Organize your library, review and annotate attached papers, keep notes, and cite directly while writing. Network maps and analysis are in the Visualization workspace.",
          12 * s, ui.c.textFaint, AL_CENTER);
}

void App::drawWelcome() {
  float s = ui.s;
  Rect r = canvasR;
  ui.fill(r, ui.c.bg);
  float cw = std::min(520 * s, r.w - 60 * s);
  float cx = r.x + (r.w - cw) / 2, cy = r.y + r.h * 0.38f;
  if (hasCorpus()) {
    const QualityReport& q = statQuality();
    ui.text({cx, cy - 34 * s, cw, 28 * s}, plural(q.records, "record"), 20 * s, ui.c.text, AL_CENTER, 600);
    string sub = (q.yearMin > 0 ? std::to_string(q.yearMin) + "\xE2\x80\x93" + std::to_string(q.yearMax) + "   \xC2\xB7   " : string()) + plural(q.sources, "source") + "   \xC2\xB7   " + plural(q.authors, "author");
    ui.text({cx, cy - 4 * s, cw, 20 * s}, sub, 12.5f * s, ui.c.textDim, AL_CENTER);
    float bw = 150 * s, gap = 10 * s, bx = cx + (cw - 2 * bw - gap) / 2;
    if (ui.button({bx, cy + 32 * s, bw, 28 * s}, "Choose Analysis\xE2\x80\xA6", BTN_NORMAL)) page = PG_BUILD;
    if (ui.button({bx + bw + gap, cy + 32 * s, bw, 28 * s}, "Build Map", BTN_PRIMARY, "", !busy())) cmdBuild();
    return;
  }
  ui.text({cx, cy - 34 * s, cw, 28 * s}, "No Data", 20 * s, ui.c.text, AL_CENTER, 600);
  ui.text({cx, cy - 4 * s, cw, 20 * s}, "Import records or open a project to begin.", 12.5f * s, ui.c.textDim, AL_CENTER);
  float bw = 130 * s, gap = 10 * s, bx = cx + (cw - 3 * bw - 2 * gap) / 2;
  if (ui.button({bx, cy + 32 * s, bw, 28 * s}, "Import\xE2\x80\xA6", BTN_PRIMARY)) cmdOpenFiles();
  if (ui.button({bx + bw + gap, cy + 32 * s, bw, 28 * s}, "Open Project\xE2\x80\xA6", BTN_NORMAL)) cmdOpenProject();
  if (ui.button({bx + 2 * (bw + gap), cy + 32 * s, bw, 28 * s}, "Open Sample", BTN_NORMAL)) cmdSample(false);
}

// Start window: recent projects plus New / Open / Sample, nothing else.
void App::drawStart() {
  float s = ui.s;
  float W = float(g.W), H = float(g.H);
  ui.fill({0, 0, W, H}, ui.dark ? Color::hex(0x1a1a1a) : Color::hex(0xececec));
  float pw = std::min(780 * s, W - 40 * s), ph = std::min(460 * s, H - 40 * s);
  Rect p{std::round((W - pw) / 2), std::round((H - ph) / 2), pw, ph};
  ui.shadow(p, 12 * s, 24 * s);
  float lw = std::round(pw * 0.44f);
  Rect lp{p.x, p.y, lw, p.h}, rp{p.x + lw, p.y, p.w - lw, p.h};
  ui.fill(p, ui.c.bg, 12 * s);
  ui.fill({rp.x, rp.y, rp.w, rp.h}, ui.c.panel, 12 * s);
  ui.fill({rp.x, rp.y, 14 * s, rp.h}, ui.c.panel);
  ui.line(rp.x + 0.5f, rp.y, rp.x + 0.5f, rp.b(), ui.c.border);
  ui.stroke(p, ui.dark ? Color(1, 1, 1, 0.12f) : Color(0, 0, 0, 0.12f), 12 * s);
  // mark: three linked nodes
  float mx = lp.x + lp.w / 2, my = lp.y + 92 * s;
  Color a = ui.c.accent;
  ui.line(mx - 22 * s, my + 12 * s, mx + 2 * s, my - 20 * s, a.withA(0.45f), 2.5f * s);
  ui.line(mx + 2 * s, my - 20 * s, mx + 24 * s, my + 14 * s, a.withA(0.45f), 2.5f * s);
  ui.line(mx - 22 * s, my + 12 * s, mx + 24 * s, my + 14 * s, a.withA(0.45f), 2.5f * s);
  ui.circle(mx - 22 * s, my + 12 * s, 11 * s, Color::hex(0xe8554e));
  ui.circle(mx + 2 * s, my - 20 * s, 14 * s, a);
  ui.circle(mx + 24 * s, my + 14 * s, 9 * s, Color::hex(0x34a853));
  ui.text({lp.x, my + 40 * s, lp.w, 34 * s}, "VOSStudio", 26 * s, ui.c.text, AL_CENTER, 600);
  ui.text({lp.x, my + 74 * s, lp.w, 18 * s}, "Version 1.7", 12 * s, ui.c.textDim, AL_CENTER);
  // actions
  struct Act { const char* icon; const char* label; int cmd; };
  const Act acts[3] = {{"plus", "New Project", 0}, {"folder", "Open\xE2\x80\xA6", 1}, {"data", "Open Sample", 2}};
  float ay = my + 118 * s, aw = std::min(260 * s, lp.w - 60 * s), ax = lp.x + (lp.w - aw) / 2;
  for (int i = 0; i < 3; i++) {
    Rect rr{ax, ay + float(i) * 40 * s, aw, 36 * s};
    uint64_t idv = ui.id(string("start:") + acts[i].label);
    bool hov = false;
    bool clicked = ui.behave(idv, rr, &hov);
    if (hov) ui.fill(rr, ui.c.hover, 8 * s);
    Rect ib{rr.x + 8 * s, rr.y + 5 * s, 26 * s, 26 * s};
    ui.fill(ib, ui.c.active, 6 * s);
    ui.icon(acts[i].icon, ib.x + ib.w / 2, ib.y + ib.h / 2, 15 * s, ui.c.text, 1.6f);
    ui.text({rr.x + 44 * s, rr.y, rr.w - 50 * s, rr.h}, acts[i].label, 13 * s, ui.c.text, AL_LEFT, 500);
    if (clicked) {
      if (acts[i].cmd == 0) { showStart = false; newProjectNow(); }
      else if (acts[i].cmd == 1) cmdOpenAny();
      else { showStart = false; cmdSample(false); }
    }
  }
  // recent projects
  auto rec = settings.recent();
  float ry = rp.y + 14 * s;
  ui.text({rp.x + 20 * s, ry, rp.w - 40 * s, 22 * s}, "Recent", 12 * s, ui.c.textDim, AL_LEFT, 600);
  ry += 28 * s;
  if (rec.empty()) {
    ui.text({rp.x, rp.y, rp.w, rp.h}, "No Recent Projects", 13 * s, ui.c.textFaint, AL_CENTER);
  } else {
    for (size_t i = 0; i < std::min<size_t>(8, rec.size()); i++) {
      Rect rr{rp.x + 10 * s, ry, rp.w - 20 * s, 44 * s};
      if (rr.b() > rp.b() - 8 * s) break;
      if (ui.listRow(rr, "start-recent" + std::to_string(i), false)) { string path = rec[i]; cmdOpenProject(path); if (hasCorpus() || hasMap() || P->library.hasData() || !wdoc.empty()) showStart = false; return; }
      ui.icon("file", rr.x + 18 * s, rr.y + rr.h / 2, 18 * s, ui.c.textDim, 1.5f);
      string nm = fileName(rec[i]);
      if (nm.size() > 8 && lower(nm.substr(nm.size() - 8)) == ".vosproj") nm = nm.substr(0, nm.size() - 8);
      ui.text({rr.x + 38 * s, rr.y + 5 * s, rr.w - 46 * s, 18 * s}, nm, 13 * s, ui.c.text, AL_LEFT, 500);
      string dir = rec[i].substr(0, rec[i].size() - fileName(rec[i]).size());
      ui.text({rr.x + 38 * s, rr.y + 23 * s, rr.w - 46 * s, 16 * s}, dir, 11 * s, ui.c.textFaint);
      ry += 46 * s;
    }
  }
}

void App::drawMainChart(const Rect& r) {
  float s = ui.s;
  ui.fill(r, ui.c.bg);
  Rect hdr{r.x, r.y, r.w, 48 * s};
  ui.text({hdr.x + 18 * s, hdr.y, hdr.w - 400 * s, hdr.h}, mainChart.title, 15 * s, ui.c.text, AL_LEFT, 700);
  float bx = hdr.r() - 12 * s;
  auto btn = [&](const string& l, const string& ic) { float w = ui.textW(l, 12.5f * s, 550) + 40 * s; bx -= w + 6 * s; return ui.button({bx, hdr.y + 9 * s, w, 30 * s}, l, BTN_NORMAL, ic); };
  if (view != ViewKind::Matrix || mainChartOpen) { if (btn("Close", "x")) { mainChartOpen = false; return; } }
  if (btn("PNG", "download")) cmdExportChart(mainChart, "png");
  if (btn("PDF", "download")) cmdExportChart(mainChart, "pdf");
  if (btn("SVG", "download")) cmdExportChart(mainChart, "svg");
  Rect card{r.x + 18 * s, hdr.b(), r.w - 36 * s, r.h - hdr.h - 18 * s};
  ui.fill(card, ui.c.card, 10 * s);
  ui.stroke(card, ui.c.border, 10 * s);
  ChartTheme t = chartTheme(false);
  double sw = (card.w - 24 * s) / s, sh = (card.h - 24 * s) / s;
  ChartCacheEntry& ce = chartCache["#main|" + mainChart.title];
  uint64_t sig = chartSig();
  if (chartFresh > 0 || ce.frame == 0 || ce.sig != sig || ce.w != float(sw) || ce.h != float(sh) || ce.dark != ui.dark) {
    ce.sc = mainChart.make(sw, sh, t);
    ce.sig = sig; ce.w = float(sw); ce.h = float(sh); ce.dark = ui.dark; ce.gen++;
  }
  ce.frame = std::max(1, frameNo);
  const Scene& sc = ce.sc;
  // charts made at their own size (the assistant's show_chart) are fitted into the card and centred; page charts fill it exactly
  float k = 1;
  if (sc.W > 0 && sc.H > 0 && (std::fabs(sc.W - sw) > 0.5 || std::fabs(sc.H - sh) > 0.5)) k = float(std::min(sw / sc.W, sh / sc.H));
  float ox = card.x + 12 * s + float((sw - sc.W * k) * s) / 2, oy = card.y + 12 * s + float((sh - sc.H * k) * s) / 2;
  if (k == 1) { ox = card.x + 12 * s; oy = card.y + 12 * s; }
  const float ks = s * k;
  drawChartCached("#main|" + mainChart.title, ce.gen, sc, ox, oy, ks);
  addChartHoverRegion(sc, card, ox, oy, ks, ui.id("chart-hover:main:" + mainChart.title));
  if (card.has(ui.in.mx, ui.in.my) && !ui.anyPopup() && !ui.anyModal()) chartHover(sc, ox, oy, ks);
  if (card.has(ui.in.mx, ui.in.my) && mainChart.onClick) {
    int tag = hitTag(sc, (ui.in.mx - ox) / ks, (ui.in.my - oy) / ks);
    if (tag >= 0) { ui.cursor = "hand"; if (ui.in.released[0] && ui.active == 0) mainChart.onClick(tag); }
  }
}

void App::drawCanvas() {
  chromeRects.clear();
  if (live.open) chromeRects.push_back(liveHitRect());
  if (page == PG_AI) { drawAssistant(mainR); return; }
  if (!writerOpen) writerDropImages();  // figure bitmaps are for the open writer only
  if (readerOpen) { drawReader(mainR); return; }
  if (writerOpen) { drawWriter(mainR); return; }
  if (papersOpen) { drawPapers(mainR); return; }
  if (workspace == WS_BIBLIOGRAPHY) { drawBibliographyHome(mainR); return; }
  if (!hasMap()) { drawWelcome(); return; }
  if (view == ViewKind::Matrix && !mainChartOpen) {
    ChartDef m;
    m.title = "Matrix: link strength between the top items";
    m.make = [this](double w, double h, const ChartTheme& t) { return chartDensityMatrix(P->net, clusterColors(), w, h, t); };
    ChartDef keep = mainChart;
    mainChart = m;
    drawMainChart(canvasR);
    mainChart = keep;
    return;
  }
  if (mainChartOpen && mainChart.valid()) { drawMainChart(mainR); return; }
  if (splitActive()) {
    drawSplitPanes();
    if (splitMapPane() < 0) return;  // every pane is a chart: no map this frame
  }
  drawCanvasChrome();
  canvasInput();
}

void App::drawCanvasChrome() {
  float s = ui.s;
  Theme th = canvasTheme(P->style);
  // Controls on the map follow the canvas lightness (a white VOSviewer canvas inside the dark theme and vice versa).
  struct PaletteGuard {
    Ui& u; UiColors saved;
    PaletteGuard(Ui& ui_, bool light) : u(ui_), saved(ui_.c) {
      if (light == !u.dark) return;
      if (light) {
        u.c.text = Color::hex(0x1d1d1f); u.c.textDim = Color::hex(0x6e6e73); u.c.textFaint = Color::hex(0xa1a1a6);
        u.c.hover = Color(0, 0, 0, 0.045f); u.c.active = Color(0, 0, 0, 0.08f); u.c.border = Color::hex(0xdedede); u.c.card = Color::hex(0xffffff); u.c.panel2 = Color::hex(0xffffff);
      } else {
        u.c.text = Color::hex(0xf2f2f4); u.c.textDim = Color::hex(0xa1a1a6); u.c.textFaint = Color::hex(0x6e6e73);
        u.c.hover = Color(1, 1, 1, 0.06f); u.c.active = Color(1, 1, 1, 0.1f); u.c.border = Color::hex(0x3a3a3a); u.c.card = Color::hex(0x2c2c2e); u.c.panel2 = Color::hex(0x3a3a3c);
      }
    }
    ~PaletteGuard() { u.c = saved; }
  } paletteGuard(ui, th.light);
  Color cardBg = th.light ? Color(1, 1, 1, 0.92f) : Color(0.09f, 0.1f, 0.12f, 0.9f);
  Color cardBd = th.light ? Color(0, 0, 0, 0.1f) : Color(1, 1, 1, 0.1f);
  Color fg = th.fg, dim = th.muted;
  const Network& N = P->net;
  // ---- timeline axis
  if (view == ViewKind::Timeline && tlS1 > tlS0) {
    float y = canvasR.b() - 34 * s;
    ui.fill({canvasR.x, y - 8 * s, canvasR.w, 42 * s}, th.bg.withA(0.85f));
    ui.line(canvasR.x + 20 * s, y, canvasR.r() - 20 * s, y, dim.withA(0.5f));
    int y0 = int(std::floor(tlS0)), y1 = int(std::ceil(tlS1));
    int step = std::max(1, (y1 - y0) / 10);
    for (int yr = y0; yr <= y1; yr += step) {
      double wx = tlX0 + (yr - tlS0) / (tlS1 - tlS0) * (tlX1 - tlX0);
      float sx, sy;
      nv.worldToScreen(wx, 0, 0, sx, sy);
      if (sx < canvasR.x + 10 * s || sx > canvasR.r() - 10 * s) continue;
      ui.line(sx, y - 4 * s, sx, y + 4 * s, dim);
      ui.line(sx, canvasR.y, sx, y - 8 * s, dim.withA(0.08f));
      ui.text({sx - 30 * s, y + 6 * s, 60 * s, 16 * s}, std::to_string(yr), 11 * s, dim, AL_CENTER, 600);
    }
  }
  // ---- legend (top-left)
  geoChip();
  bool geoOverview = geoMode() == 2;
  if (showLegend && geoOverview) geoLegend(canvasR.x + 14 * s, canvasR.y + 14 * s);
  bool geoNet = view == ViewKind::Geo && geoMode() == 1;
  if (showLegend && geoNet && geoLayerKind == 2) geoDensityLegend(canvasR.x + 14 * s, canvasR.y + 14 * s, pluralWord(N.unitNoun));
  else if (showLegend && !geoOverview) {
    float x = canvasR.x + 14 * s, y = canvasR.y + 14 * s;
    bool scoreMode = view == ViewKind::Overlay || view == ViewKind::Timeline || P->style.colorBy == ColorBy::Score || (geoNet && geoLayerKind == 1);
    if (scoreMode && N.scoreIdx >= 0) {
      Rect r{x, y, 236 * s, 62 * s};
      ui.fill(r, cardBg, 10 * s);
      ui.stroke(r, cardBd, 10 * s);
      chromeRects.push_back(r);
      string sname = N.scoreIdx < int(N.scoreNames.size()) ? N.scoreNames[size_t(N.scoreIdx)] : "Score";
      ui.text({r.x + 12 * s, r.y + 6 * s, r.w - 24 * s, 18 * s}, sname, 11.5f * s, fg, AL_LEFT, 650);
      Rect bar{r.x + 12 * s, r.y + 28 * s, r.w - 24 * s, 9 * s};
      int K = 48;
      for (int k = 0; k < K; k++) ui.fill({bar.x + bar.w * k / K, bar.y, bar.w / K + 0.8f, bar.h}, simulateCvd(cmapAt(P->style.scheme, (k + 0.5) / K), P->style.cvd));
      const Encoder& e = nv.enc();
      bool yr = contains(lower(sname), "year");
      ui.text({bar.x, bar.b() + 2 * s, 80 * s, 14 * s}, yr ? fmtFixed(e.sMin, 1) : fmtNum(e.sMin, 2), 10.5f * s, dim);
      ui.text({bar.r() - 80 * s, bar.b() + 2 * s, 80 * s, 14 * s}, yr ? fmtFixed(e.sMax, 1) : fmtNum(e.sMax, 2), 10.5f * s, dim, AL_RIGHT);
    } else if (N.nClusters > 0) {
      int shown = std::min(N.nClusters, 12);
      Rect r{x, y, 250 * s, 34 * s + shown * 24 * s + (N.nClusters > shown ? 18 * s : 0)};
      ui.fill(r, cardBg, 10 * s);
      ui.stroke(r, cardBd, 10 * s);
      chromeRects.push_back(r);
      ui.text({r.x + 12 * s, r.y + 6 * s, r.w - 50 * s, 20 * s}, "Clusters", 11.5f * s, fg, AL_LEFT, 700);
      if (clusterFilter >= 0 && ui.button({r.r() - 60 * s, r.y + 6 * s, 50 * s, 20 * s}, "All", BTN_GHOST)) clusterFilter = -1;
      std::vector<int> sizes(size_t(N.nClusters), 0);
      for (auto& nd : N.nodes) if (nd.cluster >= 0 && nd.cluster < N.nClusters) sizes[size_t(nd.cluster)]++;
      auto cols = clusterColors();
      for (int c = 0; c < shown; c++) {
        Rect rr{r.x + 6 * s, r.y + 30 * s + c * 24 * s, r.w - 12 * s, 22 * s};
        uint64_t idv = ui.id("legend" + std::to_string(c));
        bool hov = false;
        if (ui.behave(idv, rr, &hov)) { if (clusterFilter == c) clusterFilter = -1; else focusCluster(c); }
        if (hov || clusterFilter == c) ui.fill(rr, th.light ? Color(0, 0, 0, 0.05f) : Color(1, 1, 1, 0.07f), 5 * s);
        ui.circle(rr.x + 11 * s, rr.y + rr.h / 2, 5.5f * s, cols[size_t(c) % cols.size()]);
        bool dimmed = clusterFilter >= 0 && clusterFilter != c;
        ui.text({rr.x + 24 * s, rr.y, rr.w - 70 * s, rr.h}, N.clusterName(c), 11.5f * s, dimmed ? dim : fg, AL_LEFT, 500);
        ui.text({rr.r() - 44 * s, rr.y, 38 * s, rr.h}, std::to_string(sizes[size_t(c)]), 11 * s, dim, AL_RIGHT);
        ui.tipFor(idv, "Click to focus this cluster; click again to show all");
      }
      if (N.nClusters > shown) ui.text({r.x + 12 * s, r.b() - 20 * s, r.w, 16 * s}, "+ " + std::to_string(N.nClusters - shown) + " more (see Analyse)", 10.5f * s, dim);
    }
  }
  // ---- view chip (top-right); the Geo view shows its layer switch there
  if (view == ViewKind::Geo && geoMode() > 0) geoLayerSwitch();
  else if (splitActive()) {}  // the map pane's picker shows the chip text (split.cpp)
  else {
    string chip = string(viewLabel(view)) + "  \xC2\xB7  " + plural(N.n(), N.unitNoun);
    if (view == ViewKind::ThreeD && !pos3dValid) chip += "  \xC2\xB7  computing 3D layout\xE2\x80\xA6";
    float w = ui.textW(chip, 11.5f * s, 600) + 24 * s;
    Rect r{canvasR.r() - w - 14 * s, canvasR.y + 14 * s, w, 28 * s};
    ui.fill(r, cardBg, 14 * s);
    ui.stroke(r, cardBd, 14 * s);
    ui.text(r, chip, 11.5f * s, fg, AL_CENTER, 600);
  }
  // ---- zoom controls (bottom-right)
  {
    float bw = 34 * s;
    int nb = 7;
    Rect r{canvasR.r() - bw - 14 * s, canvasR.b() - (bw * nb + 10 * s) - 14 * s - (view == ViewKind::Timeline ? 40 * s : 0), bw, bw * nb + 10 * s};
    ui.fill(r, cardBg, 10 * s);
    ui.stroke(r, cardBd, 10 * s);
    chromeRects.push_back(r);
    float y = r.y + 5 * s;
    auto b = [&](const string& ic, const string& tip, bool on = false) { Rect br{r.x + 2 * s, y, bw - 4 * s, bw - 2 * s}; y += bw; return ui.iconButton(br, ic, tip, on); };
    auto zoomBy = [&](double f) {
      if (view == ViewKind::ThreeD) { nv.cam.dist3 = clampv(nv.cam.dist3 / f, 0.2, 6.0); return; }
      camTargetZoom = clampv((camTargetZoom > 0 ? camTargetZoom : nv.cam.zoom) * f, nv.fitZoom() * 0.2, nv.fitZoom() * 80);
      if (camTargetZoom > 0 && std::fabs(camTargetX - nv.cam.x) > 1e9) {}
      camTargetX = nv.cam.x; camTargetY = nv.cam.y;
    };
    if (b("plus", "Zoom in  (+)")) zoomBy(1.4);
    if (b("minus", "Zoom out  (\xE2\x88\x92)")) zoomBy(1 / 1.4);
    if (b("fit", "Fit to window  (F)")) { nv.fit(); camTargetZoom = -1; if (view == ViewKind::ThreeD) { nv.cam.yaw = -0.55f; nv.cam.pitch = 0.32f; } }
    if (b("tag", "Labels  (L)", showLabels)) showLabels = !showLabels;
    if (b("legend", "Legend  (G)", showLegend)) showLegend = !showLegend;
    if (b("grid", "Minimap  (M)", showMinimap)) showMinimap = !showMinimap;
    float ey = y;
    if (b("download", "Export the " + viewName(view) + " view as SVG, PDF or PNG  (Ctrl+Shift+E)")) ui.openPopup("viewexp");
    if (ui.isPopupOpen("viewexp")) {
      Rect anchor{r.x, ey, 0, bw};
      ui.overlay([this, anchor, s]() {
        Rect pr{anchor.x - 258 * s, anchor.y + anchor.h - 4 * 32 * s - 18 * s - 20 * s, 250 * s, 4 * 32 * s + 18 * s + 20 * s};
        ui.shadow(pr, 8 * s);
        ui.fill(pr, ui.c.panel2, 8 * s);
        ui.stroke(pr, ui.c.border, 8 * s);
        ui.popupRect(pr);
        ui.text({pr.x + 12 * s, pr.y + 4 * s, pr.w, 18 * s}, "Export the " + viewName(view) + " view", 12 * s, ui.c.textDim, AL_LEFT, 600);
        const char* f[3] = {"svg", "pdf", "png"};
        const char* lbl[3] = {"SVG  (vector, editable)", "PDF  (vector)", "PNG  (2\xC3\x97 screen resolution)"};
        for (int i = 0; i < 3; i++) {
          Rect rr{pr.x + 4 * s, pr.y + 24 * s + i * 32 * s, pr.w - 8 * s, 30 * s};
          if (ui.listRow(rr, string("vx") + f[i], false)) { ui.closePopup(); cmdExportCurrentView(f[i]); }
          ui.text({rr.x + 10 * s, rr.y, rr.w, rr.h}, lbl[i], 12.5f * s, ui.c.text);
        }
        float ly = pr.y + 24 * s + 3 * 32 * s + 4 * s;
        ui.line(pr.x + 10 * s, ly, pr.r() - 10 * s, ly, ui.c.border);
        Rect rr{pr.x + 4 * s, ly + 6 * s, pr.w - 8 * s, 30 * s};
        if (ui.listRow(rr, "vxfig", false)) { ui.closePopup(); page = PG_PUBLISH; }
        ui.icon("publish", rr.x + 14 * s, rr.y + rr.h / 2, 14 * s, ui.c.textDim, 1.6f);
        ui.text({rr.x + 30 * s, rr.y, rr.w - 34 * s, rr.h}, "Publication figure (any views)\xE2\x80\xA6", 12.5f * s, ui.c.text);
      });
    }
  }
  // ---- minimap (bottom-left, 2D only)
  if (showMinimap && view != ViewKind::ThreeD && !(view == ViewKind::Geo && !geoModeA) && N.n() > 0) {  // the corpus overview hides the nodes
    Rect r{canvasR.x + 14 * s, canvasR.b() - 124 * s - 14 * s - (view == ViewKind::Timeline ? 40 * s : 0), 184 * s, 124 * s};
    ui.fill(r, cardBg, 10 * s);
    ui.stroke(r, cardBd, 10 * s);
    chromeRects.push_back(r);
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (size_t i = 0; i < nv.pos.size(); i++) {
      if (nv.flags.size() == nv.pos.size() && (nv.flags[i] & NF_HIDDEN)) continue;
      x0 = std::min(x0, double(nv.pos[i][0])); x1 = std::max(x1, double(nv.pos[i][0]));
      y0 = std::min(y0, double(nv.pos[i][1])); y1 = std::max(y1, double(nv.pos[i][1]));
    }
    if (x1 >= x0) {  // at least one item shown (a single item, or items on one line, has a zero extent)
      Rect in = inset(r, 10 * s, 10 * s);
      const Encoder& e = nv.enc();
      int step = std::max(1, N.n() / 2500);
      auto hidden = [&](int i) { return nv.flags.size() == nv.pos.size() && (nv.flags[size_t(i)] & NF_HIDDEN); };
      // the scale: the items' extent plus their marks must fit the box (a map of two items with large circles used
      // to spill over its edges). VOS-style marks have a screen size (capped here), Studio-style ones a world size.
      const double rCap = 0.2 * std::min(in.w, in.h);
      double rwMax = 0, rpxMax = 0;
      for (int i = 0; i < N.n(); i += step) {
        if (hidden(i)) continue;
        if (e.vos()) rpxMax = std::max(rpxMax, std::min(rCap, e.vosRadiusPx(i) * s * 0.28));
        else rwMax = std::max(rwMax, e.radius(i) * 0.9);
      }
      const double spanX = x1 - x0, spanY = y1 - y0;
      auto fitK = [&](double w, double h, double pad) {  // px per world unit so that the extent (plus a world pad) fits w x h
        double sx = spanX + 2 * pad, sy = spanY + 2 * pad;
        double kx = sx > 1e-9 ? w / sx : 1e18, ky = sy > 1e-9 ? h / sy : 1e18;
        double kk = std::min(kx, ky);
        return kk >= 1e17 || !(kk > 0) ? 1.0 : kk;
      };
      const double k = e.vos() ? fitK(std::max(4.0, in.w - 2 * rpxMax), std::max(4.0, in.h - 2 * rpxMax), 0) : fitK(in.w, in.h, rwMax);
      double ox = in.x + (in.w - spanX * k) / 2, oy = in.y + (in.h - spanY * k) / 2;
      ui.pushClip(r);
      for (int i = 0; i < N.n(); i += step) {
        if (hidden(i)) continue;
        float px = float(ox + (nv.pos[size_t(i)][0] - x0) * k), py = float(oy + (nv.pos[size_t(i)][1] - y0) * k);
        float rr = std::max(1.2f * s, float(e.vos() ? std::min(rCap, e.vosRadiusPx(i) * s * 0.28) : e.radius(i) * k * 0.9));
        ui.fill({px - rr, py - rr, rr * 2, rr * 2}, e.nodeColor(i, view).withA(0.85f), rr);
      }
      double wx0, wy0, wx1, wy1;
      nv.screenToWorld(canvasR.x, canvasR.y, wx0, wy0);
      nv.screenToWorld(canvasR.r(), canvasR.b(), wx1, wy1);
      Rect vr{float(ox + (wx0 - x0) * k), float(oy + (wy0 - y0) * k), float((wx1 - wx0) * k), float((wy1 - wy0) * k)};
      ui.fill(vr, ui.c.text.withA(0.04f), 2 * s);
      ui.stroke(vr, ui.c.accent.withA(0.8f), 2 * s, 1.f * s);
      ui.popClip();
      bool over = r.has(ui.in.mx, ui.in.my);
      if (over) ui.cursor = "hand";
      if (over && ui.in.pressed[0] && ui.active == 0) drag = DR_MINIMAP;
      if (drag == DR_MINIMAP) {
        camTargetZoom = -1;
        nv.cam.x = x0 + (clampv(ui.in.mx, in.x, in.r()) - ox) / k;
        nv.cam.y = y0 + (clampv(ui.in.my, in.y, in.b()) - oy) / k;
        if (!ui.in.down[0]) drag = DR_NONE;
      }
    }
  }
  // ---- box selection
  if (drag == DR_BOX) {
    Rect br{std::min(dragX0, ui.in.mx), std::min(dragY0, ui.in.my), std::fabs(ui.in.mx - dragX0), std::fabs(ui.in.my - dragY0)};
    ui.fill(br, ui.c.accent.withA(0.1f));
    ui.stroke(br, ui.c.accent, 0, 1.2f * s);
  }
  // ---- hover card
  if (hover >= 0 && hover < N.n() && drag == DR_NONE) {
    const Node& nd = N.nodes[size_t(hover)];
    auto cols = clusterColors();
    vector<std::pair<string, string>> rows;
    for (size_t k = 0; k < N.weightNames.size() && k < nd.w.size(); k++) rows.push_back({N.weightNames[k], fmtNum(nd.w[k], nd.w[k] == std::floor(nd.w[k]) ? 0 : 2)});
    for (size_t k = 0; k < N.scoreNames.size() && k < nd.sc.size(); k++) if (std::isfinite(nd.sc[k])) rows.push_back({N.scoreNames[k], fmtNum(nd.sc[k], k == 0 ? 1 : 2)});
    float w = 250 * s;
    float lh = ui.textWrap({0, 0, w - 24 * s, 1000}, nd.label, 13.5f * s, fg, 650, false);
    float h = 16 * s + lh + 26 * s + rows.size() * 19 * s + 8 * s;
    float x = ui.in.mx + 18 * s, y = ui.in.my + 14 * s;
    if (x + w > canvasR.r() - 8 * s) x = ui.in.mx - w - 14 * s;
    if (y + h > canvasR.b() - 8 * s) y = ui.in.my - h - 10 * s;
    Rect r{x, y, w, h};
    ui.shadow(r, 10 * s, 14 * s);
    ui.fill(r, ui.c.panel2, 10 * s);
    ui.stroke(r, ui.c.border, 10 * s);
    ui.textWrap({r.x + 12 * s, r.y + 10 * s, w - 24 * s, lh + 4}, nd.label, 13.5f * s, ui.c.text, 650);
    float yy = r.y + 14 * s + lh;
    Color cc = cols.empty() ? ui.c.accent : cols[size_t(std::max(0, nd.cluster)) % cols.size()];
    ui.circle(r.x + 17 * s, yy + 10 * s, 5 * s, cc);
    ui.text({r.x + 28 * s, yy, w - 40 * s, 20 * s}, "Cluster " + std::to_string(nd.cluster + 1) + " \xC2\xB7 " + N.clusterName(nd.cluster), 11.5f * s, ui.c.textDim);
    yy += 24 * s;
    for (auto& rw : rows) {
      ui.text({r.x + 12 * s, yy, w * 0.6f, 18 * s}, rw.first, 11.5f * s, ui.c.textDim);
      ui.text({r.x + w * 0.5f, yy, w * 0.5f - 12 * s, 18 * s}, rw.second, 11.5f * s, ui.c.text, AL_RIGHT, 600);
      yy += 19 * s;
    }
  }
  // ---- 3D hint
  if (view == ViewKind::ThreeD) ui.text({canvasR.x, canvasR.b() - 28 * s, canvasR.w, 18 * s}, "Drag to orbit \xC2\xB7 wheel to zoom \xC2\xB7 click a node to inspect", 11 * s, dim.withA(0.8f), AL_CENTER);
}

void App::addChrome(const Rect& r) { chromeRects.push_back(r); }
void App::timelineRange(double* s0, double* s1, double* x0, double* x1) { *s0 = tlS0; *s1 = tlS1; *x0 = tlX0; *x1 = tlX1; }

void App::addChartHoverRegion(const Scene& scene, const Rect& bounds, float ox, float oy, float scale, uint64_t context) {
  if (scale <= 0 || bounds.w <= 0 || bounds.h <= 0) return;
  chartHoverRegions_.push_back({&scene, bounds, ui.activeClip(), ui.hasClip(), ox, oy, scale, context});
}

bool App::chartHoverTargetAt(float x, float y, uint64_t& target) const {
  target = 0;
  for (auto it = chartHoverRegions_.rbegin(); it != chartHoverRegions_.rend(); ++it) {
    if (!it->scene || !it->bounds.has(x, y) || (it->clipped && !it->clip.has(x, y))) continue;
    const Scene& scene = *it->scene;
    const double sx = (x - it->ox) / it->scale, sy = (y - it->oy) / it->scale;
    uint64_t item = 0;
    const int tag = hitTag(scene, sx, sy);
    const Prim* p = tipAt(scene, sx, sy);
    uint64_t primitive = 0;
    if (p && !scene.items.empty()) {
      const std::ptrdiff_t index = p - scene.items.data();
      if (index >= 0 && size_t(index) < scene.items.size() && uint64_t(index) < 0xffffffffull) primitive = uint64_t(index) + 1;
    }
    if (tag >= 0) item = (uint64_t(uint32_t(tag) + 1) << 32) | primitive;
    else if (primitive) item = (0xffffffffull << 32) | primitive;
    if (item) {
      uint64_t h = it->context ^ (item + 0x9e3779b97f4a7c15ull + (it->context << 6) + (it->context >> 2));
      h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 27; h *= 0x94d049bb133111ebull; h ^= h >> 31;
      target = h ? h : 0x6000000000000001ull;
    }
    return true;  // the topmost chart owns blank space too; do not hit a chart/map below it
  }
  return false;
}

uint64_t App::pointerHoverIdAt(float x, float y) {
  if (captionBtn(1).has(x, y)) return 0x2000000000000001ull;  // system-handled maximize button still has a hover state
  uint64_t target = 0;
  if (readerOpen && readerHoverTargetAt(x, y, target)) return target;
  target = ui.hoverTargetAt(x, y);
  if (target) return target;
  bool overChrome = false;
  for (const Rect& r : chromeRects) if (r.has(x, y)) { overChrome = true; break; }
  if (!overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen && chartHoverTargetAt(x, y, target)) return target;
  if (!overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen && splitActive()) {
    for (int i = 3; i >= 0; --i) {
      const LivePane& L = livePane_[i];
      if (!L.ready || L.vp.w <= 0 || !L.vp.has(x, y)) continue;
      if (L.chrome.has(x, y)) return 0;
      const int node = L.nv.hitNode(x, y);
      return node >= 0 ? (0x9000000000000000ull | (uint64_t(i + 1) << 32) | uint64_t(node + 1)) : 0;
    }
  }

  const bool networkCanvas = hasMap() && workspace != WS_BIBLIOGRAPHY && view != ViewKind::Matrix && !mainChartOpen && !(papersOpen && hasCorpus()) &&
                             !writerOpen && !readerOpen && page != PG_AI && !(splitActive() && splitMapPane() < 0);
  if (networkCanvas && canvasR.has(x, y)) {
    bool overChrome = false;
    for (const Rect& r : chromeRects) if (r.has(x, y)) { overChrome = true; break; }
    if (!overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen) {
      const int node = nv.hitNode(x, y);
      if (node >= 0) return 0x8000000000000000ull | uint64_t(node + 1);
      if (view == ViewKind::Geo && (geoModeA || geoDocsMax > 0)) {
        const int country = geoHit(x, y);
        return country >= 0 ? (0xB000000000000000ull | uint64_t(country + 1)) : 0;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

bool App::pointerMoveNeedsFrame(float x, float y) {
  if (input.down[0] || input.down[1] || input.down[2] || ui.active != 0 || maxBtnDown) return true;  // drag/capture paths stay full-rate
  const uint64_t target = pointerHoverIdAt(x, y);
  const bool changed = target != pointerHoverTarget_;
  pointerHoverTarget_ = target;

  return changed;
}

void App::canvasInput() {
  const Input& in = ui.in;
  float s = ui.s;
  bool overChrome = false;
  for (auto& r : chromeRects) if (r.has(in.mx, in.my)) overChrome = true;
  bool inside = canvasR.has(in.mx, in.my) && !overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen;
  bool free = ui.active == 0 && drag != DR_MINIMAP;
  bool is3D = view == ViewKind::ThreeD;
  bool canMove = !is3D && (view == ViewKind::Network || view == ViewKind::Overlay || view == ViewKind::Density);
  // hover
  if (drag == DR_NONE) {
    int h = inside && free ? nv.hitNode(in.mx, in.my) : -1;
    if (h != hover) { hover = h; needFrame = true; }  // fallback when this frame's newly drawn chrome changes the hit area
  }
  if (view == ViewKind::Geo) geoChrome(inside && free && hover < 0 && (drag == DR_NONE || drag == DR_PAN));
  if (hover >= 0 && drag == DR_NONE) ui.cursor = canMove ? "hand" : "hand";
  // wheel
  const float zoomGesture = in.wheel + in.pinch;
  if (inside && zoomGesture != 0) {
    if (is3D) nv.cam.dist3 = clampv(nv.cam.dist3 * std::pow(0.88, double(zoomGesture)), 0.2, 6.0);
    else {
      double z0 = camTargetZoom > 0 ? camTargetZoom : nv.cam.zoom;
      double cx0 = camTargetZoom > 0 ? camTargetX : nv.cam.x, cy0 = camTargetZoom > 0 ? camTargetY : nv.cam.y;
      double z1 = clampv(z0 * std::pow(1.2, double(zoomGesture)), nv.fitZoom() * 0.15, nv.fitZoom() * 120);
      // keep the world point under the cursor fixed (computed against the target camera)
      double mxw = cx0 + (in.mx - (canvasR.x + canvasR.w / 2)) / z0, myw = cy0 + (in.my - (canvasR.y + canvasR.h / 2)) / z0;
      camTargetZoom = z1;
      camTargetX = mxw - (in.mx - (canvasR.x + canvasR.w / 2)) / z1;
      camTargetY = myw - (in.my - (canvasR.y + canvasR.h / 2)) / z1;
    }
  }
  // press
  if (inside && free && drag == DR_NONE) {
    if (in.dbl) {
      if (hover >= 0) focusOn(hover);
      else { nv.fit(); camTargetZoom = -1; clusterFilter = -1; }
      return;
    }
    if (in.pressed[0]) {
      dragX0 = lastMx = in.mx; dragY0 = lastMy = in.my; dragMoved = false;
      if (hover >= 0) { drag = DR_NODE; dragNode = hover; }
      else if (in.shift && !is3D) drag = DR_BOX;
      else drag = is3D ? DR_ORBIT : DR_PAN;
    } else if (in.pressed[1] || in.pressed[2]) {
      dragX0 = lastMx = in.mx; dragY0 = lastMy = in.my; dragMoved = false;
      drag = is3D ? DR_ORBIT : DR_PAN;
      dragNode = -1;
    }
  }
  if (drag == DR_NONE || drag == DR_MINIMAP) return;
  float dx = in.mx - lastMx, dy = in.my - lastMy;
  if (std::hypot(in.mx - dragX0, in.my - dragY0) > 3 * s) dragMoved = true;
  bool anyDown = in.down[0] || in.down[1] || in.down[2];
  switch (drag) {
    case DR_PAN:
      if (dragMoved) { ui.cursor = "move"; camTargetZoom = -1; nv.cam.x -= dx / nv.cam.zoom; nv.cam.y -= dy / nv.cam.zoom; }
      break;
    case DR_ORBIT:
      if (dragMoved) { nv.cam.yaw += dx * 0.008f; nv.cam.pitch = clampv(nv.cam.pitch + dy * 0.006f, -1.45f, 1.45f); nv.nodesDirty = true; }
      break;
    case DR_NODE:
      if (dragMoved && is3D) { drag = DR_ORBIT; break; }
      if (dragMoved && canMove && dragNode >= 0) {
        if (dragSnap.pos.empty()) { pushUndo("Move item"); dragSnap = P->snapshot("drag"); }
        double wx, wy;
        nv.screenToWorld(in.mx, in.my, wx, wy);
        Node& nd = P->net.nodes[size_t(dragNode)];
        nd.x = wx; nd.y = wy;
        if (!nd.pinned) { nd.pinned = true; invalidateFlags(); }
        nv.pos[size_t(dragNode)][0] = float(wx);
        nv.pos[size_t(dragNode)][1] = float(wy);
        nv.positionsChanged();
        nv.nodesDirty = nv.linksDirty = true;
        ui.cursor = "move";
      }
      break;
    case DR_BOX: ui.cursor = "cross"; break;
    default: break;
  }
  lastMx = in.mx; lastMy = in.my;
  if (!anyDown) {
    if (!dragMoved) {
      if (drag == DR_NODE && dragNode >= 0 && !in.touchCancel) selectNode(dragNode, in.ctrl);
      else if (drag == DR_PAN || drag == DR_ORBIT) { if (in.released[0]) { clearSelection(); } }
    } else if (drag == DR_BOX) {
      Rect br{std::min(dragX0, in.mx), std::min(dragY0, in.my), std::fabs(in.mx - dragX0), std::fabs(in.my - dragY0)};
      auto v = nv.nodesInRect(br);
      bool selectionChanged = false;
      if (!in.ctrl && !selection.empty()) { selection.clear(); selectionChanged = true; }
      for (int i : v) if (std::find(selection.begin(), selection.end(), i) == selection.end()) { selection.push_back(i); selectionChanged = true; }
      if (selectionChanged) invalidateFlags();
      if (!v.empty()) ui.toast("Selection", plural(long(selection.size()), P->net.unitNoun) + " selected", 0, 1.6);
    } else if (drag == DR_NODE && !dragSnap.pos.empty()) {
      P->dirty = true;
      P->bundles = Bundles();
      if (P->style.bundle) styleDirty = true;
      pos3dValid = false;
      figSig.clear();
    }
    dragSnap = Snapshot();
    drag = DR_NONE;
    dragNode = -1;
  }
}

// =====================================================================================
// inspector (right)
// =====================================================================================
void App::drawInspector() {
  if (rightR.w <= 0.5f || !canInspect()) return;
  // slides in from the right edge: laid out at full width, the off-screen part is simply not visible
  Rect realR = rightR;
  rightR = {realR.x, realR.y, std::round(rightW), realR.h};
  drawInspectorBody();
  rightR = realR;
}

void App::drawInspectorBody() {
  float s = ui.s;
  ui.fill(rightR, ui.c.panel);
  ui.line(rightR.x + 0.5f, rightR.y, rightR.x + 0.5f, rightR.b(), ui.c.border);
  const Network& N = P->net;
  auto ccols = clusterColors();
  ui.beginScroll("inspector", rightR);
  Lay L{rightR.x + 16 * s, rightR.y + 14 * s - ui.scrollY(), rightR.w - 32 * s, 8 * s};
  float y0 = L.y;
  ui.pushId("insp");
  if (docPreview >= 0 && size_t(docPreview) < P->corpus.recs.size()) {
    drawDocPreview(L);
  } else if (!hasMap()) {
  } else if (view == ViewKind::Geo && geoMode() == 2 && geoSel >= 0) {
    drawCountryProfile(L, geoSel, false);
  } else if (selection.size() == 1 && selection[0] < N.n()) {
    int i = selection[0];
    const Node& nd = N.nodes[size_t(i)];
    Rect top = L.row(18 * s);
    ui.text(top, "Selected " + N.unitNoun, 12 * s, ui.c.textDim, AL_LEFT, 600);
    if (ui.iconButton({top.r() - 22 * s, top.y - 3 * s, 22 * s, 22 * s}, "x", "Clear selection (Esc)")) { clearSelection(); }
    float h = ui.textWrap({L.x, L.y, L.w, 1000}, nd.label, 16 * s, ui.c.text, 700);
    L.y += h + 6 * s;
    Rect cr = L.row(26 * s);
    Color cc = ccols.empty() ? ui.c.accent : ccols[size_t(std::max(0, nd.cluster)) % ccols.size()];
    string cl = "Cluster " + std::to_string(nd.cluster + 1) + " \xC2\xB7 " + N.clusterName(nd.cluster);
    float cw = std::min(L.w, ui.textW(cl, 11.5f * s, 600) + 30 * s);
    Rect chip{cr.x, cr.y, cw, cr.h};
    if (ui.listRow(chip, "clchip", false)) focusCluster(nd.cluster);
    ui.fill(chip, cc.withA(0.16f), 13 * s);
    ui.circle(chip.x + 13 * s, chip.y + chip.h / 2, 5 * s, cc);
    ui.text({chip.x + 24 * s, chip.y, chip.w - 28 * s, chip.h}, cl, 11.5f * s, ui.c.text, AL_LEFT, 600);
    L.space(4 * s);
    // actions
    auto ac = cols(L.row(30 * s), 3, 6 * s);
    if (ui.button(ac[0], "Focus", BTN_NORMAL, "target")) focusOn(i);
    if (ui.button(ac[1], nd.pinned ? "Unpin" : "Pin", BTN_NORMAL, "pin")) { pushUndo(nd.pinned ? "Unpin" : "Pin"); P->net.nodes[size_t(i)].pinned = !nd.pinned; invalidateFlags(); }
    if (ui.button(ac[2], "Copy", BTN_NORMAL, "copy")) { setClipboardText(hwnd, nd.label); ui.toast("Copied", nd.label, 1, 1.5); }
    if (ui.button(L.row(30 * s), "Explain with AI", BTN_GHOST, "sparkle", !aiLive)) aiRun(ai::Task::Explain);
    if (P->mapSource == "analysis" && !nd.key.empty()) {
      if (ui.button(L.row(30 * s), "Exclude from map and rebuild", BTN_GHOST, "eye-off", !busy())) {
        P->engine.exclSet(P->spec).insert(nd.key);
        ui.toast("Excluded", nd.label + ". Manage exclusions in Build.", 0, 3);
        cmdBuild();
      }
    }
    // metrics table
    sectionTitle(L, "Attributes");
    auto row = [&](const string& k, const string& v) {
      Rect r = L.row(20 * s);
      ui.text(r, k, 12 * s, ui.c.textDim);
      ui.text(r, v, 12 * s, ui.c.text, AL_RIGHT, 600);
    };
    for (size_t k = 0; k < N.weightNames.size() && k < nd.w.size(); k++) row(N.weightNames[k], fmtNum(nd.w[k], nd.w[k] == std::floor(nd.w[k]) ? 0 : 2));
    for (size_t k = 0; k < N.scoreNames.size() && k < nd.sc.size(); k++) if (std::isfinite(nd.sc[k])) row(N.scoreNames[k], icontains(N.scoreNames[k], "year") ? fmtFixed(nd.sc[k], 1) : fmtNum(nd.sc[k], k == 0 ? 1 : 2));  // years: no thousands separator
    if (P->metricsValid || N.n() <= 3000) {
      const ItemMetrics& M = P->itemMetricsCached();
      if (size_t(i) < M.degree.size()) {
        row("Degree", fmtNum(M.degree[size_t(i)], 0));
        row("Betweenness", fmtNum(M.betweenness[size_t(i)], 4));
        row("Participation", fmtNum(M.participation[size_t(i)], 2));
        row("Role", M.role[size_t(i)]);
      }
    }
    // neighbours
    vector<std::pair<double, int>> nb;
    for (auto& l : N.links) { if (l.a == i) nb.push_back({l.w, l.b}); else if (l.b == i) nb.push_back({l.w, l.a}); }
    std::sort(nb.begin(), nb.end(), [](auto& a, auto& b) { return a.first > b.first; });
    sectionTitle(L, "Strongest links (" + std::to_string(nb.size()) + ")");
    double mx = nb.empty() ? 1 : nb[0].first;
    for (size_t k = 0; k < std::min<size_t>(12, nb.size()); k++) {
      int j = nb[k].second;
      Rect r = L.row(24 * s);
      if (ui.listRow(r, "nb" + std::to_string(j), false)) focusOn(j);
      Color c2 = ccols.empty() ? ui.c.accent : ccols[size_t(std::max(0, N.nodes[size_t(j)].cluster)) % ccols.size()];
      ui.fill({r.x, r.b() - 3 * s, float(r.w * nb[k].first / mx), 2 * s}, c2.withA(0.5f), 1 * s);
      ui.circle(r.x + 8 * s, r.y + r.h / 2 - 1 * s, 4 * s, c2);
      ui.text({r.x + 18 * s, r.y, r.w - 64 * s, r.h - 2 * s}, N.nodes[size_t(j)].label, 12 * s, ui.c.text);
      ui.text({r.r() - 50 * s, r.y, 48 * s, r.h - 2 * s}, fmtNum(nb[k].first, nb[k].first == std::floor(nb[k].first) ? 0 : 2), 11 * s, ui.c.textDim, AL_RIGHT);
    }
    // country maps: a compact profile of the country (growth, partners, keywords)
    geoUpdate();
    if (geoModeA && hasCorpus() && size_t(i) < geoNodeCountry.size() && geoNodeCountry[size_t(i)] >= 0) drawCountryProfile(L, geoNodeCountry[size_t(i)], true);
    // documents
    if (!nd.recs.empty() && hasCorpus()) {
      vector<int> recs = nd.recs;
      std::sort(recs.begin(), recs.end(), [&](int a, int b) { return P->corpus.recs[size_t(a)].cites > P->corpus.recs[size_t(b)].cites; });
      sectionTitle(L, "Documents (" + std::to_string(recs.size()) + ")", "Most cited first \xC2\xB7 click for a preview (abstract, keywords, links)");
      for (size_t k = 0; k < std::min<size_t>(25, recs.size()); k++) docRow(L, recs[k], "doc");
    }
  } else if (selection.size() > 1) {
    Rect top = L.row(18 * s);
    ui.text(top, "Selection", 12 * s, ui.c.textDim, AL_LEFT, 600);
    ui.text(L.row(26 * s), plural(long(selection.size()), N.unitNoun), 18 * s, ui.c.text, AL_LEFT, 700);
    std::map<int, int> byC;
    for (int i : selection) byC[N.nodes[size_t(i)].cluster]++;
    for (auto& kv : byC) {
      Rect r = L.row(22 * s);
      ui.circle(r.x + 6 * s, r.y + r.h / 2, 5 * s, ccols.empty() ? ui.c.accent : ccols[size_t(std::max(0, kv.first)) % ccols.size()]);
      ui.text({r.x + 18 * s, r.y, r.w - 60 * s, r.h}, N.clusterName(kv.first), 12 * s, ui.c.text);
      ui.text(r, std::to_string(kv.second), 12 * s, ui.c.textDim, AL_RIGHT);
    }
    auto ac = cols(L.row(30 * s), 2, 6 * s);
    if (ui.button(ac[0], "Pin all", BTN_NORMAL, "pin")) { pushUndo("Pin"); bool changed = false; for (int i : selection) if (!P->net.nodes[size_t(i)].pinned) { P->net.nodes[size_t(i)].pinned = true; changed = true; } if (changed) invalidateFlags(); }
    if (ui.button(ac[1], "Clear", BTN_NORMAL, "x")) clearSelection();
    sectionTitle(L, "Items");
    vector<int> sel = selection;
    std::sort(sel.begin(), sel.end(), [&](int a, int b) { return N.weight(a) > N.weight(b); });
    for (size_t k = 0; k < std::min<size_t>(60, sel.size()); k++) {
      Rect r = L.row(22 * s);
      if (ui.listRow(r, "si" + std::to_string(sel[k]), false)) focusOn(sel[k]);
      ui.text({r.x + 6 * s, r.y, r.w - 60 * s, r.h}, N.nodes[size_t(sel[k])].label, 12 * s, ui.c.text);
      ui.text(r, fmtNum(N.weight(sel[k]), 0), 11 * s, ui.c.textDim, AL_RIGHT);
    }
  } else {
    // map overview
    ui.text(L.row(18 * s), "Map", 12 * s, ui.c.textDim, AL_LEFT, 600);
    string title = N.description.empty() ? "Network map" : titleCase(N.description.substr(0, 1)) + N.description.substr(1);
    float h = ui.textWrap({L.x, L.y, L.w, 1000}, title, 15 * s, ui.c.text, 700);
    L.y += h + 8 * s;
    auto grid = [&](const string& a, const string& av, const string& b, const string& bv) {
      auto c2 = cols(L.row(54 * s), 2, 8 * s);
      for (int k = 0; k < 2; k++) {
        ui.text({c2[size_t(k)].x + 0 * s, c2[size_t(k)].y + 6 * s, c2[size_t(k)].w - 20 * s, 22 * s}, k ? bv : av, 18 * s, ui.c.text, AL_LEFT, 500);
        ui.text({c2[size_t(k)].x + 0 * s, c2[size_t(k)].y + 30 * s, c2[size_t(k)].w - 20 * s, 16 * s}, k ? b : a, 11 * s, ui.c.textDim);
      }
    };
    grid(titleCase(pluralWord(N.unitNoun)), fmtInt(N.n()), "Links", fmtInt(N.m()));
    grid("Clusters", std::to_string(N.nClusters), "Modularity Q", P->last.Q > 0 ? fmtFixed(P->last.Q, 3) : "n/a");
    sectionTitle(L, "Clusters", "Click to focus \xC2\xB7 rename and recolour in Analyse");
    std::vector<int> sizes(size_t(std::max(0, N.nClusters)), 0);
    for (auto& nd : N.nodes) if (nd.cluster >= 0 && nd.cluster < N.nClusters) sizes[size_t(nd.cluster)]++;
    for (int c = 0; c < N.nClusters && c < 40; c++) {
      Rect r = L.row(28 * s);
      if (ui.listRow(r, "cl" + std::to_string(c), clusterFilter == c)) { if (clusterFilter == c) clusterFilter = -1; else focusCluster(c); }
      ui.circle(r.x + 10 * s, r.y + r.h / 2, 6 * s, ccols[size_t(c) % ccols.size()]);
      ui.text({r.x + 24 * s, r.y, r.w - 64 * s, r.h}, N.clusterName(c), 12.5f * s, ui.c.text, AL_LEFT, 550);
      ui.text({r.r() - 44 * s, r.y, 38 * s, r.h}, std::to_string(sizes[size_t(c)]), 11.5f * s, ui.c.textDim, AL_RIGHT);
    }
    sectionTitle(L, "Top items");
    vector<int> idx(static_cast<size_t>(N.n()));
    for (int i = 0; i < N.n(); i++) idx[size_t(i)] = i;
    std::partial_sort(idx.begin(), idx.begin() + std::min<size_t>(10, idx.size()), idx.end(), [&](int a, int b) { return N.weight(a) > N.weight(b); });
    for (size_t k = 0; k < std::min<size_t>(10, idx.size()); k++) {
      Rect r = L.row(24 * s);
      if (ui.listRow(r, "top" + std::to_string(idx[k]), false)) focusOn(idx[k]);
      ui.circle(r.x + 8 * s, r.y + r.h / 2, 4 * s, ccols[size_t(std::max(0, N.nodes[size_t(idx[k])].cluster)) % ccols.size()]);
      ui.text({r.x + 18 * s, r.y, r.w - 64 * s, r.h}, N.nodes[size_t(idx[k])].label, 12 * s, ui.c.text);
      ui.text(r, fmtNum(N.weight(idx[k]), 0), 11 * s, ui.c.textDim, AL_RIGHT);
    }
    sectionTitle(L, "Method");
    float mh = ui.textWrap({L.x, L.y, L.w, 1000}, P->methodsShort(), 11.5f * s, ui.c.textDim);
    L.y += mh + 6 * s;
    if (ui.button(L.row(30 * s), "Copy methods paragraph", BTN_GHOST, "copy")) { setClipboardText(hwnd, P->methods()); ui.toast("Copied", "Methods paragraph copied.", 1, 2); }
  }
  ui.popId();
  ui.endScroll(L.y - y0 + 30 * s);
}

// =====================================================================================
// command palette
// =====================================================================================
// ------------------------------------------------------------------ settings dialog
string App::openAlexKey() const { return trim(settings.j["openalexKey"].str()); }

void App::drawPalette() {
  if (!paletteOpen) return;
  float s = ui.s;
  ui.unblock();  // everything under the palette is blocked for this frame (App::frame); its own widgets take the mouse
  ui.fill({0, 0, float(g.W), float(g.H)}, Color(0, 0, 0, ui.dark ? 0.45f : 0.25f));
  float w = std::min(680 * s, float(g.W) - 80 * s);
  float top = std::min(90 * s, float(g.H) * 0.08f);
  Rect r{std::round((g.W - w) / 2), top, w, 0};
  // candidates: every command, ranked when there is a query (label prefix > word start > anywhere in the label > hint > all words)
  struct Item { string label, hint, sc, icon, group; std::function<void()> run; int score = 0; };
  vector<Item> items;
  string q = lower(trim(paletteQuery));
  vector<string> words;
  for (auto& wd : split(q, ' ')) if (!wd.empty()) words.push_back(wd);
  auto scoreOf = [&](const string& label, const string& hint) -> int {
    if (q.empty()) return 0;
    string l = lower(label);
    if (l.rfind(q, 0) == 0) return 400;
    size_t p = l.find(q);
    if (p != string::npos) return (l[p - 1] == ' ' || l[p - 1] == ':' || l[p - 1] == '(' || l[p - 1] == '/') ? 300 : 200;
    if (icontains(hint, q)) return 100;
    if (words.size() > 1) {
      string all = l + " " + lower(hint);
      for (auto& wd : words) if (all.find(wd) == string::npos) return -1;
      return 50;
    }
    return -1;
  };
  for (auto& c : commands) {
    if (workspace == WS_BIBLIOGRAPHY) {
      const string& id = c.id;
      if (c.group == "Map" || c.group == "Analyse" || id.rfind("page", 0) == 0 || id.rfind("view", 0) == 0 || id.rfind("split", 0) == 0 ||
          id == "fit" || id == "maphist" || id == "inspector" || id == "labels" || id == "legend" || id == "hulls" || id == "names" || id == "minimap" ||
          id == "clyears" || id == "trtopics" || id == "rpys" || id == "topdocs" || id == "topdoc" || id == "figall") continue;
    }
    int sc = scoreOf(c.label, c.hint);
    if (sc < 0) continue;
    items.push_back({c.label, c.hint, c.shortcut, c.icon, c.group, c.run, sc});
  }
  if (workspace == WS_VISUALIZATION && hasMap() && q.size() >= 2) {
    int k = 0;
    for (int i = 0; i < P->net.n() && k < 8; i++)
      if (icontains(P->net.nodes[size_t(i)].label, q)) {
        items.push_back({"Go to \xE2\x80\x9C" + P->net.nodes[size_t(i)].label + "\xE2\x80\x9D", "Cluster " + std::to_string(P->net.nodes[size_t(i)].cluster + 1), "", "search", "Items", [this, i] { focusOn(i); }, 90});
        k++;
      }
  }
  if (!q.empty()) std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.score > b.score; });
  // rows: sections with headers while browsing, one ranked list while searching
  struct Row { int item; const char* header; };
  vector<Row> rows;
  if (q.empty()) {
    for (const char* gname : kPaletteGroups) {
      bool any = false;
      for (size_t i = 0; i < items.size(); i++)
        if (items[i].group == gname) { if (!any) { rows.push_back({-1, gname}); any = true; } rows.push_back({int(i), nullptr}); }
    }
  } else for (size_t i = 0; i < items.size(); i++) rows.push_back({int(i), nullptr});
  vector<int> sel;                              // row of each selectable entry
  vector<int> selOfRow(rows.size(), -1);        // selectable index of a row
  for (size_t k = 0; k < rows.size(); k++) if (rows[k].item >= 0) { selOfRow[k] = int(sel.size()); sel.push_back(int(k)); }
  int n = int(sel.size());
  const float rowH = 40 * s, headH = 26 * s, footH = 30 * s;
  vector<float> rowY(rows.size());
  float contentH = 4 * s;
  for (size_t k = 0; k < rows.size(); k++) { rowY[k] = contentH; contentH += rows[k].header ? headH : rowH; }
  contentH += 4 * s;
  // as tall as the window allows: no more hidden commands
  float listMax = std::max(4 * rowH, float(g.H) - top - 56 * s - footH - 24 * s);
  float listH = n ? std::min(contentH, listMax) : 40 * s;
  r.h = 56 * s + listH + footH;
  ui.shadow(r, 14 * s, 30 * s);
  ui.fill(r, ui.c.panel2, 12 * s);
  ui.stroke(r, ui.c.borderStrong, 12 * s);
  ui.focus = ui.id("ti:palette");  // text inputs register as "ti:" + key
  bool submitted = false;
  string before = paletteQuery;
  ui.textInput({r.x + 10 * s, r.y + 10 * s, r.w - 20 * s, 38 * s}, "palette", paletteQuery, "Type a command, a page or an item name\xE2\x80\xA6", &submitted, "command");
  if (paletteQuery != before) { paletteSel = 0; ui.scrollSet("palette", 0); }
  int pageRows = std::max(1, int(listH / rowH) - 1);
  bool moved = false;
  for (int k : ui.in.keys) {
    if (k == VK_DOWN) { paletteSel++; moved = true; }
    if (k == VK_UP) { paletteSel--; moved = true; }
    if (k == VK_NEXT) { paletteSel += pageRows; moved = true; }
    if (k == VK_PRIOR) { paletteSel -= pageRows; moved = true; }
    if (k == VK_HOME) { paletteSel = 0; moved = true; }
    if (k == VK_END) { paletteSel = n - 1; moved = true; }
  }
  paletteSel = clampv(paletteSel, 0, std::max(0, n - 1));
  Rect listR{r.x, r.y + 56 * s, r.w, listH};
  if (n && moved) {  // keep the selection in view after a keyboard move (a section header comes along)
    int rk = sel[size_t(paletteSel)];
    float y0 = rowY[size_t(rk)] - ((rk > 0 && rows[size_t(rk) - 1].header) ? headH : 0) - 4 * s, y1 = rowY[size_t(rk)] + rowH + 4 * s;
    float cur = ui.scrollGet("palette");
    if (y0 < cur) ui.scrollSet("palette", y0);
    else if (y1 > cur + listH) ui.scrollSet("palette", y1 - listH);
    needFrame = true;
  }
  std::function<void()> run;
  bool inList = listR.has(ui.in.mx, ui.in.my);
  int selRow = n ? sel[size_t(paletteSel)] : -1;
  ui.beginScroll("palette", listR);
  float sy = ui.scrollY();
  for (size_t k = 0; k < rows.size(); k++) {
    float y = listR.y + rowY[k] - sy;
    float h = rows[k].header ? headH : rowH;
    if (y + h < listR.y || y > listR.b()) continue;
    if (rows[k].header) {
      ui.text({r.x + 18 * s, y + 7 * s, r.w - 36 * s, 16 * s}, rows[k].header, 10.5f * s, ui.c.textFaint, AL_LEFT, 700);
      continue;
    }
    const Item& it = items[size_t(rows[k].item)];
    Rect rr{r.x + 6 * s, y, r.w - 12 * s, rowH - 2 * s};
    bool hov = inList && rr.has(ui.in.mx, ui.in.my);
    if (hov && (ui.in.mx != lastMx || ui.in.my != lastMy)) { paletteSel = selOfRow[k]; selRow = int(k); }
    bool selected = int(k) == selRow;
    if (selected) ui.fill(rr, ui.c.accent.withA(0.16f), 7 * s);
    if (hov && ui.in.released[0]) run = it.run;
    ui.icon(it.icon, rr.x + 20 * s, rr.y + rr.h / 2, 16 * s, selected ? ui.c.accent : ui.c.textDim);
    ui.text({rr.x + 42 * s, rr.y + 2 * s, rr.w * 0.6f, 20 * s}, it.label, 13 * s, ui.c.text, AL_LEFT, 550);
    if (!it.hint.empty()) ui.text({rr.x + 42 * s, rr.y + 20 * s, rr.w - 170 * s, 16 * s}, it.hint, 11 * s, ui.c.textFaint);
    if (!it.sc.empty()) {
      float kw = ui.textW(it.sc, 11 * s, 600) + 14 * s;
      Rect kr{rr.r() - kw - 10 * s, rr.y + 9 * s, kw, 20 * s};
      ui.fill(kr, ui.c.input, 5 * s);
      ui.stroke(kr, ui.c.border, 5 * s);
      ui.text(kr, it.sc, 11 * s, ui.c.textDim, AL_CENTER, 600, true);
    }
  }
  ui.endScroll(contentH);
  if (!n) ui.text({r.x, listR.y, r.w, 30 * s}, "No matching commands", 12.5f * s, ui.c.textDim, AL_CENTER);
  // footer: what is listed, and the keys
  Rect fr{r.x, r.b() - footH, r.w, footH};
  ui.line(fr.x + 10 * s, fr.y + 0.5f, fr.r() - 10 * s, fr.y + 0.5f, ui.c.border);
  string what = q.empty() ? plural(long(n), "command") + " in " + plural(long(std::count_if(rows.begin(), rows.end(), [](const Row& rw) { return rw.header != nullptr; })), "section")
                          : (n ? std::to_string(n) + (n == 1 ? " match" : " matches") : string("No matches"));
  ui.text({fr.x + 16 * s, fr.y, fr.w / 2, fr.h}, what, 11 * s, ui.c.textFaint);
  ui.text({fr.x + fr.w / 2 - 16 * s, fr.y, fr.w / 2, fr.h}, "\xE2\x86\x91\xE2\x86\x93 move \xC2\xB7 PgUp/PgDn \xC2\xB7 Enter run \xC2\xB7 Esc close", 11 * s, ui.c.textFaint, AL_RIGHT);
  if (submitted && n) run = items[size_t(rows[size_t(sel[size_t(paletteSel)])].item)].run;
  if (ui.in.pressed[0] && !r.has(ui.in.mx, ui.in.my)) { paletteOpen = false; ui.focus = 0; }
  if (run) { paletteOpen = false; ui.focus = 0; run(); }
  lastMx = ui.in.mx; lastMy = ui.in.my;
}

// =====================================================================================
// script mode: one command per frame; used for automated screenshots and smoke tests
// =====================================================================================
static bool g_clickHeld = false;  // synthetic input for tests: a scripted click is released on the following frame

bool App::shotPending() const { return pendingShot; }
void App::shotRequest(const string& path, bool canvasOnly) { pendingShot = true; pendingShotPath = path; pendingViewCrop = canvasOnly; needFrame = true; }
const char* App::pageTitle(int p) { return p >= 0 && p < PG_COUNT ? kPageTitles[p] : ""; }

void App::stepScript() {
  if (g_clickHeld) { input.down[0] = false; input.released[0] = true; g_clickHeld = false; }
  if (scriptWait > 0) { scriptWait--; return; }
  if (busy() || (job && job->finished) || animT0 >= 0) return;
  if (scriptPos >= script.size()) { if (P) P->dirty = false; PostMessageW(hwnd, WM_CLOSE, 0, 0); scriptMode = false; return; }
  string line = script[scriptPos++];
  runCommand(line);
}

// One automation command. Shared by the script runner and by the AI's run_commands tool (agent.cpp), which filters the
// commands it allows and waits for jobs between them. `note` receives a short outcome for the AI where it matters.
bool App::runCommand(const string& line, string* note) {
  auto parts = split(line, ' ');
  string cmd = parts.empty() ? "" : parts[0];
  string arg = parts.size() > 1 ? trim(line.substr(cmd.size())) : "";
  auto say = [&](const string& t) { if (note) *note = t; };
  if (cmd == "sample") { cmdSample(arg == "scopus"); say("Loaded the " + string(arg == "scopus" ? "Scopus" : "Web of Science") + " sample: " + plural(long(P->corpus.recs.size()), "record") + " now loaded."); }
  else if (cmd == "open") {  // several files: a|b (e.g. VOSviewer map|network)
    vector<string> fs = split(arg, '|');
    vector<string> missing;
    for (auto& f : fs) { bool ok = false; (void)readFileU(f, &ok); if (!ok) missing.push_back(f); }
    if (!missing.empty()) say("Cannot read: " + join(missing, ", ") + ". Check the path (use the full path).");
    else { dropFiles(fs); say(busy() ? "Importing " + plural(long(fs.size()), "file") + "\xE2\x80\xA6" : "Opened."); }
  }
  else if (cmd == "build") { if (!hasCorpus()) say("No records are loaded, nothing to build."); else { cmdBuild(); say(busy() ? "Building the map\xE2\x80\xA6" : (lastJobError.empty() ? "The build did not start." : lastJobError)); } }
  else if (cmd == "type") { AnaType t; if (typeFromId(arg, t)) { P->spec.type = t; P->spec.unit = typeInfo(t).units[0].first; P->spec.setDefaults(); say("Analysis type " + string(typeInfo(t).label) + ", unit reset to " + string(unitId(P->spec.unit)) + "."); } else say("Unknown type \"" + arg + "\": use cooc, coauth, citation, coupling or cocit."); }
  else if (cmd == "unit") { Unit u; if (unitFromId(arg, u)) { P->spec.unit = u; P->spec.setDefaults(); say("Unit " + string(unitId(u)) + " (threshold reset to " + std::to_string(P->spec.min) + ")."); } else say("Unknown unit \"" + arg + "\": use allKeywords, keywords, indexTerms, terms, authors, orgs, countries, docs, sources, refs, csources or cauthors."); }
  else if (cmd == "min") { P->spec.min = toInt(arg, 1); say("Minimum occurrences " + std::to_string(P->spec.min) + "."); }
  else if (cmd == "view") { bool f = false; for (auto& v : kViews) if (iequals(v.label, arg) || (iequals(arg, "3d") && v.label == string("3D"))) { setView(v.v, false); f = true; } say(f ? "View: " + viewName(view) + "." : "Unknown view \"" + arg + "\": network, overlay, density, timeline, matrix, geo or 3d."); }
  else if (cmd == "page") { page = PG_NONE; for (int p = 0; p < PG_COUNT; p++) if (iequals(kPageTitles[p], arg)) page = p; if (page == PG_WRITER && !writerOpen) openWriter(); say(page >= 0 ? "Page " + string(kPageTitles[page]) + "." : "Unknown page \"" + arg + "\" (Data, Build, Look, Analyse, Trends, Actors, Read, Write, Publish, Assistant)."); }
  else if (cmd == "tab") { int t = toInt(arg); anaTab = trTab = acTab = t; say("Tab " + std::to_string(t) + "."); }
  else if (cmd == "theme") { setTheme(arg != "light"); say(string("Theme ") + (ui.dark ? "dark" : "light") + "."); }
  else if (cmd == "look") { bool f = false; for (auto& lk : lookPresets()) if (lk.id == arg) f = true; if (f) { applyLook(P->style, arg); P->style.darkTheme = ui.dark; styleDirty = true; if (hasMap()) { nv.fit(); camTargetZoom = -1; } say("Look " + arg + "."); } else say("Unknown look \"" + arg + "\": vosviewer, studio, paper or midnight."); }
  else if (cmd == "select") { runSearch(); search = arg; runSearch(); if (!searchHits.empty()) focusOn(searchHits[0]); camTargetZoom = -1; say(searchHits.empty() ? "No item matches \"" + arg + "\"." : "Selected " + P->net.nodes[size_t(searchHits[0])].label + " (" + plural(long(searchHits.size()), "hit") + ")."); }
  else if (cmd == "search") { search = arg; runSearch(); say(plural(long(searchHits.size()), "hit") + " for \"" + arg + "\"."); }
  else if (cmd == "clearsearch") { search.clear(); searchHits.clear(); invalidateFlags(); }
  else if (cmd == "hover") { input.mx = float(toDouble(split(arg, ' ')[0])); input.my = float(toDouble(split(arg, ' ').back())); }
  else if (cmd == "palette") { paletteOpen = true; paletteQuery = arg; }
  else if (cmd == "maxlines") { P->style.maxLines = toInt(arg, 1000); styleDirty = true; }
  else if (cmd == "canvascache") { setCanvasCache(arg != "0" && arg != "off"); say(string("Canvas cache ") + (canvasCacheOn ? "on." : "off.")); }
  else if (cmd == "textcache") { setTextCache(arg != "0" && arg != "off"); say(string("Text cache ") + (ui.textCacheOn ? "on." : "off.")); }
  else if (cmd == "perf") { perfHud = arg.empty() ? !perfHud : (arg != "0" && arg != "off"); perf_ = PerfWindow{}; perfShown_ = PerfWindow{}; perfT0_ = 0; perfCpuT_ = 0; say(string("Performance overlay ") + (perfHud ? "on." : "off.")); }
  else if (cmd == "linkgeom") { P->style.linkGeom = arg == "straight" ? LinkGeom::Straight : arg == "arc" ? LinkGeom::Arc : LinkGeom::Curved; styleDirty = true; }
  else if (cmd == "keys") { for (unsigned char ch : arg) if (ch >= 32 && ch < 127) input.chars.push_back(char32_t(ch)); }  // typed text (ASCII)
  else if (cmd == "key") input.keys.push_back(toInt(arg));                                                               // virtual-key code
  else if (cmd == "click") { input.mx = float(toDouble(split(arg, ' ')[0])); input.my = float(toDouble(split(arg, ' ').back())); input.down[0] = input.pressed[0] = true; g_clickHeld = true; }
  else if (cmd == "closepalette") paletteOpen = false;
  else if (cmd == "inspector") { inspectorOpen = arg != "off"; say(inspectorOpen ? (canInspect() ? "The inspector is open on the right." : "The inspector is switched on, but it only appears with a map or an open document preview.") : "The inspector is closed."); }
  else if (cmd == "hulls") { P->style.hulls = arg != "off"; styleDirty = true; }
  else if (cmd == "names") { P->style.clusterNames = arg != "off"; styleDirty = true; }
  else if (cmd == "bundle") { if (!hasMap()) say("There is no map."); else { cmdBundles(); say("Computing edge bundles\xE2\x80\xA6"); } }
  else if (cmd == "relayout") { if (!hasMap()) say("There is no map."); else { cmdRelayout(); say("Running the layout again\xE2\x80\xA6"); } }
  else if (cmd == "run") {  // trigger analysis runs used by the pages
    if (arg == "bursts") { if (!hasCorpus()) say("No records."); else { bursts = detectBursts(P->corpus, Unit(trUnit), burstMin, burstS, burstG); burstsValid = true; say(plural(long(bursts.size()), "burst") + " detected (Trends \xE2\x86\x92 Bursts)."); } }
    else if (arg == "stability") { if (!hasMap()) say("There is no map."); else { stab = clusterStability(P->net, P->params.clusterOpts(), stabRuns); stabValid = true; say("Cluster stability computed (Analyse \xE2\x86\x92 Stability): mean agreement " + fmtFixed(stab.meanAri, 2) + "."); } }
    else say("run bursts or run stability.");
  }
  else if (cmd == "expand") {
    if (arg == "strategic") {
      if (!hasMap()) say("There is no map, so there is no strategic diagram.");
      else { mainChart.title = "Strategic diagram"; mainChart.make = [this](double w, double h, const ChartTheme& t) { if (!cinfoValid) { cinfo = clusterInfo(P->net, &P->corpus); cinfoValid = true; } return chartStrategic(cinfo, clusterColors(), w, h, t); }; mainChartOpen = true; papersOpen = false; say("The strategic diagram fills the main area."); }
    } else say("expand strategic is the only expand target; use show_chart / expandflow for other charts.");
  }
  else if (cmd == "closechart") { say(mainChartOpen ? "The chart is closed; the map is shown again." : "No chart was open."); mainChartOpen = false; }
  else if (cmd == "split") {  // split <0|1|2|3|off|cols|rows|4> [chart ids for the panes, "map" for the live map]
    vector<string> a = splitAny(arg, " ,");
    auto norm = [](string x) { x = lower(trim(x)); for (auto& c : x) if (c == ' ' || c == '-') c = '_'; if (startsWith(x, "live_")) x = "live:" + x.substr(5); return x; };
    string m = a.empty() ? "1" : lower(a[0]);
    int mode = m == "0" || m == "off" || m == "single" || m == "none" ? 0 : m == "1" || m == "cols" || m == "columns" || m == "side" ? 1 : m == "2" || m == "rows" ? 2 : m == "3" || m == "4" || m == "quad" || m == "four" ? 3 : -1;
    if (mode < 0) say("split off | 1 (side by side) | 2 (above each other) | 4 (four panes) [chart ids...]");
    else if (mode && !hasMap()) say("There is no map yet; build one first (the split view shows the map and charts side by side).");
    else {
      setSplit(mode);
      int n = splitPanes();
      for (size_t i = 1; i < a.size() && int(i) - 1 < n; i++) setSplitChart(int(i) - 1, norm(a[i]));
      string what;
      for (int i = 0; i < n; i++) what += (i ? ", " : "") + (splitChart[i] == "map" ? string("the live map") : replaceAll(splitChart[i], "_", " "));
      say(mode ? "The main area shows " + std::to_string(n) + " panes: " + what + "." : "Single pane: the map alone.");
    }
  }
  else if (cmd == "pane") {  // pane <1..4> <chart id|map>
    vector<string> a = splitAny(arg, " ");
    auto norm = [](string x) { x = lower(trim(x)); for (auto& c : x) if (c == ' ' || c == '-') c = '_'; if (startsWith(x, "live_")) x = "live:" + x.substr(5); return x; };
    if (a.size() < 2) say("pane <1..4> <chart id | map | live:<view>>");
    else if (splitMode == 0) say("The split view is off; use split first.");
    else { int i = clampv(toInt(a[0], 1), 1, splitPanes()) - 1; setSplitChart(i, norm(a[1])); say("Pane " + std::to_string(i + 1) + " shows " + (splitChart[i] == "map" ? string("the live map") : replaceAll(splitChart[i], "_", " ")) + "."); }
  }
  else if (cmd == "svg" || cmd == "pdf" || cmd == "png") { if (!hasMap()) say("There is no map to export."); else if (arg.empty()) say("Give the file path."); else { cmdExportFigure(cmd, arg); bool ok = false; (void)readFileU(arg, &ok); say(ok ? "Wrote " + arg + "." : "The figure could not be written to " + arg + "."); } }
  else if (cmd == "viewsvg" || cmd == "viewpdf" || cmd == "viewpng") { if (!hasMap()) say("There is no map to export."); else if (arg.empty()) say("Give the file path."); else { cmdExportCurrentView(cmd.substr(4), arg); say("Exporting the " + viewName(view) + " view to " + arg + "."); } }
  else if (cmd == "legend") showLegend = arg != "off";
  else if (cmd == "geofill") geoFill = clampv(toInt(arg, 0), 0, 2);
  else if (cmd == "geodenstyle") geoDenStyle = arg == "clusters" ? 1 : arg == "countries" ? 2 : arg == "heat" ? 0 : clampv(toInt(arg, 0), 0, 2);
  else if (cmd == "geodenmeasure") geoDenMeasure = arg == "citations" ? 1 : arg == "percapita" || arg == "cpd" ? 2 : arg == "collaboration" ? 3 : clampv(toInt(arg, 0), 0, 3);
  // 1.8 time and comparison
  else if (cmd == "diffmap") {
    auto v = splitAny(arg, " ,");
    if (v.size() == 4) { cmpA0 = toInt(v[0], 0); cmpA1 = toInt(v[1], 0); cmpB0 = toInt(v[2], 0); cmpB1 = toInt(v[3], 0); }
    diffShowOnMap();
  }
  else if (cmd == "diffclear") diffClear();
  else if (cmd == "compare") {
    // compare periods a0 a1 b0 b1 | compare sources <a> <b> | compare thresholds <tA> <tB> | compare side|diff|off
    auto v = splitAny(arg, " ,");
    string k = v.empty() ? string() : lower(v[0]);
    if (k == "side" || k == "sidebyside") { string err; if (compareShowSideBySide(&err)) say("Side by side: " + compareSummary() + "."); else say("Compare: " + err); }
    else if (k == "diff" || k == "difference") { if (cmpKind == 2) say("Thresholds have no difference map."); else { diffCompute(); if (pdiff.ok) { diffShowOnMap(); say("Difference map shown: " + std::to_string(pdiff.counts[0]) + " appearing, " + std::to_string(pdiff.counts[1]) + " growing, " + std::to_string(pdiff.counts[2]) + " stable, " + std::to_string(pdiff.counts[3]) + " fading."); } else say("Compare: " + pdiff.error); } }
    else if (k == "off" || k == "close") { compareClose(); diffClear(); say("Comparison closed."); }
    else if (k == "periods" || k == "sources" || k == "thresholds") {
      cmpKind = k == "periods" ? 0 : k == "sources" ? 1 : 2;
      if (cmpKind == 0 && v.size() >= 5) { cmpA0 = toInt(v[1], 0); cmpA1 = toInt(v[2], 0); cmpB0 = toInt(v[3], 0); cmpB1 = toInt(v[4], 0); }
      if (cmpKind == 1 && v.size() >= 3) { cmpFileA = clampv(toInt(v[1], 1) - 1, 0, std::max(0, int(P->corpus.files.size()) - 1)); cmpFileB = clampv(toInt(v[2], 2) - 1, 0, std::max(0, int(P->corpus.files.size()) - 1)); }
      if (cmpKind == 2 && v.size() >= 3) { cmpThA = atof(v[1].c_str()); cmpThB = atof(v[2].c_str()); }
      cmpValid = false; pdiffValid = false;
      page = PG_TRENDS; trTab = 3;
      if (cmpSideOpen) compareShowSideBySide();
      say("Compare " + k + " set" + (hasMap() ? ": " + compareSummary() : string()) + ".");
    } else say("Usage: compare periods a0 a1 b0 b1 | compare sources <a> <b> | compare thresholds <tA> <tB> | compare side|diff|off");
  }
  else if (cmd == "scope") {
    // scope cluster <n> | scope off | scope link on|off
    auto v = splitAny(arg, " ");
    string k = v.empty() ? string() : lower(v[0]);
    if (k == "cluster" && v.size() >= 2) {
      int c = toInt(v[1], 0);
      if (!hasMap() || c < 1 || c > P->net.nClusters) say("Cluster numbers run from 1 to " + std::to_string(hasMap() ? P->net.nClusters : 0) + ".");
      else { focusCluster(c - 1); say(scopeActive() ? "Scope: " + scopeLabel_ + " \xC2\xB7 " + plural(long(scopeRecs_.size()), "record") + "." : linkScope ? "Cluster " + std::to_string(c) + " highlighted; it covers all records or the map has no record links." : "Cluster " + std::to_string(c) + " highlighted (linked selection is off)."); }
    } else if (k == "off" || k == "clear") { clearScope(); say("Scope cleared."); }
    else if (k == "link") { linkScope = v.size() < 2 || (v[1] != "0" && v[1] != "off"); settings.j.set("linkScope", linkScope); settings.save(); scopeActive(); say(string("Linked selection ") + (linkScope ? "on." : "off.")); }
    else say(scopeActive() ? "Scope: " + scopeLabel_ + " \xC2\xB7 " + plural(long(scopeRecs_.size()), "record") + " of " + fmtInt(long(P->corpus.recs.size())) + "." : "No scope. Usage: scope cluster <n> | scope off | scope link on|off");
  }
  else if (cmd == "mainpathroutes") mpRoutes = clampv(toInt(arg, 5), 0, 20);
  else if (cmd == "sweep") {
    if (!hasMap()) say("There is no map.");
    else {
      sweep = resolutionSweep(P->net, P->params.clusterOpts(), defaultSweepResolutions(), sweepSeeds);
      sweepValid = true;
      sweepSig = std::to_string(P->net.n()) + ":" + std::to_string(P->net.m()) + ":" + clusterTag(P->params.clusterOpts());
      string r;
      for (auto& sw : sweep.pts) r += (r.empty() ? "" : "; ") + fmtNum(sw.resolution, 2) + " \xE2\x86\x92 " + plural(sw.clusters, "cluster") + " (agreement " + fmtFixed(sw.meanAri, 2) + ")";
      if (sweep.best >= 0 && sweep.best < int(sweep.pts.size())) r += ". Suggested: " + fmtNum(sweep.pts[size_t(sweep.best)].resolution, 2);
      say("Resolution sweep: " + r + ". Apply one with useres <r>.");
    }
  }
  else if (cmd == "useres") { useResolution(toDouble(arg, 1.0)); say("Re-clustered at resolution " + arg + ": " + plural(P->net.nClusters, "cluster") + "."); }
  else if (cmd == "livingcheck") livingCheck(false);
  else if (cmd == "livingadd") livingAdd(arg == "rebuild");
  else if (cmd == "livingauto") { P->living.set("auto", toInt(arg, 1) != 0); P->dirty = true; }
  else if (cmd == "livingsince") P->living.set("lastCheck", arg);  // tests: pretend the last check was on this date
  else if (cmd == "geolayer") geoLayerKind = arg == "overlay" ? 1 : arg == "density" ? 2 : arg == "network" ? 0 : clampv(toInt(arg, 0), 0, 2);
  else if (cmd == "geosel") { geoUpdate(); geoSel = findCountry(arg); if (geoSel >= 0) { closeDoc(); inspectorOpen = true; } say(geoSel >= 0 || arg == "none" ? "Country selection: " + arg + "." : "No country named \"" + arg + "\"."); }
  else if (cmd == "preview") {  // preview <n> (0-based record number) | R<n> (the ids of read_papers, 1-based) | top | -1
    int r = -1;
    string a = lower(trim(arg));
    if (a == "top") { long best = -1; for (size_t i = 0; i < P->corpus.recs.size(); i++) if (P->corpus.recs[i].cites > best) { best = P->corpus.recs[i].cites; r = int(i); } }
    else if (!a.empty() && a[0] == 'r' && toInt(a.substr(1), 0) > 0) r = toInt(a.substr(1), 0) - 1;
    else r = toInt(a, -1);
    if (a.empty() || a == "-1" || a == "off" || a == "close") { closeDoc(); say("The document preview is closed."); }
    else if (!hasCorpus()) say("No records are loaded, so there is nothing to preview.");
    else if (r < 0 || size_t(r) >= P->corpus.recs.size()) say("There is no record " + arg + " (the records are R1 to R" + std::to_string(P->corpus.recs.size()) + ").");
    else {
      openDoc(r);
      const Record& d = P->corpus.recs[size_t(r)];
      say("Preview of R" + std::to_string(r + 1) + " is open in the inspector on the right: " + truncate(trim(d.title), 90) + (d.year ? " (" + std::to_string(d.year) + ")" : string()) + ".");
    }
  }
  else if (cmd == "papers") {  // papers [off | <filter words>] — the papers table in the main area
    if (arg == "off" || arg == "0") { papersOpen = false; say("The papers table is closed."); }
    else if (!hasCorpus()) say("No records are loaded, so there is no papers table to show.");
    else { openPapers(arg == "on" || arg == "1" ? string() : arg); say(papersSummary(0)); }
  }
  else if (cmd == "writer") {  // writer [on|off|pdf|docx|html|all|compile|preview|openpdf [path]] — the document editor / exports
    string a = lower(trim(arg)), rest;
    size_t sp = a.find(' ');
    if (sp != string::npos) { rest = trim(arg.substr(sp + 1)); a = a.substr(0, sp); }
    if (a == "off" || a == "0" || a == "close") { closeWriter(); say("The writer is closed."); }
    else if (a == "compile" || a == "recompile") {
      if (writerPdfCompileBusy_) say("A Writer LaTeX compile is already in progress.");
      else if (busy()) say(jobLabel + " is still running.");
      else if (wdoc.empty()) say("Add content to the document before compiling a PDF.");
      else { writerCompileLatex(); say(writerPdfCompileBusy_ ? "Compiling the Writer's LaTeX preview." : "The LaTeX preview could not be started."); }
    }
    else if (a == "preview") { bool on = rest.empty() ? !writerPdfPreviewOpen_ : lower(rest) != "off" && rest != "0"; writerSetPdfPreview(on); say(on ? "The PDF preview is shown beside the document." : "The PDF preview is hidden."); }
    else if (a == "openpdf") { if (writerOpenPdfSeparately()) say("Opening the latest compiled PDF in the system viewer."); }
    else if (a == "pdf" || a == "docx" || a == "word" || a == "html") { string pth = writerExport(a == "word" ? "docx" : a, rest.empty() ? reportsDir() + "\\" + writerSlug() + "." + (a == "word" ? "docx" : a) : rest); say(pth.empty() ? "The document could not be exported." : "Exported the document as " + pth + "."); }
    else if (a == "all") { string base = rest.empty() ? reportsDir() + "\\" + writerSlug() : rest; string p1 = writerExport("pdf", base + ".pdf"), p2 = writerExport("docx", base + ".docx"), p3 = writerExport("html", base + ".html"); say(p1.empty() && p2.empty() && p3.empty() ? "The document could not be exported." : "Exported " + join(vector<string>{p1, p2, p3}, ", ") + "."); }
    else { openWriter(); say(wdoc.empty() ? "The writer is open with an empty document." : "The writer is open: " + writerSummary() + "."); }
  }
  else if (cmd == "maps") { restoreMap(toInt(arg, 0)); say("Map " + arg + " of the history is shown (" + plural(long(mapHistory.size()), "map") + " in the history)."); }
  else if (cmd == "scroll") { if (arg.rfind("insp", 0) == 0) ui.scrollTo("inspector", float(toDouble(split(arg, ' ').back()))); else if (page >= 0) ui.scrollTo("page" + std::to_string(page), float(toDouble(arg))); }
  else if (cmd == "popup") ui.openPopup(arg);
  else if (cmd == "docrank") docRank = clampv(toInt(arg, 0), 0, 2);
  else if (cmd == "cymode") cyMode = clampv(toInt(arg, 0), 0, 1);
  else if (cmd == "oaquery") oaQuery = replaceAll(arg, " | ", "\n");
  else if (cmd == "oamax") oaMax = arg;
  else if (cmd == "oafrom") oaFrom = arg;
  else if (cmd == "oafetch") { oaKind = 0; cmdOpenAlex(); say(busy() ? "Searching OpenAlex\xE2\x80\xA6" : (lastJobError.empty() ? "The search did not start (is a query set?)." : lastJobError)); }
  else if (cmd == "getpdf") {  // getpdf [all|R3,R7] [retry]
    if (!hasCorpus()) say("No records loaded.");
    else {
      vector<string> toks = splitAny(lower(arg), " ,;");
      bool retry = false;
      vector<int> among;
      for (auto& x : toks) {
        if (x == "retry") retry = true;
        else if (x.size() > 1 && x[0] == 'r' && isdigit(uint8_t(x[1]))) { int rec = toInt(x.substr(1), 0) - 1; if (rec >= 0 && size_t(rec) < P->corpus.recs.size()) among.push_back(rec); }
      }
      if (among.empty()) for (size_t i = 0; i < P->corpus.recs.size(); i++) among.push_back(int(i));
      vector<int> cands;
      for (int rec : among) {
        const Record& r = P->corpus.recs[size_t(rec)];
        if (fetchRecordHasFile(rec) || oa::normDoi(r.doi).empty()) continue;
        auto f = P->library.fetch.find(PdfLibrary::keyOf(r));
        if (!retry && f != P->library.fetch.end() && f->second.needsFile()) continue;
        cands.push_back(rec);
      }
      if (cands.empty()) say("Nothing to fetch. " + fetchSummary());
      else if (fetchPdfs(cands)) say("Getting " + plural(long(cands.size()), "PDF") + "\xE2\x80\xA6");
      else say(lastJobError.empty() ? string("The download did not start.") : lastJobError);
    }
  }
  else if (cmd == "rdview") {  // rdview one|two|cover|turn|turnback|upright|print  (the open PDF's view)
    const string v = lower(trim(arg));
    if (!readerOpen && v != "one" && v != "two" && v != "cover") say("No PDF is open in the reader.");
    else if (v == "one") readerSetLayout(false, readerCoverAlone());
    else if (v == "two") readerSetLayout(true, readerCoverAlone());
    else if (v == "cover") readerSetLayout(true, !readerCoverAlone());
    else if (v == "turn") readerRotate(1);
    else if (v == "turnback") readerRotate(-1);
    else if (v == "upright") readerRotate(-readerRotation());
    else if (v == "print") readerPrintDialog();
    else say("rdview one|two|cover|turn|turnback|upright|print");
  }
  else if (cmd == "start") showStart = arg != "0";
  else if (cmd == "ai") {  // ai <task-id> [extra text]
    string id = parts.size() > 1 ? parts[1] : "chat";
    string extra;
    for (size_t k = 2; k < parts.size(); k++) extra += (k > 2 ? " " : "") + parts[k];
    for (auto& ti : ai::tasks()) if (id == ti.id) { if (ti.task == ai::Task::Chat) { page = PG_AI; aiSend(ai::Task::Chat, extra); } else aiRun(ti.task, extra); }
  }
  else if (cmd == "aiinput") { aiInput = arg; }
  else if (cmd == "settab") { setTab = clampv(toInt(arg, 0), 0, 3); }
  else if (cmd == "aiconfig") {  // aiconfig <provider 0-3> <model> <base url> [key]
    int pv = parts.size() > 1 ? clampv(toInt(parts[1], 0), 0, 3) : 3;
    settings.j.set("aiProvider", pv);
    if (parts.size() > 2) settings.j.set("aiModel" + std::to_string(pv), parts[2]);
    if (parts.size() > 3) settings.j.set("aiBase", parts[3]);
    string key = parts.size() > 4 ? parts[4] : string();
    string enc = protectSecret(key);
    settings.j.set("aiKey" + std::to_string(pv), enc);
    settings.j.set("aiKeyPlain" + std::to_string(pv), enc.empty() ? key : string());
    aiCfgValid_ = false;
  }
  else if (cmd == "aiwait") { if (aiLive) { scriptPos--; scriptWait = 5; } }
  // records flow
  else if (cmd == "expandflow") { if (!hasCorpus()) say("No records are loaded."); else { mainChart.title = "Records flow"; mainChart.make = [this](double w, double h, const ChartTheme& t) { return chartFlow(recordsFlow(), w, h, t); }; mainChartOpen = true; papersOpen = false; say("The records flow diagram fills the main area."); } }
  else if (cmd == "flowsvg") { Scene sc = chartFlow(recordsFlow(), 520, 560, chartTheme(true)); string err; bool ok = !arg.empty() && hybridizeForExport(g, P->fig, sc, &err) && writeFileU(arg, toSVG(sc)); say(ok ? "Wrote the records-flow diagram to " + arg + "." : (err.empty() ? "Could not write " + arg + "." : err)); }
  // clean terms
  else if (cmd == "cleanunit") { cleanUnit = clampv(toInt(arg, 0), 0, int(cleanUnits().size()) - 1); variantsScanned = false; }
  else if (cmd == "cleanscan") cleanScan();
  else if (cmd == "cleanai") aiCleanTerms(cleanUnit);
  else if (cmd == "cleanpick") { auto q = split(arg, ' '); int i = toInt(q[0], -1); if (i >= 0 && i < int(variants.size())) variants[size_t(i)].apply = q.size() > 1 ? q[1] != "0" : !variants[size_t(i)].apply; }
  else if (cmd == "cleanlabel") { size_t sp = arg.find(' '); int i = toInt(arg.substr(0, sp), -1); if (i >= 0 && i < int(variants.size()) && sp != string::npos) { variants[size_t(i)].target = trim(arg.substr(sp + 1)); variants[size_t(i)].targetCount = 0; } }
  else if (cmd == "cleanmerge") cleanMergeSelected();
  else if (cmd == "cleanundo") cleanUndo();
  else if (cmd == "cleanlog") {
    string o = readFileU("script.log");
    o += "clean unit=" + string(cleanUnits()[size_t(clampv(cleanUnit, 0, int(cleanUnits().size()) - 1))].second) + " groups=" + std::to_string(variants.size()) + " thesaurus=" + std::to_string(P->engine.thesaurus.replace.size()) + (cleanNote.empty() ? string() : " note=" + cleanNote) + "\n";
    for (size_t i = 0; i < variants.size() && i < 40; i++) {
      auto& v = variants[i];
      o += "  " + std::to_string(i) + (v.ai ? " [ai]" : "") + (v.ignore ? " [ignore]" : "") + (v.apply ? " [x] " : " [ ] ") + v.target + " (" + std::to_string(v.targetCount) + ")";
      if (!v.ignore) { o += " <- "; for (size_t m = 0; m < v.members.size(); m++) o += (m ? ", " : "") + v.members[m]; }
      o += " {" + v.reason + (v.note.empty() ? string() : ": " + v.note) + "}\n";
    }
    writeFileU("script.log", o);
  }
  else if (cmd == "crashtest") crashTest();
  // agent
  else if (cmd == "agent") { page = PG_AI; agentMode = true; agentStart(arg); }
  else if (cmd == "agentwait") { if (agent.active && !agent.waitApproval) { scriptPos--; scriptWait = 5; } }
  else if (cmd == "agentapprove") agentApprove(arg != "no", arg == "all");
  else if (cmd == "agentundo") agentUndoChanges();
  else if (cmd == "agentauto") { settings.j.set("agentAuto", arg == "1"); }
  // live AI
  else if (cmd == "live") { liveOpen(false); if (!arg.empty()) liveSend(arg); }
  else if (cmd == "livemic") { if (arg == "off") { if (live.s && live.talk) liveToggleMic(); } else liveOpen(true); }
  else if (cmd == "livewait") { if (liveBusy()) { scriptPos--; scriptWait = 5; } }
  else if (cmd == "liveapprove") liveApprove(arg != "no", arg == "all");
  else if (cmd == "liveallow") live.allowAll = arg != "0";
  else if (cmd == "livestop") liveDisconnect(false);
  else if (cmd == "livevad") {  // speech detection by the app (activityStart/End) or by the server; a voice session reconnects with the new setup
    bool on = arg.empty() ? !settings.j["liveClientVad"].boolean(true) : (arg != "0" && arg != "off" && arg != "server");
    settings.j.set("liveClientVad", on); settings.save(); setLiveVad = on;
    if (live.s && live.phase > 0 && live.audioMode) liveConnect(true);
    say(string("Speech detection: ") + (on ? "the app tells the model when you stop talking." : "the server's own detection."));
  }
  else if (cmd == "cite") { cmdCite(); say("APA and BibTeX references copied to the clipboard."); }
  else if (cmd == "themeanim") {  // the circular theme reveal on / off (scripts themselves always switch instantly)
    bool on = arg.empty() ? !settings.j["themeReveal"].boolean(true) : (arg != "0" && arg != "off");
    settings.j.set("themeReveal", on); settings.save();
    say(string("Theme reveal ") + (on ? "on." : "off."));
  }
  else if (cmd == "liveclose") liveClose();
  else if (cmd == "livemini") { if (!live.open) liveOpen(arg != "off"); liveSetMini(arg != "off"); }
  else if (cmd == "livelog") {
    string o = readFileU("script.log");
    o += "live open=" + std::to_string(live.open) + " phase=" + std::to_string(live.phase) + " audio=" + std::to_string(live.audioMode) + " talk=" + std::to_string(live.talk) +
         " tool=" + std::to_string(live.toolRunning) + " queue=" + std::to_string(live.queue.size()) + " changes=" + std::to_string(live.changes) + " status=" + liveStatus() + "\n";
    o += live.log.plain();
    writeFileU("script.log", o);
  }
  else if (cmd == "agentlog") {
    string o = readFileU("script.log");
    o += "agent active=" + std::to_string(agent.active) + " waiting=" + std::to_string(agent.waitApproval) + " steps=" + std::to_string(agent.steps) + " changes=" + std::to_string(agent.changes) + " undo=" + std::to_string(agentUndos.size()) + "\n";
    for (auto& st : agent.log) o += "  [" + std::to_string(st.status) + "] " + st.title + (st.detail.empty() ? string() : " : " + st.detail) + "\n";
    if (!agent.active && !agent.final.empty()) o += "  final: " + replaceAll(truncate(agent.final, 300), "\n", " / ") + "\n";
    if (!agentReportPath.empty()) o += "  report: " + agentReportPath + "\n";
    writeFileU("script.log", o);
  }
  else if (cmd == "settings") { if (arg == "0") ui.closeModal(); else settingsWanted = true; }
  else if (cmd == "oakind") oaKind = clampv(std::atoi(arg.c_str()), 0, 1);
  else if (cmd == "oasem") oaSemStrategy = clampv(std::atoi(arg.c_str()), 0, 1);
  else if (cmd == "oasemq") { oaSemQueries = split(arg, '|'); for (auto& q : oaSemQueries) q = trim(q); if (oaSemQueries.empty()) oaSemQueries = {""}; }
  else if (cmd == "records") { string f = split(arg, ' ')[0], pth = arg.substr(std::min(arg.size(), f.size() + 1)); bool ok = !pth.empty() && writeFileU(pth, writeRecords(P->corpus.recs, f == "ris" ? RecordExport::RIS : f == "csv" ? RecordExport::CSV : (f == "bib" || f == "bibtex") ? RecordExport::BibTeX : RecordExport::WoS)); say(ok ? "Wrote " + plural(long(P->corpus.recs.size()), "record") + " to " + pth + "." : "Could not write the records (records wos|ris|bib|csv <file>)."); }
  else if (cmd == "figopt") {  // figopt transparent|pdfpages|hybrid 0|1
    auto sp = arg.find(' ');
    string k = arg.substr(0, sp);
    bool v = sp != string::npos && arg.substr(sp + 1) == "1";
    if (k == "transparent") P->fig.transparent = v;
    else if (k == "pdfpages") P->fig.pdfPages = v;
    else if (k == "hybrid") P->fig.hybridExport = v;
    else if (k == "halo") { P->style.labelHalo = v; styleDirty = true; }
  }
  else if (cmd == "chartspdf") { if (arg.empty()) say("Give the file path."); else { cmdExportChartsPdf(pageCharts, arg); say(pageCharts.empty() ? "No charts are displayed on the current page; open the page and tab first." : "Wrote " + plural(long(pageCharts.size()), "chart") + " to " + arg + "."); } }
  else if (cmd == "figpanels") {  // figpanels all | network,overlay,density,timeline,geo,3d,matrix
    auto& F = P->fig;
    bool all = arg == "all";
    auto has = [&](const char* k) { return all || ("," + arg + ",").find(string(",") + k + ",") != string::npos; };
    F.panelNetwork = has("network"); F.panelOverlay = has("overlay"); F.panelDensity = has("density"); F.panelTimeline = has("timeline");
    F.panelGeo = has("geo") && geoAvailable(); F.panel3D = has("3d"); F.panelMatrix = has("matrix");
  }
  else if (cmd == "figzoom") { if (arg != "off" && !hasMap()) say("There is no map, so there is no figure to zoom."); else { figZoomOpen = arg != "off"; figZoomK = 1; figZoomX = figZoomY = 0; say(figZoomOpen ? "The publication figure is shown full-screen (Esc closes it)." : "The figure zoom is closed."); } }
  else if (cmd == "save") { string err; mapsToProject(); writerToProject(); if (P->save(arg, &err)) { P->path = arg; P->dirty = false; settings.addRecent(arg); say("Saved the project as " + arg + "."); } else say("Could not save " + arg + ": " + err); }
  else if (cmd == "openproj") { bool ok = false; (void)readFileU(arg, &ok); if (!ok) say("Cannot read " + arg + "."); else { cmdOpenProject(arg); say(P->path == arg ? "Opened the project " + fileName(arg) + ": " + plural(long(P->corpus.recs.size()), "record") + ", " + plural(P->net.n(), "item") + "." : "The project could not be opened."); } }
  else if (cmd == "shot") { pendingShot = true; pendingViewCrop = false; pendingShotPath = arg; say("Screenshot of the window to " + arg + "."); }
  else if (cmd == "viewshot") { pendingShot = true; pendingViewCrop = true; pendingShotPath = arg; say("Screenshot of the canvas to " + arg + "."); }
  else if (cmd == "wait") scriptWait = toInt(arg, 10);
  else if (cmd == "zoom") { camTargetZoom = -1; nv.cam.zoom *= toDouble(arg, 1); }
  else if (cmd == "orbit") { nv.cam.yaw += float(toDouble(arg, 0.5)); }
  else if (cmd == "log") { string o = readFileU("script.log"); o += line + " | items=" + std::to_string(P->net.n()) + " links=" + std::to_string(P->net.m()) + " clusters=" + std::to_string(P->net.nClusters) + " Q=" + fmtFixed(P->last.Q, 3) + " records=" + std::to_string(P->corpus.recs.size()) + " labels=" + std::to_string(nv.labelsShown) + " gpu=" + g.adapter + " canvascache=" + std::to_string(canvasHits) + "/" + std::to_string(canvasHits + canvasMisses) + " textcache=" + std::to_string(ui.textHitsPrev) + "/" + std::to_string(ui.textHitsPrev + ui.textMissesPrev) + "/" + std::to_string(ui.textCacheSize()) + " mem=" + fmtNum(processWorkingSetMB(), 0) + "MB\n"; writeFileU("script.log", o); canvasHits = canvasMisses = 0; }
  else if (cmd == "quit") { if (P) P->dirty = false; PostMessageW(hwnd, WM_CLOSE, 0, 0); scriptMode = false; }
  else { needFrame = true; return false; }
  needFrame = true;
  return true;
}

}  // namespace win
}  // namespace vs
