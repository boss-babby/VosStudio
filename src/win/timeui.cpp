// VOSStudio Native — 1.8 time and comparison: difference map, main path, resolution sweep, living maps
#include <algorithm>
#include <cmath>

#include "app.h"

namespace vs {
namespace win {

namespace {
string todayIso() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[16];
  snprintf(b, sizeof b, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
  return b;
}
Rect field(Ui& ui, Lay& L, const string& label, float h = 30, float labelFrac = 0.42f) {
  float s = ui.s;
  Rect r = L.row(h * s);
  float lw = r.w * labelFrac;
  ui.text({r.x, r.y, lw - 6 * s, r.h}, label, 12.5f * s, ui.c.textDim);
  return {r.x + lw, r.y, r.w - lw, r.h};
}
string period(int a, int b) { return a == b ? std::to_string(a) : std::to_string(a) + "\xE2\x80\x93" + std::to_string(b); }
string niceDate(const string& iso) {
  static const char* mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (iso.size() < 10) return iso.empty() ? string("never") : iso;
  int y = toInt(iso.substr(0, 4), 0), m = toInt(iso.substr(5, 2), 0), d = toInt(iso.substr(8, 2), 0);
  if (m < 1 || m > 12) return iso;
  return std::to_string(d) + " " + mo[m - 1] + " " + std::to_string(y);
}
}  // namespace

// =====================================================================================
// difference map
// =====================================================================================
void App::diffCompute() {
  if (!cmpA0) suggestPeriods(P->corpus, cmpA0, cmpA1, cmpB0, cmpB1);
  bool same = pdiffValid && pdA0 == cmpA0 && pdA1 == cmpA1 && pdB0 == cmpB0 && pdB1 == cmpB1 && pdCorpusVer == P->corpusVersion && pdNet == &P->net && pdNetN == P->net.n();
  if (same) return;
  pdiff = periodDiff(P->net, P->corpus, cmpA0, cmpA1, cmpB0, cmpB1);
  pdA0 = cmpA0; pdA1 = cmpA1; pdB0 = cmpB0; pdB1 = cmpB1;
  pdCorpusVer = P->corpusVersion;
  pdNet = &P->net;
  pdNetN = P->net.n();
  pdiffValid = true;
}

void App::diffShowOnMap() {
  if (!hasMap()) return;
  diffCompute();
  if (!pdiff.ok) { ui.toast("Difference map", pdiff.error, 2); return; }
  pushUndo("Difference map");
  if (!diffRestore.active) {
    diffRestore.colorBy = P->style.colorBy;
    diffRestore.scheme = P->style.scheme;
    diffRestore.lo = P->style.scoreMin;
    diffRestore.hi = P->style.scoreMax;
    diffRestore.scoreIdx = P->net.scoreIdx;
    diffRestore.view = view;
  }
  double m = diffScoreInto(P->net);
  P->style.scheme = "coolwarm";
  P->style.scoreMin = -m;
  P->style.scoreMax = m;
  P->dirty = true;
  styleDirty = true;
  diffRestore.active = true;
  setView(ViewKind::Overlay);
  string msg = "Red items grew, blue items faded between the two periods";
  if (pdiff.counts[0]) msg += " \xC2\xB7 " + plural(pdiff.counts[0], "item") + " appeared";
  ui.toast("Difference map", msg + ".", 1, 5);
}

double App::diffScoreInto(Network& N) {
  string name = "Change " + period(pdiff.a0, pdiff.a1) + " to " + period(pdiff.b0, pdiff.b1);
  int k = -1;
  for (size_t i = 0; i < N.scoreNames.size(); i++) if (startsWith(N.scoreNames[i], "Change ")) { k = int(i); break; }
  if (k < 0) { k = int(N.scoreNames.size()); N.scoreNames.push_back(name); }
  else N.scoreNames[size_t(k)] = name;
  vector<double> mag;
  for (int i = 0; i < N.n(); i++) {
    auto& sc = N.nodes[size_t(i)].sc;
    if (int(sc.size()) <= k) sc.resize(size_t(k + 1), NAN);
    const ItemChange& c = pdiff.items[size_t(i)];
    double v = c.few ? 0.0 : clampv(c.logRatio, -4.0, 4.0);
    sc[size_t(k)] = v;
    if (!c.few) mag.push_back(std::fabs(v));
  }
  double m = 1.5;
  if (!mag.empty()) { std::sort(mag.begin(), mag.end()); m = clampv(mag[size_t(0.95 * double(mag.size() - 1))], 1.0, 3.0); }
  N.scoreIdx = k;
  return m;
}

void App::diffClear() {
  if (!diffRestore.active) return;
  P->style.colorBy = diffRestore.colorBy;
  P->style.scheme = diffRestore.scheme.empty() ? P->style.scheme : diffRestore.scheme;
  P->style.scoreMin = diffRestore.lo;
  P->style.scoreMax = diffRestore.hi;
  if (diffRestore.scoreIdx < int(P->net.scoreNames.size())) P->net.scoreIdx = diffRestore.scoreIdx;
  diffRestore.active = false;
  styleDirty = true;
  setView(diffRestore.view);
}

void App::trendsDifference(Lay& L) {
  float s = ui.s;
  sectionTitle(L, "Difference map",
               "Colours every item of the current map by how its share of documents changed from period A to period B. Red items grew, blue items faded; grey items stayed about the same. "
               "Growing and fading mean a change of at least 1.5 times; items with fewer than two documents are left neutral.");
  if (!hasMap() || P->mapSource != "analysis") {
    float h = ui.textWrap({L.x, L.y, L.w, 200 * s}, "Build a map from the records to compare its items between the periods.", 12 * s, ui.c.textDim);
    L.y += h + 8 * s;
    return;
  }
  diffCompute();
  if (!pdiff.ok) {
    float h = ui.textWrap({L.x, L.y, L.w, 200 * s}, pdiff.error, 12 * s, ui.c.textDim);
    L.y += h + 8 * s;
    return;
  }
  // counts
  auto c4 = cols(L.row(54 * s), 4, 6 * s);
  const Color cc[4] = {Color::hex(0xb40426), Color::hex(0xdc6e5a), ui.c.textDim, Color::hex(0x3b4cc0)};
  for (int i = 0; i < 4; i++) {
    const Rect& r = c4[size_t(i)];
    ui.fill(r, ui.c.card, 8 * s);
    ui.stroke(r, ui.c.border, 8 * s);
    ui.text({r.x + 8 * s, r.y + 6 * s, r.w - 10 * s, 24 * s}, fmtInt(pdiff.counts[i]), 17 * s, cc[i], AL_LEFT, 700);
    ui.text({r.x + 8 * s, r.y + 32 * s, r.w - 10 * s, 16 * s}, changeLabel(Change(i)), 11 * s, ui.c.textDim);
  }
  {
    auto b2 = cols(L.row(32 * s), 2, 6 * s);
    if (ui.button(b2[0], diffRestore.active ? "Update Map" : "Show on Map", BTN_PRIMARY, "sparkle", !busy())) diffShowOnMap();
    if (ui.button(b2[1], "Restore Colours", BTN_NORMAL, "undo", diffRestore.active)) diffClear();
  }
  ui.text(L.row(18 * s), "A: " + period(pdiff.a0, pdiff.a1) + ", " + plural(pdiff.nA, "document") + " \xC2\xB7 B: " + period(pdiff.b0, pdiff.b1) + ", " + plural(pdiff.nB, "document"), 11.5f * s, ui.c.textDim);
  L.space(4 * s);
  ui.segmented(L.row(28 * s), {"Growing", "Appearing", "Fading"}, pdList, "pdlist");
  vector<int> idx;
  Change want = pdList == 0 ? Change::Growing : pdList == 1 ? Change::Appearing : Change::Fading;
  for (auto& it : pdiff.items) if (it.status == want) idx.push_back(it.node);
  std::sort(idx.begin(), idx.end(), [&](int a, int b) {
    const ItemChange &x = pdiff.items[size_t(a)], &y = pdiff.items[size_t(b)];
    if (want == Change::Appearing) return x.b != y.b ? x.b > y.b : a < b;
    return want == Change::Fading ? x.logRatio < y.logRatio : x.logRatio > y.logRatio;
  });
  if (idx.empty()) ui.text(L.row(22 * s), "None", 12 * s, ui.c.textFaint);
  const Network& N = P->net;
  for (size_t k = 0; k < std::min<size_t>(10, idx.size()); k++) {
    const ItemChange& c = pdiff.items[size_t(idx[k])];
    Rect r = L.row(22 * s);
    if (ui.listRow(r, "pd" + std::to_string(idx[k]), false)) focusOn(idx[k]);
    ui.text({r.x + 6 * s, r.y, r.w - 110 * s, r.h}, truncate(N.nodes[size_t(idx[k])].label, 40), 12 * s, ui.c.text);
    string v = std::to_string(c.a) + " / " + std::to_string(c.b) + (want == Change::Appearing ? string() : "  \xC2\xB7  " + fmtNum(std::pow(2.0, c.logRatio), 1) + "x");
    ui.text({r.x, r.y, r.w - 6 * s, r.h}, v, 11.5f * s, ui.c.textDim, AL_RIGHT);
    uint64_t id = ui.id("pd" + std::to_string(idx[k]));
    ui.tipFor(id, "Documents in A / B" + string(want == Change::Appearing ? "" : ", change of the share"));
  }
}

// =====================================================================================
// main path
// =====================================================================================
const MainPath& App::mainPathNow() {
  const CitationIndex& X = citations();
  if (mpVer != P->corpusVersion || mpRoutesDone != mpRoutes) {
    mpath = mainPath(P->corpus, X, mpRoutes);
    mpVer = P->corpusVersion;
    mpRoutesDone = mpRoutes;
  }
  return mpath;
}

void App::trendsMainPath(Lay& L) {
  float s = ui.s;
  sectionTitle(L, "Main path",
               "The backbone of the citations in this data set: the chain of papers that most of the citation paths run through (search path count, Liu and Lu 2012). "
               "The blue line is the global main path, orange circles are key routes through the most used citations. Click a circle for a preview.");
  const CitationIndex& X = citations();
  if (X.links == 0) {
    float h = ui.textWrap({L.x, L.y, L.w, 200 * s}, "No citations between documents of this data set were found (needs cited references in the records).", 12 * s, ui.c.textDim);
    L.y += h + 8 * s;
    return;
  }
  Rect f = field(ui, L, "Key routes", 30, 0.58f);
  ui.numberInput(f, "mproutes", mpRoutes, 0, 20);
  const MainPath& M = mainPathNow();
  ChartDef d;
  d.title = "Main path";
  d.make = [this](double w, double h, const ChartTheme& t) { return chartMainPath(P->corpus, mainPathNow(), w, h, t); };
  d.onClick = [this](int rec) { openDoc(rec); };
  chartBox(L, "mainpath", d, 380);
  if (!M.ok) return;
  int withCites = 0;
  for (size_t i = 0; i < P->corpus.recs.size() && i < X.citedBy.size(); i++) if (!X.cites[i].empty() || !X.citedBy[i].empty()) withCites++;
  ui.text(L.row(18 * s), plural(M.dagNodes, "connected document") + " \xC2\xB7 " + plural(M.dagLinks, "citation") + " out of " + plural(withCites, "document") + " with references \xC2\xB7 " + std::to_string(M.global.size()) + " on the global main path", 11.5f * s, ui.c.textDim);
  sectionTitle(L, "Papers on the main path", "Oldest first. Click a paper for a preview.");
  for (int rec : M.global) {
    const Record& r = P->corpus.recs[size_t(rec)];
    Rect row = L.row(34 * s);
    if (ui.listRow(row, "mp" + std::to_string(rec), false)) openDoc(rec);
    ui.text({row.x + 6 * s, row.y + 2 * s, 44 * s, 16 * s}, std::to_string(r.year), 12 * s, ui.c.accent, AL_LEFT, 600);
    ui.text({row.x + 52 * s, row.y + 2 * s, row.w - 58 * s, 16 * s}, truncate(r.title, 70), 12 * s, ui.c.text);
    ui.text({row.x + 52 * s, row.y + 18 * s, row.w - 58 * s, 14 * s}, truncate(shortCite(r) + (r.source.empty() ? "" : " \xC2\xB7 " + r.source), 70), 11 * s, ui.c.textDim);
  }
}

// =====================================================================================
// resolution sweep
// =====================================================================================
void App::runSweep() {
  if (!hasMap() || busy()) return;
  auto copy = std::make_shared<Network>(P->net);
  auto out = std::make_shared<Sweep>();
  ClusterOpts co = P->params.clusterOpts();
  int seeds = sweepSeeds;
  string sig = std::to_string(P->net.n()) + ":" + std::to_string(P->net.m()) + ":" + clusterTag(co);
  startJob("Resolution sweep", [copy, out, co, seeds](Job& j) {
    *out = resolutionSweep(*copy, co, defaultSweepResolutions(), seeds, [&j](int d, int t) { j.progress = t ? double(d) / t : 0; return !j.cancel.load(); });
  }, [this, out, sig] {
    if (out->cancelled) return;
    sweep = *out;
    sweepValid = true;
    sweepSig = sig;
    if (sweep.best >= 0) {
      const SweepPoint& b = sweep.pts[size_t(sweep.best)];
      ui.toast("Resolution sweep", "Most reproducible: resolution " + fmtNum(b.resolution, 2) + " with " + plural(b.clusters, "cluster") + " (agreement " + fmtFixed(b.meanAri, 2) + ")", 1, 5);
    }
  });
}

void App::useResolution(double r) {
  if (!hasMap()) return;
  P->params.cluster.resolution = r;
  cmdRecluster();
  stabValid = false;
  sweepSig = std::to_string(P->net.n()) + ":" + std::to_string(P->net.m()) + ":" + clusterTag(P->params.clusterOpts());
}

void App::analyseSweep(Lay& L) {
  float s = ui.s;
  sectionTitle(L, "Resolution sweep",
               "Clusters the map at resolutions from 0.25 to 3, several times each with different seeds. Bars show the number of clusters, the line shows how well the runs agree "
               "(1 means the same clusters every time). A resolution with high agreement is a robust choice. Click a bar to use its resolution.");
  float sv = float(sweepSeeds);
  if (ui.slider(L.row(28 * s), "Seeds", sv, 2, 10, "%.0f", 1)) sweepSeeds = int(sv);
  if (ui.button(L.row(34 * s), "Run Sweep", BTN_PRIMARY, "play", !busy())) runSweep();
  if (!sweepValid) return;
  double cur = P->params.cluster.resolution;
  ChartDef d;
  d.title = "Resolution sweep";
  Sweep sw = sweep;
  d.make = [sw, cur](double w, double h, const ChartTheme& t) { return chartSweep(sw, cur, w, h, t); };
  d.onClick = [this](int i) { if (i >= 0 && i < int(sweep.pts.size())) useResolution(sweep.pts[size_t(i)].resolution); };
  chartBox(L, "sweep", d, 250);
  string sig = std::to_string(P->net.n()) + ":" + std::to_string(P->net.m()) + ":" + clusterTag(P->params.clusterOpts());
  if (sig != sweepSig) ui.text(L.row(18 * s), "The map or its settings changed since the sweep.", 11.5f * s, ui.c.warn);
  if (sweep.best >= 0) {
    const SweepPoint& b = sweep.pts[size_t(sweep.best)];
    string t = "Most reproducible: resolution " + fmtNum(b.resolution, 2) + ", " + plural(b.clusters, "cluster") + ", agreement " + fmtFixed(b.meanAri, 2) + ". Current: " + fmtNum(cur, 2) + ".";
    float h = ui.textWrap({L.x, L.y, L.w, 200 * s}, t, 12 * s, ui.c.text);
    L.y += h + 6 * s;
  }
  auto b2 = cols(L.row(32 * s), 2, 6 * s);
  bool can = sweep.best >= 0 && std::fabs(sweep.pts[size_t(sweep.best)].resolution - cur) > 1e-9;
  if (ui.button(b2[0], sweep.best >= 0 ? "Use " + fmtNum(sweep.pts[size_t(sweep.best)].resolution, 2) : string("Use"), BTN_NORMAL, "check", can && !busy())) useResolution(sweep.pts[size_t(sweep.best)].resolution);
  if (ui.button(b2[1], "Export CSV", BTN_NORMAL, "download")) {
    string p = saveFileDialog(hwnd, "Export resolution sweep", {{"CSV (UTF-8)", "*.csv"}}, "resolution-sweep.csv", "csv");
    if (!p.empty()) {
      if (writeFileU(p, sweepCsv(sweep))) ui.toast("Exported", fileName(p), 1);
      else ui.toast("Export failed", "Could not write " + fileName(p), 3);
    }
  }
}

// =====================================================================================
// living maps
// =====================================================================================
void App::livingRemember(const string& label) {
  Json& J = P->living;
  bool autoCheck = J.t == Json::Obj && J.has("auto") && J["auto"].t == Json::Bool ? J["auto"].b : false;
  Json o = Json::object();
  o.set("query", oaQuery);
  o.set("label", label);
  o.set("kind", oaKind);
  Json sem = Json::array();
  for (auto& q : oaSemQueries) sem.push(q);
  o.set("sem", sem);
  o.set("field", oaField);
  o.set("mode", oaMode);
  o.set("max", clampv(toInt(oaMax, 500), 10, 10000));
  o.set("strategy", oaSemStrategy);
  o.set("strict", oaStrict);
  o.set("lastCheck", todayIso());
  o.set("auto", autoCheck);
  J = o;
}

void App::livingCheck(bool quiet) {
  if (busy()) { if (!quiet) ui.toast("Please wait", "Another task is running.", 2); return; }
  Json& J = P->living;
  if (J.t != Json::Obj || !J.has("query")) { if (!quiet) ui.toast("Updates", "Available for data fetched with Search OpenAlex.", 2); return; }
  // run the saved search from the year of the last check, without disturbing the search form
  string q0 = oaQuery, f0 = oaFrom, t0 = oaTo, m0 = oaMax;
  int k0 = oaKind, fd0 = oaField, md0 = oaMode, st0 = oaSemStrategy, sr0 = oaStrict;
  vector<string> s0 = oaSemQueries;
  oaQuery = J["query"].str();
  oaKind = J["kind"].integer(0);
  oaSemQueries.clear();
  for (auto& x : J["sem"].a) oaSemQueries.push_back(x.str());
  if (oaSemQueries.empty()) oaSemQueries = {""};
  oaField = J["field"].integer(0);
  oaMode = J["mode"].integer(0);
  oaSemStrategy = J["strategy"].integer(1);
  oaStrict = J["strict"].integer(1);
  oaMax = std::to_string(J["max"].integer(500));
  string last = J["lastCheck"].str();
  int fromYear = last.size() >= 4 ? toInt(last.substr(0, 4), 0) : 0;
  if (!fromYear) for (auto& r : P->corpus.recs) fromYear = std::max(fromYear, r.year);
  oaFrom = fromYear ? std::to_string(fromYear) : "";
  oaTo = "";
  livingSince = last;
  livingQuiet = quiet;
  oaPurpose = 1;
  cmdOpenAlex();
  oaPurpose = 0;
  oaQuery = q0; oaFrom = f0; oaTo = t0; oaMax = m0; oaKind = k0; oaField = fd0; oaMode = md0; oaSemStrategy = st0; oaStrict = sr0; oaSemQueries = s0;
}

void App::livingDone(vector<Record>& recs) {
  Network none;
  living = placeNewPapers(hasMap() ? P->net : none, P->spec, P->engine.thesaurus, P->corpus, recs);
  livingRecs = std::move(recs);
  livingValid = true;
  P->living.set("lastCheck", todayIso());
  P->living.set("lastNew", int(living.papers.size()));
  P->dirty = true;
  string since = livingSince.empty() ? string() : " since " + niceDate(livingSince);
  int n = int(living.papers.size());
  if (!n) { ui.toast("No new papers", "Nothing new" + since + ".", 1, 4); return; }
  string where;
  if (hasMap() && !living.perCluster.empty()) {
    int bc = int(std::max_element(living.perCluster.begin(), living.perCluster.end()) - living.perCluster.begin());
    if (living.perCluster[size_t(bc)] > 0) where = " Most fall in \"" + P->net.clusterName(bc) + "\".";
  }
  ui.toast(plural(n, "new paper") + since, "See Data \xC2\xB7 Updates to review and add them." + where, 1, 7);
  if (!livingQuiet) page = PG_DATA;
}

void App::livingAdd(bool rebuild) {
  if (!livingValid || living.papers.empty() || busy()) return;
  vector<Record> v;
  for (auto& p : living.papers) if (p.rec >= 0 && size_t(p.rec) < livingRecs.size()) v.push_back(livingRecs[size_t(p.rec)]);
  int n = P->addRecords("OpenAlex update " + todayIso(), BibFormat::OpenAlex, v);
  livingValid = false;
  livingRecs.clear();
  living = Placement();
  pageStateReset();
  if (rebuild && hasMap()) cmdBuild();
  else ui.toast("Records added", plural(n, "new record") + " added. Rebuild the map to include them.", 1, 4);
}

void App::livingOnOpen() {
  const Json& J = P->living;
  if (J.t != Json::Obj || !J.has("query") || !J.has("auto") || J["auto"].t != Json::Bool || !J["auto"].b) return;
  if (J["lastCheck"].str() == todayIso()) return;
  livingCheck(true);
}

void App::dataLiving(Lay& L) {
  float s = ui.s;
  Json& J = P->living;
  sectionTitle(L, "Updates",
               "Runs the OpenAlex search this data came from again and looks for papers published since the last check. It shows which clusters of the map the new papers fall into and "
               "which keywords are new. Nothing is added until you choose to.");
  if (J.t != Json::Obj || !J.has("query")) {
    ui.text(L.row(20 * s), "Available for data fetched with Search OpenAlex.", 12 * s, ui.c.textDim);
    return;
  }
  {
    float rh = 28 * s;
    Rect box = L.row(rh * 2);
    ui.fill(box, ui.c.card, 8 * s);
    ui.stroke(box, ui.c.border, 8 * s);
    string lab = J["label"].str();
    if (lab.empty()) lab = J["query"].str();
    std::pair<string, string> rows[2] = {{"Search", truncate(replaceAll(lab, "\n", "; "), 40)}, {"Last checked", niceDate(J["lastCheck"].str())}};
    for (int i = 0; i < 2; i++) {
      Rect r{box.x + 12 * s, box.y + rh * float(i), box.w - 24 * s, rh};
      if (i) ui.line(r.x, r.y + 0.5f, box.r(), r.y + 0.5f, ui.c.border);
      ui.text({r.x, r.y, r.w * 0.4f, r.h}, rows[i].first, 12.5f * s, ui.c.text);
      ui.text({r.x + r.w * 0.3f, r.y, r.w * 0.7f, r.h}, rows[i].second, 12.5f * s, ui.c.textDim, AL_RIGHT);
    }
  }
  bool autoCheck = J.has("auto") && J["auto"].t == Json::Bool && J["auto"].b;
  if (ui.toggle(L.row(28 * s), "Check when the project opens", autoCheck)) { J.set("auto", autoCheck); P->dirty = true; }
  if (ui.button(L.row(32 * s), "Check for New Papers", BTN_PRIMARY, "download", !busy())) livingCheck(false);
  if (!livingValid) return;
  int n = int(living.papers.size());
  L.space(4 * s);
  string head = plural(n, "new paper") + (livingSince.empty() ? string() : " since " + niceDate(livingSince));
  if (living.known) head += " \xC2\xB7 " + fmtInt(living.known) + " already in the data";
  ui.text(L.row(20 * s), head, 12.5f * s, ui.c.text, AL_LEFT, 600);
  if (!n) return;
  if (hasMap() && !living.perCluster.empty()) {
    vector<int> ord;
    for (size_t c = 0; c < living.perCluster.size(); c++) if (living.perCluster[c] > 0) ord.push_back(int(c));
    std::sort(ord.begin(), ord.end(), [&](int a, int b) { return living.perCluster[size_t(a)] > living.perCluster[size_t(b)]; });
    auto colors = clusterColors();
    int mx = ord.empty() ? 1 : living.perCluster[size_t(ord[0])];
    for (size_t k = 0; k < std::min<size_t>(8, ord.size()); k++) {
      int c = ord[k];
      Rect r = L.row(22 * s);
      if (ui.listRow(r, "lvc" + std::to_string(c), false)) focusCluster(c);
      Color col = colors.empty() ? ui.c.accent : colors[size_t(c) % colors.size()];
      ui.fill({r.x + 6 * s, r.y + 7 * s, 8 * s, 8 * s}, col, 4 * s);
      ui.text({r.x + 20 * s, r.y, r.w * 0.55f, r.h}, truncate(P->net.clusterName(c), 28), 12 * s, ui.c.text);
      Rect bar{r.x + r.w * 0.62f, r.y + 9 * s, r.w * 0.26f * float(living.perCluster[size_t(c)]) / float(mx), 4 * s};
      ui.fill(bar, col.withA(0.8f), 2 * s);
      ui.text({r.x, r.y, r.w - 6 * s, r.h}, fmtInt(living.perCluster[size_t(c)]), 11.5f * s, ui.c.textDim, AL_RIGHT);
    }
    if (living.unplaced) ui.text(L.row(18 * s), fmtInt(living.unplaced) + " not linked to any item of the map", 11.5f * s, ui.c.textDim);
  }
  if (!living.emerging.empty()) {
    string e;
    for (size_t k = 0; k < std::min<size_t>(8, living.emerging.size()); k++) e += (k ? ", " : "") + living.emerging[k].first + " (" + std::to_string(living.emerging[k].second) + ")";
    L.space(2 * s);
    ui.text(L.row(18 * s), "New keywords", 11.5f * s, ui.c.textDim, AL_LEFT, 600);
    float h = ui.textWrap({L.x, L.y, L.w, 200 * s}, e, 12 * s, ui.c.text);
    L.y += h + 6 * s;
  }
  for (size_t k = 0; k < std::min<size_t>(5, living.papers.size()); k++) {
    const Record& r = livingRecs[size_t(living.papers[k].rec)];
    Rect row = L.row(34 * s);
    ui.text({row.x, row.y + 2 * s, row.w, 16 * s}, truncate(r.title, 64), 12 * s, ui.c.text);
    ui.text({row.x, row.y + 18 * s, row.w, 14 * s}, truncate(shortCite(r) + (r.source.empty() ? "" : " \xC2\xB7 " + r.source), 64), 11 * s, ui.c.textDim);
  }
  if (living.papers.size() > 5) ui.text(L.row(18 * s), "and " + fmtInt(long(living.papers.size() - 5)) + " more", 11.5f * s, ui.c.textFaint);
  auto b2 = cols(L.row(32 * s), 2, 6 * s);
  if (ui.button(b2[0], "Add to Data", BTN_NORMAL, "", !busy())) livingAdd(false);
  if (ui.button(b2[1], "Add and Rebuild", BTN_PRIMARY, "", !busy() && hasMap())) livingAdd(true);
}

}  // namespace win
}  // namespace vs
