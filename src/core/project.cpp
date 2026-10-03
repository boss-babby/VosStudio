#include "project.h"
#include <atomic>

#include <stdexcept>

#include "records.h"

namespace vs {

string normId(Norm n) {
  switch (n) {
    case Norm::Association: return "association";
    case Norm::Fractionalization: return "fractionalization";
    case Norm::LinLog: return "linlog";
    case Norm::None: return "none";
  }
  return "association";
}
bool normFromId(const string& s, Norm& n) {
  if (s == "association") n = Norm::Association;
  else if (s == "fractionalization") n = Norm::Fractionalization;
  else if (s == "linlog") n = Norm::LinLog;
  else if (s == "none") n = Norm::None;
  else return false;
  return true;
}

int Project::addFile(const string& name, const string& text, string* err) {
  BibFormat f = detectFormat(text, name);
  if (f == BibFormat::VOSviewer || f == BibFormat::Bundle) { if (err) *err = "This is a map/project file, not a bibliographic export."; return 0; }
  if (f == BibFormat::Unknown) { if (err) *err = "Unrecognised file format. Supported: Web of Science (plain text / tab-delimited), Scopus CSV, RIS, BibTeX, OpenAlex JSON."; return 0; }
  vector<Record> recs;
  int n = 0;
  if (f == BibFormat::OpenAlex) { string e; Json j = Json::parse(text, &e); if (!e.empty()) { if (err) *err = "JSON error: " + e; return 0; } n = parseOpenAlex(j, recs); }
  else n = parseRecords(text, f, recs, err);
  if (n <= 0) { if (err && err->empty()) *err = "No records found in " + fileName(name) + "."; return 0; }
  return addRecords(name, f, recs);
}

int Project::addRecords(const string& name, BibFormat f, vector<Record>& recs) {
  int n = int(recs.size());
  size_t before = corpus.recs.size();
  uint64_t bit = Corpus::fileBit(corpus.files.size());
  for (auto& r : recs) r.src = bit;
  std::unordered_set<string> existingIds;
  existingIds.reserve(corpus.recs.size());
  for (const auto& r : corpus.recs) if (!r.id.empty()) existingIds.insert(r.id);
  lastRecordIdConflicts = 0;
  std::unordered_set<string> incomingIds = existingIds;
  for (const Record& r : recs) if (!r.id.empty() && !incomingIds.insert(r.id).second) lastRecordIdConflicts++;
  ensureRecordIds(recs, existingIds);
  corpus.recs.insert(corpus.recs.end(), std::make_move_iterator(recs.begin()), std::make_move_iterator(recs.end()));
  recs.clear();
  SourceFile sf;
  sf.name = fileName(name);
  sf.format = f;
  sf.records = n;
  corpus.files.push_back(sf);
  corpus.format = corpus.files.size() > 1 && corpus.files.front().format != f ? BibFormat::Unknown : f;
  if (corpus.files.size() == 1) corpus.format = f;
  corpusChanged();
  dirty = true;
  return int(corpus.recs.size() - before);
}

int parseBibText(const string& name, const string& text, BibFormat& f, vector<Record>& recs, string* err) {
  f = detectFormat(text, name);
  if (f == BibFormat::VOSviewer || f == BibFormat::Bundle) { if (err) *err = "This is a map/project file, not a bibliographic export."; return 0; }
  if (f == BibFormat::Unknown) { if (err) *err = "Unrecognised file format. Supported: Web of Science (plain text / tab-delimited), Scopus CSV, RIS, BibTeX, OpenAlex JSON."; return 0; }
  int n = 0;
  if (f == BibFormat::OpenAlex) { string e; Json j = Json::parse(text, &e); if (!e.empty()) { if (err) *err = "JSON error: " + e; return 0; } n = parseOpenAlex(j, recs); }
  else n = parseRecords(text, f, recs, err);
  if (n <= 0 && err && err->empty()) *err = "No records found in " + fileName(name) + ".";
  return n;
}

int Project::addSample(bool scopus) {
  string t = scopus ? sampleScopusExport() : sampleWoSExport();
  return addFile(scopus ? "sample-scopus.csv" : "sample-wos.txt", t);
}

void Project::clearCorpus() { corpus.clear(); living = Json::object(); lastRecordIdConflicts = 0; corpusChanged(); }

int Project::removeFile(int k) {
  if (k < 0 || k >= int(corpus.files.size()) || k >= 63 || !corpus.provenanceKnown()) return -1;
  uint64_t bit = Corpus::fileBit(size_t(k));
  vector<Record> keep;
  keep.reserve(corpus.recs.size());
  vector<int> map(corpus.recs.size(), -1);
  int removed = 0;
  for (size_t i = 0; i < corpus.recs.size(); i++) {
    Record& r = corpus.recs[i];
    uint64_t m = r.src & ~bit;
    if (m == 0) { removed++; continue; }
    m = (m & (bit - 1)) | ((m >> (k + 1)) << k);  // the files after k move down one slot
    r.src = m;
    map[i] = int(keep.size());
    keep.push_back(std::move(r));
  }
  corpus.recs.swap(keep);
  corpus.files.erase(corpus.files.begin() + k);
  long long parsed = 0;
  for (auto& f : corpus.files) parsed += f.records;
  corpus.duplicatesRemoved = int(std::max(0ll, parsed - (long long)corpus.recs.size()));
  if (corpus.files.empty()) corpus.format = BibFormat::Unknown;
  else {
    corpus.format = corpus.files.front().format;
    for (auto& f : corpus.files) if (f.format != corpus.format) corpus.format = BibFormat::Unknown;
  }
  auto fix = [&](Network& N) {
    for (auto& nd : N.nodes) {
      vector<int> nr;
      for (int ri : nd.recs) if (ri >= 0 && ri < int(map.size()) && map[size_t(ri)] >= 0) nr.push_back(map[size_t(ri)]);
      nd.recs.swap(nr);
    }
  };
  fix(net);
  for (auto& m : maps) fix(m.net);
  corpusChanged();
  dirty = true;
  return removed;
}

void Project::corpusChanged() {
  static std::atomic<uint64_t> counter{0};
  corpusVersion = ++counter;
  engine.corpus = &corpus;
  engine.invalidate();
  metricsValid = false;
}

void Project::finishPositions() {
  worldScale(net);
  if (params.overlap && net.n() > 1) {
    Encoder e;
    e.prepare(net, style);
    vector<double> r(static_cast<size_t>(net.n()));
    if (e.vos()) {
      // constant-pixel circles: convert to world units at the zoom that fits the map into a typical
      // canvas (~1100x720 px, 40 px margin), so dense cores are pulled apart as they appear by default
      double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
      for (auto& nd : net.nodes) { x0 = std::min(x0, nd.x); x1 = std::max(x1, nd.x); y0 = std::min(y0, nd.y); y1 = std::max(y1, nd.y); }
      double ppw = std::min(1020 / std::max(1e-9, x1 - x0), 640 / std::max(1e-9, y1 - y0));
      for (int i = 0; i < net.n(); i++) r[i] = e.vosRadiusPx(i) / std::max(1e-9, ppw) * 0.7;
    } else
      for (int i = 0; i < net.n(); i++) r[i] = e.radius(i) * 1.05;
    removeOverlap(net, r, 40);
  }
}

Network Project::computeNetwork(BuildReport& R, Progress prog, const std::atomic<bool>* cancel) {
  if (corpus.empty()) throw std::runtime_error("Load a bibliographic file first (or open the sample data).");
  Timer t0;
  R = BuildReport();
  if (prog) prog(0.02);
  Network nn = engine.build(spec);  // throws with a readable reason
  R.rawMs = t0.ms();
  if (cancel && *cancel) throw std::runtime_error("Cancelled.");
  normalise(nn, params.norm);
  Timer tl;
  R.V = vosLayout(nn, params.layout, [&](double p) { if (prog) prog(0.1 + 0.75 * p); }, cancel);
  R.layoutMs = tl.ms();
  if (cancel && *cancel) throw std::runtime_error("Cancelled.");
  Timer tc;
  R.Q = leiden(nn, params.clusterOpts());
  R.clusterMs = tc.ms();
  if (prog) prog(0.95);
  R.ms = t0.ms();
  return nn;
}

void Project::commitBuild(Network&& nn, BuildReport R) {
  Timer t0;
  std::swap(net, nn);
  bundles = Bundles();
  metricsValid = false;
  finishPositions();
  if (params.autoName) applyAutoNames(net, &corpus);
  style.clusterOverride.clear();
  mapSource = "analysis";
  builtSig = specSig();
  R.n = net.n();
  R.m = net.m();
  R.clusters = net.nClusters;
  R.ms += t0.ms();
  last = R;
  dirty = true;
}

BuildReport Project::build(Progress prog, const std::atomic<bool>* cancel) {
  BuildReport R;
  Network nn = computeNetwork(R, prog, cancel);
  commitBuild(std::move(nn), R);
  if (prog) prog(1);
  return last;
}

void Project::relayout(Progress prog, const std::atomic<bool>* cancel) {
  if (!net.n()) return;
  normalise(net, params.norm);
  Timer tl;
  last.V = vosLayout(net, params.layout, prog, cancel);
  last.layoutMs = tl.ms();
  finishPositions();
  bundles = Bundles();
  dirty = true;
}

void Project::recluster() {
  if (!net.n()) return;
  normalise(net, params.norm);
  Timer tc;
  last.Q = leiden(net, params.clusterOpts());
  last.clusterMs = tc.ms();
  last.clusters = net.nClusters;
  if (params.autoName) applyAutoNames(net, corpus.empty() ? nullptr : &corpus);
  else net.clusterNames.clear();
  style.clusterOverride.clear();
  metricsValid = false;
  dirty = true;
}

bool Project::loadVosviewer(const string& mapText, const string& netText, string* err) {
  Network nn;
  if (!parseVosviewer(mapText, netText, nn, err)) return false;
  bool hasPos = false;
  for (auto& nd : nn.nodes) if (nd.x != 0 || nd.y != 0) { hasPos = true; break; }
  bool hasCl = false;
  for (auto& nd : nn.nodes) if (nd.cluster > 0) { hasCl = true; break; }
  std::swap(net, nn);
  normalise(net, params.norm);
  if (!hasPos) vosLayout(net, params.layout);
  if (!hasCl) leiden(net, params.clusterOpts());
  else { net.countClusters(); net.clusterTag.clear(); }
  if (hasPos) worldScale(net);
  else finishPositions();
  bundles = Bundles();
  metricsValid = false;
  mapSource = "vosviewer";
  last = BuildReport();
  last.n = net.n(); last.m = net.m(); last.clusters = net.nClusters; last.Q = net.quality;
  dirty = true;
  return true;
}

void Project::computeBundles(Progress prog) { bundles = fdeb(net, 0.6, 5, 60, prog); }

const ItemMetrics& Project::itemMetricsCached() {
  if (!metricsValid) { metrics = itemMetrics(net); metricsValid = true; }
  return metrics;
}

Snapshot Project::snapshot(const string& what) const {
  Snapshot s;
  s.what = what;
  for (auto& nd : net.nodes) { s.pos.push_back({nd.x, nd.y, nd.z}); s.cluster.push_back(nd.cluster); }
  s.names = net.clusterNames;
  s.overrides = style.clusterOverride;
  return s;
}

void Project::restore(const Snapshot& s) {
  if (s.pos.size() != net.nodes.size()) return;
  for (size_t i = 0; i < net.nodes.size(); i++) { net.nodes[i].x = s.pos[i][0]; net.nodes[i].y = s.pos[i][1]; net.nodes[i].z = s.pos[i][2]; net.nodes[i].cluster = s.cluster[i]; }
  net.clusterNames = s.names;
  style.clusterOverride = s.overrides;
  net.countClusters();
  bundles = Bundles();
  metricsValid = false;
  dirty = true;
}

// ------------------------------------------------------------ serialisation
static Json strArr(const vector<string>& v) { Json a = Json::array(); for (auto& s : v) a.push(s); return a; }
static vector<string> arrStr(const Json& j) { vector<string> v; for (auto& x : j.a) v.push_back(x.str()); return v; }

Json recordToJson(const Record& r) {
  Json j = Json::object();
  if (!r.id.empty()) j.set("rid", r.id);
  j.set("ti", r.title);
  if (!r.abstract_.empty()) j.set("ab", r.abstract_);
  j.set("so", r.source);
  if (!r.doi.empty()) j.set("doi", r.doi);
  if (!r.url.empty()) j.set("url", r.url);
  if (!r.volume.empty()) j.set("vl", r.volume);
  if (!r.issue.empty()) j.set("is", r.issue);
  if (!r.pages.empty()) j.set("pg", r.pages);
  if (!r.docType.empty()) j.set("dt", r.docType);
  if (!r.publisher.empty()) j.set("pu", r.publisher);
  if (!r.language.empty()) j.set("la", r.language);
  if (!r.key.empty()) j.set("sourceKey", r.key);
  if (!r.extra.empty()) { Json ex = Json::object(); for (const auto& kv : r.extra) ex.set(kv.first, kv.second); j.set("extra", ex); }
  if (r.duplicateReviewed) j.set("duplicateReviewed", true);
  j.set("py", r.year);
  j.set("tc", r.cites);
  j.set("au", strArr(r.authors));
  if (!r.authorIds.empty()) j.set("ai", strArr(r.authorIds));
  if (!r.keywords.empty()) j.set("de", strArr(r.keywords));
  if (!r.indexTerms.empty()) j.set("id", strArr(r.indexTerms));
  if (!r.affiliations.empty()) j.set("c1", strArr(r.affiliations));
  if (!r.countries.empty()) j.set("cu", strArr(r.countries));
  if (!r.refs.empty()) j.set("cr", strArr(r.refs));
  if (r.src) { Json sf = Json::array(); for (int k = 0; k < 64; k++) if (r.src & (1ull << k)) sf.push(k); j.set("sf", sf); }
  return j;
}

Record recordFromJson(const Json& j) {
  Record r;
  r.id = j["rid"].str();
  r.title = j["ti"].str(); r.abstract_ = j["ab"].str(); r.source = j["so"].str(); r.doi = j["doi"].str(); r.url = j["url"].str(); r.volume = j["vl"].str(); r.issue = j["is"].str();
  r.pages = j["pg"].str(); r.docType = j["dt"].str(); r.publisher = j["pu"].str(); r.language = j["la"].str(); r.key = j["sourceKey"].str();
  const Json& ex = j["extra"]; if (ex.t == Json::Obj) for (const auto& kv : ex.o) r.extra[kv.first] = kv.second.str();
  r.duplicateReviewed = j["duplicateReviewed"].boolean(false);
  r.year = j["py"].integer(); r.cites = j["tc"].integer();
  r.authors = arrStr(j["au"]); r.authorIds = arrStr(j["ai"]); if (r.authorIds.size() != r.authors.size()) r.authorIds.clear(); r.keywords = arrStr(j["de"]); r.indexTerms = arrStr(j["id"]); r.affiliations = arrStr(j["c1"]);
  r.countries = arrStr(j["cu"]); r.refs = arrStr(j["cr"]);
  const Json& sf = j["sf"];
  if (sf.t == Json::Arr) for (size_t i = 0; i < sf.size(); i++) { int k = sf[i].integer(); if (k >= 0 && k < 64) r.src |= 1ull << k; }
  return r;
}


Json styleToJson(const ViewStyle& S) {
  Json st = Json::object();
  st.set("look", S.look); st.set("palette", S.palette); st.set("flat", S.flat); st.set("nodeOpacity", S.nodeOpacity);
  st.set("border", S.border); st.set("borderCol", int(S.borderCol)); st.set("baseSize", S.baseSize); st.set("maxSize", S.maxSize);
  st.set("scale", S.scale); st.set("labelVar", S.labelVar); st.set("colorBy", int(S.colorBy)); st.set("scheme", S.scheme);
  st.set("labelPlace", int(S.labelPlace)); st.set("labelWeight", S.labelWeight); st.set("labelSize", S.labelSize); st.set("maxLen", S.maxLen);
  st.set("labelHalo", S.labelHalo); st.set("labelByCluster", S.labelByCluster); st.set("linksVisible", S.linksVisible);
  st.set("linkColor", int(S.linkColor)); st.set("linkOpacity", S.linkOpacity); st.set("curvature", S.curvature); st.set("linkGeom", int(S.linkGeom));
  st.set("maxLines", S.maxLines); st.set("minStrength", S.minStrength); st.set("kernel", S.kernel); st.set("densityAlpha", S.densityAlpha);
  st.set("densityScheme", S.densityScheme); st.set("densityByCluster", S.densityByCluster); st.set("backdrop", int(S.backdrop));
  st.set("hulls", S.hulls); st.set("clusterNames", S.clusterNames); st.set("bundle", S.bundle);
  st.set("single", S.single.hexStr()); st.set("scoreMin", std::isfinite(S.scoreMin) ? Json(S.scoreMin) : Json()); st.set("scoreMax", std::isfinite(S.scoreMax) ? Json(S.scoreMax) : Json());
  st.set("borderCustom", S.borderCustom.hexStr()); st.set("maxLabels", S.maxLabels); st.set("truncate", S.truncate); st.set("linkSingle", S.linkSingle.hexStr());
  st.set("linkWidth", S.linkWidth); st.set("linkMaxW", S.linkMaxW); st.set("darkTheme", S.darkTheme); st.set("cvd", S.cvd); st.set("zScale", S.zScale);
  st.set("sizing", S.sizing); st.set("sizeVar", S.sizeVar); st.set("linkVar", S.linkVar); st.set("densityFull", S.densityFull); st.set("kernelAuto", S.kernelAuto);
  Json ov = Json::object();
  for (auto& kv : S.clusterOverride) ov.set(std::to_string(kv.first), Json(kv.second.hexStr()));
  st.set("clusterColors", ov);
  return st;
}
void styleFromJson(ViewStyle& S, const Json& jj) {
  const Json& st = jj;
  if (st.t == Json::Obj) {
    S.look = st["look"].str("studio"); S.palette = st["palette"].str("cvd"); S.flat = st["flat"].boolean(); S.nodeOpacity = float(st["nodeOpacity"].num(.95));
    S.border = float(st["border"].num(1.2)); S.borderCol = BorderCol(st["borderCol"].integer()); S.baseSize = float(st["baseSize"].num(3.4));
    S.maxSize = float(st["maxSize"].num(13)); S.scale = float(st["scale"].num(1)); S.labelVar = float(st["labelVar"].num(.75));
    S.colorBy = ColorBy(st["colorBy"].integer()); S.scheme = st["scheme"].str("viridis"); S.labelPlace = LabelPlace(st["labelPlace"].integer());
    S.labelWeight = st["labelWeight"].integer(450); S.labelSize = float(st["labelSize"].num(1)); S.maxLen = st["maxLen"].integer(30);
    S.labelHalo = st["labelHalo"].boolean(true); S.labelByCluster = st["labelByCluster"].boolean(); S.linksVisible = st["linksVisible"].boolean(true);
    S.linkColor = LinkColor(st["linkColor"].integer()); S.linkOpacity = float(st["linkOpacity"].num(.5)); S.curvature = float(st["curvature"].num(.24));
    S.linkGeom = LinkGeom(st["linkGeom"].integer(1)); S.maxLines = st["maxLines"].integer(1000); S.minStrength = st["minStrength"].num();
    S.kernel = float(st["kernel"].num(1)); S.densityAlpha = float(st["densityAlpha"].num(.55)); S.densityScheme = st["densityScheme"].str("bgy");
    S.densityByCluster = st["densityByCluster"].boolean(); S.backdrop = Backdrop(st["backdrop"].integer()); S.hulls = st["hulls"].boolean();
    S.clusterNames = st["clusterNames"].boolean(); S.bundle = st["bundle"].boolean();
    S.single = st.has("single") ? Color::parse(st["single"].str()) : S.single; S.scoreMin = st["scoreMin"].isNull() ? NAN : st["scoreMin"].num(); S.scoreMax = st["scoreMax"].isNull() ? NAN : st["scoreMax"].num();
    if (st.has("borderCustom")) S.borderCustom = Color::parse(st["borderCustom"].str());
    S.maxLabels = st["maxLabels"].integer(0); S.truncate = st["truncate"].boolean(true); if (st.has("linkSingle")) S.linkSingle = Color::parse(st["linkSingle"].str());
    S.linkWidth = float(st["linkWidth"].num(1)); S.linkMaxW = float(st["linkMaxW"].num(6)); S.darkTheme = st["darkTheme"].boolean(true); S.cvd = st["cvd"].integer(0); S.zScale = float(st["zScale"].num(1));
    S.clusterOverride.clear();
    for (auto& kv : st["clusterColors"].o) S.clusterOverride[toInt(kv.first)] = Color::parse(kv.second.str());
    if (st.has("sizing")) {
      S.sizing = st["sizing"].integer(1); S.sizeVar = float(st["sizeVar"].num(.5)); S.linkVar = float(st["linkVar"].num(.5));
      S.densityFull = st["densityFull"].boolean(true); S.kernelAuto = st["kernelAuto"].boolean(true);
    } else if (S.look == "studio") {
      // saved by 1.3 or earlier with the old default look: move to the new VOSviewer default, keep the palette
      string pal = S.palette; bool dark = S.darkTheme;
      applyLook(S, "vosviewer");
      S.palette = pal; S.darkTheme = dark;
    } else {
      S.sizing = 1; S.labelHalo = S.look == "midnight"; S.densityScheme = "vos"; S.densityFull = true; S.kernelAuto = true; S.curvature = .5f;
    }
  }
}

namespace {
Json specToJson(const AnaSpec& spec) {
  Json a = Json::object();
  a.set("type", typeInfo(spec.type).id);
  a.set("unit", unitId(spec.unit));
  a.set("counting", spec.fractional ? "fractional" : "full");
  a.set("min", spec.min); a.set("minCites", spec.minCites); a.set("maxItems", spec.maxItems); a.set("minLink", spec.minLink);
  a.set("maxAuth", spec.maxAuth); a.set("y0", spec.y0); a.set("y1", spec.y1); a.set("largest", spec.largest); a.set("termKeep", spec.termKeep);
  return a;
}
void specFromJson(AnaSpec& spec, const Json& a) {
  AnaType t;
  Unit u;
  if (typeFromId(a["type"].str(), t)) spec.type = t;
  if (unitFromId(a["unit"].str(), u)) spec.unit = u;
  spec.fractional = a["counting"].str() == "fractional";
  spec.min = a["min"].integer(spec.min); spec.minCites = a["minCites"].integer(); spec.maxItems = a["maxItems"].integer(1000);
  spec.minLink = a["minLink"].num(); spec.maxAuth = a["maxAuth"].integer(25); spec.y0 = a["y0"].integer(); spec.y1 = a["y1"].integer();
  spec.largest = a["largest"].boolean(true); spec.termKeep = a["termKeep"].num(0.6);
}
Json paramsToJson(const BuildParams& params) {
  Json b = Json::object();
  b.set("norm", normId(params.norm));
  b.set("attraction", params.layout.attraction); b.set("repulsion", params.layout.repulsion); b.set("starts", params.layout.starts); b.set("layoutSeed", params.layout.seed);
  b.set("resolution", params.cluster.resolution); b.set("clusterStarts", params.cluster.starts); b.set("iterations", params.cluster.iterations);
  b.set("clusterSeed", params.cluster.seed); b.set("minSize", params.cluster.minSize); b.set("overlap", params.overlap); b.set("autoName", params.autoName);
  b.set("clusterMethod", params.clusterMethod == 1 ? "modularity" : "vosviewer");
  return b;
}
void paramsFromJson(BuildParams& params, const Json& b) {
  normFromId(b["norm"].str("association"), params.norm);
  params.layout.attraction = b["attraction"].num(2); params.layout.repulsion = b["repulsion"].num(1); params.layout.starts = b["starts"].integer(3);
  params.layout.seed = b["layoutSeed"].integer(); params.cluster.resolution = b["resolution"].num(1); params.cluster.starts = b["clusterStarts"].integer(10);
  params.cluster.iterations = b["iterations"].integer(10); params.cluster.seed = b["clusterSeed"].integer(); params.cluster.minSize = b["minSize"].integer(1);
  params.overlap = b["overlap"].boolean(true); params.autoName = b["autoName"].boolean(true);
  // projects saved before 1.6 have no method: they were clustered with modularity, and keep it until the user switches
  params.clusterMethod = b["clusterMethod"].str("modularity") == "modularity" ? 1 : 0;
}
Json netToJson(const Network& net, const string& mapSource) {
  Json m = Json::object();
  m.set("source", mapSource);
  m.set("description", net.description); m.set("unitNoun", net.unitNoun);
  m.set("weightNames", strArr(net.weightNames)); m.set("scoreNames", strArr(net.scoreNames));
  m.set("weightIdx", net.weightIdx); m.set("scoreIdx", net.scoreIdx); m.set("quality", net.quality); m.set("clusterTag", net.clusterTag);
  m.set("clusterNames", strArr(net.clusterNames));
  Json nodes = Json::array();
  for (auto& nd : net.nodes) {
    Json o = Json::object();
    o.set("id", nd.id); o.set("label", nd.label);
    if (!nd.title.empty()) o.set("title", nd.title);
    if (!nd.doi.empty()) o.set("doi", nd.doi);
    o.set("key", nd.key);
    o.set("x", std::round(nd.x * 1000) / 1000); o.set("y", std::round(nd.y * 1000) / 1000);
    if (nd.z != 0) o.set("z", std::round(nd.z * 1000) / 1000);
    o.set("c", nd.cluster);
    Json w = Json::array(); for (double v : nd.w) w.push(v); o.set("w", w);
    Json s = Json::array(); for (double v : nd.sc) s.push(std::isfinite(v) ? Json(v) : Json()); o.set("s", s);
    if (!nd.recs.empty()) { Json r = Json::array(); for (int v : nd.recs) r.push(v); o.set("r", r); }
    if (nd.pinned) o.set("pin", true);
    nodes.push(o);
  }
  m.set("nodes", nodes);
  Json links = Json::array();
  for (auto& l : net.links) { Json o = Json::array(); o.push(l.a); o.push(l.b); o.push(l.w); links.push(o); }
  m.set("links", links);
  return m;
}
Network netFromJson(const Json& m) {
  Network nn;
  nn.description = m["description"].str(); nn.unitNoun = m["unitNoun"].str("item");
  nn.weightNames = arrStr(m["weightNames"]); nn.scoreNames = arrStr(m["scoreNames"]);
  if (nn.weightNames.empty()) nn.weightNames = {"Links", "Total link strength"};
  nn.weightIdx = m["weightIdx"].integer(); nn.scoreIdx = m["scoreIdx"].integer(-1); nn.quality = m["quality"].num(); nn.clusterTag = m["clusterTag"].str("?");
  nn.clusterNames = arrStr(m["clusterNames"]);
  nn.nodes.reserve(m["nodes"].a.size());
  for (auto& o : m["nodes"].a) {
    Node nd;
    nd.id = o["id"].str(); nd.label = o["label"].str(); nd.title = o["title"].str(); nd.doi = o["doi"].str(); nd.key = o["key"].str();
    nd.x = o["x"].num(); nd.y = o["y"].num(); nd.z = o["z"].num(); nd.cluster = o["c"].integer();
    for (auto& v : o["w"].a) nd.w.push_back(v.num());
    for (auto& v : o["s"].a) nd.sc.push_back(v.isNull() ? NAN : v.num());
    for (auto& v : o["r"].a) nd.recs.push_back(v.integer());
    nd.pinned = o["pin"].boolean();
    nn.nodes.push_back(std::move(nd));
  }
  nn.links.reserve(m["links"].a.size());
  for (auto& l : m["links"].a) {
    Link L; L.a = l[0].integer(); L.b = l[1].integer(); L.w = l[2].num(1);
    if (L.a >= 0 && L.b >= 0 && L.a < nn.n() && L.b < nn.n()) nn.links.push_back(L);
  }
  nn.countClusters();
  return nn;
}
}  // namespace

string recordsDump(const vector<Record>& recs) {
  string out = "[";
  for (size_t i = 0; i < recs.size(); i++) {
    if (i) out += ',';
    out += recordToJson(recs[i]).dump();
  }
  out += ']';
  return out;
}

Json Project::toJson(bool includeRecords, string* recDumpOut) const {
  Json j = Json::object();
  j.set("format", "vosstudio-native-project");
  j.set("version", 1);
  j.set("saved", nowIso());
  // analysis
  Json a = specToJson(spec);
  j.set("analysis", a);
  Json b = paramsToJson(params);
  j.set("build", b);
  // thesaurus + exclusions
  if (!engine.thesaurus.empty()) j.set("thesaurus", engine.thesaurus.save());
  Json ex = Json::array();
  for (auto& kv : engine.excluded) for (auto& k : kv.second) { Json e = Json::object(); e.set("scope", kv.first); e.set("key", k); ex.push(e); }
  j.set("excluded", ex);
  // look
  j.set("style", styleToJson(style));
  {
    Json f = Json::object();
    f.set("wmm", fig.wmm); f.set("hmm", fig.hmm); f.set("labelPt", fig.labelPt); f.set("linkMaxPt", fig.linkMaxPt); f.set("theme", int(fig.theme));
    f.set("network", fig.panelNetwork); f.set("overlay", fig.panelOverlay); f.set("density", fig.panelDensity);
    f.set("timeline", fig.panelTimeline); f.set("geo", fig.panelGeo); f.set("threeD", fig.panel3D); f.set("matrix", fig.panelMatrix);
    f.set("legend", fig.legend); f.set("sizeLegend", fig.sizeLegend); f.set("colorbar", fig.colorbar); f.set("letters", fig.letters);
    f.set("transparent", fig.transparent); f.set("pdfPages", fig.pdfPages); f.set("hybridExport", fig.hybridExport); f.set("serif", fig.serif); f.set("footer", fig.footer); f.set("shading", fig.shading);
    f.set("title", fig.title); f.set("caption", fig.caption); f.set("dpi", fig.dpi);
    j.set("figure", f);
  }
  // map
  Json m = netToJson(net, mapSource);
  j.set("map", m);
  // map history: the current entry is the map above, the others are stored in full
  if (maps.size() > 1 || (maps.size() == 1 && mapCur != 0)) {
    Json hs = Json::array();
    for (size_t k = 0; k < maps.size(); k++) {
      const SavedMap& e = maps[k];
      Json o = Json::object();
      o.set("title", e.title); o.set("when", e.when);
      if (int(k) == mapCur) o.set("current", true);
      else { o.set("analysis", specToJson(e.spec)); o.set("build", paramsToJson(e.params)); o.set("map", netToJson(e.net, e.mapSource)); }
      hs.push(o);
    }
    j.set("maps", hs);
    j.set("mapCur", mapCur);
  }
  if (assistant.t == Json::Arr && assistant.size() > 0) j.set("assistant", assistant);
  if (document.t == Json::Obj) j.set("document", document);
  if (library.hasData()) j.set("library", library.toJson());
  if (living.t == Json::Obj && living.has("query")) j.set("living", living);
  // corpus
  Json c = Json::object();
  Json files = Json::array();
  for (auto& f : corpus.files) { Json o = Json::object(); o.set("name", f.name); o.set("format", formatLabel(f.format)); o.set("records", f.records); files.push(o); }
  c.set("files", files);
  c.set("duplicatesRemoved", corpus.duplicatesRemoved);
  c.set("records", int(corpus.recs.size()));
  // Records are serialised one at a time: identical to dumping the whole array, without building its tree.
  string recDump = recordsDump(corpus.recs);
  c.set("sha256", sha256Hex(recDump));
  if (includeRecords) c.set("data", Json::parse(recDump));
  if (recDumpOut) *recDumpOut = std::move(recDump);
  j.set("corpus", c);
  // reproducibility manifest
  Json man = Json::object();
  man.set("map_sha256", sha256Hex(m.dump()));
  man.set("settings_sha256", sha256Hex(a.dump() + b.dump()));
  man.set("methods", methods());
  j.set("manifest", man);
  return j;
}

bool Project::fromJson(const Json& j, string* err, vector<Record>* preloaded) {
  bool migratedRecordIds = false;
  if (j["format"].str() != "vosstudio-native-project" && j["format"].str() != "vosstudio-bundle") { if (err) *err = "Not a VOSStudio project file."; return false; }
  specFromJson(spec, j["analysis"]);
  paramsFromJson(params, j["build"]);
  engine.thesaurus = Thesaurus();
  if (j.has("thesaurus")) engine.thesaurus.load(j["thesaurus"].str());
  engine.excluded.clear();
  for (auto& e : j["excluded"].a) engine.excluded[e["scope"].str()].insert(e["key"].str());
  styleFromJson(style, j["style"]);
  if (j.has("figure")) {
    const Json& f = j["figure"];
    auto num = [&](const char* k, double& v) { if (f.has(k)) v = f[k].num(); };
    auto flag = [&](const char* k, bool& v) { if (f.has(k)) v = f[k].boolean(); };
    num("wmm", fig.wmm); num("hmm", fig.hmm); num("labelPt", fig.labelPt); num("linkMaxPt", fig.linkMaxPt); num("dpi", fig.dpi);
    if (f.has("theme")) fig.theme = FigTheme(clampv(f["theme"].integer(), 0, 2));
    if (f.has("shading")) fig.shading = clampv(f["shading"].integer(), 0, 2);
    flag("network", fig.panelNetwork); flag("overlay", fig.panelOverlay); flag("density", fig.panelDensity);
    flag("timeline", fig.panelTimeline); flag("geo", fig.panelGeo); flag("threeD", fig.panel3D); flag("matrix", fig.panelMatrix);
    flag("legend", fig.legend); flag("sizeLegend", fig.sizeLegend); flag("colorbar", fig.colorbar); flag("letters", fig.letters);
    flag("transparent", fig.transparent); flag("pdfPages", fig.pdfPages); flag("hybridExport", fig.hybridExport); flag("serif", fig.serif); flag("footer", fig.footer);
    fig.title = f["title"].str(); fig.caption = f["caption"].str();
  }
  // corpus
  corpus.clear();
  const Json& c = j["corpus"];
  for (auto& f : c["files"].a) {
    SourceFile sf; sf.name = f["name"].str(); sf.records = f["records"].integer();
    string fl = f["format"].str();
    for (auto bf : {BibFormat::WoS, BibFormat::WoSTab, BibFormat::Scopus, BibFormat::RIS, BibFormat::BibTeX, BibFormat::OpenAlex, BibFormat::VOSviewer}) if (fl == formatLabel(bf)) sf.format = bf;
    corpus.files.push_back(sf);
  }
  if (!corpus.files.empty()) corpus.format = corpus.files.front().format;
  corpus.duplicatesRemoved = c["duplicatesRemoved"].integer();
  if (c.has("data")) {
    for (auto& r : c["data"].a) corpus.recs.push_back(recordFromJson(r));
    string sha = sha256Hex(c["data"].dump());
    if (c.has("sha256") && c["sha256"].str() != sha && err) *err = "Warning: record checksum mismatch (file edited outside VOSStudio).";
  } else if (preloaded) {
    corpus.recs = std::move(*preloaded);
  }
  migratedRecordIds = ensureRecordIds(corpus.recs) > 0;
  corpusChanged();
  // map
  const Json& m = j["map"];
  Network nn = netFromJson(m);
  net = std::move(nn);
  if (net.clusterTag == "?") net.clusterTag = clusterTag(params.clusterOpts());  // saved before 1.6
  normalise(net, params.norm);
  bundles = Bundles();
  metricsValid = false;
  mapSource = m["source"].str("bundle");
  builtSig = mapSource == "analysis" ? specSig() : string();
  last = BuildReport();
  last.n = net.n(); last.m = net.m(); last.clusters = net.nClusters; last.Q = net.quality;
  // map history
  maps.clear();
  mapCur = -1;
  if (j.has("maps")) {
    int cur = j["mapCur"].integer(-1);
    for (size_t k = 0; k < j["maps"].a.size(); k++) {
      const Json& o = j["maps"].a[k];
      SavedMap e;
      e.title = o["title"].str("Map"); e.when = o["when"].str();
      if (o["current"].boolean() || int(k) == cur) {
        e.net = net; e.spec = spec; e.params = params; e.mapSource = mapSource;
        mapCur = int(maps.size());
      } else {
        if (!o.has("map")) continue;
        specFromJson(e.spec, o["analysis"]);
        paramsFromJson(e.params, o["build"]);
        e.net = netFromJson(o["map"]);
        if (e.net.clusterTag == "?") e.net.clusterTag = clusterTag(e.params.clusterOpts());
        normalise(e.net, e.params.norm);
        e.mapSource = o["map"]["source"].str("bundle");
      }
      maps.push_back(std::move(e));
    }
    if (mapCur < 0 && !maps.empty()) { maps.clear(); }  // malformed: fall back to the single current map
  }
  assistant = j.has("assistant") && j["assistant"].t == Json::Arr ? j["assistant"] : Json::array();
  document = j.has("document") && j["document"].t == Json::Obj ? j["document"] : Json();
  library.fromJson(j["library"]);
  int migratedLibraryKeys = library.migrateRecordKeys(corpus);
  living = j.has("living") && j["living"].t == Json::Obj ? j["living"] : Json::object();
  dirty = migratedRecordIds || migratedLibraryKeys > 0;
  return true;
}

static const char* kDataToken = "@@VOSSTUDIO_RECORDS@@";

bool Project::save(const string& file, string* err) {
  // The records array is written compactly and spliced into the document, so saving a large corpus needs
  // one copy of the serialised records instead of a JSON tree plus several string copies.
  string recs;
  Json j = toJson(false, &recs);
  for (auto& kv : j.o)
    if (kv.first == "corpus") { kv.second.set("dataCompact", true); kv.second.set("data", string(kDataToken)); }
  string doc = j.dump(1);
  string tok = string("\"") + kDataToken + "\"";
  size_t at = doc.find(tok);
  if (at == string::npos) { if (err) *err = "Internal error while saving."; return false; }
  string head = doc.substr(0, at), tail = doc.substr(at + tok.size());
  doc.clear(); doc.shrink_to_fit();
  if (!writeFileParts(file, {&head, &recs, &tail})) { if (err) *err = "Could not write " + file; return false; }
  path = file;
  dirty = false;
  return true;
}

// End of the JSON value starting at s[i] (compact or pretty), or npos.
static size_t jsonValueEnd(const string& s, size_t i) {
  int depth = 0;
  bool str = false;
  for (; i < s.size(); i++) {
    char ch = s[i];
    if (str) {
      if (ch == '\\') i++;
      else if (ch == '"') str = false;
      continue;
    }
    if (ch == '"') str = true;
    else if (ch == '[' || ch == '{') depth++;
    else if (ch == ']' || ch == '}') { if (--depth == 0) return i + 1; }
  }
  return string::npos;
}

bool Project::open(const string& file, string* err) {
  bool ok = false;
  string t = readFile(file, &ok);
  if (!ok) { if (err) *err = "Could not read " + file; return false; }
  string e;
  // Fast path for files written by save(): parse records one by one, then the (small) rest of the document.
  size_t mk = t.find("\"dataCompact\": true,");
  if (mk != string::npos) {
    size_t d = t.find("\"data\": [", mk);
    if (d != string::npos) {
      size_t a = t.find('[', d), b = jsonValueEnd(t, a);
      if (b != string::npos) {
        vector<Record> recs;
        size_t i = a + 1;
        while (i < b - 1) {
          while (i < b - 1 && (t[i] == ',' || t[i] == ' ' || t[i] == '\n' || t[i] == '\r')) i++;
          if (i >= b - 1) break;
          size_t q = jsonValueEnd(t, i);
          if (q == string::npos || q > b) { if (err) *err = "Invalid project file: records"; return false; }
          Json r = Json::parse(t.substr(i, q - i), &e);
          if (!e.empty()) { if (err) *err = "Invalid project file: " + e; return false; }
          recs.push_back(recordFromJson(r));
          i = q;
        }
        string sha = sha256Hex(t.data() + a, b - a);
        string rest = t.substr(0, a) + "0" + t.substr(b);
        t.clear(); t.shrink_to_fit();
        Json j = Json::parse(rest, &e);
        if (!e.empty()) { if (err) *err = "Invalid project file: " + e; return false; }
        for (auto& kv : j.o)
          if (kv.first == "corpus") {
            Json nc = Json::object();
            for (auto& ckv : kv.second.o) if (ckv.first != "data") nc.set(ckv.first, ckv.second);
            kv.second = nc;
          }
        string stored = j["corpus"]["sha256"].str();
        if (!fromJson(j, err, &recs)) return false;
        if (!stored.empty() && stored != sha && err) *err = "Warning: record checksum mismatch (file edited outside VOSStudio).";
        path = file;
        return true;
      }
    }
  }
  Json j = Json::parse(t, &e);
  if (!e.empty()) { if (err) *err = "Invalid project file: " + e; return false; }
  if (!fromJson(j, err)) return false;
  path = file;
  return true;
}

string Project::methods() const { return methodsParagraph(corpus, spec, net, params.norm, params.layout, params.clusterOpts(), style.bundle && bundles.valid()); }

string Project::methodsShort() const {
  string o = fmtInt(net.n()) + " items \xC2\xB7 " + fmtInt(net.m()) + " links \xC2\xB7 ";
  o += params.norm == Norm::Association ? "association strength" : normId(params.norm);
  o += " \xC2\xB7 VOS layout (a=" + fmtNum(params.layout.attraction, 1) + ", r=" + fmtNum(params.layout.repulsion, 1) + ") \xC2\xB7 Leiden \xCE\xB3=" +
       fmtNum(params.cluster.resolution, 2) + " \xC2\xB7 Q=" + fmtFixed(net.quality, 3) + " \xC2\xB7 seed " + std::to_string(params.cluster.seed) + " \xC2\xB7 VOSStudio Native";
  return o;
}


string Project::specSig() {
  const AnaSpec& s = spec;
  string o = string(typeInfo(s.type).id) + "|" + unitId(s.unit) + "|" + (s.fractional ? "f" : "F") + "|" + std::to_string(s.min) + "|" +
             std::to_string(s.minCites) + "|" + std::to_string(s.maxItems) + "|" + fmtNum(s.minLink, 4) + "|" + std::to_string(s.maxAuth) + "|" +
             std::to_string(s.y0) + "|" + std::to_string(s.y1) + "|" + (s.largest ? "L" : "-") + "|" + fmtNum(s.termKeep, 3) + "|" + engine.thesaurus.sig() + "|" +
             std::to_string(engine.exclSet(s).size());
  return o;
}

RecordsFlow Project::recordsFlow(bool withItems) {
  RecordsFlow f;
  if (corpus.empty()) return f;
  f.valid = true;
  for (auto& sf : corpus.files) f.sources.push_back({sf.name, sf.records});
  f.duplicates = corpus.duplicatesRemoved;
  f.screened = int(corpus.recs.size());
  f.identified = f.screened + f.duplicates;
  f.unitNoun = unitNounPlural(spec.unit);
  if (!withItems) {
    f.itemsKnown = false;
    f.analysed = f.screened;
    return f;
  }
  try {
    const RawNet& R = engine.raw(spec);
    int outYears = f.screened - R.nRecs;
    if (outYears > 0) {
      string why = spec.y0 && spec.y1 ? "Outside " + std::to_string(spec.y0) + "\xE2\x80\x93" + std::to_string(spec.y1)
                   : spec.y0 ? "Before " + std::to_string(spec.y0) : "After " + std::to_string(spec.y1);
      f.excluded.push_back({why, outYears});
    }
    const char* field = "the data this analysis needs";
    switch (spec.unit) {
      case Unit::Keywords: field = "author keywords"; break;
      case Unit::AllKeywords: field = "keywords"; break;
      case Unit::IndexTerms: field = "index terms"; break;
      case Unit::Terms: field = "title or abstract terms"; break;
      case Unit::Authors: field = "authors"; break;
      case Unit::Orgs: field = "affiliations"; break;
      case Unit::Countries: field = "countries"; break;
      default: if (spec.type == AnaType::Coupling || spec.type == AnaType::Cocit || spec.type == AnaType::Citation) field = "cited references";
    }
    int noField = R.nRecs - R.used;
    if (noField > 0) f.excluded.push_back({string("No ") + field, noField});
    f.analysed = R.used;
    const Selection& S = engine.select(spec);
    f.found = S.nTotal;
    f.meet = S.nMeet;
    f.userExcluded = S.nMeet - S.nPass;
    f.thresholdText = thresholdLabel(spec) + ": " + std::to_string(spec.min);
  } catch (...) {
    f.analysed = f.screened;
  }
  f.inMap = mapSource == "analysis" && net.n() && builtSig == specSig() ? net.n() : -1;
  return f;
}

}  // namespace vs
