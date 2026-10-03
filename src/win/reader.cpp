// VOSStudio Native — the PDF reader (Read stage of the workflow). One engine thread (pdf::Worker) renders tiles,
// extracts text and searches; the UI thread only draws cached bitmaps, so scrolling and zooming stay at the frame
// rate whatever the document does. Highlights, notes and codes live in the project's library (P->library), drawn as
// overlays over the page; "Save into PDF" writes them into the file on request (readlib.cpp).
#include "reader.h"

#include <algorithm>
#include <cmath>

#include "../core/doc.h"

namespace vs {
namespace win {

namespace {
constexpr int kTile = 512;                    // device pixels
constexpr size_t kTileBudget = 48u << 20;     // GPU bytes kept for tiles at most (LRU beyond it, whatever is on screen)
constexpr size_t kTileSoft = 16u << 20;       // above this, tiles that left the view a moment ago are dropped
constexpr double kTileIdleS = 1.5;            // ... after this long
constexpr int kTextPages = 24, kPreviewPages = 24;  // per-page text / preview bitmaps kept around the view
constexpr double kReleaseAfterS = 45;         // a closed reader keeps its document this long (instant return), then frees it
constexpr double kTrimAfterS = 90;            // …and this long after that, with the reader unused, the engine's own memory goes too
constexpr float kMinZoom = 0.25f, kMaxZoom = 6.f;

float pxPerPt(const Ui& ui, float zoom) { return zoom * ui.s * 96.f / 72.f; }
int rdPreviewTargetW(const ReaderState& R) {
  const float target = clampv(R.pageArea.w * 0.55f, 240.f, 520.f);
  return clampv(int(std::lround(target / 40.f)) * 40, 240, 520);  // quantize resize changes to avoid needless rerenders
}

// displayed size of a page under the view rotation (a quarter turn swaps width and height)
inline float rdW(const ReaderState& R, const RdPage& p) { return (R.rot & 1) ? p.h : p.w; }
inline float rdH(const ReaderState& R, const RdPage& p) { return (R.rot & 1) ? p.w : p.h; }
// the two-page layout: pages sit in rows of two; `coverAlone` puts the first page alone on the right, as a book
// opens, so that the facing pages are the ones facing in print
inline int rdRowOf(const ReaderState& R, int i) { return !R.twoUp ? i : R.coverAlone ? (i + 1) / 2 : i / 2; }
inline int rdRowFirst(const ReaderState& R, int row) { return !R.twoUp ? row : !R.coverAlone ? 2 * row : row == 0 ? 0 : 2 * row - 1; }
inline int rdSlotOf(const ReaderState& R, int i) { return !R.twoUp ? 0 : R.coverAlone ? (i == 0 ? 1 : (i + 1) % 2) : i % 2; }
// the width of the widest row in points: what "fit width" fits (a page alone in a two-page row counts as a spread)
float rdMaxRowPts(const ReaderState& R) {
  float m = 1;
  const int n = int(R.doc.pages.size());
  for (int i = 0; i < n;) {
    const int row = rdRowOf(R, i);
    float w = 0;
    int cnt = 0;
    while (i < n && rdRowOf(R, i) == row) { w += rdW(R, R.doc.pages[size_t(i)]); cnt++; i++; }
    if (R.twoUp && cnt == 1) w *= 2;
    m = std::max(m, w);
  }
  return m;
}
// the page column at k px per point: the top and the left of every page (column-relative), the column's size;
// rows top-aligned and centred in the column
void rdLayout(const ReaderState& R, float k, float gap, float top0, vector<float>& top, vector<float>& left, float& colW, float& colH) {
  const int n = int(R.doc.pages.size());
  top.assign(size_t(n), 0);
  left.assign(size_t(n), 0);
  const int rows = n == 0 ? 0 : rdRowOf(R, n - 1) + 1;
  vector<float> rowW(size_t(rows), 0), rowH(size_t(rows), 0);
  vector<int> rowN(size_t(rows), 0);
  for (int i = 0; i < n; i++) {
    const int row = rdRowOf(R, i);
    rowW[size_t(row)] += rdW(R, R.doc.pages[size_t(i)]) * k;
    rowH[size_t(row)] = std::max(rowH[size_t(row)], rdH(R, R.doc.pages[size_t(i)]) * k);
    rowN[size_t(row)]++;
  }
  colW = 1;
  for (int r = 0; r < rows; r++) {
    if (R.twoUp) rowW[size_t(r)] = (rowN[size_t(r)] == 1 ? rowW[size_t(r)] * 2 : rowW[size_t(r)]) + gap;
    colW = std::max(colW, rowW[size_t(r)]);
  }
  float y = top0;
  for (int r = 0; r < rows; r++) {
    const float rx = (colW - rowW[size_t(r)]) / 2;
    const int first = rdRowFirst(R, r);
    float x = rx;
    for (int i = first; i < n && rdRowOf(R, i) == r; i++) {
      const float w = rdW(R, R.doc.pages[size_t(i)]) * k;
      if (R.twoUp && rowN[size_t(r)] == 1 && rdSlotOf(R, i) == 1) x = rx + w + gap;  // alone on the right
      top[size_t(i)] = y;
      left[size_t(i)] = x;
      x += w + gap;
    }
    y += rowH[size_t(r)] + gap;
  }
  colH = (rows ? y - gap : top0) + top0;
}

string joinLines(const string& slice) { return pdf::joinSelectionLines(slice); }
}  // namespace

Color rdCodeColor(uint32_t argb) { return Color(((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, ((argb >> 24) & 255) / 255.f); }

static ReaderState& RS(App& a) {
  if (!a.rd) a.rd = std::make_shared<ReaderState>();
  ReaderState& R = *a.rd;
  if (!R.darkLoaded) {
    R.darkLoaded = true;
    R.dark = a.settings.j["readerDark"].boolean(false);
    R.filterMode = clampv(a.settings.j["readerFilter"].integer(0), 0, 2);
    if (R.dark) R.filterMode = 0;  // the inverting dark page and non-inverting filters are mutually exclusive
    R.inkColor = uint32_t(a.settings.j["readerInkColor"].num(0xFF1D4ED8));
    R.inkWidth = float(clampv(a.settings.j["readerInkWidth"].num(1.8), 0.25, 12.0));
    R.twoUp = a.settings.j["readerTwoUp"].boolean(false);
    R.coverAlone = a.settings.j["readerCover"].boolean(false);
  }
  return R;
}

// ------------------------------------------------------------------ the dark page
// A reading mode of the reader alone (the application theme is separate): every tile is rendered by the engine as
// usual and then inverted in luma with its hues kept (pdfdoc.cpp darkenPage) — paper becomes dark grey, ink light
// grey, a blue link a light blue; photographs and micrographs are recognised and left as they are, plots and scans
// are inverted like text. Nothing in the file changes; copies and figures taken from the page stay light.
bool App::readerDark() const { return rd && rd->dark; }
void App::readerSetDark(bool on) {
  ReaderState& R = RS(*this);
  if (R.dark == on && (!on || R.filterMode == 0)) return;
  R.dark = on;
  if (on) R.filterMode = 0;
  settings.j.set("readerDark", on);
  settings.j.set("readerFilter", R.filterMode);
  settings.save();
  // previews are re-rendered in the new mode (the old ones would flash through); tiles are keyed by the appearance.
  // Keep an in-flight preview marked busy: its completion notices the stale mode and asks for the current one.
  for (auto& p : R.doc.pages) { p.preview.reset(); p.pw = p.ph = 0; }
  needFrame = true;
}

static void rdSetFilter(App& A, int mode) {
  mode = clampv(mode, 0, 2);
  ReaderState& R = RS(A);
  if (!R.dark && R.filterMode == mode) return;
  R.dark = false;
  R.filterMode = mode;
  A.settings.j.set("readerDark", false);
  A.settings.j.set("readerFilter", mode);
  A.settings.save();
  // The original, warm and grayscale versions are separate render/cache keys; never recolor an old bitmap in place.
  for (auto& p : R.doc.pages) { p.preview.reset(); p.pw = p.ph = 0; }
  A.needFrame = true;
}

// ------------------------------------------------------------------ view rotation and the page layout
// The rotation turns the whole view of this document (a scan that lies on its side); it is kept with the PDF in the
// project. Everything the engine reports stays in the unrotated page frame — the tiles are rendered turned (keyed by
// the rotation), the overlays go through pdf::rotateBox, the mouse comes back through pdf::unrotatePoint.
// the page at the top of the view and how far into it, from the last frame's geometry (the reading position)…
static bool rdTopPage(const ReaderState& R, int& pg, float& frac) {
  const size_t n = std::min(R.pageTop.size(), R.doc.pages.size());
  if (n == 0 || !R.doc.ready) return false;
  for (size_t i = 0; i < n; i++) {
    const float top = R.pageTop[i], h = rdH(R, R.doc.pages[i]) * R.kShown;
    if (top + h > 0 || i + 1 == n) { pg = int(i); frac = top < 0 ? clampv(-top / std::max(1.f, h), 0.f, 1.f) : 0.f; return true; }
  }
  return false;
}
// …put back after the rotation or the layout changed: the scroll is derived from the new geometry at the shown
// scale, so the same page stays at the top of the view (a following change of the fit-width scale is compensated
// by the ordinary zoom anchoring, which now sees a consistent layout)
static void rdKeepTop(App& A, int pg, float frac) {
  ReaderState& R = RS(A);
  if (pg < 0 || size_t(pg) >= R.doc.pages.size()) return;
  vector<float> top, left;
  float cw = 0, ch = 0;
  const float k = R.kShown > 0 ? R.kShown : 1;
  rdLayout(R, k, 14 * A.ui.s, 20 * A.ui.s, top, left, cw, ch);
  R.scroll = R.scrollTarget = std::max(0.f, top[size_t(pg)] + frac * rdH(R, R.doc.pages[size_t(pg)]) * k);
  R.scrollX = 0;
}

int App::readerRotation() const { return rd ? rd->rot : 0; }
void App::readerRotate(int quarterTurns) {
  ReaderState& R = RS(*this);
  int pg = -1;
  float frac = 0;
  const bool keep = rdTopPage(R, pg, frac);
  R.rot = ((R.rot + quarterTurns) % 4 + 4) % 4;
  R.inkDrag = false; R.inkPage = -1; R.inkPoints.clear();
  if (PdfItem* it = readerItem()) { if (it->rot != R.rot) { it->rot = R.rot; P->dirty = true; } }
  for (auto& p : R.doc.pages) if (p.previewRot != R.rot) { p.preview.reset(); p.pw = p.ph = 0; }
  for (auto& kv : R.tileReq) kv.second.cancel();
  R.tileReq.clear();
  R.miniShow = false;
  readerCloseCard();
  if (keep) rdKeepTop(*this, pg, frac);
  needFrame = true;
}
void App::readerSetLayout(bool twoUp, bool coverAlone) {
  ReaderState& R = RS(*this);
  if (R.twoUp == twoUp && R.coverAlone == coverAlone) return;
  int pg = -1;
  float frac = 0;
  const bool keep = rdTopPage(R, pg, frac);
  R.twoUp = twoUp;
  R.coverAlone = coverAlone;
  settings.j.set("readerTwoUp", twoUp);
  settings.j.set("readerCover", coverAlone);
  settings.save();
  R.miniShow = false;
  readerCloseCard();
  if (keep) rdKeepTop(*this, pg, frac);
  needFrame = true;
}
bool App::readerTwoUp() const { return rd && rd->twoUp; }
bool App::readerCoverAlone() const { return rd && rd->coverAlone; }

// ------------------------------------------------------------------ engine glue
void App::readerEnsureEngine() {
  ReaderState& R = RS(*this);
  if (R.workerStarted) return;
  R.workerStarted = true;
  R.worker.start([this](std::function<void()> fn) { post(std::move(fn)); });
  // the first task loads (and on first use unpacks) the DLL; nothing else runs before it
  R.worker.submit(-100, [this](pdf::Worker&) {
    PdfEngine e = pdfEngineLoad();
    post([this, e]() {
      ReaderState& r = RS(*this);
      r.engineTried = true;
      r.engine = e;
      r.engineReady = e.ready;
      if (!e.ready) { r.doc.failed = true; r.doc.error = e.error; }
      needFrame = true;
    });
  });
}

void App::readerShutdown() {
  if (!rd) return;
  rd->tiles.clear();
  rd->worker.stop();
  rd->workerStarted = false;
}

// Everything of the open document goes: the engine's handle (closed on the worker), tiles, previews, text, hits, the
// selection and the page geometry of the last frame. The generation keeps growing so that results of the old document
// still in flight are recognised and dropped.
void App::readerResetDoc() {
  if (!rd) return;
  ReaderState& R = *rd;
  if (R.doc.id >= 0 && R.workerStarted) { int id = R.doc.id; R.worker.submit(0, [id](pdf::Worker& w) { w.closeDoc(id); }); }
  for (auto& kv : R.tileReq) kv.second.cancel();
  R.tileReq.clear();
  R.tiles.clear();
  R.tileBytes = 0;
  R.tileK = R.tileKPrev = 0;
  R.hits.clear();
  R.hitCur = -1;
  R.findGen++;
  R.findRunning = false;
  R.findLast.clear();
  R.selPage = -1;
  R.selA = R.selB = -1;
  R.inkDrag = false; R.inkPage = -1; R.inkPoints.clear(); R.inkUndo.clear();
  R.inkMove = R.inkMoveDragged = false; R.inkMoveId.clear(); R.inkMovePage = -1; R.inkMoveOrig.clear(); R.inkMoveOrigRect = {}; R.inkMoveOrigInFile = false;
  R.miniShow = false;
  R.curAnnot.clear();
  R.hoverAnnot.clear();
  R.cardAnnot.clear();
  R.pageTop.clear();
  R.pageLeft.clear();
  R.kPrev = 0;
  R.doc = RdDoc();
  R.doc.gen = ++R.docGen;
  R.closedT = 0;
}

void App::readerProjectChanged() {
  bibliographyReaderWasOpen = false;
  bibliographyReaderItem.clear();
  bibliographyReaderReturnToPapers = false;
  bibliographyReaderReturnRoute = BR_REVIEW;
  if (rd) { rd->paperTagCacheKey.clear(); rd->paperTagCacheVersion = 0; rd->paperTagCache.clear(); }
  if (!rd) { readerOpen = false; return; }
  readerResetDoc();
  readerOpen = false;
}

// Memory of a closed reader, in tiers (the reader itself keeps small budgets while it is open — tiles beyond the
// view leave after 1.5 s, text and previews are kept for 24 pages around the view):
//  * on close: the tiles and previews go at once (GPU memory), the document stays for an instant return;
//  * kReleaseAfterS later: the document and its page objects are closed (the engine's page caches with them);
//  * kTrimAfterS after that, in a quiet moment (no input for 2 s, no job): the engine library itself is destroyed
//    (its allocator only returns memory then; it is initialised again on the next open, in milliseconds), the GPU
//    transient allocations are trimmed and the process working set is trimmed once. Nothing is done periodically:
//    a working set trimmed while the user works only comes back through page faults, as stutter.
double App::readerIdleTick() {
  if (!rd || readerOpen || rd->closedT <= 0) return -1;
  ReaderState& R = *rd;
  if (readerPrinting) return 5;
  const double idle = nowSeconds() - R.closedT;
  if (!R.released) {
    const double left = kReleaseAfterS - idle;
    if (left > 0) return left;
    const double closedT = R.closedT;
    readerResetDoc();
    R.closedT = closedT;
    R.released = true;
    return std::max(0.5, kTrimAfterS + left);
  }
  const double left = kReleaseAfterS + kTrimAfterS - idle;
  if (left > 0) return left;
  if (nowSeconds() - lastInputT < 2 || job) return 1;  // a quiet moment
  if (R.workerStarted) R.worker.submit(0, [](pdf::Worker& w) { w.releaseLibrary(); });
  if (g.dev) g.trim();
  SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
  R.closedT = 0;
  return -1;
}

// loads the item's file into the engine and fetches what the first paint needs
static void rdOpenDoc(App& A, const PdfItem& it) {
  ReaderState& R = RS(A);
  A.readerEnsureEngine();
  string pw = R.password;
  A.readerResetDoc();
  R.positionRestored = false;
  uint32_t gen = R.doc.gen;
  R.doc.id = R.worker.newDocId();
  R.doc.itemId = it.id;
  R.doc.path = it.path;
  R.zoom = float(it.zoom);
  R.rot = it.rot & 3;
  R.zoomShown = 0;
  R.scroll = R.scrollTarget = 0;
  R.scrollX = 0;
  int id = R.doc.id;
  string path = it.path;
  R.worker.submit(0, [&A, id, gen, path, pw](pdf::Worker& w) {
    pdf::Document& d = w.doc(id);
    string err;
    bool ok = d.open(path, &err, pw);
    pdf::DocInfo info = ok ? d.info() : pdf::DocInfo();
    bool needsPw = !ok && d.passwordNeeded();
    auto first = std::make_shared<pdf::TextPage>();
    if (ok && info.pages > 0) d.text(0, *first);
    A.post([&A, id, gen, ok, err, info, needsPw, first]() {
      ReaderState& r = RS(A);
      if (r.doc.gen != gen || r.doc.id != id) return;
      r.doc.ready = ok;
      r.doc.failed = !ok;
      r.doc.error = err;
      r.doc.needsPassword = needsPw;
      if (needsPw) r.passwordFocus = true;
      r.doc.info = info;
      r.doc.pages.assign(size_t(std::max(0, info.pages)), RdPage());
      for (size_t i = 0; i < r.doc.pages.size() && i < info.sizes.size(); i++) { r.doc.pages[i].w = std::max(1.f, info.sizes[i][0]); r.doc.pages[i].h = std::max(1.f, info.sizes[i][1]); }
      r.pageTop.clear();  // the geometry of the previous document is void until the next frame lays this one out
      if (ok && !r.doc.pages.empty() && !first->empty()) { r.doc.pages[0].text = *first; r.doc.pages[0].textDone = r.doc.pages[0].textReq = true; }
      if (ok) {  // unlinked items are read for a DOI / title (once per open at most; linked ones only need the page count)
        PdfItem* it2 = A.P->library.find(r.doc.itemId);
        if (it2 && it2->pages != info.pages) { it2->pages = info.pages; A.P->dirty = true; }
        if (it2 && it2->recordKey.empty()) A.readerIdentify(r.doc.itemId);
      }
      A.needFrame = true;
    });
  });
}

void App::openReader(const string& itemId, int toPage, float toY) {
  PdfItem* it = P->library.find(itemId);
  if (!it) return;
  if (workspace != WS_BIBLIOGRAPHY) setWorkspace(WS_BIBLIOGRAPHY);
  if (!readerOpen && !bibliographyReaderWasOpen) {
    bibliographyReaderReturnToPapers = papersOpen;
    bibliographyReaderReturnRoute = lastBibliographyRoute == BR_PAPERS ? BR_PAPERS : BR_REVIEW;
  }
  ReaderState& R = RS(*this);
  writerOpen = false;
  readerOpen = true;
  showStart = false;
  papersOpen = false;
  mainChartOpen = false;
  figZoomOpen = false;
  ui.focus = 0;
  page = PG_READ;
  lastBibliographyRoute = BR_REVIEW;
  bibliographyReaderItem = itemId;
  settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  it->opened = nowIso();
  if (it->status == ReadStatus::ToRead) { it->status = ReadStatus::Reading; P->library.touchOrganization(); P->dirty = true; }
  R.pendingPage = toPage;
  R.pendingY = toY;
  R.closedT = 0;
  if (R.doc.itemId == itemId && R.doc.id >= 0 && !R.doc.failed) return;
  R.password.clear();
  rdOpenDoc(*this, *it);
}

// The document stays open for a while (coming back from Write is instant); the big caches go at once: the tiles and
// the previews are GPU bitmaps that render again in a few milliseconds. readerIdleTick() releases the rest later.
void App::closeReader() {
  if (!readerOpen) return;
  readerStorePosition();
  readerCloseCard();
  readerOpen = false;
  ui.focus = 0;
  if (workspace == WS_BIBLIOGRAPHY) {
    if (!bibliographyReaderWasOpen) lastBibliographyRoute = bibliographyReaderReturnRoute;
    page = PG_READ;
    papersOpen = bibliographyReaderReturnToPapers;
    settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  }
  if (!bibliographyReaderWasOpen) bibliographyReaderReturnToPapers = false;
  if (!rd) return;
  ReaderState& R = *rd;
  R.miniShow = false;
  R.selPage = -1;
  R.selA = R.selB = -1;
  R.dragging = R.panning = R.selGesture = false;
  R.inkDrag = false; R.inkPage = -1; R.inkPoints.clear();
  R.barDrag = 0;
  for (auto& kv : R.tileReq) kv.second.cancel();
  R.tileReq.clear();
  R.tiles.clear();
  R.tileBytes = 0;
  R.tileK = R.tileKPrev = 0;
  for (auto& p : R.doc.pages) { p.preview.reset(); p.previewReq = false; p.pw = p.ph = 0; }
  R.closedT = nowSeconds();
  R.released = false;
  // the engine's page objects of this document are not needed until the reader opens again
  if (R.doc.id >= 0 && R.workerStarted) { const int id = R.doc.id; R.worker.submit(30, [id](pdf::Worker& w) { if (w.hasDoc(id)) w.doc(id).dropPages(); }); }
}

// ------------------------------------------------------------------ small helpers
PdfItem* App::readerItem() {
  if (!rd || rd->doc.itemId.empty()) return nullptr;
  return P->library.find(rd->doc.itemId);
}

int App::readerRecord() {
  PdfItem* it = readerItem();
  if (!it || !hasCorpus()) return -1;
  return P->library.recordIndex(P->corpus, *it);
}

void App::readerStorePosition() {
  if (!rd || rd->pageTop.empty() || !rd->doc.ready) return;
  ReaderState& R = *rd;
  PdfItem* it = readerItem();
  if (!it) return;
  // the page at the top of the view and how far into it
  float k = R.kShown;
  const size_t n = std::min(R.pageTop.size(), R.doc.pages.size());  // the geometry can lag a page count change by a frame
  for (size_t i = 0; i < n; i++) {
    float top = R.pageTop[i], h = rdH(R, R.doc.pages[i]) * k;
    if (top + h > 0 || i + 1 == n) { it->lastPage = double(i) + (top < 0 ? clampv(-top / std::max(1.f, h), 0.f, 0.999f) : 0.0); break; }
  }
  it->zoom = R.zoom;
}

string App::readerSelectedText() {
  ReaderState& R = RS(*this);
  if (R.selPage < 0 || R.selA < 0 || size_t(R.selPage) >= R.doc.pages.size()) return string();
  const pdf::TextPage& tp = R.doc.pages[size_t(R.selPage)].text;
  if (tp.empty()) return string();
  return joinLines(tp.slice(std::min(R.selA, R.selB), std::max(R.selA, R.selB)));
}

void App::readerClearSelection() {
  ReaderState& R = RS(*this);
  R.selPage = -1;
  R.selA = R.selB = -1;
  R.miniShow = false;
  R.miniArea.clear();
}

void App::readerGoTo(int pg, float y, bool animate) {
  ReaderState& R = RS(*this);
  if (!R.doc.ready || pg < 0 || size_t(pg) >= R.doc.pages.size()) return;
  float k = R.kShown > 0 ? R.kShown : 1;
  vector<float> top, left;
  float colW = 0, colH = 0;
  rdLayout(R, k, 14 * ui.s, 20 * ui.s, top, left, colW, colH);
  // `y` is a point of the unrotated page (a hit, a bookmark): on a turned page only the half turn keeps a height
  const RdPage& p = R.doc.pages[size_t(pg)];
  const float yd = y > 0 ? (R.rot == 0 ? y : R.rot == 2 ? std::max(0.f, p.h - y) : 0.f) : 0.f;
  float target = top[size_t(pg)] + yd * k - (yd > 0 ? 40 * ui.s : 12 * ui.s);
  R.scrollTarget = std::max(0.f, target);
  if (!animate) R.scroll = R.scrollTarget;
  needFrame = true;
}

int App::readerCurrentPage() const {
  if (!rd) return 0;
  const ReaderState& R = *rd;
  // pageTop is the last frame's geometry; right after an open it can still describe the previous document
  const size_t n = std::min(R.pageTop.size(), R.doc.pages.size());
  if (n == 0) return 0;
  float mid = R.viewH * 0.35f;
  for (size_t i = 0; i < n; i++) if (R.pageTop[i] + rdH(R, R.doc.pages[i]) * R.kShown > mid) return int(i);
  return int(n) - 1;
}

// the page one step (a row of the layout) before or after the current one: the target of the arrows and the buttons
int App::readerPageStep(int dir) const {
  if (!rd) return 0;
  const ReaderState& R = *rd;
  const int n = int(R.doc.pages.size());
  if (n == 0) return 0;
  const int row = rdRowOf(R, readerCurrentPage()) + dir;
  const int rows = rdRowOf(R, n - 1) + 1;
  return rdRowFirst(R, clampv(row, 0, rows - 1));
}

// the annotation under a page point (page coordinates in points)
static string rdAnnotAt(App& A, const PdfItem& it, int pg, float x, float y, float k) {
  float pad = 3 / std::max(0.1f, k);
  for (auto it2 = it.annots.rbegin(); it2 != it.annots.rend(); ++it2) {
    const PdfAnnot& a = *it2;
    if (a.page != pg) continue;
    if (a.kind == 3) { if (x >= a.rect.x0 - pad && x <= a.rect.x0 + 18 / k + pad && y >= a.rect.y0 - pad && y <= a.rect.y0 + 18 / k + pad) return a.id; continue; }
    if (a.kind == 4 || a.kind == 5) { if (x >= a.rect.x0 - pad && x <= a.rect.x1 + pad && y >= a.rect.y0 - pad && y <= a.rect.y1 + pad) return a.id; continue; }
    for (auto& q : a.quads) if (x >= q.x0 - pad && x <= q.x1 + pad && y >= q.y0 - pad && y <= q.y1 + pad) return a.id;
  }
  (void)A;
  return string();
}

// ------------------------------------------------------------------ annotations from the selection
PdfAnnot* App::readerAddMarkup(int kind, Code code) {
  ReaderState& R = RS(*this);
  PdfItem* it = readerItem();
  if (!it || R.selPage < 0 || R.selA < 0) return nullptr;
  const pdf::TextPage& tp = R.doc.pages[size_t(R.selPage)].text;
  int a = std::min(R.selA, R.selB), b = std::max(R.selA, R.selB);
  PdfAnnot an;
  an.kind = kind;
  an.page = R.selPage;
  an.quads = tp.rects(a, b);
  an.text = joinLines(tp.slice(a, b));
  an.start = a;
  an.count = b - a + 1;
  an.code = code;
  an.color = codeColor(code);
  if (an.quads.empty()) return nullptr;
  PdfAnnot& ref = P->library.addAnnot(*it, an);
  P->dirty = true;
  R.curAnnot = ref.id;
  readerClearSelection();
  if (R.pane == 0 || R.pane == 3) R.pane = 2;
  if (!R.cardAnnot.empty()) readerCloseCard();
  return &ref;
}

PdfAnnot* App::readerAddNote(int pg, float x, float y) {
  PdfItem* it = readerItem();
  if (!it || pg < 0) return nullptr;
  PdfAnnot an;
  an.kind = 3;
  an.page = pg;
  an.rect = {x, y, x + 18, y + 18};
  an.color = 0xFFFFC94D;
  PdfAnnot& ref = P->library.addAnnot(*it, an);
  P->dirty = true;
  ReaderState& R = RS(*this);
  R.curAnnot = ref.id;
  if (R.pane == 0 || R.pane == 3) R.pane = 2;
  readerOpenCard(ref.id, true);
  return &ref;
}

static void rdOpenWriterBehind(App& A);

// An area of a page: a scanned page, a figure, a table, an equation — anything without selectable text. The text
// under the rectangle (when the page has a text layer) is kept as the passage, so quotes, notes exports and the
// coding matrix treat it like a highlight; the area is saved into the PDF as a Square annotation.
PdfAnnot* App::readerAddArea(int pg, float x0, float y0, float x1, float y1) {
  PdfItem* it = readerItem();
  ReaderState& R = RS(*this);
  if (!it || pg < 0 || size_t(pg) >= R.doc.pages.size()) return nullptr;
  const RdPage& p = R.doc.pages[size_t(pg)];
  pdf::Box b{clampv(std::min(x0, x1), 0.f, p.w), clampv(std::min(y0, y1), 0.f, p.h), clampv(std::max(x0, x1), 0.f, p.w), clampv(std::max(y0, y1), 0.f, p.h)};
  if (b.x1 - b.x0 < 2 || b.y1 - b.y0 < 2) return nullptr;
  PdfAnnot an;
  an.kind = 4;
  an.page = pg;
  an.rect = b;
  an.color = codeColor(Code::None);
  if (p.textDone && !p.text.empty()) {  // the text inside, line by line
    string t;
    for (auto& ln : p.text.lines) {
      string line;
      for (int c = ln[0]; c <= ln[1] && size_t(c) < p.text.chars.size(); c++) {
        const pdf::Char& ch = p.text.chars[size_t(c)];
        if (ch.generated) continue;
        float cx = (ch.b.x0 + ch.b.x1) / 2, cy = (ch.b.y0 + ch.b.y1) / 2;
        if (cx < b.x0 || cx > b.x1 || cy < b.y0 || cy > b.y1) continue;
        size_t at = size_t(c) < p.text.byteAt.size() ? p.text.byteAt[size_t(c)] : p.text.text.size();
        size_t to = size_t(c) + 1 < p.text.byteAt.size() ? p.text.byteAt[size_t(c) + 1] : p.text.text.size();
        if (at < to && to <= p.text.text.size()) line += p.text.text.substr(at, to - at);
      }
      line = trim(line);
      if (!line.empty()) t += (t.empty() ? "" : "\n") + line;
    }
    an.text = t;
  }
  PdfAnnot& ref = P->library.addAnnot(*it, an);
  P->dirty = true;
  R.curAnnot = ref.id;
  readerClearSelection();
  if (R.pane == 0 || R.pane == 3) R.pane = 2;
  if (!R.cardAnnot.empty()) readerCloseCard();
  return &ref;
}

PdfAnnot* App::readerAddInk(int pg, vector<pdf::InkPoint> points) {
  PdfItem* it = readerItem();
  ReaderState& R = RS(*this);
  if (!it || pg < 0 || size_t(pg) >= R.doc.pages.size() || points.size() < 2) return nullptr;
  if (points.size() > 4096) points.resize(4096);
  const RdPage& page = R.doc.pages[size_t(pg)];
  pdf::InkStroke stroke;
  stroke.points.reserve(points.size());
  pdf::Box bounds;
  bool first = true;
  for (auto& p : points) {
    pdf::InkPoint pt{clampv(p.x, 0.f, page.w), clampv(p.y, 0.f, page.h)};
    stroke.points.push_back(pt);
    if (first) { bounds = {pt.x, pt.y, pt.x, pt.y}; first = false; }
    else { bounds.x0 = std::min(bounds.x0, pt.x); bounds.y0 = std::min(bounds.y0, pt.y); bounds.x1 = std::max(bounds.x1, pt.x); bounds.y1 = std::max(bounds.y1, pt.y); }
  }
  if (stroke.points.size() < 2) return nullptr;
  if (bounds.x1 <= bounds.x0) bounds.x1 = bounds.x0 + 0.01f;
  if (bounds.y1 <= bounds.y0) bounds.y1 = bounds.y0 + 0.01f;
  PdfAnnot ink;
  ink.kind = 5;
  ink.page = pg;
  ink.strokes.push_back(std::move(stroke));
  ink.rect = bounds;
  ink.color = R.inkColor;
  ink.strokeWidth = clampv(R.inkWidth, 0.25f, 12.f);
  PdfAnnot& ref = P->library.addAnnot(*it, ink);
  R.inkUndo.push_back({ref.id, true, false, {}, {}});
  if (R.inkUndo.size() > 100) R.inkUndo.erase(R.inkUndo.begin());
  P->dirty = true;
  R.curAnnot = ref.id;
  readerClearSelection();
  if (R.pane == 0 || R.pane == 3) R.pane = 2;
  return &ref;
}

void App::readerUndoInk() {
  PdfItem* it = readerItem();
  if (!it) return;
  ReaderState& R = RS(*this);
  while (!R.inkUndo.empty()) {
    ReaderState::InkUndoAction action = std::move(R.inkUndo.back());
    R.inkUndo.pop_back();
    PdfAnnot* a = P->library.annot(*it, action.id);
    if (!a || a->kind != 5) continue;
    if (action.created) {
      readerDeleteAnnot(action.id);
      ui.toast("Drawing undone", "The last freehand annotation was removed from the project.", 1, 2);
    } else {
      a->strokes = std::move(action.strokes);
      a->rect = action.rect;
      a->inFile = action.inFile;
      a->modified = nowIso();
      P->dirty = true;
      ui.toast("Drawing move undone", "The freehand annotation was restored to its previous position.", 1, 2);
    }
    needFrame = true;
    return;
  }
}

// The picture of an area: rendered by the engine at 200 dpi (with the file's own annotations left out), then put
// on the clipboard as a bitmap, or into the document as a figure captioned with the paper and the page.
void App::readerAreaImage(const string& id, int how) {
  PdfItem* it = readerItem();
  ReaderState& R = RS(*this);
  PdfAnnot* a = it ? P->library.annot(*it, id) : nullptr;
  if (!a || a->kind != 4 || !R.doc.ready || a->page < 0 || size_t(a->page) >= R.doc.pages.size()) return;
  const double sc = 200.0 / 72.0;
  const int rot = R.rot;  // the picture comes out as the page is viewed (a turned scan reads upright)
  const RdPage& pge = R.doc.pages[size_t(a->page)];
  const pdf::Box b = pdf::rotateBox(rot, pge.w, pge.h, a->rect);
  int x = int(std::floor(b.x0 * sc)), y = int(std::floor(b.y0 * sc));
  int w = std::max(1, int(std::ceil(b.x1 * sc)) - x), h = std::max(1, int(std::ceil(b.y1 * sc)) - y);
  if (int64_t(w) * h > 40000000) { ui.toast("Area", "This area is too large to render as a picture.", 2, 4); return; }
  const int docId = R.doc.id, pg = a->page;
  const uint32_t gen = R.doc.gen;
  const int rec = readerRecord();
  string caption = rec >= 0 && !P->corpus.recs[size_t(rec)].authors.empty() ? P->corpus.recs[size_t(rec)].authors[0] + (P->corpus.recs[size_t(rec)].authors.size() > 1 ? " et al." : "") + (P->corpus.recs[size_t(rec)].year > 0 ? " (" + std::to_string(P->corpus.recs[size_t(rec)].year) + ")" : "")
                                                                          : truncate(it->title.empty() ? fileName(it->path) : it->title, 60);
  caption += ", p. " + std::to_string(pg + 1);
  if (!a->note.empty()) caption = truncate(a->note, 120) + " \xE2\x80\x94 " + caption;
  const string title = caption;
  const float wPt = b.x1 - b.x0, hPt = b.y1 - b.y0;
  R.worker.submit(-5, [this, docId, gen, pg, sc, x, y, w, h, how, caption, title, wPt, hPt, rec, rot](pdf::Worker& wk) {
    if (!wk.hasDoc(docId)) return;
    auto bm = std::make_shared<pdf::Bitmap>();
    wk.doc(docId).render(pg, sc, x, y, w, h, *bm, false, false, rot);
    post([this, gen, bm, how, caption, title, wPt, hPt, rec]() {
      if (bm->w <= 0 || bm->h <= 0) { ui.toast("Area", "The area could not be rendered.", 2, 4); return; }
      if (how == 0) {
        if (setClipboardImage(hwnd, bm->w, bm->h, bm->bgra.data())) ui.toast("Copied", "The area is on the clipboard as a picture (" + std::to_string(bm->w) + "\xC3\x97" + std::to_string(bm->h) + " px).", 1, 3);
        else ui.toast("Copy", "The clipboard was not available.", 2, 3);
        return;
      }
      (void)gen;
      Scene sc2;
      sc2.W = wPt; sc2.H = hPt;
      Prim im;
      im.type = Prim::Image; im.x = 0; im.y = 0; im.w = float(wPt); im.h = float(hPt); im.imgW = bm->w; im.imgH = bm->h; im.group = "image";
      im.rgba.resize(size_t(bm->w) * size_t(bm->h) * 4);
      for (size_t i = 0; i + 3 < im.rgba.size() && i + 3 < bm->bgra.size(); i += 4) { im.rgba[i] = bm->bgra[i + 2]; im.rgba[i + 1] = bm->bgra[i + 1]; im.rgba[i + 2] = bm->bgra[i]; im.rgba[i + 3] = 255; }
      sc2.items.push_back(std::move(im));
      rdOpenWriterBehind(*this);
      int asset = wdoc.addAsset(sc2, "pdf_area", title, "Picture of an area of a PDF page: " + caption);
      wed.begin("Insert figure");
      wed.insertFigure(asset, caption, wPt > 300 ? 100 : 60);
      if (rec >= 0) writerCiteMany({rec});
      writerFollow_ = true;
      writerCaretX_ = -1;
      ui.toast("Figure added", "The area is in the document at the caret" + string(rec >= 0 ? ", with a citation of this paper." : "."), 1, 4);
    });
  });
}

void App::readerDeleteAnnot(const string& id) {
  PdfItem* it = readerItem();
  if (!it) return;
  ReaderState& R = RS(*this);
  if (R.cardAnnot == id) { R.cardAnnot.clear(); R.cardFocus = false; if (ui.focus == ui.id("ta:rdcard")) ui.focus = 0; }
  R.inkUndo.erase(std::remove_if(R.inkUndo.begin(), R.inkUndo.end(), [&](const ReaderState::InkUndoAction& action) { return action.id == id; }), R.inkUndo.end());
  if (P->library.removeAnnot(*it, id)) P->dirty = true;
  if (R.curAnnot == id) R.curAnnot.clear();
  if (R.hoverAnnot == id) R.hoverAnnot.clear();
  if (R.miniArea == id) { R.miniArea.clear(); R.miniShow = false; }
}

// the comment card: opened next to an annotation; what is typed is stored at once, closing only trims it
void App::readerOpenCard(const string& annotId, bool focus) {
  ReaderState& R = RS(*this);
  PdfItem* it = readerItem();
  const PdfAnnot* a = it ? P->library.annot(*it, annotId) : nullptr;
  if (!a) return;
  if (R.cardAnnot != annotId) readerCloseCard();
  R.cardAnnot = annotId;
  R.curAnnot = annotId;
  R.noteBuf = a->note;
  R.cardFocus = focus;
  R.miniShow = false;
  if (!focus && ui.focus == ui.id("ta:rdcard")) ui.focus = 0;
}

void App::readerCloseCard() {
  if (!rd || rd->cardAnnot.empty()) return;
  ReaderState& R = *rd;
  if (PdfItem* it = readerItem()) {
    if (PdfAnnot* a = P->library.annot(*it, R.cardAnnot)) {
      string t = trim(R.noteBuf);
      if (a->note != t) { a->note = t; a->modified = nowIso(); a->inFile = false; P->dirty = true; }
    }
  }
  R.cardAnnot.clear();
  R.cardFocus = false;
  R.cardR = {0, 0, 0, 0};
  if (ui.focus == ui.id("ta:rdcard")) ui.focus = 0;
}

// the writer is opened underneath the reader when it is not open yet (openWriter() would close the reader)
static void rdOpenWriterBehind(App& A) {
  if (A.writerOpen) return;
  bool keep = A.readerOpen;
  A.readerOpen = false;
  A.openWriter();
  A.readerOpen = keep;
  if (keep) A.page = PG_READ;
}

void App::readerQuote(const string& text, int rec) {
  if (text.empty()) return;
  rdOpenWriterBehind(*this);
  wed.begin("Quote");
  if (const Para* p = docPara(wdoc, wed.caret)) { if (p->size() > 0) wed.insertParagraphBreak(); }
  wed.setStyle(PStyle::Quote);
  wed.insertText(text);
  if (rec >= 0) { wed.insertText(" "); writerCiteMany({rec}); }
  wed.insertParagraphBreak();
  wed.setStyle(PStyle::Body);
  writerFollow_ = true;
  ui.toast("Quoted into the writer", rec >= 0 ? "With a citation of the linked paper. Open Write to see it." : "Open Write to see it. Link the PDF to a record to cite it automatically.", 1, 4);
}

void App::readerCite() {
  int rec = readerRecord();
  if (rec < 0) { ui.toast("Not linked", "Link this PDF to a record (Paper pane) to cite it.", 2, 4); return; }
  rdOpenWriterBehind(*this);
  writerCiteMany({rec});
}

// ------------------------------------------------------------------ find
void App::readerFindStart() {
  ReaderState& R = RS(*this);
  R.findGen++;
  R.hits.clear();
  R.hitCur = -1;
  R.findPages = 0;
  R.findLast = R.findQ;
  R.findRunning = false;
  string q = trim(R.findQ);
  if (q.empty() || !R.doc.ready) return;
  R.findRunning = true;
  std::u16string needle = pdf::utf8to16(q);
  int gen = R.findGen, id = R.doc.id, n = int(R.doc.pages.size()), from = readerCurrentPage();
  bool mc = R.findCase, ww = R.findWord;
  uint32_t dgen = R.doc.gen;
  for (int i = 0; i < n; i++) {
    int pg = (from + i) % n;
    R.worker.submit(40, [this, id, gen, dgen, pg, needle, mc, ww, n](pdf::Worker& w) {
      if (w.current().cancelled()) return;
      if (!w.hasDoc(id)) return;
      auto out = std::make_shared<vector<pdf::Hit>>();
      w.doc(id).find(pg, needle, mc, ww, *out, nullptr);
      post([this, gen, dgen, out, n]() {
        ReaderState& r = RS(*this);
        if (r.findGen != gen || r.doc.gen != dgen) return;
        int curPage = -1, curStart = -1;
        if (r.hitCur >= 0 && size_t(r.hitCur) < r.hits.size()) { curPage = r.hits[size_t(r.hitCur)].page; curStart = r.hits[size_t(r.hitCur)].start; }
        r.hits.insert(r.hits.end(), out->begin(), out->end());
        std::stable_sort(r.hits.begin(), r.hits.end(), [](const pdf::Hit& a, const pdf::Hit& b) { return a.page != b.page ? a.page < b.page : a.start < b.start; });
        if (curPage >= 0) for (size_t i = 0; i < r.hits.size(); i++) if (r.hits[i].page == curPage && r.hits[i].start == curStart) { r.hitCur = int(i); break; }
        r.findPages++;
        if (r.findPages >= n) r.findRunning = false;
        if (r.hitCur < 0 && !r.hits.empty()) { readerFindNext(1, true); }
        needFrame = true;
      });
    });
  }
}

void App::readerFindNext(int dir, bool fromView) {
  ReaderState& R = RS(*this);
  if (R.hits.empty()) return;
  int n = int(R.hits.size());
  if (fromView || R.hitCur < 0) {
    int cur = readerCurrentPage();
    R.hitCur = 0;
    for (int i = 0; i < n; i++) if (R.hits[size_t(i)].page >= cur) { R.hitCur = i; break; }
  } else {
    R.hitCur = (R.hitCur + dir + n) % n;
  }
  const pdf::Hit& h = R.hits[size_t(R.hitCur)];
  float y = h.rects.empty() ? 0 : h.rects[0].y0;
  // scroll so that the hit sits in the upper third of the view
  float k = R.kShown > 0 ? R.kShown : 1;
  float gap = 14 * ui.s, top = 20 * ui.s;
  for (int i = 0; i < h.page; i++) top += R.doc.pages[size_t(i)].h * k + gap;
  float target = top + y * k - R.viewH * 0.33f;
  R.scrollTarget = std::max(0.f, target);
  needFrame = true;
}

// ------------------------------------------------------------------ keys
bool App::readerKey(int vk) {
  ReaderState& R = RS(*this);
  const Input& in = ui.in;
  const bool ctrl = in.ctrl, shift = in.shift;
  const float lineStep = 56 * ui.s, pageStep = std::max(80 * ui.s, R.viewH - 48 * ui.s);
  auto zoomBy = [&](float f) { float z = R.zoom > 0 ? R.zoom : R.zoomShown; R.zoom = clampv(z * f, kMinZoom, kMaxZoom); R.zoomT = ui.time; R.zoomAnchorX = R.zoomAnchorY = -1; };
  if (ctrl) {
    switch (vk) {
      case 'F': R.pane = 3; R.findFocus = true; return true;
      case 'C': { string t = readerSelectedText(); if (!t.empty()) { setClipboardText(hwnd, t); ui.toast("Copied", truncate(t, 60), 1, 2); } return true; }
      case 'G': R.pageBuf = std::to_string(readerCurrentPage() + 1); ui.focusText(ui.id("ti:rdpage"), R.pageBuf); return true;
      case VK_OEM_PLUS: case VK_ADD: zoomBy(1.2f); return true;
      case VK_OEM_MINUS: case VK_SUBTRACT: zoomBy(1 / 1.2f); return true;
      case '0': case VK_NUMPAD0: R.zoom = 0; R.zoomT = ui.time; return true;
      case '1': R.zoom = 1; R.zoomT = ui.time; return true;
      case '2': R.zoom = 2; R.zoomT = ui.time; return true;
      case VK_HOME: R.scrollTarget = 0; return true;
      case VK_END: R.scrollTarget = 1e9f; return true;
      case 'E': readerCite(); return true;
      case 'Q': { string t = readerSelectedText(); if (!t.empty()) readerQuote(t, readerRecord()); return true; }
      case 'D': if (shift && !R.curAnnot.empty()) { readerDeleteAnnot(R.curAnnot); return true; } break;
      case 'Z': if (!R.inkUndo.empty()) { readerUndoInk(); return true; } break;
      case 'P': if (!shift) { readerPrintDialog(); return true; } break;
    }
    return false;
  }
  switch (vk) {
    case VK_ESCAPE:
      if (ui.anyPopup()) return true;  // the menu closes (Ui::endFrame); the reader stays
      if (!R.cardAnnot.empty()) { readerCloseCard(); return true; }
      if (R.inkDrag) { R.inkDrag = false; R.inkPage = -1; R.inkPoints.clear(); needFrame = true; return true; }
      if (R.miniShow || R.selPage >= 0) { readerClearSelection(); return true; }
      if (R.inkTool) { R.inkTool = false; return true; }
      if (R.areaTool) { R.areaTool = false; return true; }
      if (R.pane == 3 && !R.findQ.empty()) { R.findQ.clear(); readerFindStart(); return true; }
      closeReader();
      return true;
    case VK_F3: readerFindNext(shift ? -1 : 1, false); return true;
    case VK_PRIOR: R.scrollTarget -= pageStep; return true;
    case VK_NEXT: R.scrollTarget += pageStep; return true;
    case VK_SPACE: R.scrollTarget += shift ? -pageStep : pageStep; return true;
    case VK_HOME: R.scrollTarget = 0; return true;
    case VK_END: R.scrollTarget = 1e9f; return true;
    case VK_UP: R.scrollTarget -= lineStep; return true;
    case VK_DOWN: R.scrollTarget += lineStep; return true;
    case VK_LEFT:  // sideways when the page is wider than the view, else the previous page
      if (R.maxScrollX > 0) { R.scrollX = clampv(R.scrollX - 80 * ui.s, 0.f, R.maxScrollX); needFrame = true; return true; }
      readerGoTo(readerPageStep(-1), -1, true);
      return true;
    case VK_RIGHT:
      if (R.maxScrollX > 0) { R.scrollX = clampv(R.scrollX + 80 * ui.s, 0.f, R.maxScrollX); needFrame = true; return true; }
      readerGoTo(readerPageStep(1), -1, true);
      return true;
    case VK_OEM_PLUS: case VK_ADD: zoomBy(1.2f); return true;
    case VK_OEM_MINUS: case VK_SUBTRACT: zoomBy(1 / 1.2f); return true;
    case VK_DELETE: if (!R.curAnnot.empty()) { readerDeleteAnnot(R.curAnnot); return true; } return false;
    case 'U': if (R.selPage >= 0) { readerAddMarkup(1, Code::None); return true; } return false;
    case 'S': if (R.selPage >= 0) { readerAddMarkup(2, Code::None); return true; } return false;
    case 'N': if (R.selPage >= 0) { if (PdfAnnot* a = readerAddMarkup(0, Code::None)) readerOpenCard(a->id, true); return true; } return false;
    case 'H': if (R.selPage >= 0) { readerAddMarkup(0, Code::None); return true; } return false;
    case 'D': readerSetDark(!R.dark); ui.toast(R.dark ? "Dark page" : "Light page", R.dark ? "Pages light-on-dark; photographs stay as they are. D switches back." : "The paper as it is.", 1, 2); return true;
    case 'A': R.areaTool = !R.areaTool; if (R.areaTool) R.inkTool = false; ui.toast(R.areaTool ? "Area tool on" : "Area tool off", R.areaTool ? "Drag a rectangle on a page to mark an area. A or Esc turns the tool off; Alt + drag works any time." : "", 1, R.areaTool ? 5 : 1); return true;
    case 'I': R.inkTool = !R.inkTool; if (R.inkTool) R.areaTool = false; ui.toast(R.inkTool ? "Draw tool on" : "Draw tool off", R.inkTool ? "Drag over a page to draw a freehand annotation; I or Esc turns the tool off." : "", 1, R.inkTool ? 5 : 1); return true;
    case 'R': readerRotate(shift ? -1 : 1); return true;
    case VK_F1: return false;
  }
  // 1..7 code the selection, or re-code the area the mini toolbar is about
  if (vk >= '1' && vk <= '7' && R.selPage >= 0) { readerAddMarkup(0, Code(vk - '0')); return true; }
  if (vk >= '1' && vk <= '7' && !R.miniArea.empty()) {
    if (PdfItem* it = readerItem()) if (PdfAnnot* a = P->library.annot(*it, R.miniArea)) { a->code = Code(vk - '0'); a->color = codeColor(a->code); a->modified = nowIso(); a->inFile = false; P->dirty = true; }
    return true;
  }
  return false;
}

void App::readerTyped() {}  // the reader has no text of its own; text fields take the keyboard when focused

// ------------------------------------------------------------------ requests to the engine
static void rdRequestPreview(App& A, int pg) {
  ReaderState& R = RS(A);
  RdPage& P2 = R.doc.pages[size_t(pg)];
  if (P2.previewReq) return;
  P2.previewReq = true;
  int id = R.doc.id;
  uint32_t gen = R.doc.gen;
  const int targetW = rdPreviewTargetW(R);
  double sc = targetW / std::max(1.f, rdW(R, P2));
  const int rot = R.rot, filter = R.filterMode;
  int w = int(std::lround(rdW(R, P2) * sc)), h = int(std::lround(rdH(R, P2) * sc));
  const bool dark = R.dark;
  R.worker.submit(5, [&A, id, gen, pg, sc, w, h, targetW, dark, filter, rot](pdf::Worker& wk) {
    if (!wk.hasDoc(id)) return;
    auto bm = std::make_shared<pdf::Bitmap>();
    wk.doc(id).render(pg, sc, 0, 0, w, h, *bm, true, dark, rot);
    if (!dark && filter) pdf::filterPage(*bm, filter);
    A.post([&A, gen, pg, bm, targetW, dark, filter, rot]() {
      ReaderState& r = RS(A);
      if (r.doc.gen != gen || size_t(pg) >= r.doc.pages.size()) return;
      RdPage& p = r.doc.pages[size_t(pg)];
      p.previewReq = false;
      if (!A.readerOpen) return;
      if (dark != r.dark || filter != r.filterMode || rot != r.rot || targetW != rdPreviewTargetW(r)) { A.needFrame = true; return; }  // stale appearance, rotation or viewport size
      if (bm->w <= 0 || bm->h <= 0) { A.needFrame = true; return; }
      D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      if (FAILED(A.g.dc->CreateBitmap(D2D1::SizeU(UINT32(bm->w), UINT32(bm->h)), bm->bgra.data(), UINT32(bm->w * 4), bp, p.preview.put()))) { A.needFrame = true; return; }
      p.pw = bm->w;
      p.ph = bm->h;
      p.previewTargetW = targetW;
      p.previewDark = dark;
      p.previewFilter = filter;
      p.previewRot = rot;
      A.needFrame = true;
    });
  });
}

static void rdRequestText(App& A, int pg, int prio) {
  ReaderState& R = RS(A);
  RdPage& P2 = R.doc.pages[size_t(pg)];
  if (P2.textReq) return;
  P2.textReq = true;
  int id = R.doc.id;
  uint32_t gen = R.doc.gen;
  R.worker.submit(prio, [&A, id, gen, pg](pdf::Worker& wk) {
    if (!wk.hasDoc(id)) return;
    auto tp = std::make_shared<pdf::TextPage>();
    auto links = std::make_shared<vector<pdf::Link>>();
    auto fa = std::make_shared<vector<pdf::FileAnnot>>();
    pdf::Document& d = wk.doc(id);
    d.text(pg, *tp);
    d.links(pg, *links);
    d.readAnnots(pg, *fa);
    A.post([&A, gen, pg, tp, links, fa]() {
      ReaderState& r = RS(A);
      if (r.doc.gen != gen || size_t(pg) >= r.doc.pages.size()) return;
      RdPage& p = r.doc.pages[size_t(pg)];
      p.text = std::move(*tp);
      p.textDone = true;
      p.links = std::move(*links);
      p.linksDone = true;
      p.fileAnnots = std::move(*fa);
      p.fileAnnotsDone = true;
      A.needFrame = true;
    });
  });
}

static void rdRequestTile(App& A, const RdTileKey& key, int tw, int th, int prio) {
  ReaderState& R = RS(A);
  if (R.tiles.count(key) || R.tileReq.count(key)) return;
  int id = R.doc.id;
  uint32_t gen = R.doc.gen;
  double sc = key.k / 1000.0;
  pdf::Worker::Handle h = R.worker.submit(prio, [&A, id, gen, key, sc, tw, th](pdf::Worker& wk) {
    if (wk.current().cancelled() || !wk.hasDoc(id)) return;
    auto bm = std::make_shared<pdf::Bitmap>();
    wk.doc(id).render(key.page, sc, key.tx * kTile, key.ty * kTile, tw, th, *bm, true, key.dark != 0, key.rot);
    if (!key.dark && key.filter) pdf::filterPage(*bm, key.filter);
    A.post([&A, gen, key, bm]() {
      ReaderState& r = RS(A);
      r.tileReq.erase(key);
      if (r.doc.gen != gen || bm->w <= 0 || !A.readerOpen) return;  // a closed reader keeps no tiles
      RdTile t;
      D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      if (FAILED(A.g.dc->CreateBitmap(D2D1::SizeU(UINT32(bm->w), UINT32(bm->h)), bm->bgra.data(), UINT32(bm->w * 4), bp, t.bmp.put()))) return;
      t.w = bm->w;
      t.h = bm->h;
      t.used = uint32_t(A.frameNo);
      t.usedT = A.ui.time;
      r.tileBytes += size_t(t.w) * size_t(t.h) * 4;
      r.tiles[key] = std::move(t);
      A.needFrame = true;
    });
  });
  R.tileReq[key] = h;
}

// Tiles are kept small: what left the view a moment ago goes as soon as the soft budget is exceeded (so the memory
// does not grow while scrolling through a long paper), and the hard budget is enforced by least-recent use.
static void rdEvictTiles(App& A, const std::set<RdTileKey>& wanted) {
  ReaderState& R = RS(A);
  if (R.tileBytes <= kTileSoft) return;
  vector<std::pair<double, RdTileKey>> order;
  for (auto& kv : R.tiles) order.push_back({kv.second.usedT, kv.first});
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  const uint32_t now = uint32_t(A.frameNo);
  const double t = A.ui.time;
  auto drop = [&](const RdTileKey& k) {
    auto it = R.tiles.find(k);
    if (it == R.tiles.end()) return;
    R.tileBytes -= size_t(it->second.w) * size_t(it->second.h) * 4;
    R.tiles.erase(it);
  };
  for (auto& e : order) {  // idle and out of the band
    if (R.tileBytes <= kTileSoft) break;
    if (t - e.first < kTileIdleS) break;
    if (wanted.count(e.second)) continue;
    drop(e.second);
  }
  if (R.tileBytes <= kTileBudget) return;
  for (auto& e : order) {  // over the hard budget: least recent first, never what this frame drew
    if (R.tileBytes <= kTileBudget * 3 / 4) break;
    auto it = R.tiles.find(e.second);
    if (it == R.tiles.end()) continue;
    if (it->second.used == now) continue;
    drop(e.second);
  }
}

// ------------------------------------------------------------------ drawing
void App::drawReader(const Rect& r) {
  ReaderState& R = RS(*this);
  const float s = ui.s;
  ui.fill(r, ui.c.bg);
  PdfItem* item = readerItem();
  if (!item) { closeReader(); return; }
  const bool editable = !ui.anyModal();
  const int rec = readerRecord();
  const Record* record = rec >= 0 ? &P->corpus.recs[size_t(rec)] : nullptr;
  // the file's annotations count for the status line
  int nHl = 0, nAreas = 0, nDrawings = 0, nNotes = 0;
  for (auto& a : item->annots) {
    if (a.kind == 3) nNotes++;
    else if (a.kind == 4) nAreas++;
    else if (a.kind == 5) nDrawings++;
    else if (a.kind >= 0 && a.kind <= 2) nHl++;
  }

  // ---- toolbar
  const float tbH = 40 * s, bh = 28 * s, by0 = 6 * s;
  Rect tb{r.x, r.y, r.w, tbH};
  ui.fill(tb, ui.c.panel);
  ui.line(tb.x, tb.b() - 0.5f, tb.r(), tb.b() - 0.5f, ui.c.border);
  float x = tb.x + 8 * s;
  if (ui.button({x, tb.y + by0, 92 * s, bh}, "Library", BTN_GHOST, "chev-left")) { closeReader(); return; }
  ui.tip("Back to the reading list  (Esc)");
  x += 98 * s;
  if (ui.iconButton({x, tb.y + by0, 28 * s, bh}, "area", "Mark an area  (A)\nDrag a rectangle on a page: a figure, a table, an equation, a passage of a scanned PDF. Code it, comment on it, copy it as a picture or put it in the document. Alt + drag marks an area any time.", R.areaTool)) { R.areaTool = !R.areaTool; if (R.areaTool) R.inkTool = false; }
  x += 34 * s;
  if (ui.iconButton({x, tb.y + by0, 28 * s, bh}, "pen", "Draw on the PDF  (I)\nFreehand ink is saved with the project and can be written into the PDF as a standard Ink annotation. Escape turns the tool off.", R.inkTool)) { R.inkTool = !R.inkTool; if (R.inkTool) R.areaTool = false; }
  x += 34 * s;
  if (ui.iconButton({x, tb.y + by0, 28 * s, bh}, R.dark ? "sun" : "moon", R.dark ? "Light page  (D)\nBack to the paper as it is." : "Dark page  (D)\nThe pages light-on-dark, for reading in the dark: text and graphics inverted with their colours kept, photographs left as they are. Only the reader changes; the file, copies and figures stay as they are.", R.dark)) readerSetDark(!R.dark);
  x += 34 * s;
  const string filterName = R.filterMode == 1 ? "Warm paper" : R.filterMode == 2 ? "Grayscale" : "Original paper";
  if (ui.iconButton({x, tb.y + by0, 28 * s, bh}, "sliders", "Reading filters\nCurrent: " + filterName + ". Choose a subtle warm-paper or grayscale view; neither inverts the page.", R.filterMode != 0)) { R.ctxAnnot = "filter"; R.ctxX = x; R.ctxY = tb.b(); ui.openPopup("rctx"); }
  x += 34 * s;
  if (ui.iconButton({x, tb.y + by0, 28 * s, bh}, R.twoUp ? "twopage" : "pages", "View\nOne page or two facing pages, turn the view (R), print (Ctrl+P).", R.twoUp || R.rot != 0)) { R.ctxAnnot = "view"; R.ctxX = x; R.ctxY = tb.b(); ui.openPopup("rctx"); }
  x += 34 * s;
  // right block, from the right edge
  float rx = tb.r() - 8 * s;
  auto ibR = [&](const char* icon, const string& tip, bool on = false, bool en = true) { rx -= 30 * s; Rect b{rx, tb.y + by0, 28 * s, bh}; rx -= 2 * s; return ui.iconButton(b, icon, tip, on, en); };
  if (ibR("menu", "More: save highlights into the PDF, export notes, open elsewhere")) { R.ctxAnnot = "menu"; R.ctxX = rx; R.ctxY = tb.b(); ui.openPopup("rctx"); }
  rx -= 6 * s;
  // the panes: one group of five
  struct PaneBtn { int id; const char* icon; const char* tip; };
  const PaneBtn panes[] = {{5, "info", "Paper: record, metadata, file"}, {4, "list", "Outline (bookmarks)"}, {3, "search", "Find in this PDF  (Ctrl+F)"}, {2, "mark", "Annotations on the pages"}, {1, "text", "Reading notes, status, rating, tags"}};
  for (auto& pb : panes) if (ibR(pb.icon, pb.tip, R.pane == pb.id)) R.pane = R.pane == pb.id ? 0 : pb.id;
  rx -= 8 * s;
  ui.line(rx, tb.y + 11 * s, rx, tb.b() - 11 * s, ui.c.border);
  rx -= 6 * s;
  // zoom
  {
    float zoomNow = R.zoom > 0 ? R.zoom : (R.zoomShown > 0 ? R.zoomShown : 1);
    if (ibR("plus", "Zoom in  (Ctrl +)")) { R.zoom = clampv(zoomNow * 1.2f, kMinZoom, kMaxZoom); R.zoomT = ui.time; R.zoomAnchorX = R.zoomAnchorY = -1; }
    rx -= 66 * s;
    vector<string> opts = {"Fit width", "50%", "75%", "100%", "125%", "150%", "200%", "300%", "400%"};
    const float vals[] = {0, 0.5f, 0.75f, 1, 1.25f, 1.5f, 2, 3, 4};
    int sel = -1;
    for (int i = 0; i < 9; i++) if (std::fabs(vals[i] - R.zoom) < 0.01f) sel = i;
    if (sel < 0) { opts.insert(opts.begin(), fmtNum(zoomNow * 100, 0) + "%"); sel = 0; }
    int before = sel;
    if (ui.combo({rx, tb.y + by0, 64 * s, bh}, "rdzoom", opts, sel) && sel != before) {
      int k = opts.size() == 10 ? sel - 1 : sel;
      if (k >= 0) { R.zoom = vals[k]; R.zoomT = ui.time; R.zoomAnchorX = R.zoomAnchorY = -1; }
    }
    rx -= 2 * s;
    if (ibR("minus", "Zoom out  (Ctrl -)")) { R.zoom = clampv(zoomNow / 1.2f, kMinZoom, kMaxZoom); R.zoomT = ui.time; R.zoomAnchorX = R.zoomAnchorY = -1; }
  }
  rx -= 8 * s;
  ui.line(rx, tb.y + 11 * s, rx, tb.b() - 11 * s, ui.c.border);
  rx -= 6 * s;
  // page box
  {
    int n = int(R.doc.pages.size());
    int cur = readerCurrentPage();
    if (ibR("chev-right", "Next page  (\xE2\x86\x92)", false, n > 0 && readerPageStep(1) > cur)) readerGoTo(readerPageStep(1), -1, true);
    string total = "/ " + std::to_string(std::max(1, n));
    float tw = ui.textW(total, 12 * s) + 6 * s;
    rx -= tw;
    ui.text({rx, tb.y + by0, tw, bh}, total, 12 * s, ui.c.textDim, AL_LEFT);
    rx -= 44 * s;
    if (!ui.editing() || ui.focus != ui.id("ti:rdpage")) R.pageBuf = R.doc.ready && n > 0 ? std::to_string(cur + 1) : "";
    bool sub = false;
    if (ui.textInput({rx, tb.y + by0, 42 * s, bh}, "rdpage", R.pageBuf, "", &sub) && sub) {
      int pgn = atoi(R.pageBuf.c_str());
      if (pgn >= 1 && pgn <= n) readerGoTo(pgn - 1, -1, true);
      ui.focus = 0;
    }
    rx -= 2 * s;
    if (ibR("chev-left", "Previous page  (\xE2\x86\x90)", false, cur > 0)) readerGoTo(readerPageStep(-1), -1, true);
  }
  rx -= 10 * s;
  // title block
  {
    string title = record ? record->title : item->title;
    string sub;
    if (record) {
      if (!record->authors.empty()) sub = record->authors[0] + (record->authors.size() > 1 ? " et al." : "");
      if (record->year) sub += (sub.empty() ? "" : " \xC2\xB7 ") + std::to_string(record->year);
      if (!record->source.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + truncate(record->source, 40);
    } else {
      if (!item->author.empty()) sub = item->author;
      if (item->year) sub += (sub.empty() ? "" : " \xC2\xB7 ") + std::to_string(item->year);
      if (sub.empty()) sub = "Not linked to a record";
    }
    Rect tr{x, tb.y + 3 * s, std::max(40.f, rx - x), 20 * s};
    ui.text(tr, title.empty() ? PdfLibrary::fileTitle(item->path) : title, 13 * s, ui.c.text, AL_LEFT, 600);
    ui.text({tr.x, tr.b() - 2 * s, tr.w, 16 * s}, sub, 11 * s, ui.c.textDim);
  }

  // ---- body
  const float statusH = 24 * s;
  Rect body{r.x, tb.b(), r.w, r.h - tbH - statusH};
  Rect status{r.x, body.b(), r.w, statusH};
  const float paneW = R.pane ? std::min(320 * s, r.w * 0.36f) : 0;
  Rect pageArea{body.x, body.y, body.w - paneW, body.h};
  Rect pane{pageArea.r(), body.y, paneW, body.h};
  R.pageArea = pageArea;
  R.viewW = pageArea.w;
  R.viewH = pageArea.h;

  // ---- engine / document state screens
  if (!R.doc.ready) {
    ui.fill(pageArea, ui.c.bg);
    Rect c{pageArea.x + pageArea.w / 2 - 200 * s, pageArea.y + pageArea.h * 0.3f, 400 * s, 200 * s};
    if (R.doc.needsPassword) {
      ui.text({c.x, c.y, c.w, 22 * s}, "This PDF is password-protected", 14 * s, ui.c.text, AL_CENTER, 600);
      ui.maskNext = true;
      bool sub = false;
      if (R.passwordFocus) { ui.focusText(ui.id("ti:rdpw"), R.password); R.passwordFocus = false; }
      ui.textInput({c.x + 60 * s, c.y + 36 * s, c.w - 120 * s, 30 * s}, "rdpw", R.password, "Password", &sub);
      if (ui.button({c.x + c.w / 2 - 50 * s, c.y + 76 * s, 100 * s, 30 * s}, "Open", BTN_PRIMARY) || sub) { R.doc.failed = false; rdOpenDoc(*this, *item); }
    } else if (R.doc.failed) {
      ui.icon("warn", c.x + c.w / 2, c.y + 10 * s, 26 * s, ui.c.warn, 1.6f);
      ui.text({c.x, c.y + 30 * s, c.w, 22 * s}, R.engineTried && !R.engineReady ? "The PDF engine could not be loaded" : "This PDF could not be opened", 14 * s, ui.c.text, AL_CENTER, 600);
      ui.textWrap({c.x, c.y + 58 * s, c.w, 80 * s}, R.doc.error.empty() ? R.engine.error : R.doc.error, 12 * s, ui.c.textDim);
      float by = c.y + 130 * s;
      if (ui.button({c.x + c.w / 2 - 130 * s, by, 120 * s, 30 * s}, "Try again", BTN_NORMAL)) rdOpenDoc(*this, *item);
      if (ui.button({c.x + c.w / 2 + 10 * s, by, 120 * s, 30 * s}, "Locate file\xE2\x80\xA6", BTN_NORMAL)) readerRelocate(item->id);
    } else {
      ui.text({c.x, c.y + 20 * s, c.w, 22 * s}, R.engineTried ? "Opening\xE2\x80\xA6" : "Starting the PDF engine\xE2\x80\xA6", 13 * s, ui.c.textDim, AL_CENTER);
      ui.animating = true;
    }
    ui.fill(status, ui.c.panel);
    ui.line(status.x, status.y + 0.5f, status.r(), status.y + 0.5f, ui.c.border);
    if (paneW > 0) drawReaderPane(pane);
    return;
  }

  // ---- scale: the shown zoom eases towards the target on a clock; the tiles are re-rendered once it rests
  const float gap = 14 * s, top0 = 20 * s;
  const float maxRowPts = rdMaxRowPts(R);  // the widest page, or the widest pair of facing pages
  const float fitZoom = clampv((pageArea.w - 32 * s - (R.twoUp ? gap : 0)) / std::max(1.f, maxRowPts) / (s * 96.f / 72.f), kMinZoom, kMaxZoom);
  const float zoomTarget = R.zoom > 0 ? R.zoom : fitZoom;
  if (R.zoomShown <= 0) R.zoomShown = zoomTarget;
  {
    float dt = R.prevT > 0 ? clampv(float(ui.time - R.prevT), 0.f, 0.05f) : 1 / 60.f;
    R.prevT = ui.time;
    if (std::fabs(R.zoomShown - zoomTarget) > 0.0015f) { R.zoomShown += (zoomTarget - R.zoomShown) * (1 - std::exp(-dt / 0.045f)); ui.animating = true; R.zoomT = ui.time; }
    else R.zoomShown = zoomTarget;
    // scrolling eases too (wheel, keys, go-to)
    float maxS = 1e9f;
    (void)maxS;
    if (std::fabs(R.scrollTarget - R.scroll) > 0.5f) { R.scroll += (R.scrollTarget - R.scroll) * (1 - std::exp(-dt / 0.07f)); ui.animating = true; }
    else R.scroll = R.scrollTarget;
  }
  const float kD = pxPerPt(ui, R.zoomShown);
  const bool settled = std::fabs(R.zoomShown - zoomTarget) <= 0.0015f && ui.time - R.zoomT > 0.12;
  if (!settled) ui.animating = true;
  // page column geometry: rows of one or two pages (rdLayout), turned pages take their turned size
  float colW = 1, colH = top0;
  rdLayout(R, kD, gap, top0, R.pageTop, R.pageLeft, colW, colH);
  R.colWShown = colW;
  const float maxScroll = std::max(0.f, colH - pageArea.h);
  const float maxScrollX = std::max(0.f, colW + 32 * s - pageArea.w);
  // scroll compensation when the scale changed: the document point under the anchor (mouse or middle) stays still
  if (R.kPrev > 0 && std::fabs(kD - R.kPrev) > 1e-4f) {
    vector<float> oldTop, oldLeft;
    float prevW = 1, oldH = 0;
    rdLayout(R, R.kPrev, gap, top0, oldTop, oldLeft, prevW, oldH);
    float anchor = R.zoomAnchorY >= 0 ? R.zoomAnchorY : pageArea.y + pageArea.h / 2;
    float docY = anchor - pageArea.y + R.scroll;  // in the old column
    float newY = top0;
    bool placed = false;
    for (size_t i = 0; i < R.doc.pages.size() && !placed; i++) {
      float hOld = rdH(R, R.doc.pages[i]) * R.kPrev, hNew = rdH(R, R.doc.pages[i]) * kD;
      if (docY < oldTop[i] + hOld + gap || i + 1 == R.doc.pages.size()) { float frac = clampv((docY - oldTop[i]) / std::max(1.f, hOld), 0.f, 1.2f); newY = R.pageTop[i] + frac * hNew; placed = true; }
    }
    R.scroll = newY - (anchor - pageArea.y);
    R.scrollTarget = R.scroll;
    float prevLeft = prevW + 32 * s > pageArea.w ? pageArea.x + 16 * s - R.scrollX : pageArea.x + (pageArea.w - prevW) / 2;
    float anchorX = R.zoomAnchorX >= 0 ? R.zoomAnchorX : pageArea.x + pageArea.w / 2;
    float newLeft = anchorX - (anchorX - prevLeft) * (kD / R.kPrev);
    R.scrollX = maxScrollX > 0 ? pageArea.x + 16 * s - newLeft : 0;
  }
  R.kPrev = kD;
  R.kShown = kD;
  // a pending navigation (opened from the library at a page, a hit, an annotation)
  if (!R.positionRestored) {
    R.positionRestored = true;
    if (R.pendingPage >= 0) { readerGoTo(R.pendingPage, R.pendingY, false); R.pendingPage = -1; }
    else if (item->lastPage > 0) { int pg = int(item->lastPage); float frac = float(item->lastPage - pg); if (pg < int(R.doc.pages.size())) { R.scrollTarget = R.pageTop[size_t(pg)] + frac * rdH(R, R.doc.pages[size_t(pg)]) * kD - 12 * s; R.scroll = R.scrollTarget; } }
  } else if (R.pendingPage >= 0) { readerGoTo(R.pendingPage, R.pendingY, true); R.pendingPage = -1; }
  R.scroll = clampv(R.scroll, 0.f, maxScroll);
  R.scrollTarget = clampv(R.scrollTarget, 0.f, maxScroll);
  R.scrollX = clampv(R.scrollX, 0.f, maxScrollX);
  R.maxScroll = maxScroll;
  R.maxScrollX = maxScrollX;

  // ---- scrollbars: thin thumbs over the page area's right and bottom edges; drag, or click the track to page.
  // Their input is taken before the page area's (Ui::behave is first-come), before the geometry is final.
  const float barHit = 14 * s;
  const bool vBar = maxScroll > 0, hBar = maxScrollX > 0;
  const Rect vTrack{pageArea.r() - barHit, pageArea.y + 3 * s, barHit, pageArea.h - 6 * s - (hBar ? barHit : 0)};
  const Rect hTrack{pageArea.x + 3 * s, pageArea.b() - barHit, pageArea.w - 6 * s - (vBar ? barHit : 0), barHit};
  const float vLen = vBar ? clampv(vTrack.h * pageArea.h / std::max(1.f, colH), 28 * s, vTrack.h) : 0;
  const float hLen = hBar ? clampv(hTrack.w * pageArea.w / std::max(1.f, colW + 32 * s), 28 * s, hTrack.w) : 0;
  bool vHov = false, hHov = false;
  if (vBar && !ui.anyPopup()) {
    ui.behave(ui.id("rdvbar"), vTrack, &vHov);
    float pos = vTrack.y + (vTrack.h - vLen) * (R.scroll / maxScroll);
    if (vHov && ui.in.pressed[0]) {
      if (ui.in.my >= pos && ui.in.my <= pos + vLen) { R.barDrag = 1; R.barGrab = ui.in.my - pos; }
      else R.scrollTarget = clampv(R.scrollTarget + (ui.in.my < pos ? -1.f : 1.f) * pageArea.h * 0.9f, 0.f, maxScroll);
      R.miniShow = false;
    }
  }
  if (hBar && !ui.anyPopup()) {
    ui.behave(ui.id("rdhbar"), hTrack, &hHov);
    float pos = hTrack.x + (hTrack.w - hLen) * (R.scrollX / maxScrollX);
    if (hHov && ui.in.pressed[0]) {
      if (ui.in.mx >= pos && ui.in.mx <= pos + hLen) { R.barDrag = 2; R.barGrab = ui.in.mx - pos; }
      else R.scrollX = clampv(R.scrollX + (ui.in.mx < pos ? -1.f : 1.f) * pageArea.w * 0.9f, 0.f, maxScrollX);
      R.miniShow = false;
    }
  }
  if (R.barDrag && !ui.in.down[0]) R.barDrag = 0;
  if (R.barDrag == 1 && vBar) { float f = (ui.in.my - R.barGrab - vTrack.y) / std::max(1.f, vTrack.h - vLen); R.scroll = R.scrollTarget = clampv(f * maxScroll, 0.f, maxScroll); }
  if (R.barDrag == 2 && hBar) { float f = (ui.in.mx - R.barGrab - hTrack.x) / std::max(1.f, hTrack.w - hLen); R.scrollX = clampv(f * maxScrollX, 0.f, maxScrollX); }
  if (R.barDrag || vHov || hHov) ui.cursor = "arrow";
  const bool overBar = (vBar && vTrack.has(ui.in.mx, ui.in.my)) || (hBar && hTrack.has(ui.in.mx, ui.in.my));
  // the wheel: scroll, Shift = sideways, Ctrl = zoom about the pointer; a touchpad's sideways swipe scrolls sideways
  const bool wheelHov = ui.mouseIn(pageArea) && !ui.anyPopup();  // the wheel works over the card and the bars too
  const float readerZoomGesture = ui.in.wheel + ui.in.pinch;
  if (wheelHov && readerZoomGesture != 0) {
    R.miniShow = false;
    if (ui.in.pinch != 0 || ui.in.ctrl) { float z = R.zoom > 0 ? R.zoom : R.zoomShown; R.zoom = clampv(z * std::pow(1.1f, clampv(readerZoomGesture, -4.f, 4.f)), kMinZoom, kMaxZoom); R.zoomT = ui.time; R.zoomAnchorX = ui.in.mx; R.zoomAnchorY = ui.in.my; }
    else if (ui.in.shift) R.scrollX = clampv(R.scrollX - ui.in.wheel * 80 * s, 0.f, maxScrollX);
    else R.scrollTarget = clampv(R.scrollTarget - ui.in.wheel * 110 * s, 0.f, maxScroll);
  }
  if (wheelHov && ui.in.hwheel != 0) R.scrollX = clampv(R.scrollX + ui.in.hwheel * 80 * s, 0.f, maxScrollX);  // touchpad sideways, tilt wheel
  if (wheelHov && (readerZoomGesture != 0 || ui.in.hwheel != 0)) { R.scroll = clampv(R.scroll, 0.f, maxScroll); if (ui.in.pinch == 0 && !ui.in.ctrl && !ui.in.shift && ui.in.wheel != 0) ui.animating = true; }

  for (auto& t : R.pageTop) t -= R.scroll;  // now relative to pageArea.y
  const float colLeft = maxScrollX > 0 ? pageArea.x + 16 * s - R.scrollX : pageArea.x + (pageArea.w - colW) / 2;
  // the sheet of page i on screen (its turned size), and a screen point back to the unrotated page frame (points)
  auto pageRect = [&](size_t i) { const RdPage& p = R.doc.pages[i]; return Rect{colLeft + R.pageLeft[i], pageArea.y + R.pageTop[i], rdW(R, p) * kD, rdH(R, p) * kD}; };
  auto toPage = [&](size_t i, float mx, float my, float& px, float& py) {
    const RdPage& p = R.doc.pages[i];
    const Rect pr = pageRect(i);
    const float X = clampv((mx - pr.x) / kD, 0.f, rdW(R, p)), Y = clampv((my - pr.y) / kD, 0.f, rdH(R, p));
    pdf::unrotatePoint(R.rot, p.w, p.h, X, Y, px, py);
  };

  // ---- mouse over the page area
  uint64_t pid = ui.id("rdpages");
  bool areaHov = false, areaHeld = false;
  const bool overMini = R.miniShow && R.miniR.has(ui.in.mx, ui.in.my);
  const bool overCard = !R.cardAnnot.empty() && R.cardR.w > 0 && R.cardR.has(ui.in.mx, ui.in.my);  // the card (last frame's place) owns its clicks
  if (!overMini && !overCard && !overBar && !R.barDrag) ui.behave(pid, pageArea, &areaHov, &areaHeld);
  // which page and point is under the mouse
  auto pageAt = [&](float mx, float my, int& pg, float& px, float& py) {
    pg = -1;
    for (size_t i = 0; i < R.pageTop.size() && i < R.doc.pages.size(); i++) {
      const Rect pr = pageRect(i);
      if (pr.y > pageArea.b() || pr.b() < pageArea.y) continue;
      if (mx >= pr.x && mx < pr.r() && my >= pr.y && my < pr.b()) { pg = int(i); toPage(i, mx, my, px, py); return true; }
    }
    return false;
  };
  int hpg = -1;
  float hpx = 0, hpy = 0;
  const bool overPage = areaHov && pageAt(ui.in.mx, ui.in.my, hpg, hpx, hpy);
  string hoverLink;
  int hoverLinkPage = -1;
  float hoverLinkY = -1;
  R.hoverAnnot.clear();
  if (overPage) {
    const RdPage& p = R.doc.pages[size_t(hpg)];
    for (auto& l : p.links) if (hpx >= l.b.x0 && hpx <= l.b.x1 && hpy >= l.b.y0 && hpy <= l.b.y1) { hoverLink = l.url; hoverLinkPage = l.page; hoverLinkY = l.y; break; }
    R.hoverAnnot = rdAnnotAt(*this, *item, hpg, hpx, hpy, kD);
  }
  R.spaceDown = (GetKeyState(VK_SPACE) & 0x8000) != 0 && !ui.editing();
  if (ui.in.touchCancel) {
    R.touchPanPending = false;
    R.dragging = R.selGesture = false;
    R.panning = false;
    R.touchPanPending = false;
    R.touchPanMaySelect = false;
    R.miniShow = false;
    if (R.inkDrag) { R.inkDrag = false; R.inkPage = -1; R.inkPoints.clear(); }
    if (R.areaDrag) { R.areaDrag = false; R.areaPage = -1; }
    if (R.inkMove) {
      if (R.inkMoveDragged && P) {
        if (PdfAnnot* moving = P->library.annot(*item, R.inkMoveId)) {
          moving->strokes = R.inkMoveOrig;
          moving->rect = R.inkMoveOrigRect;
        }
      }
      R.inkMove = R.inkMoveDragged = false;
      R.inkMoveId.clear(); R.inkMovePage = -1; R.inkMoveOrig.clear();
    }
  }
  const bool touchCanPan = ui.in.touch && ui.in.pressed[0] && areaHov && !overBar && !R.barDrag && !R.spaceDown && !R.inkTool && !R.areaTool && R.hoverAnnot.empty() && hoverLink.empty();
  if (touchCanPan) {
    R.touchPanStartX = ui.in.mx; R.touchPanStartY = ui.in.my; R.touchPanStartT = ui.time;
    R.touchPanPending = true;
    R.touchPanMaySelect = overPage && editable && R.doc.pages[size_t(hpg)].textDone && R.doc.pages[size_t(hpg)].text.hit(hpx, hpy, false) >= 0;
  }
  if (R.touchPanPending && ui.in.down[0] && !R.inkDrag && !R.areaDrag && !R.inkMove) {
    if (R.touchPanMaySelect && ui.time - R.touchPanStartT >= 0.5) {
      // Long-press keeps the normal text-selection drag; a quicker movement remains a page pan.
      R.touchPanPending = false;
      R.touchPanMaySelect = false;
    } else if (ui.in.touch && std::hypot(ui.in.mx - R.touchPanStartX, ui.in.my - R.touchPanStartY) >= 8 * s) {
      R.touchPanPending = false;
      R.touchPanMaySelect = false;
      R.dragging = R.selGesture = false;
      R.panning = true;
      R.panX = R.touchPanStartX; R.panY = R.touchPanStartY;
      R.panScroll = R.scroll; R.panScrollX = R.scrollX;
      R.miniShow = false;
    } else if (R.touchPanMaySelect) {
      const double wake = R.touchPanStartT + 0.5;
      if (ui.wakeAt <= 0 || ui.wakeAt > wake) ui.wakeAt = wake;
    }
  }
  if (ui.in.touch && ui.in.released[0] && !ui.in.down[0]) {
    R.touchPanPending = false;
    R.touchPanMaySelect = false;
  }
  // panning: the middle button, space + the left button, or a touch drag after its movement threshold
  if (areaHov && !ui.anyPopup() && ((ui.in.pressed[2]) || (ui.in.pressed[0] && R.spaceDown))) { R.panning = true; R.panX = ui.in.mx; R.panY = ui.in.my; R.panScroll = R.scroll; R.panScrollX = R.scrollX; R.miniShow = false; }
  if (R.panning) {
    if (!ui.in.down[2] && !ui.in.down[0]) R.panning = false;
    else { R.scroll = R.scrollTarget = clampv(R.panScroll - (ui.in.my - R.panY), 0.f, maxScroll); R.scrollX = clampv(R.panScrollX - (ui.in.mx - R.panX), 0.f, maxScrollX); ui.cursor = "hand"; }
  }
  // cursor
  const bool inkMode = (R.inkTool || R.inkDrag) && !R.spaceDown;
  const bool areaMode = !inkMode && (R.areaTool || ui.in.alt) && !R.spaceDown;  // the Area tool, or Alt held: a drag marks an area
  if (areaHov && !ui.anyPopup() && !R.panning) {
    if (R.spaceDown) ui.cursor = "hand";
    else if (inkMode && overPage) ui.cursor = "cross";
    else if (areaMode && overPage) ui.cursor = "cross";
    else if (!hoverLink.empty() || hoverLinkPage >= 0) ui.cursor = "hand";
    else if (!R.hoverAnnot.empty()) { const PdfAnnot* a = P->library.annot(*item, R.hoverAnnot); ui.cursor = a && a->kind == 5 ? "move" : "hand"; }
    else if (overPage) { const RdPage& p = R.doc.pages[size_t(hpg)]; if (p.textDone && p.text.hit(hpx, hpy, false) >= 0) ui.cursor = "ibeam"; }
  }
  // left button: annotations, links, text selection
  if (areaHov && ui.in.pressed[0] && editable && !R.spaceDown && !ui.anyPopup()) {
    ui.focus = 0;  // a click on the page takes the keyboard back from any text field
    double now = ui.time;
    R.clicks = (now - R.clickT < 0.45 && !ui.in.shift) ? R.clicks + 1 : 1;
    R.clickT = now;
    R.miniShow = false;
    R.miniArea.clear();
    if (inkMode && overPage) {  // freehand paths are recorded in unrotated page points
      readerCloseCard();
      readerClearSelection();
      R.curAnnot.clear();
      R.inkDrag = true;
      R.inkPage = hpg;
      R.inkPoints.clear();
      R.inkPoints.push_back({hpx, hpy});
      R.clicks = 0;
    } else if (areaMode && overPage) {  // an area: the rectangle grows with the drag, the annotation is made on release
      readerCloseCard();
      readerClearSelection();
      R.curAnnot.clear();
      R.areaDrag = true;
      R.areaPage = hpg;
      R.areaX0 = R.areaX1 = hpx;
      R.areaY0 = R.areaY1 = hpy;
      R.clicks = 0;
    } else if (!R.hoverAnnot.empty()) {  // drawing paths can be moved by dragging; a click opens their comment card
      readerClearSelection();
      PdfAnnot* hit = P->library.annot(*item, R.hoverAnnot);
      if (hit && hit->kind == 5) {
        readerCloseCard();
        R.curAnnot = hit->id;
        R.inkMove = true;
        R.inkMoveDragged = false;
        R.inkMoveId = hit->id;
        R.inkMovePage = hpg;
        R.inkMoveX = hpx; R.inkMoveY = hpy;
        R.inkMoveScreenX = ui.in.mx; R.inkMoveScreenY = ui.in.my;
        R.inkMoveOrig = hit->strokes;
        R.inkMoveOrigRect = hit->rect;
        R.inkMoveOrigInFile = hit->inFile;
      } else readerOpenCard(R.hoverAnnot, false);
      R.clicks = 0;
    } else if (!hoverLink.empty() || hoverLinkPage >= 0) {
      readerCloseCard();
      if (hoverLinkPage >= 0) readerGoTo(hoverLinkPage, hoverLinkY, true);
      else openUrl(hoverLink);
    } else if (overPage) {
      readerCloseCard();
      R.curAnnot.clear();
      const RdPage& p = R.doc.pages[size_t(hpg)];
      int ch = p.textDone ? p.text.hit(hpx, hpy, false) : -1;
      if (R.clicks >= 3 && p.textDone) {  // the line
        int nearest = p.text.hit(hpx, hpy, true);
        int li = nearest >= 0 ? p.text.lineOf(nearest) : -1;
        if (li >= 0) { R.selPage = hpg; R.selA = p.text.lines[size_t(li)][0]; R.selB = p.text.lines[size_t(li)][1]; while (R.selB > R.selA && p.text.chars[size_t(R.selB)].generated) R.selB--; R.selGesture = true; }
        R.clicks = 0;
        R.dragging = false;
      } else if ((R.clicks == 2 || ui.in.dbl) && ch >= 0) {
        auto w = p.text.word(ch);
        R.selPage = hpg; R.selA = w[0]; R.selB = w[1];
        R.selGesture = true;
        R.dragging = false;
      } else {
        if (ui.in.shift && R.selPage == hpg && R.selA >= 0 && ch >= 0) R.selB = ch;
        else { R.selPage = hpg; R.selA = ch; R.selB = ch; }
        R.dragging = true;
        R.selGesture = true;
        R.panX = ui.in.mx; R.panY = ui.in.my;
      }
    } else { readerCloseCard(); readerClearSelection(); R.curAnnot.clear(); }
  }
  if (R.inkMove) {
    PdfAnnot* moving = P->library.annot(*item, R.inkMoveId);
    if (!moving || moving->kind != 5 || R.inkMovePage < 0 || size_t(R.inkMovePage) >= R.doc.pages.size()) {
      R.inkMove = R.inkMoveDragged = false; R.inkMoveId.clear(); R.inkMovePage = -1; R.inkMoveOrig.clear();
    } else if (ui.in.down[0]) {
      float px = 0, py = 0;
      toPage(size_t(R.inkMovePage), ui.in.mx, ui.in.my, px, py);
      float dx = px - R.inkMoveX, dy = py - R.inkMoveY;
      const float sdX = ui.in.mx - R.inkMoveScreenX, sdY = ui.in.my - R.inkMoveScreenY;
      if (sdX * sdX + sdY * sdY > 9 * s * s) R.inkMoveDragged = true;
      if (R.inkMoveDragged) {
        const RdPage& pg = R.doc.pages[size_t(R.inkMovePage)];
        dx = clampv(dx, -R.inkMoveOrigRect.x0, pg.w - R.inkMoveOrigRect.x1);
        dy = clampv(dy, -R.inkMoveOrigRect.y0, pg.h - R.inkMoveOrigRect.y1);
        moving->strokes = R.inkMoveOrig;
        for (auto& stroke : moving->strokes) for (auto& point : stroke.points) { point.x += dx; point.y += dy; }
        moving->rect = {R.inkMoveOrigRect.x0 + dx, R.inkMoveOrigRect.y0 + dy, R.inkMoveOrigRect.x1 + dx, R.inkMoveOrigRect.y1 + dy};
        needFrame = true;
      }
      ui.cursor = "move";
    } else {
      if (R.inkMoveDragged) {
        moving->modified = nowIso();
        moving->inFile = false;
        P->dirty = true;
        R.inkUndo.push_back({R.inkMoveId, false, R.inkMoveOrigInFile, std::move(R.inkMoveOrig), R.inkMoveOrigRect});
        if (R.inkUndo.size() > 100) R.inkUndo.erase(R.inkUndo.begin());
        ui.toast("Drawing moved", "Ctrl+Z restores its previous position.", 1, 2);
        needFrame = true;
      } else readerOpenCard(R.inkMoveId, false);
      R.inkMove = R.inkMoveDragged = false; R.inkMoveId.clear(); R.inkMovePage = -1; R.inkMoveOrig.clear();
    }
  }
  if (R.inkDrag) {
    if (R.inkPage < 0 || size_t(R.inkPage) >= R.doc.pages.size() || size_t(R.inkPage) >= R.pageTop.size()) {
      R.inkDrag = false; R.inkPage = -1; R.inkPoints.clear();
    } else {
      float px = 0, py = 0;
      toPage(size_t(R.inkPage), ui.in.mx, ui.in.my, px, py);
      const float minStep = 1.25f / std::max(0.1f, kD);
      if (!R.inkPoints.empty()) {
        const pdf::InkPoint& last = R.inkPoints.back();
        if (std::hypot(px - last.x, py - last.y) >= minStep && R.inkPoints.size() < 4096) R.inkPoints.push_back({px, py});
        else if (R.inkPoints.size() >= 4096) R.inkPoints.back() = {px, py};
      }
      ui.cursor = "cross";
      if (ui.in.my < pageArea.y + 10 * s) { R.scrollTarget = R.scroll = clampv(R.scroll - 14 * s, 0.f, maxScroll); ui.animating = true; }
      else if (ui.in.my > pageArea.b() - 10 * s) { R.scrollTarget = R.scroll = clampv(R.scroll + 14 * s, 0.f, maxScroll); ui.animating = true; }
      if (!ui.in.down[0]) {
        const int pg = R.inkPage;
        vector<pdf::InkPoint> points = std::move(R.inkPoints);
        R.inkPage = -1; R.inkDrag = false; R.inkPoints.clear();
        if (points.size() >= 2) readerAddInk(pg, std::move(points));
        needFrame = true;
      }
    }
  }
  if (R.areaDrag) {
    if (R.areaPage < 0 || size_t(R.areaPage) >= R.doc.pages.size() || size_t(R.areaPage) >= R.pageTop.size()) R.areaDrag = false;
    else {
      toPage(size_t(R.areaPage), ui.in.mx, ui.in.my, R.areaX1, R.areaY1);
      ui.cursor = "cross";
      if (ui.in.my < pageArea.y + 10 * s) { R.scrollTarget = R.scroll = clampv(R.scroll - 14 * s, 0.f, maxScroll); ui.animating = true; }
      else if (ui.in.my > pageArea.b() - 10 * s) { R.scrollTarget = R.scroll = clampv(R.scroll + 14 * s, 0.f, maxScroll); ui.animating = true; }
      if (!ui.in.down[0]) {
        R.areaDrag = false;
        if (std::fabs(R.areaX1 - R.areaX0) * kD >= 6 && std::fabs(R.areaY1 - R.areaY0) * kD >= 6) {
          if (PdfAnnot* a = readerAddArea(R.areaPage, R.areaX0, R.areaY0, R.areaX1, R.areaY1)) {
            R.miniArea = a->id;
            R.miniShow = true;
            R.miniX = ui.in.mx;
            R.miniY = ui.in.my;
          }
        }
      }
    }
  }
  if (R.dragging && areaHeld) {
    int pg2 = -1;
    float px2 = 0, py2 = 0;
    // stay on the page the selection started on; a drag beyond it clamps to that page's edges
    if (R.selPage >= 0) {
      const RdPage& p = R.doc.pages[size_t(R.selPage)];
      toPage(size_t(R.selPage), ui.in.mx, ui.in.my, px2, py2);
      pg2 = R.selPage;
      if (p.textDone && (std::fabs(ui.in.mx - R.panX) > 2 || std::fabs(ui.in.my - R.panY) > 2)) {
        int ch = p.text.hit(px2, py2, true);
        if (R.selA < 0) { float sx, sy; toPage(size_t(R.selPage), R.panX, R.panY, sx, sy); R.selA = p.text.hit(sx, sy, true); }
        if (ch >= 0) R.selB = ch;
      }
    }
    (void)pg2;
    if (ui.in.my < pageArea.y + 10 * s) { R.scrollTarget = R.scroll = clampv(R.scroll - 14 * s, 0.f, maxScroll); ui.animating = true; }
    else if (ui.in.my > pageArea.b() - 10 * s) { R.scrollTarget = R.scroll = clampv(R.scroll + 14 * s, 0.f, maxScroll); ui.animating = true; }
  }
  if (!ui.in.down[0]) R.dragging = false;
  if (ui.in.released[0] && R.selGesture) {
    R.selGesture = false;
    if (R.selPage >= 0 && R.selA >= 0 && R.selB >= 0) {
      if (R.selA == R.selB && R.clicks <= 1 && !ui.in.dbl) { readerClearSelection(); }  // a click, not a selection
      else { R.miniShow = true; R.miniX = ui.in.mx; R.miniY = ui.in.my; }
    } else if (R.selA < 0) readerClearSelection();
  }
  // right button: context menu on the selection, an annotation or the page
  if (areaHov && ui.in.pressed[1] && editable && !ui.anyPopup()) {  // right button
    R.ctxX = ui.in.mx;
    R.ctxY = ui.in.my;
    R.ctxAnnot = !R.hoverAnnot.empty() ? R.hoverAnnot : R.selPage >= 0 ? string("sel") : string("page");
    if (!R.hoverAnnot.empty()) R.curAnnot = R.hoverAnnot;
    R.ctxPage = hpg;  // the page point for "add a note here"
    R.ctxPX = hpx;
    R.ctxPY = hpy;
    R.miniShow = false;
    ui.openPopup("rctx");
  }

  // ---- the pages
  ui.pushClip(pageArea);
  if (R.dark) ui.fill(pageArea, Color::hex(0x2A2A2A));  // the dark page's own backdrop, whatever the application theme
  ID2D1DeviceContext* dc = ui.dc();
  const int kTarget = int(std::lround(pxPerPt(ui, zoomTarget) * 1000));
  if (settled && kTarget != R.tileK) { R.tileKPrev = R.tileK; R.tileK = kTarget; }
  std::set<RdTileKey> wanted;
  const float margin = 200 * s;  // prefetch band above and below the view
  const Color shadowCol = ui.c.shadow;
  const Color selCol = ui.c.accent.withA(0.28f), hitCol = Color::hex(0xF59E0B, 0.35f), hitCur = Color::hex(0xEA580C, 0.55f);
  for (size_t i = 0; i < R.doc.pages.size(); i++) {
    RdPage& p = R.doc.pages[i];
    if (i >= R.pageTop.size() || i >= R.pageLeft.size()) break;
    Rect pr = pageRect(i);
    const float pw = pr.w, ph = pr.h, x0 = pr.x, y0 = pr.y;
    if (y0 > pageArea.b() + margin || y0 + ph < pageArea.y - margin) continue;
    bool visible = y0 <= pageArea.b() && y0 + ph >= pageArea.y;
    // the overlays: boxes and points of the unrotated page frame onto the (turned) sheet
    auto boxR = [&](const pdf::Box& b) { const pdf::Box d = pdf::rotateBox(R.rot, p.w, p.h, b); return Rect{pr.x + d.x0 * kD, pr.y + d.y0 * kD, d.w() * kD, d.h() * kD}; };
    auto ptR = [&](float x, float y, float& sx, float& sy) { float X, Y; pdf::rotatePoint(R.rot, p.w, p.h, x, y, X, Y); sx = pr.x + X * kD; sy = pr.y + Y * kD; };
    auto drawInkPath = [&](const vector<pdf::InkPoint>& points, const Color& color, float width, bool hot) {
      if (points.empty()) return;
      const float lineW = clampv(width * kD, 1.2f * s, 8.f * s);
      float x0 = 0, y0 = 0;
      ptR(points[0].x, points[0].y, x0, y0);
      for (size_t n = 1; n < points.size(); n++) {
        float x1 = 0, y1 = 0;
        ptR(points[n].x, points[n].y, x1, y1);
        if (hot) ui.line(x0, y0, x1, y1, ui.c.accent.withA(0.38f), lineW + 3.f * s);
        ui.line(x0, y0, x1, y1, color.withA(0.98f), lineW);
        x0 = x1; y0 = y1;
      }
    };
    // requests: the preview (cheap, whole page) and the text come first, the tiles once the zoom rests
    if (!p.preview || p.previewDark != R.dark || p.previewFilter != R.filterMode || p.previewRot != R.rot || p.previewTargetW != rdPreviewTargetW(R)) rdRequestPreview(*this, int(i));
    if (!p.textReq) rdRequestText(*this, int(i), visible ? 20 : 25);
    if (settled && kTarget == R.tileK) {
      const float k = R.tileK / 1000.f;
      int W = int(std::ceil(rdW(R, p) * k)), H = int(std::ceil(rdH(R, p) * k));
      // the visible part of the page in page pixels (with the margin)
      float vx0 = std::max(0.f, (pageArea.x - x0) / kD * k), vx1 = std::min(float(W), (pageArea.r() - x0) / kD * k);
      float vy0 = std::max(0.f, (pageArea.y - margin - y0) / kD * k), vy1 = std::min(float(H), (pageArea.b() + margin - y0) / kD * k);
      if (vx1 > vx0 && vy1 > vy0) {
        float cy = (pageArea.y + pageArea.h / 2 - y0) / kD * k;
        for (int ty = int(vy0) / kTile; ty * kTile < vy1; ty++)
          for (int tx = int(vx0) / kTile; tx * kTile < vx1; tx++) {
            RdTileKey key{int(i), R.tileK, R.dark ? 1 : 0, R.filterMode, R.rot, tx, ty};
            wanted.insert(key);
            int tw = std::min(kTile, W - tx * kTile), th = std::min(kTile, H - ty * kTile);
            if (tw <= 0 || th <= 0) continue;
            int prio = 10 + int(std::min(60.f, std::fabs((ty + 0.5f) * kTile - cy) / kTile));  // nearest to the view centre first
            if (!visible) prio += 20;
            rdRequestTile(*this, key, tw, th, prio);
          }
      }
    }
    if (!visible) continue;
    // the sheet
    (void)pw;
    if (std::fabs(kD * 1000 - R.tileK) < 0.1f) { pr.x = std::round(pr.x); pr.y = std::round(pr.y); }
    ui.fill({pr.x + 1.5f * s, pr.y + 2 * s, pr.w, pr.h}, shadowCol.withA(0.35f));
    ui.fill(pr, R.dark ? Color::hex(0x1C1C1C) : R.filterMode == 1 ? Color::hex(0xFFFBF0) : Color(1, 1, 1, 1));
    if (R.dark) ui.stroke(pr, Color(1, 1, 1, 0.08f), 0, 1.f);
    // preview underneath (until the tiles cover it); one of the other mode is not shown (it would flash through)
    if (p.preview && p.previewDark == R.dark && p.previewFilter == R.filterMode && p.previewRot == R.rot) dc->DrawBitmap(p.preview.get(), D2D1::RectF(pr.x, pr.y, pr.r(), pr.b()), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    // tiles: the previous scale first (they are stretched), then the current one
    for (int pass = 0; pass < 2; pass++) {
      int tk = pass == 0 ? R.tileKPrev : R.tileK;
      if (tk <= 0 || (pass == 0 && tk == R.tileK)) continue;
      float f = kD / (tk / 1000.f);
      bool exact = std::fabs(f - 1) < 1e-4f;
      const int dk = R.dark ? 1 : 0;
      auto it = R.tiles.lower_bound(RdTileKey{int(i), tk, dk, R.filterMode, R.rot, 0, 0});
      for (; it != R.tiles.end() && it->first.page == int(i) && it->first.k == tk && it->first.dark == dk && it->first.filter == R.filterMode && it->first.rot == R.rot; ++it) {
        RdTile& t = it->second;
        float tx = pr.x + it->first.tx * kTile * f, ty = pr.y + it->first.ty * kTile * f;
        float tw = t.w * f, th = t.h * f;
        if (ty > pageArea.b() || ty + th < pageArea.y) continue;
        t.used = uint32_t(frameNo);
        t.usedT = ui.time;
        dc->DrawBitmap(t.bmp.get(), D2D1::RectF(tx, ty, tx + tw + (exact ? 0 : 0.5f), ty + th + (exact ? 0 : 0.5f)), 1.f, exact ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      }
    }
    // ---- overlays: highlights and notes of the library
    for (auto& a : item->annots) {
      if (a.page != int(i)) continue;
      bool hot = a.id == R.hoverAnnot || a.id == R.curAnnot;
      Color col = rdCodeColor(a.color);
      if (a.kind <= 2) {
        for (auto& q : a.quads) {
          Rect qr = boxR(q);
          if (a.kind == 0 && !a.inFile) ui.fill(qr, col.withA(hot ? 0.5f : 0.38f));
          else if (a.kind == 1) ui.fill(boxR({q.x0, q.y1 - 2 * s / kD, q.x1, q.y1}), col.withA(0.95f).mix(Color(0, 0, 0, 1), 0.25f));
          else if (a.kind == 2) { const float mid = q.y0 + q.h() * 0.55f; ui.fill(boxR({q.x0, mid, q.x1, mid + 1.6f * s / kD}), Color::hex(0xD03030, 0.9f)); }
          if (hot) ui.stroke(qr, ui.c.accent.withA(0.9f), 2 * s, 1.2f * s);
        }
        if (!a.note.empty() && !a.quads.empty()) {  // a comment marker in the margin
          const pdf::Box& q = a.quads[0];
          float mx, my;
          ptR(std::max(2 * s / kD, q.x0 - 14 * s / kD), q.y0 + 4 * s / kD, mx, my);
          ui.circle(mx, my, 4 * s, col.mix(Color(0, 0, 0, 1), 0.3f));
        }
      } else if (a.kind == 3) {
        const Rect nb = boxR(a.rect);
        float nx = nb.x, ny = nb.y, sz = 18 * s;
        Rect nr{nx, ny, sz, sz};
        ui.fill(nr, col.withA(hot ? 1.f : 0.92f), 4 * s);
        ui.stroke(nr, hot ? ui.c.accent : Color(0, 0, 0, 0.25f), 4 * s, 1.f);
        ui.icon("chat", nr.x + sz / 2, nr.y + sz / 2, sz * 0.62f, Color(0, 0, 0, 0.7f), 1.6f);
      } else if (a.kind == 4) {
        Rect ar = boxR(a.rect);
        ui.fill(ar, col.withA(hot ? 0.16f : 0.09f), 2 * s);
        ui.stroke(ar, col.mix(Color(0, 0, 0, 1), 0.2f).withA(hot ? 1.f : 0.85f), 2 * s, hot ? 2.f * s : 1.5f * s);
        if (!a.note.empty()) {  // a comment marker at the corner
          float sz = 14 * s;
          Rect nr{ar.r() - sz / 2, ar.y - sz / 2, sz, sz};
          ui.fill(nr, col.withA(0.95f), 3 * s);
          ui.stroke(nr, Color(0, 0, 0, 0.25f), 3 * s, 1.f);
          ui.icon("chat", nr.x + sz / 2, nr.y + sz / 2, sz * 0.62f, Color(0, 0, 0, 0.7f), 1.5f);
        }
      } else if (a.kind == 5) {
        for (auto& stroke : a.strokes) drawInkPath(stroke.points, col, a.strokeWidth, hot);
      }
    }
    if (R.inkDrag && R.inkPage == int(i)) drawInkPath(R.inkPoints, rdCodeColor(R.inkColor), R.inkWidth, false);
    if (R.areaDrag && R.areaPage == int(i)) {  // the rectangle being drawn
      Rect ar = boxR({std::min(R.areaX0, R.areaX1), std::min(R.areaY0, R.areaY1), std::max(R.areaX0, R.areaX1), std::max(R.areaY0, R.areaY1)});
      ui.fill(ar, ui.c.accent.withA(0.10f), 2 * s);
      ui.stroke(ar, ui.c.accent.withA(0.9f), 2 * s, 1.5f * s);
    }
    // search hits
    for (size_t hi = 0; hi < R.hits.size(); hi++) {
      const pdf::Hit& h = R.hits[hi];
      if (h.page != int(i)) continue;
      bool cur = int(hi) == R.hitCur;
      for (auto& q : h.rects) {
        Rect qr = boxR(q);
        qr = {qr.x - 1, qr.y - 1, qr.w + 2, qr.h + 2};
        ui.fill(qr, cur ? hitCur : hitCol, 2 * s);
        if (cur) ui.stroke(qr, Color::hex(0xEA580C, 0.9f), 2 * s, 1.2f * s);
      }
    }
    // selection
    if (R.selPage == int(i) && R.selA >= 0 && R.selB >= 0 && p.textDone) {
      for (auto& q : p.text.rects(std::min(R.selA, R.selB), std::max(R.selA, R.selB))) ui.fill(boxR(q), selCol);
    }
    // links under the pointer
    if (hpg == int(i) && (!hoverLink.empty() || hoverLinkPage >= 0)) {
      for (auto& l : p.links) if (hpx >= l.b.x0 && hpx <= l.b.x1 && hpy >= l.b.y0 && hpy <= l.b.y1) { Rect lr = boxR(l.b); ui.stroke({lr.x - 1, lr.y - 1, lr.w + 2, lr.h + 2}, ui.c.accent.withA(0.8f), 2 * s, 1.f); break; }
    }
  }
  ui.popClip();
  // ---- the comment card over the pages, then the scrollbars on top of everything in the page area
  ui.pushClip(pageArea);
  drawReaderCard(pageArea, colLeft, colW, kD);
  ui.popClip();
  if (vBar) {
    bool hot = vHov || R.barDrag == 1;
    float tw = hot ? 8 * s : 5 * s, pos = vTrack.y + (vTrack.h - vLen) * (R.scroll / maxScroll);
    ui.fill({vTrack.r() - 3 * s - tw, pos, tw, vLen}, ui.c.text.withA(hot ? 0.42f : 0.2f), tw / 2);
  }
  if (hBar) {
    bool hot = hHov || R.barDrag == 2;
    float th = hot ? 8 * s : 5 * s, pos = hTrack.x + (hTrack.w - hLen) * (R.scrollX / maxScrollX);
    ui.fill({pos, hTrack.b() - 3 * s - th, hLen, th}, ui.c.text.withA(hot ? 0.42f : 0.2f), th / 2);
  }
  // page caches: the text and the preview of pages far from the view are dropped (long documents)
  {
    const int cur = readerCurrentPage();
    int nText = 0, nPrev = 0;
    for (auto& p : R.doc.pages) { if (p.textDone) nText++; if (p.preview) nPrev++; }
    if (nText > kTextPages || nPrev > kPreviewPages) {
      // farthest from the view first
      vector<std::pair<int, int>> byDist;
      for (size_t i = 0; i < R.doc.pages.size(); i++) byDist.push_back({std::abs(int(i) - cur), int(i)});
      std::sort(byDist.begin(), byDist.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (auto& e : byDist) {
        if (nText <= kTextPages && nPrev <= kPreviewPages) break;
        if (e.first < 6) break;
        if (e.second == R.selPage) continue;
        RdPage& p = R.doc.pages[size_t(e.second)];
        if (nText > kTextPages && p.textDone) { p.text = pdf::TextPage(); p.textDone = p.textReq = false; p.links.clear(); p.fileAnnots.clear(); nText--; }
        if (nPrev > kPreviewPages && p.preview) { p.preview.reset(); p.previewReq = false; nPrev--; }
      }
    }
  }
  // tiles no longer wanted: cancel what is still queued, and forget the previous scale once the current one covers the view
  for (auto it = R.tileReq.begin(); it != R.tileReq.end();) {
    if (!wanted.count(it->first) || it->first.k != R.tileK) { it->second.cancel(); it = R.tileReq.erase(it); }
    else ++it;
  }
  if (R.tileKPrev > 0 && R.tileKPrev != R.tileK) {
    bool complete = true;
    for (auto& k : wanted) if (!R.tiles.count(k)) { complete = false; break; }
    if (complete && settled) {
      for (auto it = R.tiles.begin(); it != R.tiles.end();) {
        if (it->first.k != R.tileK) { R.tileBytes -= size_t(it->second.w) * size_t(it->second.h) * 4; it = R.tiles.erase(it); }
        else ++it;
      }
      R.tileKPrev = 0;
    }
  }
  rdEvictTiles(*this, wanted);
  if (!hoverLink.empty() && !ui.anyPopup()) ui.richTip(hoverLink);
  else if (!R.hoverAnnot.empty() && !ui.anyPopup() && !R.dragging) {
    if (const PdfAnnot* a = P->library.annot(*item, R.hoverAnnot)) {
      string t = a->kind == 3 ? "Note" : a->code != Code::None ? codeName(a->code) : !a->tag.empty() ? a->tag : a->kind == 1 ? "Underline" : a->kind == 2 ? "Strikeout" : a->kind == 4 ? "Area" : a->kind == 5 ? "Drawing" : "Highlight";
      if (!a->note.empty()) t += "\n" + truncate(a->note, 160);
      if (a->kind == 5) t += "\nDrag to move \xC2\xB7 click to comment \xC2\xB7 right-click for options";
      else if (a->kind != 3 && a->note.empty()) t += "\nClick to comment or code \xC2\xB7 right-click for options";
      ui.richTip(t);
    }
  }
  // ---- floating toolbar after a selection, context menu
  drawReaderMini(pageArea);
  if (ui.isPopupOpen("rctx")) drawReaderContext();
  if (ui.isPopupOpen("rdprint")) drawReaderPrintCard();

  // ---- side pane and status
  if (paneW > 0) drawReaderPane(pane);
  {
    ui.fill(status, ui.c.panel);
    ui.line(status.x, status.y + 0.5f, status.r(), status.y + 0.5f, ui.c.border);
    int cur = readerCurrentPage();
    string left = "Page " + std::to_string(cur + 1) + " of " + std::to_string(R.doc.pages.size());
    if (!R.doc.info.labels.empty() && size_t(cur) < R.doc.info.labels.size() && R.doc.info.labels[size_t(cur)] != std::to_string(cur + 1)) left += " (" + R.doc.info.labels[size_t(cur)] + ")";
    left += "   \xC2\xB7   " + plural(nHl, "highlight");
    if (nAreas) left += "   \xC2\xB7   " + plural(nAreas, "area");
    if (nDrawings) left += "   \xC2\xB7   " + plural(nDrawings, "drawing");
    left += "   \xC2\xB7   " + plural(nNotes, "note");
    if (R.findRunning) left += "   \xC2\xB7   searching\xE2\x80\xA6";
    else if (!R.findLast.empty()) left += "   \xC2\xB7   " + (R.hits.empty() ? string("no matches") : std::to_string(R.hitCur + 1) + " of " + plural(long(R.hits.size()), "match"));
    if (!R.statusLine.empty() && ui.time - R.statusT < 6) left += "   \xC2\xB7   " + R.statusLine;
    ui.text({status.x + 12 * s, status.y, status.w * 0.6f, status.h}, left, 11 * s, ui.c.textDim);
    string right = fmtNum(R.zoomShown * 100, 0) + "%" + (R.zoom <= 0 ? " (fit)" : "");
    if (R.rot) right = "turned " + std::to_string(R.rot * 90) + "\xC2\xB0   \xC2\xB7   " + right;
    if (R.twoUp) right = "two pages   \xC2\xB7   " + right;
    if (R.selPage >= 0 && R.selA >= 0) right = plural(std::abs(R.selB - R.selA) + 1, "character") + " selected  \xC2\xB7  1-7 code, H highlight, U underline, N comment, Ctrl+C copy   \xC2\xB7   " + right;
    else if (!R.cardAnnot.empty()) right = "Enter finishes the comment  \xC2\xB7  Esc closes it  \xC2\xB7  Delete removes the highlight   \xC2\xB7   " + right;
    else right = "Space / PgDn page  \xC2\xB7  Ctrl+wheel zoom  \xC2\xB7  drag text to select  \xC2\xB7  click a highlight to comment   \xC2\xB7   " + right;
    ui.text({status.x + status.w * 0.3f, status.y, status.w * 0.7f - 12 * s, status.h}, right, 11 * s, ui.c.textFaint, AL_RIGHT);
  }
  if (frameNo % 30 == 0) readerStorePosition();
}

// ------------------------------------------------------------------ the floating toolbar (after a mouse selection)
bool App::readerHoverTargetAt(float x, float y, uint64_t& target) {
  target = 0;
  if (!readerOpen || !rd || !rd->doc.ready) return false;
  ReaderState& R = *rd;
  if (!R.pageArea.has(x, y) || ui.anyPopup() || ui.anyModal() || ui.blocked(x, y)) return false;
  if ((R.miniShow && R.miniR.has(x, y)) || (!R.cardAnnot.empty() && R.cardR.w > 0 && R.cardR.has(x, y))) return false;

  const float s = ui.s, barHit = 14 * s;
  const bool vBar = R.maxScroll > 0, hBar = R.maxScrollX > 0;
  const Rect vTrack{R.pageArea.r() - barHit, R.pageArea.y + 3 * s, barHit, R.pageArea.h - 6 * s - (hBar ? barHit : 0)};
  const Rect hTrack{R.pageArea.x + 3 * s, R.pageArea.b() - barHit, R.pageArea.w - 6 * s - (vBar ? barHit : 0), barHit};
  if (R.barDrag || (vBar && vTrack.has(x, y)) || (hBar && hTrack.has(x, y))) return false;  // ordinary scrollbar widgets own these pixels
  const bool spaceDown = (GetKeyState(VK_SPACE) & 0x8000) != 0 && !ui.editing();

  if (!R.doc.ready || R.kShown <= 0 || R.pageTop.empty() || R.pageLeft.empty()) return true;  // blank/loading page area
  float colLeft = R.pageArea.x + (R.colWShown - R.pageArea.w) / 2;
  if (R.maxScrollX > 0) colLeft = R.pageArea.x + 16 * s - R.scrollX;
  int page = -1;
  float px = 0, py = 0;
  const size_t count = std::min({R.doc.pages.size(), R.pageTop.size(), R.pageLeft.size()});
  for (size_t i = 0; i < count; i++) {
    const RdPage& p = R.doc.pages[i];
    const float pw = (R.rot & 1) ? p.h : p.w, ph = (R.rot & 1) ? p.w : p.h;
    Rect pr{colLeft + R.pageLeft[i], R.pageArea.y + R.pageTop[i], pw * R.kShown, ph * R.kShown};
    if (!pr.has(x, y)) continue;
    page = int(i);
    const float X = clampv((x - pr.x) / R.kShown, 0.f, pw), Y = clampv((y - pr.y) / R.kShown, 0.f, ph);
    pdf::unrotatePoint(R.rot, p.w, p.h, X, Y, px, py);
    break;
  }
  if (page < 0) {
    if (spaceDown) target = 0xA400000000000001ull;  // space-pan cursor over the page area, including the gaps
    return true;  // blank margin is noninteractive; it deliberately maps to target zero
  }

  const RdPage& p = R.doc.pages[size_t(page)];
  for (const pdf::Link& l : p.links) {
    if (px >= l.b.x0 && px <= l.b.x1 && py >= l.b.y0 && py <= l.b.y1) {
      uint64_t h = 1469598103934665603ull;
      const string key = l.url + "|" + std::to_string(page) + "|" + std::to_string(l.page) + "|" + std::to_string(l.y);
      for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
      target = 0xA100000000000000ull | (h & 0x00ffffffffffffffull);
      return true;
    }
  }
  if (PdfItem* item = readerItem()) {
    string annot = rdAnnotAt(*this, *item, page, px, py, R.kShown);
    if (!annot.empty()) {
      uint64_t h = 1469598103934665603ull;
      for (unsigned char c : annot) { h ^= c; h *= 1099511628211ull; }
      target = 0xA200000000000000ull | (h & 0x00ffffffffffffffull);
      return true;
    }
  }
  const bool inkMode = (R.inkTool || R.inkDrag) && !spaceDown;
  const bool areaMode = !inkMode && (R.areaTool || GetKeyState(VK_MENU) < 0) && !spaceDown;
  if (inkMode || areaMode) target = 0xA300000000000001ull;
  else if (spaceDown) target = 0xA400000000000001ull;
  else if (p.textDone && p.text.hit(px, py, false) >= 0) target = 0xA500000000000001ull;  // the I-beam state spans the selectable text
  return true;
}

void App::drawReaderMini(const Rect& pageArea) {
  ReaderState& R = RS(*this);
  if (!R.miniShow) { R.miniR = {0, 0, 0, 0}; return; }
  PdfItem* item = readerItem();
  PdfAnnot* area = !R.miniArea.empty() && item ? P->library.annot(*item, R.miniArea) : nullptr;  // the toolbar of a fresh area
  if (!R.miniArea.empty() && !area) { R.miniArea.clear(); R.miniShow = false; R.miniR = {0, 0, 0, 0}; return; }
  if (!area && (R.selPage < 0 || R.selA < 0)) { R.miniShow = false; R.miniR = {0, 0, 0, 0}; return; }
  const float s = ui.s;
  const bool hold = ui.anyPopup();
  if (!hold && R.miniR.w > 0) {
    float dx = std::max({R.miniR.x - ui.in.mx, 0.f, ui.in.mx - R.miniR.r()}), dy = std::max({R.miniR.y - ui.in.my, 0.f, ui.in.my - R.miniR.b()});
    if (std::hypot(dx, dy) > 150 * s) { R.miniShow = false; R.miniArea.clear(); R.miniR = {0, 0, 0, 0}; return; }
  }
  // row 1: the coding scheme (one colour per code) + underline; row 2: note, copy, quote, cite
  const float bw = 26 * s, gap = 3 * s, rowH = 26 * s, pad = 4 * s;
  if (area) {  // an area: code it (row 1); comment, copy as a picture, put it in the document, delete (row 2)
    const float row1 = pad + 7 * (bw + gap) + 8 * s + (bw + gap) - gap + pad;
    const float row2 = pad + 4 * (bw + gap) - gap + pad;
    const float w = std::max(row1, row2), h = pad + rowH + pad + rowH + pad;
    float x = clampv(R.miniX - 30 * s, pageArea.x + 8 * s, std::max(pageArea.x + 8 * s, pageArea.r() - w - 8 * s));
    float y = R.miniY + 14 * s;
    if (y + h > pageArea.b() - 6 * s) y = R.miniY - h - 14 * s;
    y = clampv(y, pageArea.y + 6 * s, std::max(pageArea.y + 6 * s, pageArea.b() - h - 6 * s));
    Rect r{x, y, w, h};
    R.miniR = r;
    ui.shadow(r, 8 * s, 14 * s);
    ui.fill(r, ui.c.panel2, 8 * s);
    ui.stroke(r, ui.c.border, 8 * s);
    float bx = r.x + pad, by = r.y + pad;
    auto recode = [&](Code c) { area->code = c; area->color = codeColor(c); area->modified = nowIso(); area->inFile = false; P->dirty = true; };
    for (int c = 1; c < int(Code::Count); c++) {
      Rect b{bx, by, bw, rowH};
      uint64_t idv = ui.id("rdacode" + std::to_string(c));
      bool hov = false;
      bool click = ui.behave(idv, b, &hov);
      const bool on = area->code == Code(c);
      if (hov || on) ui.fill(b, on ? ui.c.accent.withA(0.18f) : ui.c.hover, 5 * s);
      Color col = rdCodeColor(codeColor(Code(c)));
      ui.circle(b.x + b.w / 2, b.y + b.h / 2, 7 * s, col);
      ui.circle(b.x + b.w / 2, b.y + b.h / 2, 7 * s, Color(0, 0, 0, 0.35f), false, 1.f);
      ui.tipFor(idv, string(codeName(Code(c))) + "  (" + std::to_string(c) + ")");
      if (click) recode(on ? Code::None : Code(c));
      bx += bw + gap;
    }
    bx += 4 * s;
    ui.line(bx, by + 4 * s, bx, by + rowH - 4 * s, ui.c.border, 1);
    bx += 4 * s;
    if (ui.iconButton({bx, by, bw, rowH}, "x", "No code", area->code == Code::None)) recode(Code::None);
    bx = r.x + pad;
    by += rowH + pad;
    if (ui.iconButton({bx, by, bw, rowH}, "chat", "Write a comment on this area")) { readerOpenCard(area->id, true); R.miniShow = false; R.miniArea.clear(); }
    bx += bw + gap;
    if (ui.iconButton({bx, by, bw, rowH}, "copy", area->text.empty() ? "Copy the area as a picture" : "Copy the area as a picture (right-click for its text)")) readerAreaImage(area->id, 0);
    bx += bw + gap;
    if (ui.iconButton({bx, by, bw, rowH}, "writer", "Put the area in the document as a figure, captioned with this paper and the page")) { readerAreaImage(area->id, 1); R.miniShow = false; R.miniArea.clear(); }
    bx += bw + gap;
    if (ui.iconButton({bx, by, bw, rowH}, "trash", "Remove this area  (Delete)")) readerDeleteAnnot(area->id);
    return;
  }
  const float row1 = pad + 7 * (bw + gap) + 8 * s + 3 * (bw + gap) - gap + pad;
  const float row2 = pad + 4 * (bw + gap) - gap + pad;
  const float w = std::max(row1, row2), h = pad + rowH + pad + rowH + pad;
  float x = clampv(R.miniX - 30 * s, pageArea.x + 8 * s, std::max(pageArea.x + 8 * s, pageArea.r() - w - 8 * s));
  float y = R.miniY - h - 14 * s;
  if (y < pageArea.y + 6 * s) y = R.miniY + 22 * s;
  y = clampv(y, pageArea.y + 6 * s, std::max(pageArea.y + 6 * s, pageArea.b() - h - 6 * s));
  Rect r{x, y, w, h};
  R.miniR = r;
  ui.shadow(r, 8 * s, 14 * s);
  ui.fill(r, ui.c.panel2, 8 * s);
  ui.stroke(r, ui.c.border, 8 * s);
  float bx = r.x + pad, by = r.y + pad;
  for (int c = 1; c < int(Code::Count); c++) {
    Rect b{bx, by, bw, rowH};
    uint64_t idv = ui.id("rdcode" + std::to_string(c));
    bool hov = false;
    bool click = ui.behave(idv, b, &hov);
    if (hov) ui.fill(b, ui.c.hover, 5 * s);
    Color col = rdCodeColor(codeColor(Code(c)));
    ui.circle(b.x + b.w / 2, b.y + b.h / 2, 7 * s, col);
    ui.circle(b.x + b.w / 2, b.y + b.h / 2, 7 * s, Color(0, 0, 0, 0.35f), false, 1.f);
    ui.tipFor(idv, string(codeName(Code(c))) + "  (" + std::to_string(c) + ")");
    if (click) readerAddMarkup(0, Code(c));
    bx += bw + gap;
  }
  bx += 4 * s;
  ui.line(bx, by + 4 * s, bx, by + rowH - 4 * s, ui.c.border, 1);
  bx += 4 * s;
  if (ui.iconButton({bx, by, bw, rowH}, "mark", "Plain highlight  (H)")) readerAddMarkup(0, Code::None);
  bx += bw + gap;
  if (ui.iconButton({bx, by, bw, rowH}, "underline", "Underline  (U)")) readerAddMarkup(1, Code::None);
  bx += bw + gap;
  if (ui.iconButton({bx, by, bw, rowH}, "strike", "Strikeout  (S)")) readerAddMarkup(2, Code::None);
  // row 2: compact icon-only actions; hover labels explain each command
  bx = r.x + pad;
  by += rowH + pad;
  if (ui.iconButton({bx, by, bw, rowH}, "chat", "Highlight and write a comment on it  (N)")) { if (PdfAnnot* a = readerAddMarkup(0, Code::None)) readerOpenCard(a->id, true); }
  bx += bw + gap;
  if (ui.iconButton({bx, by, bw, rowH}, "copy", "Copy the selected text  (Ctrl+C)")) { string t = readerSelectedText(); setClipboardText(hwnd, t); readerClearSelection(); ui.toast("Copied", truncate(t, 60), 1, 2); }
  bx += bw + gap;
  if (ui.iconButton({bx, by, bw, rowH}, "quote", "Quote into the writer, with a citation of this paper  (Ctrl+Q)")) { string t = readerSelectedText(); readerAddMarkup(0, Code::Quote); readerQuote(t, readerRecord()); }
  bx += bw + gap;
  if (ui.iconButton({bx, by, bw, rowH}, "writer", readerRecord() >= 0 ? "Insert a citation of this paper at the writer's caret  (Ctrl+E)" : "Link the PDF to a record to cite it", false, readerRecord() >= 0)) { readerCite(); readerClearSelection(); }
}

// ------------------------------------------------------------------ the comment card
// Floats next to the annotation it belongs to (clamped into the view, so it follows a scroll). The text is stored in
// the annotation as it is typed; Enter or Done closes the card, Shift+Enter breaks a line, Esc closes it too. The
// coloured dots re-code a passage; the bin deletes it. A click anywhere on the page closes the card.
void App::drawReaderCard(const Rect& pageArea, float colLeft, float colW, float kD) {
  ReaderState& R = RS(*this);
  R.cardR = {0, 0, 0, 0};
  if (R.cardAnnot.empty()) return;
  PdfItem* item = readerItem();
  PdfAnnot* a = item ? P->library.annot(*item, R.cardAnnot) : nullptr;
  if (!a || a->page < 0 || size_t(a->page) >= R.pageTop.size() || size_t(a->page) >= R.doc.pages.size()) { R.cardAnnot.clear(); return; }
  const float s = ui.s;
  const RdPage& p = R.doc.pages[size_t(a->page)];
  (void)colW;
  if (size_t(a->page) >= R.pageLeft.size()) { R.cardAnnot.clear(); return; }
  const float x0 = colLeft + R.pageLeft[size_t(a->page)], y0 = pageArea.y + R.pageTop[size_t(a->page)];
  // the anchor: the end of the first highlighted line, or the note's icon (on the turned sheet)
  float ax, ay;
  if (a->kind == 3) { const pdf::Box d = pdf::rotateBox(R.rot, p.w, p.h, a->rect); ax = x0 + d.x0 * kD + 18 * s; ay = y0 + d.y0 * kD + 9 * s; }
  else if (a->kind >= 4 || a->quads.empty()) { const pdf::Box d = pdf::rotateBox(R.rot, p.w, p.h, a->rect); ax = x0 + d.x1 * kD; ay = y0 + d.y0 * kD; }
  else { const pdf::Box& q = a->quads[0]; float X, Y; pdf::rotatePoint(R.rot, p.w, p.h, q.x1, (q.y0 + q.y1) / 2, X, Y); ax = x0 + X * kD; ay = y0 + Y * kD; }
  const bool markup = a->kind <= 2;
  const bool hadFocus = ui.focus == ui.id("ta:rdcard");  // a click on a code dot below keeps the caret in the text
  const float w = std::min(300 * s, pageArea.w - 32 * s), taH = 74 * s, rowH = 24 * s, pad = 10 * s;
  const float h = pad + rowH + 6 * s + taH + 6 * s + 26 * s + pad;
  float x = ax + 14 * s;
  if (x + w > pageArea.r() - 18 * s) x = ax - w - 14 * s;
  x = clampv(x, pageArea.x + 8 * s, std::max(pageArea.x + 8 * s, pageArea.r() - w - 18 * s));
  float y = clampv(ay - 18 * s, pageArea.y + 8 * s, std::max(pageArea.y + 8 * s, pageArea.b() - h - 18 * s));
  Rect r{x, y, w, h};
  R.cardR = r;
  const Color col = rdCodeColor(a->color);
  // a short connector from the anchor to the card when they are apart
  if (ax < r.x || ax > r.r()) {
    float cx = ax < r.x ? r.x : r.r(), cy = clampv(ay, r.y + 10 * s, r.b() - 10 * s);
    ui.line(ax, ay, cx, cy, col.mix(ui.c.text, 0.35f).withA(0.75f), 1.5f * s);
    ui.circle(ax, ay, 3 * s, col.mix(ui.c.text, 0.35f));
  }
  ui.shadow(r, 10 * s, 18 * s);
  ui.fill(r, ui.c.panel2, 10 * s);
  ui.stroke(r, ui.c.border, 10 * s);
  ui.fill({r.x, r.y + 12 * s, 3 * s, r.h - 24 * s}, col, 1.5f * s);
  // header: what it is, where; delete and close on the right
  float hx = r.x + 12 * s, hy = r.y + pad;
  string label = a->kind == 3 ? "Note" : a->code != Code::None ? codeName(a->code) : !a->tag.empty() ? a->tag : a->kind == 1 ? "Underline" : a->kind == 2 ? "Strikeout" : a->kind == 4 ? "Area" : a->kind == 5 ? "Drawing" : "Highlight";
  label += "  \xC2\xB7  p. " + std::to_string(a->page + 1);
  float bx = r.r() - 10 * s - 22 * s;
  if (ui.iconButton({bx, hy + 1 * s, 22 * s, 22 * s}, "x", "Close  (Esc)")) { readerCloseCard(); return; }
  bx -= 24 * s;
  if (ui.iconButton({bx, hy + 1 * s, 22 * s, 22 * s}, "trash", a->kind == 3 ? "Delete this note  (Delete)" : a->kind == 5 ? "Delete this drawing  (Delete)" : "Delete this highlight  (Delete)")) { readerDeleteAnnot(a->id); return; }
  if (markup && !a->text.empty()) {
    bx -= 24 * s;
    if (ui.iconButton({bx, hy + 1 * s, 22 * s, 22 * s}, "quote", "Quote the passage into the writer")) readerQuote(a->text, readerRecord());
  }
  ui.text({hx, hy, std::max(40.f, bx - hx - 6 * s), rowH}, label, 11.5f * s, a->code != Code::None ? col.mix(ui.c.text, 0.35f) : ui.c.textDim, AL_LEFT, 600);
  // the comment
  if (R.cardFocus) { ui.focusText(ui.id("ta:rdcard"), R.noteBuf); R.cardFocus = false; }
  bool sub = false;
  Rect ta{r.x + 12 * s, hy + rowH + 6 * s, r.w - 22 * s, taH};
  if (ui.textArea(ta, "rdcard", R.noteBuf, a->kind == 3 ? "Your note\xE2\x80\xA6" : a->kind == 5 ? "Comment on this drawing\xE2\x80\xA6" : "Comment on this passage\xE2\x80\xA6", &sub, nullptr, 12.5f * s)) {
    a->note = R.noteBuf;
    a->modified = nowIso();
    a->inFile = false;
    P->dirty = true;
  }
  // footer: the codes (markup) or a hint, and Done
  float fy = ta.b() + 6 * s;
  if (markup) {
    float cx = r.x + 10 * s;
    for (int c = 1; c < int(Code::Count); c++) {
      Rect b{cx, fy, 22 * s, 26 * s};
      uint64_t idv = ui.id("rdcardcode" + std::to_string(c));
      bool hov = false;
      bool click = ui.behave(idv, b, &hov);
      if (hov) ui.fill(b, ui.c.hover, 5 * s);
      Color cc = rdCodeColor(codeColor(Code(c)));
      ui.circle(b.x + b.w / 2, b.y + b.h / 2, 6 * s, cc);
      if (a->code == Code(c)) ui.circle(b.x + b.w / 2, b.y + b.h / 2, 8.5f * s, ui.c.text.withA(0.8f), false, 1.5f * s);
      else ui.circle(b.x + b.w / 2, b.y + b.h / 2, 6 * s, Color(0, 0, 0, 0.3f), false, 1.f);
      ui.tipFor(idv, string(codeName(Code(c))) + (a->code == Code(c) ? "  (click to remove the code)" : ""));
      if (click) { a->code = a->code == Code(c) ? Code::None : Code(c); a->color = codeColor(a->code); a->modified = nowIso(); a->inFile = false; P->dirty = true; if (hadFocus) R.cardFocus = true; }
      cx += 22 * s;
    }
  } else {
    ui.text({r.x + 12 * s, fy, r.w - 100 * s, 26 * s}, "Enter finishes \xC2\xB7 Shift+Enter breaks a line", 10.5f * s, ui.c.textFaint);
  }
  if (ui.button({r.r() - 10 * s - 62 * s, fy, 62 * s, 26 * s}, "Done", BTN_PRIMARY) || sub) { readerCloseCard(); return; }
  if (ui.in.key(VK_ESCAPE) && !ui.anyPopup()) readerCloseCard();
}

// ------------------------------------------------------------------ context menu
void App::drawReaderContext() {
  const float s = ui.s;
  ui.overlay([this, s]() {
    ReaderState& r = RS(*this);
    PdfItem* item = readerItem();
    if (!item) { ui.closePopup(); return; }
    struct It { string label, key; bool enabled = true, danger = false; };
    vector<It> items;
    auto add = [&](const string& l, const string& k, bool en = true, bool danger = false) { items.push_back({l, k, en, danger}); };
    auto sep = [&]() { items.push_back({"-", "", false, false}); };
    const string& what = r.ctxAnnot;
    const PdfAnnot* an = what != "sel" && what != "page" && what != "menu" && what != "view" && what != "filter" ? P->library.annot(*item, what) : nullptr;
    const int rec = readerRecord();
    auto tick = [](bool on) { return string(on ? "\xE2\x9C\x93 " : "   "); };
    if (what == "filter") {
      add(tick(!r.dark && r.filterMode == 0) + "Original paper", "filter0");
      add(tick(!r.dark && r.filterMode == 1) + "Warm paper  (non-inverting)", "filter1");
      add(tick(!r.dark && r.filterMode == 2) + "Grayscale  (non-inverting)", "filter2");
      sep();
      add(tick(r.dark) + "Dark page  (inverting)", "darkpage");
    } else if (what == "view") {
      add(tick(!r.twoUp) + "One page", "one");
      add(tick(r.twoUp) + "Two pages, facing", "two");
      add(tick(r.twoUp && r.coverAlone) + "First page alone (as a book opens)", "cover", r.twoUp);
      sep();
      add("Turn clockwise  (R)", "rotcw");
      add("Turn anticlockwise  (Shift+R)", "rotccw");
      add("Upright again", "rot0", r.rot != 0);
      sep();
      add("Print\xE2\x80\xA6  (Ctrl+P)", "print", r.doc.ready);
    } else if (what == "menu") {
      add("Save annotations into the PDF file", "savepdf", item->annotCount() > 0);
      add("Export this paper's notes (Markdown)", "exportmd");
      add("Print\xE2\x80\xA6  (Ctrl+P)", "print", r.doc.ready);
      sep();
      add(string(r.dark ? "\xE2\x9C\x93 " : "   ") + "Dark page  (D)", "darkpage");
      sep();
      add("Open in the default PDF viewer", "openext");
      add("Show in Explorer", "reveal");
      add("Copy file path", "copypath");
      sep();
      add("Link to a different record\xE2\x80\xA6", "relink", hasCorpus());
      add("Locate the file\xE2\x80\xA6", "relocate");
      add("Detach from the project", "detach", true, true);
    } else if (an) {
      add(an->kind == 3 ? "Edit the note" : an->kind == 5 ? (an->note.empty() ? "Add a comment to the drawing" : "Edit the drawing comment") : an->note.empty() ? "Add a comment" : "Edit the comment", "note");
      if (an->kind == 4) {
        add("Copy as a picture", "copyimg");
        add("Put in the document as a figure", "figimg");
      }
      if (an->kind != 3 && an->kind != 5) {
        add("Copy text", "copytext", !an->text.empty());
        add("Quote into the writer", "quote", !an->text.empty());
        add("Cite this paper", "cite", rec >= 0);
        sep();
        for (int c = 1; c < int(Code::Count); c++) add(string(an->code == Code(c) ? "\xE2\x9C\x93 " : "   ") + codeName(Code(c)), "code" + std::to_string(c));
        add(string(an->code == Code::None ? "\xE2\x9C\x93 " : "   ") + "No code", "code0");
      }
      sep();
      add(an->kind == 5 ? "Delete drawing" : "Delete", "delete", true, true);
    } else if (what == "sel") {
      add("Copy", "copysel");
      add("Highlight  (H)", "hl");
      add("Underline  (U)", "ul");
      add("Strikeout  (S)", "strike");
      add("Highlight with a comment  (N)", "hlnote");
      sep();
      add("Quote into the writer  (Ctrl+Q)", "quotesel");
      add("Cite this paper  (Ctrl+E)", "cite", rec >= 0);
      add("Search the library for this text", "libsearch");
    } else {
      add("Add a note here", "notehere", r.ctxPage >= 0);
      add(string(r.areaTool ? "\xE2\x9C\x93 " : "   ") + "Area tool  (A)  \xE2\x80\x94 drag to mark a figure, a table, a scan", "areatool");
      add(string(r.inkTool ? "\xE2\x9C\x93 " : "   ") + "Draw tool  (I)  \xE2\x80\x94 freehand Ink annotation", "inktool");
      add("Undo last drawing or move  (Ctrl+Z)", "undoink", !r.inkUndo.empty());
      sep();
      add(string(r.inkColor == 0xFF1D4ED8 ? "\xE2\x9C\x93 " : "   ") + "Pen color  \xC2\xB7  Blue", "inkblue");
      add(string(r.inkColor == 0xFFCC3030 ? "\xE2\x9C\x93 " : "   ") + "Pen color  \xC2\xB7  Red", "inkred");
      add(string(r.inkColor == 0xFF202020 ? "\xE2\x9C\x93 " : "   ") + "Pen color  \xC2\xB7  Black", "inkblack");
      add(string(std::fabs(r.inkWidth - 1.2f) < 0.01f ? "\xE2\x9C\x93 " : "   ") + "Pen width  \xC2\xB7  1.2 pt", "inkthin");
      add(string(std::fabs(r.inkWidth - 1.8f) < 0.01f ? "\xE2\x9C\x93 " : "   ") + "Pen width  \xC2\xB7  1.8 pt", "inkmedium");
      add(string(std::fabs(r.inkWidth - 3.0f) < 0.01f ? "\xE2\x9C\x93 " : "   ") + "Pen width  \xC2\xB7  3 pt", "inkwide");
      add("Copy this page's text", "copypage", r.ctxPage >= 0);
      sep();
      add("Fit width", "fit");
      add("100%", "z100");
      add(string(r.dark ? "\xE2\x9C\x93 " : "   ") + "Dark page  (D)", "darkpage");
      add(tick(r.twoUp) + "Two pages", "twotoggle");
      add("Turn clockwise  (R)", "rotcw");
      sep();
      add("Find\xE2\x80\xA6  (Ctrl+F)", "find");
      add("Print\xE2\x80\xA6  (Ctrl+P)", "print", r.doc.ready);
    }
    float hgt = 6 * s;
    for (auto& it : items) hgt += it.label[0] == '-' ? 7 * s : 27 * s;
    float wdt = 268 * s;
    Rect pr{r.ctxX, r.ctxY, wdt, hgt};
    if (what == "menu") pr.x = r.ctxX - wdt + 30 * s;
    if (pr.r() > float(g.W) - 8 * s) pr.x = float(g.W) - 8 * s - pr.w;
    if (pr.b() > float(g.H) - 8 * s) pr.y = std::max(8 * s, float(g.H) - 8 * s - pr.h);
    ui.shadow(pr, 8 * s);
    ui.fill(pr, ui.c.panel2, 8 * s);
    ui.stroke(pr, ui.c.border, 8 * s);
    ui.popupRect(pr);
    float yy = pr.y + 3 * s;
    for (auto& it : items) {
      if (it.label[0] == '-') { ui.line(pr.x + 8 * s, yy + 3 * s, pr.r() - 8 * s, yy + 3 * s, ui.c.border, 1); yy += 7 * s; continue; }
      Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, 26 * s};
      bool click = it.enabled && ui.listRow(rr, "rctx:" + it.key, false);
      ui.text({rr.x + 10 * s, rr.y, rr.w - 14 * s, rr.h}, it.label, 12.5f * s, !it.enabled ? ui.c.textFaint : it.danger ? ui.c.danger : ui.c.text);
      yy += 27 * s;
      if (!click) continue;
      ui.closePopup();
      const string key = it.key;
      if (key == "savepdf") readerSaveIntoPdf(item->id);
      else if (key == "exportmd") readerExportNotes(item->id);
      else if (key == "openext") openUrl(item->path);
      else if (key == "reveal") revealInExplorer(item->path);
      else if (key == "copypath") setClipboardText(hwnd, item->path);
      else if (key == "relink") { r.pane = 5; r.linkQuery.clear(); ui.openPopup("rdlink"); }
      else if (key == "relocate") readerRelocate(item->id);
      else if (key == "detach") { string id = item->id; closeReader(); readerDetach(id); }
      else if (key == "note" && an) readerOpenCard(an->id, true);
      else if (key == "copytext" && an) setClipboardText(hwnd, an->text);
      else if (key == "quote" && an) readerQuote(an->text, rec);
      else if (key == "cite") readerCite();
      else if (startsWith(key, "code") && an) { PdfAnnot* a = P->library.annot(*item, an->id); if (a) { a->code = Code(atoi(key.c_str() + 4)); a->color = codeColor(a->code); a->modified = nowIso(); a->inFile = false; P->dirty = true; } }
      else if (key == "delete" && an) readerDeleteAnnot(an->id);
      else if (key == "copysel") { string t = readerSelectedText(); setClipboardText(hwnd, t); }
      else if (key == "hl") readerAddMarkup(0, Code::None);
      else if (key == "ul") readerAddMarkup(1, Code::None);
      else if (key == "strike") readerAddMarkup(2, Code::None);
      else if (key == "hlnote") { if (PdfAnnot* a = readerAddMarkup(0, Code::None)) readerOpenCard(a->id, true); }
      else if (key == "quotesel") { string t = readerSelectedText(); readerAddMarkup(0, Code::Quote); readerQuote(t, rec); }
      else if (key == "libsearch") { string t = readerSelectedText(); closeReader(); r.libQuery = truncate(t, 60); page = PG_READ; }
      else if (key == "notehere") readerAddNote(r.ctxPage, r.ctxPX, r.ctxPY);
      else if (key == "areatool") { r.areaTool = !r.areaTool; if (r.areaTool) r.inkTool = false; }
      else if (key == "inktool") { r.inkTool = !r.inkTool; if (r.inkTool) r.areaTool = false; }
      else if (key == "undoink") readerUndoInk();
      else if (key == "inkblue" || key == "inkred" || key == "inkblack" || key == "inkthin" || key == "inkmedium" || key == "inkwide") {
        if (key == "inkblue") r.inkColor = 0xFF1D4ED8;
        else if (key == "inkred") r.inkColor = 0xFFCC3030;
        else if (key == "inkblack") r.inkColor = 0xFF202020;
        else if (key == "inkthin") r.inkWidth = 1.2f;
        else if (key == "inkmedium") r.inkWidth = 1.8f;
        else r.inkWidth = 3.f;
        settings.j.set("readerInkColor", (long long)r.inkColor);
        settings.j.set("readerInkWidth", r.inkWidth);
        settings.save();
      }
      else if (key == "copyimg" && an) readerAreaImage(an->id, 0);
      else if (key == "figimg" && an) readerAreaImage(an->id, 1);
      else if (key == "copypage") { if (r.ctxPage >= 0 && size_t(r.ctxPage) < r.doc.pages.size()) setClipboardText(hwnd, r.doc.pages[size_t(r.ctxPage)].text.text); }
      else if (key == "fit") { r.zoom = 0; r.zoomT = ui.time; }
      else if (key == "filter0") rdSetFilter(*this, 0);
      else if (key == "filter1") rdSetFilter(*this, 1);
      else if (key == "filter2") rdSetFilter(*this, 2);
      else if (key == "darkpage") readerSetDark(!r.dark);
      else if (key == "z100") { r.zoom = 1; r.zoomT = ui.time; }
      else if (key == "find") { r.pane = 3; r.findFocus = true; }
      else if (key == "one") readerSetLayout(false, r.coverAlone);
      else if (key == "two") readerSetLayout(true, r.coverAlone);
      else if (key == "twotoggle") readerSetLayout(!r.twoUp, r.coverAlone);
      else if (key == "cover") readerSetLayout(true, !r.coverAlone);
      else if (key == "rotcw") readerRotate(1);
      else if (key == "rotccw") readerRotate(-1);
      else if (key == "rot0") readerRotate(-r.rot);
      else if (key == "print") readerPrintDialog();
      return;
    }
  });
}

}  // namespace win
}  // namespace vs
