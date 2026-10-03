// VOSStudio Native — application shell (window, canvas interaction, pages, commands)
#pragma once
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include "../core/ai.h"
#include "../core/live.h"
#include <optional>
#include <deque>
#include <memory>

#include "../core/charts.h"
#include "../core/report.h"
#include "../core/doc.h"
#include "../core/project.h"
#include "../core/records.h"
#include "ui.h"

namespace vs {
namespace win {

#include "version.h"  // kAppVersion

inline string pluralWord(const string& w) {
  if (w.size() > 1 && w.back() == 'y' && !strchr("aeiou", w[w.size() - 2])) return w.substr(0, w.size() - 1) + "ies";  // country -> countries, key -> keys
  return w + "s";
}
inline string plural(long long n, const string& w) { return fmtInt(n) + " " + (n == 1 ? w : pluralWord(w)); }

enum Page { PG_NONE = -1, PG_DATA = 0, PG_BUILD, PG_LOOK, PG_ANALYSE, PG_TRENDS, PG_ACTORS, PG_READ, PG_WRITER, PG_PUBLISH, PG_AI, PG_COUNT };
enum Workspace { WS_VISUALIZATION = 0, WS_BIBLIOGRAPHY = 1 };
enum BibliographyRoute { BR_PAPERS = 0, BR_REVIEW = 1, BR_WRITE = 2 };

struct ReaderState;  // reader.h

struct UndoEntry {
  string what;
  Snapshot snap;
  Json style;
  int n = 0;
};

// A chart that can be shown in a panel, expanded into the main area and exported
struct ChartDef {
  string title;
  std::function<Scene(double w, double h, const ChartTheme& t)> make;
  std::function<void(int tag)> onClick;  // optional
  bool valid() const { return bool(make); }
};

struct ChartChoice { const char* id; const char* label; bool needsMap; };
const vector<ChartChoice>& chartChoices();  // the charts of the loaded data (writer picker, split-view panes); split.cpp

struct Command { string id, label, hint, shortcut, icon; std::function<void()> run; string group; };  // group: palette section (File, Map, Views, ...)
class App;
struct WriterLayout;  // writer.cpp: DirectWrite layouts of the document (opaque here)
struct WriterPdfPreview;  // writerpdf.cpp: asynchronous PDFium page previews (opaque here)
void crashInstall(App* app);  // crash.cpp
void crashTest();
const vector<std::pair<Unit, const char*>>& cleanUnits();  // units of Data → Clean terms

struct GeoCountryShape { vector<vector<std::pair<float, float>>> rings; float x0 = 0, y0 = 0, x1 = 0, y1 = 0, lx = 0, ly = 0; };
struct GeoStat { int docs = 0, intl = 0; long long cites = 0; double yearSum = 0; int yearN = 0; };
struct GeoFill { Color col; bool data = false; };
struct GeoBubble { float wx, wy, r; Color fill, stroke; int country; };
struct GeoArc { float ax, ay, bx, by, w; Color col; int count; };
void geoProject(double lon, double lat, float& x, float& y);

struct OaTermCount { string term; int total = 0, fetched = 0, added = 0; };
vector<string> openAlexTerms(const string& q);

class App {
 public:
  bool init(HINSTANCE hi, int show, const string& scriptPath, const vector<string>& openPaths);
  int run();
  LRESULT wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

  // ---------------------------------------------------------------- state
  HWND hwnd = nullptr;
  Gfx g;
  NetView nv;
  Ui ui;
  Settings settings;
  std::unique_ptr<Project> P;
  Input input;          // accumulated by WndProc between frames
  std::map<DWORD, POINT> touchPoints_;  // native WM_TOUCH contacts, keyed by the Windows contact ID
  bool touchRegistered_ = false, touchPinching_ = false;
  float touchPinchDistance_ = 0;
  bool touchTapCandidate_ = false, touchTapMoved_ = false;
  POINT touchTapStart_{}, touchLastTap_{};
  double touchLastTapT_ = 0;
  void touchPoint(DWORD id, POINT p, bool down, bool up);
  bool needFrame = true;
  bool occluded = false;  // last Present reported the window fully covered
  void pumpWork();
  double lastFrameT = 0, fps = 0;
  double lastInputT = 0;                        // the last key, click, wheel or mouse move (idle housekeeping waits for quiet)
  float inputMx = -1, inputMy = -1;
  uint64_t pointerHoverTarget_ = ~uint64_t(0);  // cached pointer target; unchanged hover movement skips a full frame
  struct ChartHoverRegion { const Scene* scene = nullptr; Rect bounds, clip; bool clipped = false; float ox = 0, oy = 0, scale = 1; uint64_t context = 0; };
  vector<ChartHoverRegion> chartHoverRegions_;  // last full frame's custom chart hit areas
  Scene sensitivityHoverScene_;  // the small transient threshold chart has no persistent chart-cache entry
  bool readerPrinting = false;                  // a print job is using the engine (the reader's memory stays)
  int frameNo = 0;
  string gpuError;

  // layout and workspaces. Both workspaces are views over the same Project; only route/UI state changes.
  int page = PG_DATA;
  Workspace workspace = WS_VISUALIZATION;
  int lastVisualizationPage = PG_DATA;
  bool lastVisualizationChartOpen = false;
  BibliographyRoute lastBibliographyRoute = BR_PAPERS;
  int dataSection = 0;  // Overview, Sources, Search, Clean; keeps the Data workflow progressive rather than one long page
  string bibliographyReaderItem;          // restore the active paper when returning from Visualization
  bool bibliographyReaderWasOpen = false;
  bool bibliographyReaderReturnToPapers = false;
  BibliographyRoute bibliographyReaderReturnRoute = BR_REVIEW;
  BibliographyRoute bibliographyWriterReturnRoute = BR_PAPERS;
  bool bibliographyWriterReturnToPapers = true;
  bool railExpanded = true;               // automatically renders compact below the usable-width breakpoint
  void setWorkspace(Workspace target);
  void activateWorkspaceNav(const string& id);
  void showVisualizationMap();
  void showPapersOnMap();
  bool inspectorOpen = true;
  float leftW = 344, rightW = 300;
  Rect canvasR, leftR, rightR, topR, statusR, railR;
  Rect mainR;  // the whole main area; canvasR is the map's part of it (the same unless the split view is on)
  // ---- split view (dashboard): the main area in 1, 2 or 4 panes; one may be the live map, the others charts (split.cpp)
  int splitMode = 0;             // 0 single, 1 two columns, 2 two rows, 3 four panes
  string splitChart[4] = {"map", "live:density", "publications_per_year", "top_sources"};  // per pane: "map", "live:<view>" (a linked view) or a chart id
  int splitMax = -1;             // pane shown alone for the moment (-1 = none)
  int splitPanes() const { return splitMode == 3 ? 4 : splitMode ? 2 : 1; }
  bool splitActive() const;      // panes are drawn this frame
  int splitMapPane() const;      // the pane that shows the live map, -1 = none
  Rect splitRect(int pane) const;
  void setSplit(int mode);
  void setSplitChart(int pane, const string& id);
  void splitSave();
  void splitLoad();
  void drawSplitPanes();
  void drawSplitPane(int pane, const Rect& r);
  bool makeChartDef(const string& id, const Json& args, ChartDef& def, string& err, string* summary = nullptr);  // a chart by id (agent.cpp)
  struct PaneState { string id; ChartDef def; string err; bool ready = false; };
  // 1.16 linked views: a pane can be a second live view of the map ("live:<view>") — its own camera and view kind
  // (network, overlay, density, timeline, geography, 3D) on the same data; selection, filters and hover are shared.
  struct LivePane {
    NetView nv;
    ViewKind kind = ViewKind::Network;
    bool ready = false, failed = false, fitted = false, dataDirty = true, posDirty = true;
    uint64_t flagsSig = 0, posSig = 0, dataSig = 0;
    bool asked3d = false;             // the 3D layout was requested for this pane (once per kind change)
    int hover = -1;
    int drag = 0;                // 1 pan / orbit with the left button
    float lastMx = 0, lastMy = 0, dragX0 = 0, dragY0 = 0;
    bool dragMoved = false;
    Rect vp{0, 0, 0, 0};         // the pane this frame (w = 0: not shown)
    Rect chrome{0, 0, 0, 0};     // cached controls hit area for pointer target tests
  };
  LivePane livePane_[4];
  static bool liveId(const string& id, ViewKind* kind = nullptr);  // "live:overlay" -> Overlay
  bool liveAny() const;                                             // a live pane is shown this frame
  bool liveClean() const;                                           // no pane needs a render (buffers clean, no press / wheel in it)
  void liveSync();                                                  // before the GPU pass: data, positions, flags, framing
  void liveBackground();                                            // Geo panes: the world painted into their background layer (before beginGpu)
  void liveRender(const Theme& th);                                 // inside Gfx::beginGpu / endGpu
  static void timelineRange(double* s0, double* s1, double* x0, double* x1);  // the Timeline view's year -> world x mapping
  void liveOverlay(const Theme& th);                                // labels (Direct2D), after the GPU pass
  uint64_t liveSignature() const;                                   // what the canvas copy depends on (folded into canvasSignature)
  void drawLivePane(int pane, const Rect& r);                       // chrome and interaction (split.cpp)
  Rect livePaneChromeRect(int pane, const Rect& r);
  int liveHover() const;                                            // the node under the pointer in any live pane
  PaneState paneState_[4];
  bool welcome = true;

  // view
  ViewKind view = ViewKind::Network;
  vector<std::array<float, 3>> animFrom, animTo;
  double animT0 = -1, animDur = 0.35;
  vector<std::array<float, 3>> pos3d;
  bool pos3dValid = false;
  vector<int> selection;
  int hover = -1, focusNode = -1;
  string search;
  vector<int> searchHits;
  int searchIdx = 0;
  int clusterFilter = -1;  // highlight a single cluster
  // ---- linked selection (1.10): the highlighted cluster or the selected items scope the analysis pages, the papers
  // table and the geo view to the records behind them. scopeActive() refreshes the cached subset and says whether a
  // scope narrows the corpus; scopeCorpus() is what the pages analyse.
  bool linkScope = true;      // setting "linkScope"
  uint64_t scopeVer_ = 0;     // bumps whenever the scope changes (memoised analyses key on it)
  string scopeKey_;
  Corpus scopeC_;
  vector<int> scopeRecs_;     // record indices in the scope (sorted); empty = whole corpus
  string scopeLabel_;         // "Cluster 3: deep learning" / "2 selected items"
  bool scopeActive();
  const Corpus& scopeCorpus() { return scopeActive() ? scopeC_ : P->corpus; }
  const string& scopeLabel() { scopeActive(); return scopeLabel_; }
  void scopeChanged();        // drops analyses derived from the scoped corpus
  void clearScope();
  void drawScopeBanner(Lay& L);  // "Cluster 3 · 412 of 1,200 records — Clear · Unlink" at the top of analysis pages
  bool showLabels = true, showMinimap = true, showLegend = true;
  bool styleDirty = true, posDirty = true;

  // canvas interaction
  enum Drag { DR_NONE, DR_PAN, DR_NODE, DR_BOX, DR_ORBIT, DR_MINIMAP } drag = DR_NONE;
  float dragX0 = 0, dragY0 = 0, lastMx = 0, lastMy = 0;
  int dragNode = -1;
  bool dragMoved = false;
  Snapshot dragSnap;
  double camTargetZoom = -1, camTargetX = 0, camTargetY = 0;  // smooth camera

  // main-area chart (Matrix view or an expanded chart)
  ChartDef mainChart;
  bool mainChartOpen = false;
  string mainChartSummary;  // what the assistant's show_chart put there (for get_ui_state)

  // papers table: every record as a sortable, filterable list in the main area (papers.cpp)
  bool papersOpen = false;
  string papersFilter;         // words that must all match title, authors, source, keywords, DOI or year; "2015-2020" = year range
  int papersStatusFilter = -1; // reference-management view: -1 any; otherwise ReadStatus value
  int papersPdfFilter = -1;    // -1 any, 0 no linked PDF, 1 linked PDF
  string papersCollectionFilter, papersTagFilter;
  bool papersNewCollectionOpen = false, papersNewViewOpen = false;
  string papersNewCollectionName, papersNewViewName, papersNewTagName;
  int papersSort = 0;          // 0 year, 1 title, 2 authors, 3 source, 4 citations
  bool papersDesc = true;
  bool papersDetailed = true;  // two-line rows (keywords under the title) or compact single lines
  vector<int> papersRows;      // record indices after filter and sort
  string papersBuiltKey;       // filter|sort|corpus version the rows were built for
  std::unordered_set<string> papersAmbiguousRecordKeys_;  // prevent org actions from conflating duplicate ref keys
  uintptr_t papersAmbiguityProject_ = 0;
  uint64_t papersAmbiguityCorpusVersion_ = 0;
  size_t papersAmbiguityRecordCount_ = 0;
  bool papersAmbiguityIndexReady_ = false;
  vector<char> papersSel;      // multi-selection (per record), for exports
  int papersCursor = -1;       // record of the keyboard cursor / last click
  float papersRowH_ = 30, papersListH_ = 300;
  int papersMetadataRecord_ = -1;
  Record papersMetadataDraft_;
  string papersEditYear_, papersEditAuthors_, papersEditKeywords_, papersEditIndexTerms_, papersEditReferences_, papersEditAffiliations_, papersEditCountries_, papersEditExtra_;
  vector<RecordDuplicateGroup> papersDuplicateGroups_;
  size_t papersDuplicatePos_ = 0;
  bool papersDuplicateKeepFirst_ = true;
  std::unordered_map<string, bool> papersDuplicateChoices_;
  uint64_t papersDuplicateCacheVersion_ = ~uint64_t(0);
  vector<RecordDuplicateGroup> papersDuplicateCache_;
  void openPaperMetadataEditor(int rec);
  void openPaperDuplicateReview();
  void drawPapersMetadataModal();
  void drawPapersDuplicateModal();
  void savePaperMetadata();
  void keepPaperDuplicatesBoth(int groupIndex);
  void mergePaperDuplicate(int groupIndex, bool keepFirst);
  void openPapers(const string& filter = "", int sort = -1, bool desc = true, bool resetScroll = true);
  void applyPaperView(int status, int pdf, const string& collectionId, const string& tag, const string& query, BibliographyRoute route = BR_PAPERS);
  void saveCurrentPaperView(const string& name);
  void organizePaperSelection(const string& collectionId);
  void togglePaperTagForSelection(const string& tag);
  void drawPapers(const Rect& r);
  void papersRebuild();
  bool papersKey(int vk);                 // keyboard navigation while the table is open
  int papersSelectedCount() const;
  vector<int> papersTargets() const;      // selection, or the filtered rows
  void papersExport(const string& fmt);   // ris | csv | wos | clip
  string papersSummary(int n) const;      // for the assistant
  static int papersSortFromName(const string& n);  // "year|title|authors|source|citations" -> column, -1 unknown

  // jobs
  std::unique_ptr<Job> job;
  string jobLabel;
  std::deque<std::function<void()>> uiQueue;  // posted from jobs, run on the UI thread
  std::mutex qmu;
  void post(std::function<void()> fn) {
    { std::lock_guard<std::mutex> l(qmu); uiQueue.push_back(std::move(fn)); }
    if (hwnd) PostMessageW(hwnd, WM_NULL, 0, 0);  // wake an idle message loop
  }
  bool startJob(const string& label, std::function<void(Job&)> fn, std::function<void()> done = nullptr);
  bool busy() const { return job && job->running(); }

  // undo
  vector<UndoEntry> undoStack, redoStack;
  Json styleCommitted;
  void pushUndo(const string& what);
  void undo();
  void redo();
  void commitStyleIfChanged();

  // palette
  bool paletteOpen = false;
  string paletteQuery;
  int paletteSel = 0;
  vector<Command> commands;
  void buildCommands();

  // script mode (headless screenshots under Wine / CI)
  vector<string> script;
  size_t scriptPos = 0;
  int scriptWait = 0;
  bool scriptMode = false;
  void stepScript();
  bool runCommand(const string& line, string* note = nullptr);  // one automation command (script line or run_commands); false = unknown
  bool shotPending() const;             // a screenshot is written at the end of this frame
  void shotRequest(const string& path, bool canvasOnly);  // take one at the end of this frame ("live-vision" = hand it to the Live assistant)
  // Live overlay frames: while only the assistant's orb / level meter moves, App::frame redraws that overlay over a
  // GPU copy of the last full frame (Gfx::cacheCapture) at the overlay's own rate instead of rendering the whole window
  // Canvas layer cache: the map (GPU render + label overlay) is kept as a GPU copy and drawn back while nothing that
  // feeds it changed — camera, positions, flags, style, theme, view, size, geo state — and the mouse is not over the
  // canvas; hovering a panel then redraws only the chrome. Setting "canvasCache" (script: canvascache 0|1).
  bool canvasCacheOn = true;
  uint64_t canvasSig_ = 0;          // inputs of the picture in Gfx::canvasCopy
  mutable uint64_t flagsSig_ = 0;   // cheap state signature for nv.flags (recomputed only after an input revision)
  mutable uint64_t flagsInputVersion_ = 1;
  int canvasHits = 0, canvasMisses = 0;  // frames served from the copy / rendered, since the last "log"
  uint64_t canvasSignature(const Theme& th) const;
  void invalidateFlags() const;
  void setCanvasCache(bool on);
  void setTextCache(bool on);       // Ui text-layout cache (setting "textCache", script textcache 0|1)
  // Performance overlay (palette "Toggle performance overlay", script perf 0|1): frames per second, frame time,
  // canvas/text cache hit rates, CPU and memory of the process; refreshed twice a second, drawn over the map.
  bool perfHud = false;
  struct PerfWindow { int frames = 0, overlay = 0, canvasHit = 0, canvasMiss = 0, textHit = 0, textMiss = 0; double ms = 0, msMax = 0, sec = 0; };
  PerfWindow perf_, perfShown_;
  double perfT0_ = 0, perfCpu_ = 0, perfMemMB_ = 0, perfCpuT_ = 0;
  uint64_t perfCpuTicks_ = 0;
  void perfFrameDone(double t0, bool overlayOnly, bool canvasFromCopy);
  void drawPerfHud();
  bool liveOverlayFrame_ = false;   // the next frame() may be an overlay-only one
  bool liveCached_ = false;         // the Gfx cache holds the current window without the overlay
  double liveFrameT_ = 0;           // when the overlay was last drawn (rate limiting)
  bool frameOverlay(double t);      // the overlay-only frame; false = a full frame is needed instead
  static const char* pageTitle(int p);  // "Data", "Build", ...

  // ---------------------------------------------------------------- frame
  void frame();
  void layout();
  void drawTopBar();
  // unified window frame: the app draws its own caption buttons in the top bar
  bool maxBtnDown = false;
  float captionW() const { return 3 * 46 * ui.s; }
  Rect captionBtn(int i) const { float bw = 46 * ui.s; return {float(g.W) - bw * float(3 - i), 0, bw, topR.h > 0 ? topR.h : 52 * ui.s}; }
  void drawCaption();
  void applyFrameTheme();
  // Theme reveal (1.9.6): the theme does not flip, it spreads. The frame in the old theme is kept on the GPU; the new
  // theme is drawn normally and the old picture is laid over it through a circular layer centred on the sun/moon
  // button — dark grows outward from the button, light closes in from the edges onto it (750 ms, ease in-out, hard
  // edge). The button's icon morphs from sun to moon (or back) at the same time. See setTheme / drawThemeReveal.
  struct ThemeAnim {
    bool pending = false, pendingDark = false;  // asked for: this frame ends in the old theme, is captured, then the switch
    bool active = false;
    bool toDark = false;         // target of the current leg
    bool oldDark = false;        // theme of the captured picture
    double t0 = 0, dur = 0.75;   // start and length of the current leg (a leg after a turn-round is shorter)
    float r0 = 0, r1 = 0, r = 0, rmax = 0;
    float ox = 0, oy = 0;        // centre (window pixels)
    Rect btn;                    // the sun/moon button as drawn in the last top bar
    bool btnHover = false;
    double iconT0 = -1;          // start of the icon morph (-1 none)
    string iconFrom, iconTo;
  } themeAnim;
  Gfx::BackCopy themeCopy_;      // the window in the old theme
  bool themeTarget() const { return themeAnim.pending ? themeAnim.pendingDark : themeAnim.active ? themeAnim.toDark : ui.dark; }  // the theme asked for
  bool themeRevealAllowed() const;
  double themeRevealSeconds() const;  // setting themeRevealMs (750)
  void applyTheme(bool dark);    // the switch itself (colours, style, DWM)
  void drawThemeButton(const Rect& r, bool hovered, bool held, const UiColors& col);  // background + (morphing) icon
  void drawThemeReveal(double t);  // the old picture through the circular layer; the last step of a frame
  void drawRail();
  void drawLeft();
  void drawCanvas();
  void drawCanvasChrome();
  void drawInspector();
  void drawStatus();
  void drawWelcome();
  void drawBibliographyHome(const Rect& r);
  void drawStart();
  bool showStart = false;  // start window (recent projects, New / Open / Sample)
  void drawPalette();
  void drawMainChart(const Rect& r);
  void handleShortcuts();
  uint64_t pointerHoverIdAt(float x, float y);
  bool pointerMoveNeedsFrame(float x, float y);
  void addChartHoverRegion(const Scene& scene, const Rect& bounds, float ox, float oy, float scale, uint64_t context);
  bool chartHoverTargetAt(float x, float y, uint64_t& target) const;
  void canvasInput();

  // pages (pages.cpp)
  void pageData(Lay& L);
  void pageBuild(Lay& L);
  void pageLook(Lay& L);
  void pageAnalyse(Lay& L);
  void pageTrends(Lay& L);
  void pagePublish(Lay& L);
  void pageActors(Lay& L);
  float chartBox(Lay& L, const string& key, const ChartDef& def, float h);  // panel chart with expand/export
  void sectionTitle(Lay& L, const string& t, const string& help = "", const string& status = "");
  void buildBar(const Rect& r);
  void emptyHint(Lay& L, const string& icon, const string& title, const string& msg);

  // ---------------------------------------------------------------- commands
  void cmdOpenFiles();
  void cmdOpenAny();
  void cmdAddFiles(const vector<string>& files);
  void cmdSample(bool scopus);
  void cmdCite();  // copy APA + BibTeX references to the clipboard
  void cmdBuild();
  void cmdRelayout();
  void cmdRecluster();
  void cmdBundles();
  void cmdSaveProject(bool saveAs);
  void cmdOpenProject(const string& path = "");
  void cmdNewProject();
  void newProjectNow();  // a blank project without the confirmation dialog (cmdNewProject and the assistant's new_project)
  void cmdExportFigure(const string& fmt, const string& path = "");
  void cmdExportView(const string& path = "");
  void cmdCopyFigure();
  void cmdExportChart(const ChartDef& def, const string& fmt);
  void cmdExportChartsPdf(const vector<ChartDef>& defs, const string& path0 = "");  // one chart per page
  struct ChartCacheEntry { float w = 0, h = 0; bool dark = false; uint64_t sig = 0; int frame = 0; unsigned gen = 0; Scene sc; };
  std::unordered_map<string, ChartCacheEntry> chartCache;  // chart scenes reused while nothing but the mouse position changes
  void drawChartCached(const string& cacheKey, unsigned gen, const Scene& sc, float ox, float oy, float k);  // chart bitmap cache (pages.cpp)
  int chartFresh = 3;     // frames left during which charts are always rebuilt (set after input, jobs and script steps)
  uint64_t chartSig() const;
  vector<ChartDef> pageCharts, frameCharts;  // charts shown on the current page (collected by chartBox each frame)
  void cmdLoadVosviewer();
  void cmdExportVosviewer();
  void cmdExportItemsCsv();
  void cmdOpenAlex();
  void cmdExportRecords(bool lastFetch, const string& fmt);
  void setView(ViewKind v, bool animate = true);
  void selectNode(int i, bool add = false);
  void focusOn(int i);
  void focusCluster(int c);
  void clearSelection();
  void runSearch();
  void setTheme(bool dark);
  void afterMapChanged(bool fit = true);  // new network (build/open)
  void setPage(int p);
  bool dropFiles(const vector<string>& files);

  // ---- 1.2 (insight.cpp): top-left toolbar, map history, window title, panel animation, previews
  struct MapSnap { Network net; AnaSpec spec; BuildParams params; Bundles bundles; BuildReport last; string mapSource, title, when; };
  vector<MapSnap> mapHistory;
  int mapCur = -1;
  bool restoringMap = false;
  string windowTitle_;
  void drawTopLeft(float& xEnd);
  string mapTitle() const;
  void rememberMap();
  void restoreMap(int k);
  void mapsToProject();    // copy the map history into the project before saving
  void mapsFromProject();  // rebuild the map history from an opened project
  void updateWindowTitle();
  float leftT = 1, rightT = 0;  // panel slide state 0..1
  int leftShownPage = PG_DATA;  // page drawn while the panel slides closed
  bool panelMoving_ = false, reduceMotion = false;
  void stepPanelAnim();
  float leftEase() const;
  float rightEase() const;
  bool canInspect() const;
  double lastDtSec() const;
  void drawLeftBody();
  void drawInspectorBody();
  double figZoomT0 = -1;
  Rect figZoomFrom;
  // writer: the document editor in the main area (writer.cpp). The assistant's report tools write into the same
  // document, the project file saves it, and the PDF / Word / HTML exports read it.
  Document wdoc;
  Editor wed{&wdoc};
  bool writerOpen = false;
  int writerPane = 0;                  // side pane: hidden by default; 1 outline, 2 properties, 3 find and replace
  int writerPaneBeforePdfPreview_ = 0;
  bool writerPdfPreviewOpen_ = false, writerPdfCompileBusy_ = false;
  uint64_t writerPdfCompiledVersion_ = ~0ull, writerPdfGeneration_ = 0;
  string writerPdfPath_, writerPdfCompileError_;
  string writerEquationBuf_, writerEquationError_;
  bool writerEquationFocus_ = false;
  std::shared_ptr<WriterPdfPreview> writerPdfPreview_;
  float writerZoom = 1.f;              // 0.4 .. 3 (also fitted to the width of the main area)
  float writerScroll = 0;              // page pixels
  float writerK_ = 1;                  // layout scale: device pixels per point at 100 % (zoom is a transform on top)
  float writerViewH_ = 0;
  Rect writerPageR_;                   // where the page was drawn last frame (screen)
  bool writerFollow_ = false;          // scroll so that the caret is visible
  float writerCaretX_ = -1;            // remembered x (page px) for Up / Down
  bool writerDrag_ = false;
  bool writerTouchPending_ = false, writerTouchPanning_ = false, writerTouchSelecting_ = false;
  float writerTouchStartX_ = 0, writerTouchStartY_ = 0, writerTouchLastX_ = 0, writerTouchLastY_ = 0;
  double writerTouchStartT_ = 0;
  double writerClickT_ = 0; int writerClicks_ = 0;  // double / triple click
  uint64_t writerSavedVer_ = 0, writerSeenVer_ = 0;
  string writerFind, writerReplace, writerLinkBuf, writerToast_;
  bool writerMatchCase = false, writerLinkFocus_ = false, writerPopupPrev_ = false;
  int writerTblRows = 3, writerTblCols = 3;
  float writerCtxX_ = 0, writerCtxY_ = 0;
  std::shared_ptr<WriterLayout> wlay;
  void openWriter();
  void closeWriter();
  void drawWriter(const Rect& r);
  bool writerKey(int vk);                       // keyboard map while the writer is open; true = consumed
  void writerTyped();                           // typed characters -> the document
  void writerReset();                           // new project: an empty document
  void writerFromProject();                     // P->document -> the editor (after opening a project)
  void writerToProject();                       // the editor -> P->document (before saving)
  bool writerDirty() const { return wed.version != writerSavedVer_; }
  void writerRelayout(float k);                 // (re)builds the layouts that changed
  string writerExport(const string& fmt, const string& path = "");  // "pdf" | "docx" | "html" -> the file written, "" when cancelled / failed
  void writerCompileLatex();                         // compile a snapshot of the visual document to a temporary PDF
  void writerSetPdfPreview(bool on);                 // optional split preview; restores the previous writer pane
  bool writerOpenPdfSeparately();                    // open the latest compiled PDF in the system viewer
  void writerDrawPdfPreview(const Rect& r);           // PDFium-backed, scrollable preview pane
  void writerPreviewLoad(const string& path);         // asynchronously open in the shared PDFium worker
  void writerPreviewClear();                          // drop the current preview document and bitmaps
  void writerPreviewShutdown();                       // release preview state before the PDF worker stops
  ScenePng scenePng();                          // renders a Scene to PNG bytes with the GPU (Word, HTML)
  void writerInsertChart(const string& id);     // a chart of the loaded data as a figure at the caret
  void writerInsertMap();                       // the current map view as a figure at the caret
  void writerCite(int rec);                     // "[n]" at the caret + the reference under "References"
  void writerEnsureTitle(const string& title, const string& subtitle);  // a title block when the document has none
  string writerSummary() const;                 // one line for the assistant and the status bar
  string writerSlug() const;                    // file name stem from the title
  int writerHitBlock(float px, float py) const; // block under a page point (-1 none)
  DocPos writerHit(float px, float py) const;   // caret position for a page point
  bool writerCaretRect(const DocPos& p, float& x, float& y, float& h) const;  // page px
  void writerMoveLine(int dir, bool extend);    // Up / Down through the layouts
  void writerLineEdge(bool end, bool extend);   // Home / End of the visual line
  void writerCopy(bool cut, bool toWord = false);  // text + Word HTML (+ picture) on the clipboard; toWord = paste at Word's cursor
  vector<WordSource> writerClipSources() const;    // the sources cited by the copied fragment
  bool writerWordSources_ = true;                  // setting: hand the sources of copied citations to the open Word document
  void writerPaste(bool plain);
  bool writerPasteHtml();                       // "HTML Format" on the clipboard (Word, browsers) -> blocks; false = none
  void writerOpenInWord();                      // export .docx to the Reports folder and open it
  void writerDropImages();                      // release the figure bitmaps while the writer is not shown
  DocFragment writerClip_;                      // the fragment behind the last copy (rich paste inside the application)
  string writerClipText_;
  int writerClipCites_ = 0;
  // 1.12: pagination, smooth zoom, properties pane, citation picker, menus
  float writerZoomShown_ = 1.f;                 // the zoom drawn this frame (eases towards writerZoom; the page is scaled, not re-laid out, until it settles)
  float writerF_ = 1.f;                         // display scale over the layout scale last frame
  double writerZoomT_ = 0;                      // when the zoom last changed
  double writerZoomPrevT_ = 0;                  // the previous frame's time (the easing is clocked, not per frame)
  string writerStatusRight_; uint64_t writerStatusVer_ = ~0ull; bool writerStatusDirty_ = false;  // status-bar summary cache
  float writerZoomAnchorY_ = -1;                // screen y kept still while zooming (mouse wheel) — -1 = the middle of the view
  float writerZoomAnchorX_ = -1;                // and x (when the page is wider than the area)
  float writerPageAreaW_ = 0;                   // width of the page area last frame (for the fit-width zoom)
  // the mini toolbar and the right-click rules
  bool writerMiniShow_ = false, writerMiniCtx_ = false, writerSelGesture_ = false;
  float writerMiniX_ = 0, writerMiniY_ = 0;     // pointer position the bar is placed against
  Rect writerMiniR_{0, 0, 0, 0}, writerCtxR_{0, 0, 0, 0};
  DocPos writerPrevCaret_, writerPrevAnchor_;
  double writerSelT_ = -100;                    // when the selection last changed
  float writerSelRefX_ = 0, writerSelRefY_ = 0;  // screen point of that change (pointer, or the caret for keyboard selections)
  void drawWriterMini(const Rect& pageArea);
  void writerColorButton(const Rect& b, const string& key, bool inText);
  bool writerFitOnce_ = false;                  // fit the page width when the writer opens wider than the window
  float writerFitZoom() const;
  float writerScrollX_ = 0;                     // sideways scroll of a page wider than the area (px)
  uint64_t writerRefreshedVer_ = ~0ull;         // wed.version at the last citation refresh
  int writerCaretPage_ = 0, writerPages_ = 1;
  string writerCiteQuery, writerCtxSub_, writerHeaderBuf_, writerLabelBuf_, writerAltBuf_;
  vector<char> writerCiteSel;                   // multi-select in the citation picker (per record)
  bool writerCiteFocus_ = false, writerNewConfirm_ = false;
  vector<int> writerCiteOrder_;                 // the citation picker's filtered order (rebuilt when its key changes)
  string writerCiteOrderKey_;
  vector<string> writerFonts_;                  // installed font families (filled once)
  const vector<string>& writerFonts();
  void writerCiteMany(const vector<int>& recs); // one citation field with several papers
  void writerNewDocument();                     // an empty document (undoable)
  void pageWriter(Lay& L);                      // the Write page's left panel (outline, actions, exports)
  bool writerCaretLocal(const DocPos& p, float& x, float& y, float& h) const;  // block-local layout px
  float writerDocY(int blk, float localY) const;                              // block-local -> page-column y (layout px)
  bool writerDocToLocal(float lx, float ly, int& blk, float& localY) const;     // page-column point -> block and block-local y
  string menuCur_;                              // the open menu's title ("" = the title list in narrow windows)
  // ---- the PDF reader and the reading library (reader.cpp, readlib.cpp; state in reader.h)
  bool readerOpen = false;                      // the reader fills the main area (the writer may stay open underneath)
  std::shared_ptr<ReaderState> rd;              // engine thread, open document, tiles, view, selection (lazy)
  void readerEnsureEngine();                    // starts the engine thread and loads the DLL (first use)
  void readerShutdown();                        // joins the engine thread (exit)
  void readerProjectChanged();                  // new / opened project: closes the document, forgets the caches
  void readerResetDoc();                        // closes the open document and drops every cache (tiles, text, previews)
  double readerIdleTick();                      // message loop: releases a closed reader's document after a while; seconds until then or -1
  void openReader(const string& itemId, int page = -1, float y = -1);
  void closeReader();
  void drawReader(const Rect& r);
  bool readerHoverTargetAt(float x, float y, uint64_t& target);
  void drawReaderMini(const Rect& pageArea);
  void drawReaderCard(const Rect& pageArea, float colLeft, float colW, float kD);  // the comment card next to an annotation
  void drawReaderContext();
  void drawReaderPane(const Rect& r);
  void readerOpenCard(const string& annotId, bool focus);
  void readerCloseCard();
  bool readerKey(int vk);
  void readerSetDark(bool on);   // the dark page (reader only; remembered in settings)
  bool readerDark() const;
  void readerRotate(int quarterTurns);            // turn the view of this PDF (kept with the PDF in the project)
  int readerRotation() const;                     // 0..3 quarter turns clockwise
  void readerSetLayout(bool twoUp, bool coverAlone);  // one page, or two facing pages (first alone as a book opens); settings
  bool readerTwoUp() const;
  bool readerCoverAlone() const;
  int readerPageStep(int dir) const;              // the page a row before / after the current one (arrows, buttons)
  void readerPrintDialog();                       // the Print card (pages, marks, fit), then the system's printer dialog
  void readerPrint(HDC hdc, int fromPage, int toPage, const vector<int>& pages, bool marks, bool fit, bool autoRotate, int copies);  // rdprint.cpp: the job
  void drawReaderPrintCard();                     // rdprint.cpp
  void readerTyped();
  PdfItem* readerItem();
  int readerRecord();                           // the linked record's index or -1
  int readerCurrentPage() const;
  void readerGoTo(int page, float y, bool animate);
  void readerStorePosition();
  string readerSelectedText();
  void readerClearSelection();
  PdfAnnot* readerAddMarkup(int kind, Code code);
  PdfAnnot* readerAddNote(int page, float x, float y);
  PdfAnnot* readerAddArea(int page, float x0, float y0, float x1, float y1);  // an area of a page (scans, figures, tables)
  PdfAnnot* readerAddInk(int page, vector<pdf::InkPoint> points);  // freehand annotation, stored as page-space InkList strokes
  void readerUndoInk();
  void readerAreaImage(const string& id, int how);                            // 0 = copy the area as an image, 1 = insert it into the document
  void readerDeleteAnnot(const string& id);
  void readerQuote(const string& text, int rec);
  void readerCite();
  void readerFindStart();
  void readerFindNext(int dir, bool fromView);
  // library flows (readlib.cpp)
  void pageRead(Lay& L);                        // the Read page's left panel: the reading list
  vector<string> readerAttach(const vector<string>& paths, int linkRec = -1, bool open = false);
  void readerAttachDialog(int linkRec = -1);
  void readerIdentify(const string& itemId);    // DOI / title from the first page -> link to a record
  void readerRelocate(const string& itemId);
  void readerDetach(const string& itemId);
  void readerSaveIntoPdf(const string& itemId);
  void readerExportNotes(const string& itemId); // one paper (empty = all) as Markdown
  void readerExportCoding();                    // the coding matrix as CSV
  void drawReaderLibraryMenu();
  const PdfItem* readerItemForRecord(int rec) const;
  // Get PDF (fetch.cpp, 1.18): the open-access copy of a record by its DOI — OpenAlex's cached copy (with a key),
  // the publisher's or a repository's PDF, arXiv. Runs as the app's job; outcomes land in P->library.fetch.
  struct FetchRun {                             // shared by the job thread and the UI
    std::mutex mu;
    int total = 0, done = 0, attached = 0, closed = 0, refused = 0, failed = 0, budget = 0, cachedNoKey = 0;
    string current, dir, firstKey;              // the paper being fetched; where the files go; the record key of a one-paper run
    bool haveKey = false, cancelled = false, finished = false;
    double remainingUsd = -1;                   // OpenAlex's budget after the last cached copy (from its headers), -1 unknown
    vector<string> newIds;                      // library ids of the attached files
  };
  std::shared_ptr<FetchRun> fetchRun;           // the running or the last run (the summary reads it)
  vector<int> fetchAskRecs;                     // what the confirm card offers to fetch
  int fetchSkippedNoDoi = 0;                    // records without a DOI among the request (the card says so)
  int fetchMenuRec = -1;                        // the per-record card ("fetchrec")
  float fetchMenuX = 0, fetchMenuY = 0;
  bool fetchListOpen = false;                   // the Read page's "Without a PDF" section is expanded
  string fetchDir() const;                      // "<project> attachments" next to the project file, else Documents\VOSStudio\Attachments
  vector<int> fetchCandidates(const vector<int>& among) const;  // those with a DOI whose linked file does not exist
  bool fetchRecordHasFile(int rec) const;
  void fetchAsk(const vector<int>& recs, bool always = false);  // the confirm card (skips it for one record unless `always`)
  bool fetchPdfs(const vector<int>& recs);      // starts the job; false when nothing to do or another job runs
  void fetchRecordMenu(int rec, float x, float y);
  void drawFetchPopups();                       // the confirm card, the summary and the per-record card (app.cpp calls it every frame)
  void drawFetchSection(Lay& L);                // readlib.cpp: progress + "Without a PDF" on the Read page
  vector<int> fetchNeedsFile() const;           // records whose last fetch did not end with a file (sorted: refused first)
  unsigned fetchGen = 0;                        // bumped whenever a status changes (the list above is cached per frame on it)
  mutable uint64_t fetchNeedSig_ = ~0ull;
  mutable vector<int> fetchNeedCache_;
  string fetchSummary() const;                  // one line for the assistant / scripts
  int menuSub_ = -1;                            // row whose submenu is open
  Rect menuAnchor_;                             // the title the open menu hangs from
  void drawMapChip(const Rect& chip);           // insight.cpp: the map title / switcher chip (right-hand block of the top bar)
  void drawMenuBar(float& x);                   // menus.cpp: File / Edit / View / Map / Analyse / Write / AI / Help
  void menuRun(const string& id);               // a command id from the menus (commands registry + a few menu-only ids)
  // document preview
  int docPreview = -1;
  vector<int> docBackStack;
  bool docAbsOpen = false, docRefsAll = false;
  void openDoc(int rec);
  void closeDoc();
  void drawDocPreview(Lay& L);
  float docRow(Lay& L, int rec, const string& key);
  void chipFlow(Lay& L, const vector<string>& v, const string& key, int maxN);
  void findInMap(const string& label);
  // country profile
  struct CountryProfile {
    int country = -1;
    string sig;
    vector<int> recs;
    Growth gr;
    long long cites = 0;
    int h = 0, intl = 0;
    vector<std::pair<string, int>> kw, orgs, authors, sources;
  };
  CountryProfile cprof;
  void buildCountryProfile(int country);
  void drawCountryProfile(Lay& L, int country, bool compact);
  // local citations
  CitationIndex citeIdx;
  uint64_t citeVer = 0;
  // Corpus statistics shown every frame, cached per corpus version
  struct CorpusStats {
    uint64_t ver = 0;
    std::optional<QualityReport> quality;
    std::optional<Growth> growth;
    std::optional<Bradford> bradford;
    std::optional<Lotka> lotka;
    std::optional<CitationSummary> citeSummary;
  } cstats;
  template <class T, class F> const T& memo(std::optional<T> CorpusStats::*slot, F f) {
    scopeActive();  // refresh the linked scope first: it is part of the key
    uint64_t ver = P->corpusVersion * 1000003ull + scopeVer_;
    if (cstats.ver != ver) { cstats = CorpusStats(); cstats.ver = ver; }
    auto& o = cstats.*slot;
    if (!o) o = f();
    return *o;
  }
  // the quality report always describes the whole data set; the other statistics follow the linked scope
  const QualityReport& statQuality() { return memo(&CorpusStats::quality, [&] { return qualityReport(P->corpus); }); }
  const Growth& statGrowth() { return memo(&CorpusStats::growth, [&] { return growth(scopeCorpus()); }); }
  const Bradford& statBradford() { return memo(&CorpusStats::bradford, [&] { return bradford(scopeCorpus()); }); }
  const Lotka& statLotka() { return memo(&CorpusStats::lotka, [&] { return lotka(scopeCorpus()); }); }
  const CitationSummary& statCiteSummary() { return memo(&CorpusStats::citeSummary, [&] { return citationSummary(scopeCorpus()); }); }
  const CitationIndex& citations();
  // 1.2 analyses (pages.cpp)
  int ttMin = 3, ttPerYear = 3;
  bool ttValid = false;
  vector<TrendTopic> ttopics;
  int rpFrom = 0, rpTo = 0;
  bool rpValid = false;
  Rpys rpy;
  int docRank = 0, histTop = 30;
  int cyMode = 0;
  bool cyValid = false;
  ClusterYears cyears;
  int prodUnit = -1;
  Production prod;
  bool collabValid = false;
  vector<CountryCollab> collab;
  void tabRow(Lay& L, const vector<string>& names, const vector<int>& ids, int& sel);
  void trendsTopics(Lay& L);
  void trendsRpys(Lay& L);
  void actorsDocs(Lay& L);
  void actorsTimeline(Lay& L, Unit u);
  void analyseClusterYears(Lay& L);

  // helpers
  vector<std::array<float, 3>> targetPositions(ViewKind v);
  bool geoAvailable() const;
  // ---- geo (geo.cpp)
  int geoFill = 0;          // countries map: 0 cluster colour, 1 weight (colour map), 2 none
  bool geoBasemap = true;   // countries map: draw the world basemap
  bool geoArcs = true;      // corpus overview: collaboration arcs
  int geoLayerKind = 0;     // Geo layers: 0 network (cluster colours), 1 overlay (score, or average year per country), 2 density
  int geoDenStyle = 0;      // Geo density: 0 heat (kernel density), 1 clusters (cluster density, country maps), 2 countries (shaded countries)
  int geoDenMeasure = 0;    // Geo density weighted by: 0 documents, 1 citations, 2 citations per document, 3 collaboration
  vector<double> geoDenValues() const;  // per world country: the density measure (0 = none)
  string geoDenWhat() const;            // "documents", "citations", ...
  mutable double geoYearLo = 0, geoYearHi = 0;  // corpus overview: range of the average publication year per country
  // density layer: a smooth raster over the map band, in world units (cached; CPU, then one D2D bitmap)
  mutable string geoDenSig_;
  mutable vector<uint8_t> geoDenRgba;  // straight alpha
  mutable int geoDenW = 0, geoDenH = 0;
  mutable float geoDenX0 = 0, geoDenY0 = 0, geoDenX1 = 0, geoDenY1 = 0;
  Com<ID2D1Bitmap> geoDenBmp;
  string geoDenBmpSig;
  void geoDensity() const;           // refreshes geoDenRgba for the current map
  void geoDensityLegend(float x, float y, const string& what);
  void geoLayerSwitch();
  int geoHover = -1, geoSel = -1;
  bool geoAfterBuild = false;
  vector<GeoCountryShape> geoWorld;
  vector<Com<ID2D1PathGeometry>> geoGeom;
  Com<ID2D1PathGeometry> geoGrat, geoOutline;
  mutable string geoSig_;
  mutable bool geoModeA = false;
  mutable vector<int> geoNodeCountry;
  mutable vector<GeoStat> geoStats;
  mutable vector<std::tuple<int, int, int>> geoPairs;
  mutable vector<vector<std::pair<int, int>>> geoPartners;
  mutable int geoDocsMax = 0, geoDocsTotal = 0, geoPairMax = 1;
  string geoSig() const;
  void geoUpdate() const;
  int geoMode() const;  // 0 not geo, 1 countries map on a basemap, 2 corpus overview
  void geoEnsureWorld();
  Color geoLand(const Theme& th) const;
  void geoLayer(const Theme& th, vector<GeoFill>& fills, vector<GeoBubble>& bubbles, vector<GeoArc>& arcs, const NetView& v) const;
  void geoPaint(ID2D1DeviceContext* dc, const NetView& v);  // the world under the given view (the main map or a linked pane)
  void geoPrims(Scene& sc, double k, bool under) const;
  // 1.2: every view as a publication-figure panel
  mutable bool geoFig_ = false;   // geoLayer is building a figure panel (ignore the view / selection)
  mutable double geoFigPx_ = 1;   // points per screen px for bubbles and arcs in that panel
  FigPanelDef geoFigurePanel();
  FigPanelDef threeDFigurePanel();
  vector<FigPanelDef> figurePanelDefs();
  string figurePanelSig() const;
  static string viewName(ViewKind v);
  int geoHit(float mx, float my) const;
  void geoChrome(bool mouseFree);
  void geoLegend(float x, float y);
  void geoFitBounds(ViewKind v);
  void geoFitBounds(ViewKind v, NetView& target);
  void geoChip();
  void addChrome(const Rect& r);
  // ---- WYSIWYG export of the current view (SVG / PDF / PNG)
  Scene captureView(bool forRaster) const;
  void cmdExportCurrentView(const string& fmt, const string& path = "");
  void doViewExport();
  string pendingViewFmt, pendingViewPath;
  vector<uint8_t> viewGrab;
  int viewGrabW = 0, viewGrabH = 0, lastViewExport = 0;
  // ---- chart hover
  void chartHover(const Scene& sc, float ox, float oy, float scale);
  // ---- publish preview zoom
  bool figZoomOpen = false;
  double figZoomK = 1, figZoomX = 0, figZoomY = 0;
  void drawFigureZoom();
  void registerHelp();
  bool hasMap() const { return P && P->net.n() > 0; }
  bool hasCorpus() const { return P && !P->corpus.empty(); }
  void updateFlags();
  uint64_t flagsSignature(ViewKind vk, int hoverIdx) const;                 // inputs of the flags of a view
  void computeFlags(ViewKind vk, int hoverIdx, vector<uint32_t>& out) const;  // selection, neighbours, hover, filters, hidden
  void start3DLayout();
  ChartTheme chartTheme(bool forExport) const;
  string exportName(const string& ext) const;
  vector<Color> clusterColors() const;
  void pageStateReset();

  // page state -------------------------------------------------------
  // data
  string oaQuery, oaFrom, oaTo, oaMax = "500";
  int oaMode = 0;   // 0 any term (separate searches merged), 1 all terms (AND)
  int oaField = 0;  // 0 everything (search=), 1 title & abstract, 2 title
  int oaKind = 0;         // 0 keyword search, 1 semantic search
  int oaSemStrategy = 1;  // semantic: 0 each query separately (merged), 1 expand with citations and re-rank
  int oaStrict = 1;       // re-rank strictness: 0 loose, 1 balanced, 2 strict
  vector<string> oaSemQueries = {""};
  struct OaSemInfo { int seeds = 0, cands = 0, kept = 0; double cutoff = 0; bool valid = false; } oaSemInfo;
  // settings dialog
  bool settingsWanted = false;
  string setKey, setEmail;
  // ---- settings dialog (assistant.cpp)
  int setTab = 0;                       // 0 General, 1 OpenAlex, 2 AI, 3 Live AI
  int setAiProv = 0;
  string setAiKey[ai::kProviders], setAiModel[ai::kProviders], setAiBase;
  string setLiveModel, setLiveVoice, setLiveThinking;  // Live AI tab
  bool setLiveBarge = false;
  bool setLiveVad = true;   // the app detects the start and end of the user's speech (activityStart/End) instead of the server
  bool setLiveSearch = true;
  bool setLiveAuto = true;
  struct AiProbe { std::mutex m; bool done = false, ok = false; string msg; vector<string> models; };
  std::shared_ptr<AiProbe> aiProbe;     // "Test connection" / "Load models" in Settings
  int aiProbeKind = 0;                  // 1 test, 2 models
  vector<string> aiModelList[ai::kProviders];
  // ---- AI assistant (assistant.cpp)
  struct AiStream { std::mutex m; string text, err; bool done = false; std::atomic<bool> cancel{false}; };
  std::shared_ptr<AiStream> aiLive;     // reply being streamed
  int aiLiveTurn = -1;
  ai::Task aiLiveTask = ai::Task::Chat;
  vector<ai::Turn> aiTurns;             // conversation (mirrors P->assistant)
  const Project* aiTurnsOf = nullptr;   // project the turns were loaded from
  string aiInput;
  ai::Task aiPending = ai::Task::Chat;  // task the next typed message runs (Search, Explain, Document)
  int aiDoc = -1;                       // document attached to the next request
  bool aiCtxCorpus = true, aiCtxMap = true, aiCtxTrends = true, aiCtxDocs = true, aiCtxSel = true;
  string aiCtxKey_; size_t aiCtxLen_ = 0;
  struct AiLayout { size_t len = 0; float w = 0, h = 0; };
  std::map<size_t, AiLayout> aiLayout_;
  bool aiStickBottom = true;
  mutable ai::Config aiCfg_; mutable bool aiCfgValid_ = false;
  ai::Config aiConfig() const;
  void aiSyncTurns();
  void aiSaveTurns();
  void aiSend(ai::Task t, const string& userText, const string& shown = "");
  void aiRun(ai::Task t, const string& extra = "");   // one-click task (from any page)
  void aiPump();
  void aiStop();
  void aiNewChat();
  void aiExport();
  bool aiApplyNames(const string& reply, bool quiet = false);
  bool aiApplySearch(const string& reply);
  float aiMarkdown(float x, float y, float w, const string& md, bool draw);
  void drawAssistant(const Rect& r);
  void pageAssistant(Lay& L);
  ai::ContextOpts aiContextOpts(ai::Task t) const;
  void openSettings();
  void drawSettings();
  string openAlexKey() const;
  vector<Record> oaLastRecs;
  std::shared_ptr<Json> oaLastRaw;
  string oaLastLabel;
  vector<OaTermCount> oaCounts;
  bool oaAppend = false;
  int cleanUnit = 0;
  vector<VariantGroup> variants;
  bool variantsScanned = false;
  string thesFrom, thesTo, thesFilter;
  // ---- cleaning studio (pages.cpp / assistant.cpp)
  struct ThUndo { string what; std::map<string, string> replace; };
  vector<ThUndo> thUndo;                // thesaurus before each merge (Undo)
  int cleanEdit = -1;                   // variant whose preferred label is being edited
  string cleanEditText;
  int cleanAiUnit = -1;                 // unit of the AI proposals in `variants`
  string cleanNote;                     // last AI result summary shown under the list
  void cleanScan();
  void cleanMergeSelected();
  void cleanUndo();
  void aiCleanTerms(int unitIdx);
  bool aiApplyClean(const string& reply);
  // ---- records flow (pages.cpp)
  RecordsFlow flowCache;
  string flowSig;
  const RecordsFlow& recordsFlow();
  // ---- crash reports (crash.cpp)
  struct CrashInfo { bool pending = false; string report, recovery, project, when; } crash;
  bool crashDialog = false;
  void drawCrashDialog();
  void crashCheck();
  // ---- agent (agent.cpp)
  struct AgentStepView { string title, thought, detail; int status = 0; };  // 0 running, 1 done, 2 failed, 3 declined, 4 waiting
  // One snapshot per change an assistant makes (1.9.2: a stack, newest last). "Undo" takes back the newest change only;
  // clicking again takes back the one before it. Snapshots that replaced the records keep a copy of the corpus.
  struct AgentUndo {
    bool valid = false;
    AnaSpec spec; BuildParams params; Thesaurus th; std::map<string, std::set<string>> excluded;
    Network net; Bundles bundles; string mapSource, builtSig; BuildReport last; bool clusterNames = false;
    int turn = -1;  // conversation turn of the run that made the change (-1: the Live AI session)
    bool live = false;
    string label;   // what the change was ("Build a map: co-occurrence of keywords")
    bool hasCorpus = false; Corpus corpus;  // records before a change that replaced or extended them
  };
  struct AgentRun {
    bool active = false, waitApproval = false, waitJob = false, allowAll = false, stopping = false;
    bool live = false;  // the tool runs for the Live AI session (live.cpp): results go back over the WebSocket, not to agentRequest()
    string goal, final, jobTool;
    vector<ai::Msg> msgs;
    int steps = 0, turn = -1, changes = 0, parseFails = 0;
    ai::AgentAction pending;
    std::shared_ptr<AiStream> req;
    vector<AgentStepView> log;
    uint64_t jobSeq = 0;
    // run_commands (and the typed tools built on it): application commands run one per frame, each waiting for the
    // jobs and animations of the previous one, exactly like the script runner; the notes go back to the model
    bool waitCmds = false;
    std::deque<string> cmds;
    vector<string> cmdNotes;
    int cmdJobSeq = -1;       // a command started a job: its error is reported when the job ends
    int cmdCount = 0;
    // write_report: the text model drafts the sections in the background (1.9.2)
    bool waitWrite = false;
    std::shared_ptr<AiStream> writeReq;
    Json writeArgs;
    // look_at_screen: a picture of the canvas/window is taken at the end of the frame and sent to the live session (1.9.2)
    bool waitShot = false;
    double cmdT0 = 0, cmdWaitUntil = 0;
  } agent;
  vector<AgentUndo> agentUndos;   // undo stack of the assistants' changes (Assistant agent and Live AI), newest last
  static constexpr size_t kAgentUndoMax = 12, kAgentUndoCorpusMax = 3;
  int agentUndoTurn() const { return agentUndos.empty() ? -3 : agentUndos.back().turn; }  // whose change is undone next
  string agentInput;
  bool agentMode = false;               // the composer sends goals to the agent
  uint64_t jobSeq = 0;                  // finished jobs (the agent waits for builds)
  string lastJobError;
  // the literature report: the assistant's tools (add_chart / add_section / write_report / edit_section / export_report)
  // write into the writer's document (wdoc); these remember the last export for the conversation's buttons
  string agentReportPath;   // last exported report
  string agentReportDocx;   // its Word version when one was written
  string agentReportTitle;  // title given at the last export (reused as the file name stem)
  void cmdSaveReport(const string& fmt);  // "docx" | "pdf" | "html", with a file dialog
  string docCitedMarkdown(const string& markdown, int* cites, int* bad);  // [R12] -> [3] with the references added
  int docAppendSection(const string& title, const string& markdown, int* words, int* cites, int* bad);  // returns the section count
  int docAppendFigure(int asset, const string& caption);  // returns the block index
  int agentReportTurn = -1; // conversation turn that exported it
  bool agentAuto() const { return settings.j["agentAuto"].boolean(false); }
  bool liveAuto() const { return settings.j["liveAuto"].boolean(true); }  // the Live session acts without asking (its shield toggles it)
  ai::ToolKind actionKind(const ai::AgentAction& a) const;  // run_commands is a change only when one of its commands changes the project
  void agentRunCommands(const string& tool, const vector<string>& cmds);  // queue application commands for a tool (agentPump finishes it)
  void agentPumpCommands();
  string agentExportPath(const string& given, const string& ext) const;   // where a tool writes a file when the model gives no or a bare name
  string uiStateSummary();              // what the user sees (get_ui_state and the result of run_commands)
  static const char* commandReference();  // list_commands
  static int commandKind(const string& name);  // -1 not allowed for the AI, 0 read/view, 1 changes the project or writes files
  string agentChart(const string& id, const Json& g, ReportBlock& out, double W = 500, double H = 0, const ChartTheme* theme = nullptr);  // "" or an error; W/H in chart units (H 0 = natural height)
  string agentShowChart(const string& id, const Json& g);  // puts a chart into the main area; "" or an error
  string agentReadPapers(const Json& g);
  bool agentExportReport(const Json& g, string& msg);
  bool agentReportExported = false;
  void agentStart(const string& goal);
  void agentPump();
  void agentStop();
  void agentApprove(bool yes, bool all = false);
  void agentUndoChanges();        // takes back the newest change (agentUndos.back())
  void agentWriteStart(const Json& g, const string& tool);  // write_report: prompt the text model
  void agentWriteFinish(const string& text, const string& err);
  string agentReportText(bool full) const;                   // get_report
  void agentRequest();
  void agentExecute(const ai::AgentAction& a);
  void agentFinishTool(const string& tool, bool ok, const string& result);
  void agentFinish(const string& text, bool failed = false);
  string agentRender() const;           // Markdown of the live run (shown in the conversation)
  string agentDescribe(const ai::AgentAction& a) const;
  void agentSnapshot(const string& label, bool withCorpus);
  std::shared_ptr<AiStream> aiRequest(const ai::HttpReq& rq);
  // ---- Live AI (live.cpp): a real-time voice / text session with the Gemini Live API in a light right-side pop-up.
  // The pop-up exists only while the user has it open; the model works the application through the agent's tools.
  struct LiveSession;  // WebSocket + microphone + speaker (defined in live.cpp)
  struct LiveState {
    bool open = false;            // the pop-up is shown
    bool mini = false;            // shown as the small circle (orb) instead of the panel
    bool talk = false;            // the user wants a live talking session (microphone on)
    bool audioMode = false;       // this connection answers with speech (plus transcripts) rather than text
    int phase = 0;                // 0 not connected, 1 connecting, 2 connected
    bool awaiting = false;        // a reply is expected (typed message sent, nothing received yet)
    string error;                 // last connection error
    string input, pendingText;    // composer text; text typed before the connection was ready
    vector<string> pendingResults;  // tool results that wait for a reconnection
    live::Transcript log;
    std::shared_ptr<LiveSession> s;
    std::shared_ptr<AiProbe> probe;        // "Diagnose": key / model / quota checks over REST
    std::deque<live::FunctionCall> queue;  // tool calls waiting to run (one runs at a time, on the UI thread)
    bool toolRunning = false, toolCancelled = false;
    string toolId, toolName;
    string interaction;           // Extended Thinking: "IN_PROGRESS" while it reasons or waits for tools, "IDLE" when done
    bool speaking = false;        // the speaker still plays the model's voice
    bool dropAudio = false;       // the user stopped the speech: ignore audio until the turn ends
    bool allowAll = false;        // change tools run without asking in this session (starts as the Assistant's agentAuto)
    string resumeHandle;          // continues the conversation across reconnections
    bool searchOn = false;        // this connection asked for Google Search grounding
    bool searchRetried = false;   // the setup was refused for search once already: do not loop
    int reconnects = 0, changes = 0;
    double reconnectAt = -1;      // goAway: reconnect once the model is idle
    long long tokens = -1;
    size_t seenVersion = 0;
    bool stick = true;            // transcript follows the newest entry
    float logH = 0;               // transcript content height of the last frame
    bool focusWanted = false;     // put the caret into the composer on the next frame
    uint64_t inputId = 0;         // widget id of the composer (root scope)
    double connectedAt = 0, lastEventT = 0;
    string model;
    // the orb's motion: smoothed sound levels, phases of the living shape, ripples spawned by the voice
    struct OrbAnim {
      double lastT = 0;
      float in = 0, out = 0, energy = 0, hue = -1, spin = 0, breathe = 0, ph[4] = {0, 0.9f, 2.1f, 3.4f}, hover = 0, press = 0;
      float bars[36] = {0};
      float col[3] = {0, 0, 0};       // current colour (eases towards the state colour)
      double lastRipple = 0, clickT = 0;
      struct Ripple { double t0; float r0; float a; };
      vector<Ripple> ripples;
    } orb;
    double miniT0 = -1;             // start of the panel <-> orb transition (-1 none)
    bool miniTo = false;            // transition target: true = towards the orb
    Rect miniFrom;                  // panel rect when the transition started
    // 1.9.2
    float panelX = -1, panelY = -1, orbX = -1, orbY = -1;  // positions chosen by dragging (window coordinates; -1 = the default corner)
    bool dragging = false, dragMoved = false;
    float dragDx = 0, dragDy = 0;
    int turnCalls = 0, nudges = 0;  // tool calls received in the current user turn; retries sent after a "system error" without a tool call
    bool shotUser = false;          // the camera button asked for the picture (look_at_screen otherwise)
    int animFps = 0;                // frame rate the live overlay wants after this frame (0 = nothing moves); see App::frame
    bool dirty = false;             // the overlay changed (new words, status) and wants to be redrawn once
    bool posLoaded = false;         // panel / orb positions read from Settings
    size_t savedEntries = 0;        // transcript entries already copied into the Assistant's conversation
    // 1.9.4: the application's own speech detection (see live::SpeechDetector and Options::clientVad)
    bool clientVad = true;          // this connection sends activityStart / activityEnd itself
    bool vadRetried = false;        // the setup with the markers was refused once: this session runs on the server's detection
    bool hearing = false;           // the detector hears the user talking right now
    bool waitingInput = false;      // the server said the model waits for the user to continue
    bool turnContent = false;       // something (speech, text, a tool call) came back for the current user turn
    bool turnNudged = false;        // the watchdog already asked once for an answer to this turn
    bool turnLife = false;          // a sign of life (an empty serverContent, "IN_PROGRESS") already extended the watchdog once
    bool turnAck = false;           // the server echoed ACTIVITY_END or sent a transcript: the utterance arrived
    bool turnResent = false;        // the recording of the utterance was sent a second time (it had not arrived)
    double turnT0 = 0;              // when the user stopped talking (the watchdog counts from here)
    string turnHeard;               // the server's transcript of the utterance the model has not answered yet (relayed as text by the watchdog)
  } live;
  string liveKey() const;         // the Google Gemini API key from Settings
  string liveModel() const;
  void liveSendUserText(const string& t);  // a typed / synthetic user message, as realtime text or a client turn (see live.cpp)
  void liveUtteranceStart();      // the user started talking (the app's detector or the server's)
  void liveUtteranceEnd();        // ... and stopped: a reply is due; the watchdog starts counting
  void liveOpen(bool talk);       // show the pop-up (and start talking)
  void liveClose();               // hide the pop-up and end the session
  void liveConnect(bool audio);
  void liveDisconnect(bool keepHandle);
  void liveSend(const string& text);
  bool liveStartMic();
  void liveToggleMic();
  void liveInterrupt();           // stop the model's speech
  void livePump();                // every frame: socket events, tool queue, reconnection
  void liveHandle(const live::ServerMessage& m);
  void liveRunNextTool();
  void liveToolDone(const string& tool, bool ok, const string& result);  // from agentFinishTool when agent.live
  void liveSendResult(const string& id, const string& name, bool ok, const string& result);
  void liveApprove(bool yes, bool all);
  void liveDiagnose();            // explains a refused connection: model visibility + a tiny text request (full quota message)
  void liveRequestPicture(bool canvasOnly, bool fromUser);  // a still picture of the canvas / window is taken at the end of this frame
  void liveSendPicture(const vector<uint8_t>& bgra, int w, int h, bool canvasOnly);  // JPEG -> realtimeInput video (called from frame())
  void liveSaveToChat();          // copies the new transcript entries into the Assistant's conversation (saved with the project)
  void liveSavePositions();       // remembers where the panel and the orb were dragged (Settings)
  void liveAnim(int fps) { live.animFps = std::max(live.animFps, fps); }  // the overlay wants to move: fps frames per second, redrawn alone
  string liveCarryContext() const;  // the conversation so far, for a fresh session after a text <-> voice switch
  string liveSystemPrompt() const;
  string liveStatus() const;
  bool liveBusy() const;          // connecting, thinking, speaking or running a tool (script "livewait")
  Rect liveRect() const;          // the panel
  Rect liveOrbRect() const;       // the small circle
  Rect liveHitRect() const;       // whichever is shown (occlusion of the workspace beneath)
  void liveSetMini(bool m);
  void drawLive();
  void drawLiveOrb();
  void drawLiveMorph(float u);    // the panel <-> orb transition, u in [0, 1)
  // build
  string candFilter;
  vector<std::pair<int, int>> sens;
  string sensSig;
  bool advOpen = false, methodOpen = false;
  // analyse
  int anaTab = 0;
  int itemSortCol = 2;
  bool itemSortDesc = true;
  string itemFilter;
  Stability stab;
  bool stabValid = false;
  int stabRuns = 10;
  vector<ClusterInfo> cinfo;
  bool cinfoValid = false;
  // trends
  int trTab = 0;
  int trUnit = 1;
  int burstMin = 3;
  float burstS = 2.0f, burstG = 0.5f;
  vector<Burst> bursts;
  bool burstsValid = false;
  string evoCuts;
  Evolution evo;
  bool evoValid = false;
  int cmpA0 = 0, cmpA1 = 0, cmpB0 = 0, cmpB1 = 0;
  Comparison cmp;
  bool cmpValid = false;
  // ---- 1.8 time and comparison (timeui.cpp)
  // difference map: change of each map item between two periods, shown as an overlay score
  PeriodDiff pdiff;
  bool pdiffValid = false;
  int pdA0 = 0, pdA1 = 0, pdB0 = 0, pdB1 = 0;
  uint64_t pdCorpusVer = 0;
  const Network* pdNet = nullptr;
  int pdNetN = -1, pdList = 0;  // pdList: 0 growing, 1 appearing, 2 fading
  int pdKind = 0, pdFileA = -1, pdFileB = -1;
  string pdTitleA, pdTitleB;
  struct DiffRestore { bool active = false; ColorBy colorBy = ColorBy::Cluster; string scheme; double lo = NAN, hi = NAN; int scoreIdx = -1; ViewKind view{}; } diffRestore;
  void diffCompute();
  void diffShowOnMap();
  double diffScoreInto(Network& N);  // writes the change score column (and selects it); returns the symmetric colour range
  void diffClear();
  void trendsDifference(Lay& L);
  // ---- compare mode (1.10): two periods, two source files or two thresholds, side by side or as a difference map
  int cmpKind = 0;            // 0 periods, 1 sources, 2 thresholds
  int cmpFileA = 0, cmpFileB = 1;
  double cmpThA = 0, cmpThB = 0;   // minimum item weight on each side (0 = the map's own threshold)
  bool cmpSideOpen = false;   // the side-by-side figure is in the main area
  bool compareMasks(vector<char>& inA, vector<char>& inB, string& titleA, string& titleB, string* err);  // kinds 0 and 1
  bool compareSides(vector<double>& a, vector<double>& b, double& nA, double& nB, string& titleA, string& titleB, string* err);
  bool compareShowSideBySide(string* err = nullptr);
  void compareClose();
  string compareSummary();    // one line for tools and the status text
  Scene compareScene(double w, double h, const ChartTheme* th);
  // main path analysis
  MainPath mpath;
  uint64_t mpVer = 0;
  int mpRoutes = 5, mpRoutesDone = -1;
  void trendsMainPath(Lay& L);
  const MainPath& mainPathNow();
  // resolution sweep
  Sweep sweep;
  bool sweepValid = false;
  int sweepSeeds = 4;
  string sweepSig;
  void analyseSweep(Lay& L);
  void runSweep();
  void useResolution(double r);
  // living maps: check the saved OpenAlex search for new papers
  int oaPurpose = 0;  // next cmdOpenAlex: 0 import, 1 check for new papers
  Placement living;
  vector<Record> livingRecs;  // fetched records behind `living`
  bool livingValid = false, livingQuiet = false;
  string livingSince;
  void livingRemember(const string& label);
  void livingCheck(bool quiet);
  void livingDone(vector<Record>& recs);
  void livingAdd(bool rebuild);
  void livingOnOpen();
  void dataLiving(Lay& L);
  int tfL = 6, tfM = 1, tfR = 8, tfTop = 10;
  ThreeField tf;
  bool tfValid = false;
  // actors
  int acTab = 0, acSortCol = 1;
  bool acSortDesc = true;
  vector<Actor> actors;
  int actorsUnit = -1;
  // publish
  int figPreset = 0;
  Scene figPreview;
  string figSig;
  string lastExportDir;
};

extern App* gApp;

}  // namespace win
}  // namespace vs
