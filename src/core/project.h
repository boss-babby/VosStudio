// VOSStudio Native — the document: corpus + analysis spec + map + look, build pipeline,
// project bundles (self-contained JSON with a SHA-256 manifest) and undo snapshots.
#pragma once
#include <atomic>

#include "analysis.h"
#include "json.h"
#include "figure.h"
#include "stats.h"
#include "charts.h"

namespace vs {

struct BuildParams {
  Norm norm = Norm::Association;
  LayoutOpts layout;
  ClusterOpts cluster;
  bool overlap = true, autoName = true;
  // 0 = as VOSviewer (VOS quality; modularity for LinLog normalisation), 1 = modularity (VOSStudio 1.5 and earlier)
  int clusterMethod = 0;
  ClusterOpts clusterOpts() const {
    ClusterOpts c = cluster;
    c.vosQuality = clusterMethod == 0 && norm != Norm::LinLog;
    return c;
  }
};

struct BuildReport { double ms = 0, rawMs = 0, layoutMs = 0, clusterMs = 0, V = 0, Q = 0; int n = 0, m = 0, clusters = 0; };

struct Snapshot {
  string what;
  vector<std::array<double, 3>> pos;
  vector<int> cluster;
  vector<string> names;
  std::map<int, Color> overrides;
};

// A map kept in the project's map history (the switcher next to the file menu)
struct SavedMap { Network net; AnaSpec spec; BuildParams params; string mapSource, title, when; };

string recordsDump(const vector<Record>& recs);

class Project {
 public:
  Corpus corpus;
  AnalysisEngine engine;
  AnaSpec spec;
  BuildParams params;
  Network net;
  Bundles bundles;
  ViewStyle style;
  FigureSpec fig;
  string path;        // last saved/opened .vosproj
  string mapSource;   // "analysis", "vosviewer", "bundle"
  bool dirty = false;
  BuildReport last;
  // map history saved with the project; the entry at mapCur is the current map (net/spec/params above)
  vector<SavedMap> maps;
  int mapCur = -1;
  // living map (1.8): the OpenAlex search the data came from, when it was last checked for new papers and whether to
  // check when the project opens: {query, kind, sem[], field, mode, max, strategy, strict, lastCheck, auto, lastNew}
  Json living = Json::object();
  Json assistant = Json::array();  // AI assistant conversation (ai::Turn list), saved with the project
  ItemMetrics metrics;
  bool metricsValid = false;

  Project() { engine.corpus = &corpus; }
  Project(const Project&) = delete;
  Project& operator=(const Project&) = delete;

  // data
  int addFile(const string& name, const string& text, string* err = nullptr);  // returns records added
  int addSample(bool scopus);
  int addRecords(const string& name, BibFormat f, vector<Record>& recs);  // moves recs in, dedups
  void clearCorpus();
  void corpusChanged();  // invalidate caches after cleaning / thesaurus edits
  uint64_t corpusVersion = 0;  // changes whenever the corpus or its cleaning changes (unique across projects)

  // pipeline
  BuildReport build(Progress prog = nullptr, const std::atomic<bool>* cancel = nullptr);  // throws runtime_error
  // two-phase build for background threads: compute touches only the engine caches + a local network
  Network computeNetwork(BuildReport& R, Progress prog = nullptr, const std::atomic<bool>* cancel = nullptr);
  void commitBuild(Network&& nn, BuildReport R);
  void relayout(Progress prog = nullptr, const std::atomic<bool>* cancel = nullptr);
  void recluster();
  void finishPositions();  // world scale + overlap removal
  bool loadVosviewer(const string& mapText, const string& netText, string* err);
  void computeBundles(Progress prog = nullptr);
  const ItemMetrics& itemMetricsCached();

  // undo
  Snapshot snapshot(const string& what) const;
  void restore(const Snapshot& s);

  // bundles
  Json toJson(bool includeRecords = true, string* recDumpOut = nullptr) const;
  bool fromJson(const Json& j, string* err, vector<Record>* preloaded = nullptr);
  bool save(const string& file, string* err = nullptr);
  bool open(const string& file, string* err = nullptr);
  string methodsShort() const;
  string methods() const;
  // PRISMA-style counts for Data → Records flow (uses the engine caches: not while a build runs)
  RecordsFlow recordsFlow(bool withItems = true);
  string builtSig;           // specSig() of the analysis the current map was built from ("" = not from this analysis)
  string specSig();          // analysis settings + cleaning that determine the map's items
};

// thread-safe parse (no project state): detects the format and parses records
int parseBibText(const string& name, const string& text, BibFormat& f, vector<Record>& recs, string* err);
Json styleToJson(const ViewStyle& s);
void styleFromJson(ViewStyle& s, const Json& j);
Json recordToJson(const Record& r);
Record recordFromJson(const Json& j);
string normId(Norm n);
bool normFromId(const string& s, Norm& n);

}  // namespace vs
