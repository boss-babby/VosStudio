// VOSStudio Native — application shell (window, canvas interaction, pages, commands)
#pragma once
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "../core/ai.h"
#include <optional>
#include <deque>
#include <memory>

#include "../core/charts.h"
#include "../core/report.h"
#include "../core/project.h"
#include "ui.h"

namespace vs {
namespace win {

inline string pluralWord(const string& w) {
  if (w.size() > 1 && w.back() == 'y' && !strchr("aeiou", w[w.size() - 2])) return w.substr(0, w.size() - 1) + "ies";  // country -> countries, key -> keys
  return w + "s";
}
inline string plural(long long n, const string& w) { return fmtInt(n) + " " + (n == 1 ? w : pluralWord(w)); }

enum Page { PG_NONE = -1, PG_DATA = 0, PG_BUILD, PG_LOOK, PG_ANALYSE, PG_TRENDS, PG_ACTORS, PG_PUBLISH, PG_AI, PG_COUNT };

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

struct Command { string id, label, hint, shortcut, icon; std::function<void()> run; };
class App;
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
  bool needFrame = true;
  bool occluded = false;  // last Present reported the window fully covered
  void pumpWork();
  double lastFrameT = 0, fps = 0;
  int frameNo = 0;
  string gpuError;

  // layout
  int page = PG_DATA;
  bool inspectorOpen = true;
  float leftW = 344, rightW = 300;
  Rect canvasR, leftR, rightR, topR, statusR, railR;
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
  void drawRail();
  void drawLeft();
  void drawCanvas();
  void drawCanvasChrome();
  void drawInspector();
  void drawStatus();
  void drawWelcome();
  void drawStart();
  bool showStart = false;  // start window (recent projects, New / Open / Sample)
  void drawPalette();
  void drawMainChart(const Rect& r);
  void handleShortcuts();
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
  void cmdBuild();
  void cmdRelayout();
  void cmdRecluster();
  void cmdBundles();
  void cmdSaveProject(bool saveAs);
  void cmdOpenProject(const string& path = "");
  void cmdNewProject();
  void cmdExportFigure(const string& fmt, const string& path = "");
  void cmdExportView(const string& path = "");
  void cmdCopyFigure();
  void cmdExportChart(const ChartDef& def, const string& fmt);
  void cmdExportChartsPdf(const vector<ChartDef>& defs, const string& path0 = "");  // one chart per page
  struct ChartCacheEntry { float w = 0, h = 0; bool dark = false; uint64_t sig = 0; int frame = 0; Scene sc; };
  std::unordered_map<string, ChartCacheEntry> chartCache;  // chart scenes reused while nothing but the mouse position changes
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
    if (cstats.ver != P->corpusVersion) { cstats = CorpusStats(); cstats.ver = P->corpusVersion; }
    auto& o = cstats.*slot;
    if (!o) o = f();
    return *o;
  }
  const QualityReport& statQuality() { return memo(&CorpusStats::quality, [&] { return qualityReport(P->corpus); }); }
  const Growth& statGrowth() { return memo(&CorpusStats::growth, [&] { return growth(P->corpus); }); }
  const Bradford& statBradford() { return memo(&CorpusStats::bradford, [&] { return bradford(P->corpus); }); }
  const Lotka& statLotka() { return memo(&CorpusStats::lotka, [&] { return lotka(P->corpus); }); }
  const CitationSummary& statCiteSummary() { return memo(&CorpusStats::citeSummary, [&] { return citationSummary(P->corpus); }); }
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
  void geoLayer(const Theme& th, vector<GeoFill>& fills, vector<GeoBubble>& bubbles, vector<GeoArc>& arcs) const;
  void geoPaint(ID2D1DeviceContext* dc);
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
  int setTab = 0;                       // 0 General, 1 OpenAlex, 2 AI
  int setAiProv = 0;
  string setAiKey[ai::kProviders], setAiModel[ai::kProviders], setAiBase;
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
  struct AgentUndo {
    bool valid = false;
    AnaSpec spec; BuildParams params; Thesaurus th; std::map<string, std::set<string>> excluded;
    Network net; Bundles bundles; string mapSource, builtSig; BuildReport last; bool clusterNames = false;
    int turn = -1;  // conversation turn of the run that made the changes
    bool hasCorpus = false; Corpus corpus;  // records before the agent's first OpenAlex search
  };
  struct AgentRun {
    bool active = false, waitApproval = false, waitJob = false, allowAll = false, stopping = false;
    string goal, final, jobTool;
    vector<ai::Msg> msgs;
    int steps = 0, turn = -1, changes = 0, parseFails = 0;
    ai::AgentAction pending;
    std::shared_ptr<AiStream> req;
    vector<AgentStepView> log;
    uint64_t jobSeq = 0;
  } agent;
  AgentUndo agentUndo;
  string agentInput;
  bool agentMode = false;               // the composer sends goals to the agent
  uint64_t jobSeq = 0;                  // finished jobs (the agent waits for builds)
  string lastJobError;
  // literature report assembled by the agent (add_chart / add_section / export_report)
  ReportDoc agentReport;
  string agentReportPath;   // last exported report
  int agentReportTurn = -1; // conversation turn that exported it
  bool agentAuto() const { return settings.j["agentAuto"].boolean(false); }
  string agentChart(const string& id, const Json& g, ReportBlock& out);  // "" or an error
  string agentReadPapers(const Json& g);
  bool agentExportReport(const Json& g, string& msg);
  bool agentReportExported = false;
  void agentStart(const string& goal);
  void agentPump();
  void agentStop();
  void agentApprove(bool yes, bool all = false);
  void agentUndoChanges();
  void agentRequest();
  void agentExecute(const ai::AgentAction& a);
  void agentFinishTool(const string& tool, bool ok, const string& result);
  void agentFinish(const string& text, bool failed = false);
  string agentRender() const;           // Markdown of the live run (shown in the conversation)
  string agentDescribe(const ai::AgentAction& a) const;
  void agentSnapshot();
  std::shared_ptr<AiStream> aiRequest(const ai::HttpReq& rq);
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
  struct DiffRestore { bool active = false; ColorBy colorBy = ColorBy::Cluster; string scheme; double lo = NAN, hi = NAN; int scoreIdx = -1; ViewKind view{}; } diffRestore;
  void diffCompute();
  void diffShowOnMap();
  double diffScoreInto(Network& N);  // writes the change score column (and selects it); returns the symmetric colour range
  void diffClear();
  void trendsDifference(Lay& L);
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
