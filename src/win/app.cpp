// VOSStudio Native — application shell
#include "app.h"

#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>

#include "../core/records.h"
#include "../core/semantic.h"
#include "../core/world.h"

namespace vs {
namespace win {

App* gApp = nullptr;

namespace {
const char* kPageTitles[PG_COUNT] = {"Data", "Build", "Look", "Analyse", "Trends", "Actors", "Publish", "Assistant"};
const char* kPageSubs[PG_COUNT] = {"Import, merge and clean bibliographic records", "Choose what to map: type, unit, counting and thresholds",
                                   "Colours, sizes, labels, links and density", "Clusters, items, network statistics and stability",
                                   "Growth, bursts, thematic evolution and comparisons", "Authors, sources, countries, organisations and laws",
                                   "Publication figures, exports and project bundles", "AI research assistant for your data and map"};
const char* kPageIcons[PG_COUNT] = {"data", "network", "look", "analyse", "trends", "actors", "publish", "sparkle"};
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
  P = std::make_unique<Project>();
  setTheme(dark);
  styleCommitted = styleToJson(P->style);
  buildCommands();
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
    bool iconic = IsIconic(hwnd) != 0;
    if ((iconic || occluded) && !scriptMode) {
      pumpWork();
      if (occluded && !iconic && g.presentTest()) { occluded = false; needFrame = true; continue; }
      MsgWaitForMultipleObjects(0, nullptr, FALSE, occluded ? 250 : (busy() ? 500 : INFINITE), QS_ALLINPUT);
      continue;
    }
    bool smooth = ui.animating || animT0 >= 0 || camTargetZoom > 0 || scriptMode || !uiQueue.empty() || !ui.toasts.empty() || (job && job->finished);
    if (needFrame || smooth) {
      needFrame = false;
      frame();
    } else if (busy()) {
      if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_TIMEOUT) needFrame = true;
    } else {
      DWORD wait = ui.wantsCaret ? 530 : INFINITE;
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

LRESULT App::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
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
      input.mx = float(pt.x);
      input.my = float(pt.y);
      needFrame = true;
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
    case WM_MOUSEMOVE:
      input.mx = float(GET_X_LPARAM(lp));
      input.my = float(GET_Y_LPARAM(lp));
      needFrame = true;
      {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
      }
      return 0;
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
      if (P && P->dirty && hasMap() && !scriptMode) {
        int r = MessageBoxW(h, L"Save changes to this project before closing?", L"VOSStudio", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES) { cmdSaveProject(false); if (P->dirty) return 0; }
      }
      if (job) { job->cancel = true; job->join(); }
      settings.j.set("dark", ui.dark);
      settings.save();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// =====================================================================================
// frame
// =====================================================================================
void App::setTheme(bool dark) {
  ui.setTheme(dark);
  P->style.darkTheme = dark;
  styleDirty = true;
  figSig.clear();
  settings.j.set("dark", dark);
  applyFrameTheme();
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
  railR = {0, topR.b(), 64 * s, statusR.y - topR.b()};
  float lw = std::round(leftW * leftEase());
  leftR = {railR.r(), topR.b(), lw, railR.h};
  float rw = std::round(rightW * rightEase());
  rightR = {W - rw, topR.b(), rw, railR.h};
  canvasR = {leftR.r(), topR.b(), rightR.x - leftR.r(), railR.h};
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

void App::frame() {
  double t = nowSeconds();
  lastDt = clampv(t - lastFrameT, 0.001, 0.1);
  if (lastFrameT > 0) fps = fps * 0.9 + 0.1 / lastDt;
  lastFrameT = t;
  frameNo++;
  pageCharts.swap(frameCharts);  // charts drawn in the previous frame
  frameCharts.clear();
  pumpWork();
  // input snapshot
  Input in = input;
  in.ctrl = GetKeyState(VK_CONTROL) < 0;
  in.shift = GetKeyState(VK_SHIFT) < 0;
  in.alt = GetKeyState(VK_MENU) < 0;
  for (int b = 0; b < 3; b++) { input.pressed[b] = input.released[b] = false; }
  input.dbl = false;
  input.wheel = 0;
  input.chars.clear();
  input.keys.clear();

  ui.beginFrame(in, t);
  {
    bool ev = in.pressed[0] || in.pressed[1] || in.pressed[2] || in.released[0] || in.released[1] || in.released[2] || in.down[0] || in.down[1] ||
              in.wheel != 0 || !in.chars.empty() || !in.keys.empty() || busy() || scriptMode || animT0 >= 0;
    if (ev) chartFresh = 3;
    else if (chartFresh > 0) chartFresh--;
    if (frameNo % 600 == 0) {  // drop scenes of charts that are no longer shown
      for (auto it = chartCache.begin(); it != chartCache.end();) it = frameNo - it->second.frame > 600 ? chartCache.erase(it) : std::next(it);
    }
  }
  if (scriptMode) stepScript();
  aiPump();
  agentPump();
  stepPanelAnim();
  layout();
  updateWindowTitle();
  handleShortcuts();

  // sync renderer
  if (styleDirty) {
    nv.setData(hasMap() ? &P->net : nullptr, &P->style, &P->bundles);
    styleDirty = false;
    figSig.clear();
  }
  if (posDirty && hasMap()) {
    if (animT0 < 0) nv.pos = targetPositions(view);
    nv.nodesDirty = nv.linksDirty = true;
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
    nv.nodesDirty = nv.linksDirty = true;
    if (k >= 1) { animT0 = -1; if (animTo.size() == nv.pos.size()) nv.pos = animTo; }
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
  if (hasMap()) updateFlags();

  Theme th = canvasTheme(P->style);
  bool netCanvas = hasMap() && view != ViewKind::Matrix && !mainChartOpen && page != PG_AI;
  {  // Geo layers on a country network: overlay colours, or density underneath with the marks hidden
    bool g1 = hasMap() && view == ViewKind::Geo && geoMode() == 1;
    ViewKind tint = g1 && geoLayerKind == 1 ? ViewKind::Overlay : ViewKind::Geo;
    bool hide = g1 && geoLayerKind == 2;
    if (tint != nv.geoTint || hide != nv.hideMarks) { nv.geoTint = tint; nv.hideMarks = hide; nv.nodesDirty = nv.linksDirty = true; }
  }
  if (netCanvas && view == ViewKind::Geo && geoMode() > 0) {
    if (!nv.backgroundPainter) nv.backgroundPainter = [this](ID2D1DeviceContext* dc) { geoPaint(dc); };
    nv.drawBackgroundLayer();
  } else nv.backgroundPainter = nullptr;
  g.beginGpu(netCanvas ? th.bg : ui.c.bg);
  if (netCanvas) nv.renderGpu(th.bg);
  g.endGpu();
  if (!pendingViewFmt.empty()) doViewExport();
  g.beginD2D();
  if (netCanvas) nv.drawOverlay(g.dc.get(), th, showLabels, P->style.maxLabels);
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
    drawPalette();
    drawSettings();
  }
  if (figOn) drawFigureZoom();
  drawCrashDialog();
  ui.beginModalLayer();  // window buttons stay usable while a dialog is open
  drawCaption();
  ui.endModalLayer();
  ui.endFrame();
  g.endD2D();
  if (pendingShot) {
    pendingShot = false;
    vector<uint8_t> px;
    int w = 0, h = 0;
    if (g.readback(px, w, h)) {
      if (pendingViewCrop) {
        int x0 = int(canvasR.x), y0 = int(canvasR.y), cw = int(canvasR.w), ch = int(canvasR.h);
        vector<uint8_t> crop(static_cast<size_t>(cw) * size_t(ch) * 4);
        for (int y = 0; y < ch; y++) memcpy(&crop[size_t(y) * size_t(cw) * 4], &px[(size_t(y0 + y) * size_t(w) + size_t(x0)) * 4], size_t(cw) * 4);
        px.swap(crop); w = cw; h = ch;
      }
      if (pendingShotPath == "clipboard") setClipboardImage(hwnd, w, h, px.data());
      else if (g.savePngWic(pendingShotPath, w, h, px.data(), 96 * ui.s)) { if (!scriptMode && pendingViewCrop) ui.toast("View exported", fileName(pendingShotPath), 1); }
    }
    pendingViewCrop = false;
  }
  if (!g.present(!scriptMode)) occluded = true;
  if (ui.animating) needFrame = true;
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
  for (int k : in.keys) {
    if (in.ctrl) {
      switch (k) {
        case 'O': if (in.shift) cmdOpenProject(); else cmdOpenFiles(); break;
        case 'S': cmdSaveProject(in.shift); break;
        case 'Z': if (!edit) { if (in.shift) redo(); else undo(); } break;
        case 'Y': if (!edit) redo(); break;
        case 'B': cmdBuild(); break;
        case 'J': setPage(page == PG_AI ? PG_NONE : PG_AI); break;
        case 'E': if (hasMap()) { if (in.shift) cmdExportCurrentView(""); else setPage(PG_PUBLISH); } break;
        case 'K': case 'P': paletteOpen = !paletteOpen; paletteQuery.clear(); paletteSel = 0; ui.focus = 0; break;
        case 'F': ui.focus = ui.id("ti:search"); break;
        case 'N': cmdNewProject(); break;
        case 'C': if (!edit && in.shift) cmdCopyFigure(); break;
        case 'L': if (hasMap()) cmdRelayout(); break;
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
      else if (geoSel >= 0 && view == ViewKind::Geo) geoSel = -1;
      else if (mainChartOpen) mainChartOpen = false;
      else if (!selection.empty() || clusterFilter >= 0) { clearSelection(); clusterFilter = -1; }
      else if (!search.empty()) { search.clear(); searchHits.clear(); }
      continue;
    }
    if (k == VK_F1) { paletteOpen = true; paletteQuery.clear(); continue; }
    if (edit || paletteOpen) continue;
    if (figZoomOpen) {
      if (k == '0' || k == VK_NUMPAD0) { figZoomK = 1; figZoomX = figZoomY = 0; }
      else if (k == VK_OEM_PLUS || k == VK_ADD) figZoomK = clampv(figZoomK * 1.25, 0.25, 40.0);
      else if (k == VK_OEM_MINUS || k == VK_SUBTRACT) figZoomK = clampv(figZoomK / 1.25, 0.25, 40.0);
      continue;
    }
    if (k >= '1' && k <= '7' && hasMap()) setView(kViews[k - '1'].v);
    else if (k == 'F' && hasMap()) { mainChartOpen = false; nv.fit(); camTargetZoom = -1; }
    else if (k == 'L') showLabels = !showLabels;
    else if (k == 'H' && hasMap()) { P->style.hulls = !P->style.hulls; styleDirty = true; }
    else if (k == 'N' && hasMap()) { P->style.clusterNames = !P->style.clusterNames; styleDirty = true; }
    else if (k == 'M') showMinimap = !showMinimap;
    else if (k == 'G') showLegend = !showLegend;
    else if (k == 'I') inspectorOpen = !inspectorOpen;
    else if (k == VK_OEM_2 /* / */) ui.focus = ui.id("ti:search");
    else if (k == VK_OEM_PLUS || k == VK_ADD) { if (hasMap()) { camTargetZoom = (camTargetZoom > 0 ? camTargetZoom : nv.cam.zoom) * 1.3; camTargetX = nv.cam.x; camTargetY = nv.cam.y; } }
    else if (k == VK_OEM_MINUS || k == VK_SUBTRACT) { if (hasMap()) { camTargetZoom = (camTargetZoom > 0 ? camTargetZoom : nv.cam.zoom) / 1.3; camTargetX = nv.cam.x; camTargetY = nv.cam.y; } }
    else if (k == VK_TAB && !searchHits.empty()) { searchIdx = (searchIdx + (in.shift ? -1 : 1) + int(searchHits.size())) % int(searchHits.size()); focusOn(searchHits[size_t(searchIdx)]); }
  }
}

void App::buildCommands() {
  commands.clear();
  auto add = [&](const string& id, const string& label, const string& hint, const string& sc, const string& ic, std::function<void()> fn) { commands.push_back({id, label, hint, sc, ic, fn}); };
  add("open", "Open bibliographic files…", "WoS, Scopus, RIS, BibTeX, OpenAlex JSON", "Ctrl+O", "folder", [this] { cmdOpenFiles(); });
  add("openproj", "Open project…", ".vosproj bundle", "Ctrl+Shift+O", "file", [this] { cmdOpenProject(); });
  add("save", "Save project", "Self-contained bundle with records and map", "Ctrl+S", "save", [this] { cmdSaveProject(false); });
  add("saveas", "Save project as…", "", "Ctrl+Shift+S", "save", [this] { cmdSaveProject(true); });
  add("new", "New project", "Start over", "Ctrl+N", "file", [this] { cmdNewProject(); });
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
  add("reccsv", "Export records (CSV)…", "Scopus column names", "", "download", [this] { cmdExportRecords(false, "csv"); });
  add("items", "Export items table (CSV)…", "", "", "table", [this] { cmdExportItemsCsv(); });
  add("methods", "Copy methods paragraph", "Reproducible description of the map", "", "copy", [this] { if (hasMap()) { setClipboardText(hwnd, P->methods()); ui.toast("Copied", "Methods paragraph copied to the clipboard.", 1); } });
  add("theme", "Toggle light / dark theme", "", "", "sun", [this] { setTheme(!ui.dark); });
  add("settings", "Settings", "AI provider, OpenAlex API key, appearance", "Ctrl+,", "settings", [this] { settingsWanted = true; });
  add("labels", "Toggle labels", "", "L", "tag", [this] { showLabels = !showLabels; });
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
}

void App::setPage(int p) { page = (page == p) ? PG_NONE : p; if (p == PG_NONE) page = PG_NONE; }

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
  vector<string> bib, vos;
  for (auto& f : files) {
    string ext = fileExt(f);
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
    int added = 0, before = int(P->corpus.recs.size()), dupBefore = P->corpus.duplicatesRemoved;
    for (auto& p : *out) {
      if (p.recs.empty()) { ui.toast("Skipped " + fileName(p.name), p.err, 2, 7); continue; }
      added += P->addRecords(p.name, p.f, p.recs);
    }
    if (added > 0 || int(P->corpus.recs.size()) != before) {
      int dups = P->corpus.duplicatesRemoved - dupBefore;
      ui.toast("Records imported", plural(added, "record") + " added" + (dups ? " · " + plural(dups, "duplicate") + " merged" : "") + " · " + fmtInt(long(P->corpus.recs.size())) + " total", 1, 5);
      pageStateReset();
      if (!hasMap()) { page = PG_BUILD; }
    }
  });
}

void App::cmdSample(bool scopus) {
  int n = P->addSample(scopus);
  pageStateReset();
  ui.toast("Sample loaded", plural(n, "record") + (scopus ? " (Scopus CSV)" : " (Web of Science)") + "", 1, 5);
  if (!hasMap()) page = PG_BUILD;
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
  if (!hasCorpus()) { ui.toast("No data yet", "Open a bibliographic file or load the sample first.", 2); page = PG_DATA; return; }
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
  selection.clear(); hover = -1; clusterFilter = -1; searchHits.clear();
  pos3dValid = false; pos3d.clear();
  cinfoValid = false; stabValid = false; figSig.clear();
  mainChartOpen = false;
  pdiffValid = false; sweepValid = false; diffRestore.active = false;  // 1.8
  if (geoAfterBuild) { geoAfterBuild = false; if (geoAvailable()) view = ViewKind::Geo; }
  if (view == ViewKind::Geo && !geoAvailable()) view = ViewKind::Network;
  geoSel = -1; geoHover = -1;
  geoFitBounds(view);
  if (P->net.scoreIdx < 0 && !P->net.scoreNames.empty()) P->net.scoreIdx = 0;
  nv.pos.clear();
  nv.flags.clear();
  nv.setData(hasMap() ? &P->net : nullptr, &P->style, &P->bundles);
  animT0 = -1;
  nv.pos = targetPositions(view);
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
  if (!hasCorpus() && !hasMap()) { ui.toast("Nothing to save", "Load data or build a map first.", 2); return; }
  string path = P->path;
  if (saveAs || path.empty()) {
    path = saveFileDialog(hwnd, "Save project", {{"VOSStudio project", "*.vosproj"}}, exportName("vosproj"), "vosproj");
    if (path.empty()) return;
  }
  string err;
  mapsToProject();
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
  P = std::move(np);
  P->style.darkTheme = ui.dark;
  settings.addRecent(path);
  settings.save();
  undoStack.clear(); redoStack.clear();
  pageStateReset();
  mapsFromProject();
  afterMapChanged();
  ui.toast("Project opened", fileName(path) + " · " + plural(long(P->corpus.recs.size()), "record") + " · " + plural(P->net.n(), "item"), 1, 4);
  livingValid = false; livingRecs.clear(); living = Placement();
  livingOnOpen();  // 1.8: check the saved search for new papers when the project asks for it
}

void App::cmdNewProject() {
  if (busy()) return;
  if (P->dirty && hasMap() && !scriptMode) {
    int r = MessageBoxW(hwnd, L"Discard the current project?", L"VOSStudio", MB_OKCANCEL | MB_ICONQUESTION);
    if (r != IDOK) return;
  }
  P = std::make_unique<Project>();
  P->style.darkTheme = ui.dark;
  undoStack.clear(); redoStack.clear();
  pageStateReset();
  afterMapChanged();
  welcome = true;
  page = PG_DATA;
}

string App::exportName(const string& ext) const {
  string base = P->path.empty() ? "" : fileName(P->path);
  if (!base.empty()) { size_t d = base.rfind('.'); if (d != string::npos) base = base.substr(0, d); }
  if (base.empty()) base = hasMap() && P->mapSource == "analysis" ? string(typeInfo(P->spec.type).id) + "-" + unitId(P->spec.unit) + "-map" : "vosstudio-map";
  return base + "." + ext;
}

// ------------------------------------------------------------------ exports
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
  if (fmt == "svg") ok = writeFileU(path, toSVG(sc, P->fig.serif));
  else if (fmt == "pdf" && P->fig.pdfPages && figurePanels(P->fig).size() > 1) {
    auto pages = buildFigurePages(P->net, st, P->fig, &P->bundles, P->methodsShort(), &panelDefs);
    nPages = int(pages.size());
    ok = writeFileU(path, toPDF(pages, title));
  }
  else if (fmt == "pdf") ok = writeFileU(path, toPDF(sc, title));
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
  if (fmt == "svg") ok = writeFileU(path, toSVG(sc, P->fig.serif));
  else if (fmt == "pdf") ok = writeFileU(path, toPDF(sc, "VOSStudio view"));
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
  if (fmt == "svg") ok = writeFileU(path, toSVG(sc));
  else if (fmt == "pdf") ok = writeFileU(path, toPDF(sc, def.title));
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
  if (writeFileU(path, toPDF(pages, "VOSStudio charts"))) { if (!scriptMode) ui.toast("Charts exported", fileName(path) + " · " + plural(long(pages.size()), "page"), 1); }
  else ui.toast("Export failed", "Could not write " + fileName(path), 3);
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
  ViewKind old = view;
  view = v;
  nv.kind = v;
  if (v == ViewKind::ThreeD && !pos3dValid) start3DLayout();
  if (v == ViewKind::Matrix) return;
  auto target = targetPositions(v);
  bool geoSwitch = (old == ViewKind::Geo) != (v == ViewKind::Geo);
  if (animate && nv.pos.size() == target.size() && old != ViewKind::Matrix) {
    animFrom = nv.pos;
    animTo = target;
    animT0 = nowSeconds();
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
    nv.pos = target;
    nv.fit();
    camTargetZoom = -1;
  }
  nv.nodesDirty = nv.linksDirty = true;
}

void App::updateFlags() {
  const Network& N = P->net;
  size_t n = size_t(N.n());
  vector<uint32_t> f(n, 0);
  std::unordered_set<int> sel(selection.begin(), selection.end());
  std::unordered_set<int> nb;
  if (!sel.empty())
    for (auto& l : N.links) {
      if (sel.count(l.a)) nb.insert(l.b);
      if (sel.count(l.b)) nb.insert(l.a);
    }
  std::unordered_set<int> hits(searchHits.begin(), searchHits.end());
  int si = N.scoreIndex("Avg. pub. year");
  int gmode = view == ViewKind::Geo ? geoMode() : 0;
  if (si < 0) si = N.scoreIdx;
  for (size_t i = 0; i < n; i++) {
    uint32_t x = 0;
    if (sel.count(int(i))) x |= NF_SELECTED;
    else if (nb.count(int(i))) x |= NF_NEIGHBOUR;
    if (int(i) == hover) x |= NF_HOVER;
    if (hits.count(int(i))) x |= NF_MATCH;
    if (N.nodes[i].pinned) x |= NF_PINNED;
    bool dim = false;
    if (!sel.empty() && !(x & (NF_SELECTED | NF_NEIGHBOUR))) dim = true;
    if (clusterFilter >= 0 && N.nodes[i].cluster != clusterFilter && !(x & NF_SELECTED)) dim = true;
    if (!hits.empty() && sel.empty() && !(x & NF_MATCH)) dim = true;
    if (dim) x |= NF_DIM;
    if (gmode == 2 || (gmode == 1 && (i >= geoNodeCountry.size() || geoNodeCountry[i] < 0))) x |= NF_HIDDEN;
    if (view == ViewKind::Timeline && !(si >= 0 && size_t(si) < N.nodes[i].sc.size() && std::isfinite(N.nodes[i].sc[size_t(si)]))) x |= NF_HIDDEN;
    f[i] = x;
  }
  if (f != nv.flags) { nv.flags.swap(f); nv.nodesDirty = nv.linksDirty = true; }
}

void App::selectNode(int i, bool add) {
  if (i < 0) return;
  if (add) {
    auto it = std::find(selection.begin(), selection.end(), i);
    if (it != selection.end()) selection.erase(it); else selection.push_back(i);
  } else selection = {i};
  if (!inspectorOpen) inspectorOpen = true;
  closeDoc();  // a node selection replaces an open document preview
}

void App::clearSelection() { selection.clear(); closeDoc(); }

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

void App::runSearch() {
  searchHits.clear();
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
  // view switcher (centre) — full labels when there is room, icon-only otherwise
  float rightBlock = 3 * 34 * s + 14 * s + 190 * s + captionW();  // icon buttons + gaps + search (min)
  float avail = topR.w - brandR - rightBlock - 16 * s;
  vector<float> ws;
  float segW = 0;
  for (auto& vd : kViews) { float w = ui.textW(vd.label, 12.5f * s, 600) + 42 * s; ws.push_back(w); segW += w; }
  // centred on the window (not the canvas), so opening or closing a side panel never moves it;
  // full labels only when they fit centred between the left tools and the right-hand block
  float mid = topR.w / 2, half = std::min(mid - brandR, mid - rightBlock) - 12 * s;
  bool compact = segW / 2 + 2 * s > half;
  if (compact) { segW = 0; for (auto& w : ws) { w = 38 * s; segW += w; } }
  float cx = clampv(std::round(mid - (segW + 4 * s) / 2), brandR + 8 * s, std::max(brandR + 8 * s, brandR + avail - segW - 8 * s));
  Rect seg{cx, topR.y + 11 * s, segW + 4 * s, 30 * s};
  ui.fill(seg, ui.dark ? Color(1, 1, 1, 0.07f) : Color(0, 0, 0, 0.055f), 7 * s);
  float x = seg.x + 2 * s;
  for (int i = 0; i < 7; i++) {
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
  // right: search + actions
  float rx = topR.w - 6 * s - captionW();
  auto ib = [&](const string& icon, const string& tip, bool toggled, bool enabled) {
    rx -= 34 * s;
    return ui.iconButton({rx, topR.y + 12 * s, 30 * s, 28 * s}, icon, tip, toggled, enabled);
  };
  if (ib("layers", "Inspector  (I)", inspectorOpen && canInspect(), canInspect())) inspectorOpen = !inspectorOpen;
  if (ib(ui.dark ? "sun" : "moon", ui.dark ? "Light theme" : "Dark theme", false, true)) setTheme(!ui.dark);
  if (ib("command", "Command palette  (Ctrl+K)", paletteOpen, true)) { paletteOpen = !paletteOpen; paletteQuery.clear(); paletteSel = 0; }
  rx -= 8 * s;
  float sw = std::min(250 * s, std::max(140 * s, rx - (seg.r() + 16 * s)));
  Rect sr{rx - sw, topR.y + 12 * s, sw, 28 * s};
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

void App::drawRail() {
  float s = ui.s;
  ui.fill(railR, ui.c.rail);
  ui.line(railR.r() - 0.5f, railR.y, railR.r() - 0.5f, railR.b(), ui.c.border);
  float y = railR.y + 8 * s;
  for (int p = 0; p < PG_COUNT; p++) {
    if (p == PG_AI) { ui.line(railR.x + 18 * s, y + 2 * s, railR.r() - 18 * s, y + 2 * s, ui.c.border); y += 8 * s; }
    Rect r{railR.x + 6 * s, y, railR.w - 12 * s, 54 * s};
    uint64_t idv = ui.id(string("rail:") + kPageTitles[p]);
    bool hov = false;
    if (ui.behave(idv, r, &hov)) setPage(p);
    bool sel = page == p;
    if (sel) ui.fill(r, ui.c.active, 8 * s);
    else if (hov) ui.fill(r, ui.c.hover, 8 * s);
    Color fg = sel ? ui.c.accent : (hov ? ui.c.text : ui.c.textDim);
    ui.icon(kPageIcons[p], r.x + r.w / 2, r.y + 20 * s, 19 * s, fg, 1.6f);
    ui.text({r.x, r.y + 33 * s, r.w, 16 * s}, kPageTitles[p], 10.5f * s, sel ? ui.c.text : fg, AL_CENTER, sel ? 600 : 400);
    // progress hints: a dot when a step is ready to do
    bool hint = (p == PG_DATA && !hasCorpus()) || (p == PG_BUILD && hasCorpus() && !hasMap());
    if (hint) ui.circle(r.r() - 10 * s, r.y + 10 * s, 3 * s, ui.c.accent);
    ui.tipFor(idv, kPageSubs[p]);
    y += 58 * s;
  }
  // bottom buttons
  Rect hb{railR.x + 14 * s, railR.b() - 46 * s, 36 * s, 36 * s};
  if (ui.iconButton(hb, "keyboard", "Keyboard shortcuts & commands (F1)")) { paletteOpen = true; paletteQuery.clear(); }
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
  ui.text({hr.x + 16 * s, hr.y, hr.w - 60 * s, hr.h}, kPageTitles[page], 15 * s, ui.c.text, AL_LEFT, 600);
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
      if (acts[i].cmd == 0) { showStart = false; page = PG_DATA; }
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
      if (ui.listRow(rr, "start-recent" + std::to_string(i), false)) { string path = rec[i]; cmdOpenProject(path); if (hasCorpus() || hasMap()) showStart = false; return; }
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
    ce.sig = sig; ce.w = float(sw); ce.h = float(sh); ce.dark = ui.dark;
  }
  ce.frame = std::max(1, frameNo);
  const Scene& sc = ce.sc;
  float ox = card.x + 12 * s, oy = card.y + 12 * s;
  drawScene(g.dc.get(), g.d2f.get(), g.dw.get(), g, sc, ox, oy, s);
  if (card.has(ui.in.mx, ui.in.my) && !ui.anyPopup() && !ui.anyModal()) chartHover(sc, ox, oy, s);
  if (card.has(ui.in.mx, ui.in.my) && mainChart.onClick) {
    int tag = hitTag(sc, (ui.in.mx - ox) / s, (ui.in.my - oy) / s);
    if (tag >= 0) { ui.cursor = "hand"; if (ui.in.released[0] && ui.active == 0) mainChart.onClick(tag); }
  }
}

void App::drawCanvas() {
  chromeRects.clear();
  if (page == PG_AI) { drawAssistant(canvasR); return; }
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
  if (mainChartOpen && mainChart.valid()) { drawMainChart(canvasR); return; }
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
    if (x1 > x0) {
      Rect in = inset(r, 10 * s, 10 * s);
      double k = std::min(in.w / (x1 - x0), in.h / (y1 - y0));
      double ox = in.x + (in.w - (x1 - x0) * k) / 2, oy = in.y + (in.h - (y1 - y0) * k) / 2;
      const Encoder& e = nv.enc();
      int step = std::max(1, N.n() / 2500);
      for (int i = 0; i < N.n(); i += step) {
        if (nv.flags.size() == nv.pos.size() && (nv.flags[size_t(i)] & NF_HIDDEN)) continue;
        float px = float(ox + (nv.pos[size_t(i)][0] - x0) * k), py = float(oy + (nv.pos[size_t(i)][1] - y0) * k);
        float rr = std::max(1.2f * s, float(e.vos() ? e.vosRadiusPx(i) * s * 0.28 : e.radius(i) * k * 0.9));
        ui.fill({px - rr, py - rr, rr * 2, rr * 2}, e.nodeColor(i, view).withA(0.85f), rr);
      }
      double wx0, wy0, wx1, wy1;
      nv.screenToWorld(canvasR.x, canvasR.y, wx0, wy0);
      nv.screenToWorld(canvasR.r(), canvasR.b(), wx1, wy1);
      Rect vr{float(ox + (wx0 - x0) * k), float(oy + (wy0 - y0) * k), float((wx1 - wx0) * k), float((wy1 - wy0) * k)};
      ui.pushClip(r);
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
  if (drag == DR_NONE) hover = inside && free ? nv.hitNode(in.mx, in.my) : -1;
  if (view == ViewKind::Geo) geoChrome(inside && free && hover < 0 && (drag == DR_NONE || drag == DR_PAN));
  if (hover >= 0 && drag == DR_NONE) ui.cursor = canMove ? "hand" : "hand";
  // wheel
  if (inside && in.wheel != 0) {
    if (is3D) nv.cam.dist3 = clampv(nv.cam.dist3 * std::pow(0.88, double(in.wheel)), 0.2, 6.0);
    else {
      double z0 = camTargetZoom > 0 ? camTargetZoom : nv.cam.zoom;
      double cx0 = camTargetZoom > 0 ? camTargetX : nv.cam.x, cy0 = camTargetZoom > 0 ? camTargetY : nv.cam.y;
      double z1 = clampv(z0 * std::pow(1.2, double(in.wheel)), nv.fitZoom() * 0.15, nv.fitZoom() * 120);
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
        nd.x = wx; nd.y = wy; nd.pinned = true;
        nv.pos[size_t(dragNode)][0] = float(wx);
        nv.pos[size_t(dragNode)][1] = float(wy);
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
      if (drag == DR_NODE && dragNode >= 0) selectNode(dragNode, in.ctrl);
      else if (drag == DR_PAN || drag == DR_ORBIT) { if (in.released[0]) { clearSelection(); } }
    } else if (drag == DR_BOX) {
      Rect br{std::min(dragX0, in.mx), std::min(dragY0, in.my), std::fabs(in.mx - dragX0), std::fabs(in.my - dragY0)};
      auto v = nv.nodesInRect(br);
      if (!in.ctrl) selection.clear();
      for (int i : v) if (std::find(selection.begin(), selection.end(), i) == selection.end()) selection.push_back(i);
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
    if (ui.button(ac[1], nd.pinned ? "Unpin" : "Pin", BTN_NORMAL, "pin")) { pushUndo(nd.pinned ? "Unpin" : "Pin"); P->net.nodes[size_t(i)].pinned = !nd.pinned; }
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
    if (ui.button(ac[0], "Pin all", BTN_NORMAL, "pin")) { pushUndo("Pin"); for (int i : selection) P->net.nodes[size_t(i)].pinned = true; }
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
  ui.fill({0, 0, float(g.W), float(g.H)}, Color(0, 0, 0, ui.dark ? 0.45f : 0.25f));
  float w = std::min(640 * s, float(g.W) - 80 * s);
  Rect r{(g.W - w) / 2, 90 * s, w, 0};
  // candidates
  struct Item { string label, hint, sc, icon; std::function<void()> run; };
  vector<Item> items;
  string q = lower(trim(paletteQuery));
  for (auto& c : commands) {
    if (!q.empty() && !icontains(c.label, q) && !icontains(c.hint, q)) continue;
    items.push_back({c.label, c.hint, c.shortcut, c.icon, c.run});
  }
  if (hasMap() && q.size() >= 2) {
    int k = 0;
    for (int i = 0; i < P->net.n() && k < 8; i++)
      if (icontains(P->net.nodes[size_t(i)].label, q)) { items.push_back({"Go to \xE2\x80\x9C" + P->net.nodes[size_t(i)].label + "\xE2\x80\x9D", "Cluster " + std::to_string(P->net.nodes[size_t(i)].cluster + 1), "", "target", [this, i] { focusOn(i); }}); k++; }
  }
  int n = std::min<int>(12, int(items.size()));
  r.h = 56 * s + n * 40 * s + (n ? 8 * s : 40 * s);
  ui.shadow(r, 14 * s, 30 * s);
  ui.fill(r, ui.c.panel2, 12 * s);
  ui.stroke(r, ui.c.borderStrong, 12 * s);
  ui.focus = ui.id("ti:palette");  // text inputs register as "ti:" + key
  bool submitted = false;
  string before = paletteQuery;
  ui.textInput({r.x + 10 * s, r.y + 10 * s, r.w - 20 * s, 38 * s}, "palette", paletteQuery, "Type a command or an item name\xE2\x80\xA6", &submitted, "command");
  if (paletteQuery != before) paletteSel = 0;
  for (int k : ui.in.keys) {
    if (k == VK_DOWN) paletteSel = std::min(n - 1, paletteSel + 1);
    if (k == VK_UP) paletteSel = std::max(0, paletteSel - 1);
  }
  paletteSel = clampv(paletteSel, 0, std::max(0, n - 1));
  std::function<void()> run;
  for (int k = 0; k < n; k++) {
    Rect rr{r.x + 6 * s, r.y + 56 * s + k * 40 * s, r.w - 12 * s, 38 * s};
    bool hov = rr.has(ui.in.mx, ui.in.my);
    if (hov && (ui.in.mx != lastMx || ui.in.my != lastMy)) paletteSel = k;
    if (k == paletteSel) ui.fill(rr, ui.c.accent.withA(0.16f), 7 * s);
    if (hov && ui.in.released[0]) run = items[size_t(k)].run;
    ui.icon(items[size_t(k)].icon, rr.x + 20 * s, rr.y + rr.h / 2, 16 * s, k == paletteSel ? ui.c.accent : ui.c.textDim);
    ui.text({rr.x + 42 * s, rr.y + 2 * s, rr.w * 0.55f, 20 * s}, items[size_t(k)].label, 13 * s, ui.c.text, AL_LEFT, 550);
    if (!items[size_t(k)].hint.empty()) ui.text({rr.x + 42 * s, rr.y + 20 * s, rr.w - 170 * s, 16 * s}, items[size_t(k)].hint, 11 * s, ui.c.textFaint);
    if (!items[size_t(k)].sc.empty()) {
      float kw = ui.textW(items[size_t(k)].sc, 11 * s, 600) + 14 * s;
      Rect kr{rr.r() - kw - 10 * s, rr.y + 9 * s, kw, 20 * s};
      ui.fill(kr, ui.c.input, 5 * s);
      ui.stroke(kr, ui.c.border, 5 * s);
      ui.text(kr, items[size_t(k)].sc, 11 * s, ui.c.textDim, AL_CENTER, 600, true);
    }
  }
  if (!n) ui.text({r.x, r.y + 56 * s, r.w, 30 * s}, "No matching commands", 12.5f * s, ui.c.textDim, AL_CENTER);
  if (submitted && n) run = items[size_t(paletteSel)].run;
  if (ui.in.pressed[0] && !r.has(ui.in.mx, ui.in.my)) { paletteOpen = false; ui.focus = 0; }
  if (run) { paletteOpen = false; ui.focus = 0; run(); }
  lastMx = ui.in.mx; lastMy = ui.in.my;
}

// =====================================================================================
// script mode: one command per frame; used for automated screenshots and smoke tests
// =====================================================================================
void App::stepScript() {
  // synthetic input for tests: a scripted click is released on the following frame
  static bool clickHeld = false;
  if (clickHeld) { input.down[0] = false; input.released[0] = true; clickHeld = false; }
  if (scriptWait > 0) { scriptWait--; return; }
  if (busy() || (job && job->finished) || animT0 >= 0) return;
  if (scriptPos >= script.size()) { if (P) P->dirty = false; PostMessageW(hwnd, WM_CLOSE, 0, 0); scriptMode = false; return; }
  string line = script[scriptPos++];
  auto parts = split(line, ' ');
  string cmd = parts.empty() ? "" : parts[0];
  string arg = parts.size() > 1 ? trim(line.substr(cmd.size())) : "";
  if (cmd == "sample") cmdSample(arg == "scopus");
  else if (cmd == "open") dropFiles(split(arg, '|'));  // several files: a|b (e.g. VOSviewer map|network)
  else if (cmd == "build") cmdBuild();
  else if (cmd == "type") { AnaType t; if (typeFromId(arg, t)) { P->spec.type = t; P->spec.unit = typeInfo(t).units[0].first; P->spec.setDefaults(); } }
  else if (cmd == "unit") { Unit u; if (unitFromId(arg, u)) { P->spec.unit = u; P->spec.setDefaults(); } }
  else if (cmd == "min") P->spec.min = toInt(arg, 1);
  else if (cmd == "view") { for (auto& v : kViews) if (iequals(v.label, arg)) setView(v.v, false); }
  else if (cmd == "page") { page = PG_NONE; for (int p = 0; p < PG_COUNT; p++) if (iequals(kPageTitles[p], arg)) page = p; }
  else if (cmd == "tab") { int t = toInt(arg); anaTab = trTab = acTab = t; }
  else if (cmd == "theme") setTheme(arg != "light");
  else if (cmd == "look") { applyLook(P->style, arg); P->style.darkTheme = ui.dark; styleDirty = true; if (hasMap()) { nv.fit(); camTargetZoom = -1; } }
  else if (cmd == "select") { runSearch(); search = arg; runSearch(); if (!searchHits.empty()) focusOn(searchHits[0]); camTargetZoom = -1; }
  else if (cmd == "search") { search = arg; runSearch(); }
  else if (cmd == "clearsearch") { search.clear(); searchHits.clear(); }
  else if (cmd == "hover") { input.mx = float(toDouble(split(arg, ' ')[0])); input.my = float(toDouble(split(arg, ' ').back())); }
  else if (cmd == "palette") { paletteOpen = true; paletteQuery = arg; }
  else if (cmd == "maxlines") { P->style.maxLines = toInt(arg, 1000); styleDirty = true; }
  else if (cmd == "linkgeom") { P->style.linkGeom = arg == "straight" ? LinkGeom::Straight : arg == "arc" ? LinkGeom::Arc : LinkGeom::Curved; styleDirty = true; }
  else if (cmd == "keys") { for (unsigned char ch : arg) if (ch >= 32 && ch < 127) input.chars.push_back(char32_t(ch)); }  // typed text (ASCII)
  else if (cmd == "key") input.keys.push_back(toInt(arg));                                                               // virtual-key code
  else if (cmd == "click") { input.mx = float(toDouble(split(arg, ' ')[0])); input.my = float(toDouble(split(arg, ' ').back())); input.down[0] = input.pressed[0] = true; clickHeld = true; }
  else if (cmd == "closepalette") paletteOpen = false;
  else if (cmd == "inspector") inspectorOpen = arg != "off";
  else if (cmd == "hulls") { P->style.hulls = arg != "off"; styleDirty = true; }
  else if (cmd == "names") { P->style.clusterNames = arg != "off"; styleDirty = true; }
  else if (cmd == "bundle") cmdBundles();
  else if (cmd == "relayout") cmdRelayout();
  else if (cmd == "run") {  // trigger analysis runs used by the pages
    if (arg == "bursts") { bursts = detectBursts(P->corpus, Unit(trUnit), burstMin, burstS, burstG); burstsValid = true; }
    else if (arg == "stability") { stab = clusterStability(P->net, P->params.clusterOpts(), stabRuns); stabValid = true; }
  }
  else if (cmd == "expand") {
    if (arg == "strategic") { mainChart.title = "Strategic diagram"; mainChart.make = [this](double w, double h, const ChartTheme& t) { if (!cinfoValid) { cinfo = clusterInfo(P->net, &P->corpus); cinfoValid = true; } return chartStrategic(cinfo, clusterColors(), w, h, t); }; mainChartOpen = true; }
  }
  else if (cmd == "closechart") mainChartOpen = false;
  else if (cmd == "svg" || cmd == "pdf" || cmd == "png") cmdExportFigure(cmd, arg);
  else if (cmd == "viewsvg" || cmd == "viewpdf" || cmd == "viewpng") cmdExportCurrentView(cmd.substr(4), arg);
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
  else if (cmd == "mainpathroutes") mpRoutes = clampv(toInt(arg, 5), 0, 20);
  else if (cmd == "sweep") {
    sweep = resolutionSweep(P->net, P->params.clusterOpts(), defaultSweepResolutions(), sweepSeeds);
    sweepValid = true;
    sweepSig = std::to_string(P->net.n()) + ":" + std::to_string(P->net.m()) + ":" + clusterTag(P->params.clusterOpts());
  }
  else if (cmd == "useres") useResolution(toDouble(arg, 1.0));
  else if (cmd == "livingcheck") livingCheck(false);
  else if (cmd == "livingadd") livingAdd(arg == "rebuild");
  else if (cmd == "livingauto") { P->living.set("auto", toInt(arg, 1) != 0); P->dirty = true; }
  else if (cmd == "livingsince") P->living.set("lastCheck", arg);  // tests: pretend the last check was on this date
  else if (cmd == "geolayer") geoLayerKind = arg == "overlay" ? 1 : arg == "density" ? 2 : arg == "network" ? 0 : clampv(toInt(arg, 0), 0, 2);
  else if (cmd == "geosel") { geoUpdate(); geoSel = findCountry(arg); if (geoSel >= 0) { closeDoc(); inspectorOpen = true; } }
  else if (cmd == "preview") {
    int r = -1;
    if (arg == "top") { long best = -1; for (size_t i = 0; i < P->corpus.recs.size(); i++) if (P->corpus.recs[i].cites > best) { best = P->corpus.recs[i].cites; r = int(i); } }
    else r = toInt(arg, -1);
    if (r >= 0) openDoc(r); else closeDoc();
  }
  else if (cmd == "maps") restoreMap(toInt(arg, 0));
  else if (cmd == "scroll") { if (arg.rfind("insp", 0) == 0) ui.scrollTo("inspector", float(toDouble(split(arg, ' ').back()))); else if (page >= 0) ui.scrollTo("page" + std::to_string(page), float(toDouble(arg))); }
  else if (cmd == "popup") ui.openPopup(arg);
  else if (cmd == "docrank") docRank = clampv(toInt(arg, 0), 0, 2);
  else if (cmd == "cymode") cyMode = clampv(toInt(arg, 0), 0, 1);
  else if (cmd == "oaquery") oaQuery = replaceAll(arg, " | ", "\n");
  else if (cmd == "oamax") oaMax = arg;
  else if (cmd == "oafrom") oaFrom = arg;
  else if (cmd == "oafetch") { oaKind = 0; cmdOpenAlex(); }
  else if (cmd == "start") showStart = arg != "0";
  else if (cmd == "ai") {  // ai <task-id> [extra text]
    string id = parts.size() > 1 ? parts[1] : "chat";
    string extra;
    for (size_t k = 2; k < parts.size(); k++) extra += (k > 2 ? " " : "") + parts[k];
    for (auto& ti : ai::tasks()) if (id == ti.id) { if (ti.task == ai::Task::Chat) { page = PG_AI; aiSend(ai::Task::Chat, extra); } else aiRun(ti.task, extra); }
  }
  else if (cmd == "aiinput") { aiInput = arg; }
  else if (cmd == "settab") { setTab = clampv(toInt(arg, 0), 0, 2); }
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
  else if (cmd == "expandflow") { mainChart.title = "Records flow"; mainChart.make = [this](double w, double h, const ChartTheme& t) { return chartFlow(recordsFlow(), w, h, t); }; mainChartOpen = true; }
  else if (cmd == "flowsvg") writeFileU(arg, toSVG(chartFlow(recordsFlow(), 520, 560, chartTheme(true))));
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
  else if (cmd == "agentlog") {
    string o = readFileU("script.log");
    o += "agent active=" + std::to_string(agent.active) + " waiting=" + std::to_string(agent.waitApproval) + " steps=" + std::to_string(agent.steps) + " changes=" + std::to_string(agent.changes) + " undo=" + std::to_string(agentUndo.valid) + "\n";
    for (auto& st : agent.log) o += "  [" + std::to_string(st.status) + "] " + st.title + (st.detail.empty() ? string() : " : " + st.detail) + "\n";
    if (!agent.active && !agent.final.empty()) o += "  final: " + replaceAll(truncate(agent.final, 300), "\n", " / ") + "\n";
    if (!agentReportPath.empty()) o += "  report: " + agentReportPath + "\n";
    writeFileU("script.log", o);
  }
  else if (cmd == "settings") { if (arg == "0") ui.closeModal(); else settingsWanted = true; }
  else if (cmd == "oakind") oaKind = clampv(std::atoi(arg.c_str()), 0, 1);
  else if (cmd == "oasem") oaSemStrategy = clampv(std::atoi(arg.c_str()), 0, 1);
  else if (cmd == "oasemq") { oaSemQueries = split(arg, '|'); for (auto& q : oaSemQueries) q = trim(q); if (oaSemQueries.empty()) oaSemQueries = {""}; }
  else if (cmd == "records") { string f = split(arg, ' ')[0], pth = arg.substr(std::min(arg.size(), f.size() + 1)); writeFileU(pth, writeRecords(P->corpus.recs, f == "ris" ? RecordExport::RIS : f == "csv" ? RecordExport::CSV : RecordExport::WoS)); }
  else if (cmd == "figopt") {  // figopt transparent|pdfpages 0|1
    auto sp = arg.find(' ');
    string k = arg.substr(0, sp);
    bool v = sp != string::npos && arg.substr(sp + 1) == "1";
    if (k == "transparent") P->fig.transparent = v;
    else if (k == "pdfpages") P->fig.pdfPages = v;
    else if (k == "halo") { P->style.labelHalo = v; styleDirty = true; }
  }
  else if (cmd == "chartspdf") cmdExportChartsPdf(pageCharts, arg);
  else if (cmd == "figpanels") {  // figpanels all | network,overlay,density,timeline,geo,3d,matrix
    auto& F = P->fig;
    bool all = arg == "all";
    auto has = [&](const char* k) { return all || ("," + arg + ",").find(string(",") + k + ",") != string::npos; };
    F.panelNetwork = has("network"); F.panelOverlay = has("overlay"); F.panelDensity = has("density"); F.panelTimeline = has("timeline");
    F.panelGeo = has("geo") && geoAvailable(); F.panel3D = has("3d"); F.panelMatrix = has("matrix");
  }
  else if (cmd == "figzoom") { figZoomOpen = arg != "off"; figZoomK = 1; figZoomX = figZoomY = 0; }
  else if (cmd == "save") { string err; mapsToProject(); if (P->save(arg, &err)) { P->path = arg; P->dirty = false; } }
  else if (cmd == "openproj") cmdOpenProject(arg);
  else if (cmd == "shot") { pendingShot = true; pendingViewCrop = false; pendingShotPath = arg; }
  else if (cmd == "viewshot") { pendingShot = true; pendingViewCrop = true; pendingShotPath = arg; }
  else if (cmd == "wait") scriptWait = toInt(arg, 10);
  else if (cmd == "zoom") { camTargetZoom = -1; nv.cam.zoom *= toDouble(arg, 1); }
  else if (cmd == "orbit") { nv.cam.yaw += float(toDouble(arg, 0.5)); }
  else if (cmd == "log") { string o = readFileU("script.log"); o += line + " | items=" + std::to_string(P->net.n()) + " links=" + std::to_string(P->net.m()) + " clusters=" + std::to_string(P->net.nClusters) + " Q=" + fmtFixed(P->last.Q, 3) + " records=" + std::to_string(P->corpus.recs.size()) + " labels=" + std::to_string(nv.labelsShown) + " gpu=" + g.adapter + "\n"; writeFileU("script.log", o); }
  else if (cmd == "quit") { if (P) P->dirty = false; PostMessageW(hwnd, WM_CLOSE, 0, 0); scriptMode = false; }
  needFrame = true;
}

}  // namespace win
}  // namespace vs
