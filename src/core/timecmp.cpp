#include "timecmp.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "common.h"
#include "report.h"

namespace vs {

// ================================================================= period difference
const char* changeLabel(Change c) {
  switch (c) {
    case Change::Appearing: return "Appearing";
    case Change::Growing: return "Growing";
    case Change::Fading: return "Fading";
    default: return "Stable";
  }
}

void suggestPeriods(const Corpus& c, int& a0, int& a1, int& b0, int& b1) {
  std::map<int, int> perYear;
  int total = 0;
  for (auto& r : c.recs) if (r.year) { perYear[r.year]++; total++; }
  if (perYear.empty()) { a0 = a1 = b0 = b1 = 0; return; }
  a0 = perYear.begin()->first;
  b1 = perYear.rbegin()->first;
  if (a0 == b1) { a1 = a0; b0 = b1; return; }
  int acc = 0;
  a1 = a0;
  for (auto& kv : perYear) {
    acc += kv.second;
    a1 = kv.first;
    if (acc * 2 >= total) break;
  }
  if (a1 >= b1) a1 = b1 - 1;
  b0 = a1 + 1;
}

vector<char> yearMask(const Corpus& c, int y0, int y1) {
  if (y0 > y1) std::swap(y0, y1);
  vector<char> m(c.recs.size(), 0);
  for (size_t i = 0; i < c.recs.size(); i++) { int y = c.recs[i].year; m[i] = y && y >= y0 && y <= y1; }
  return m;
}

vector<char> fileMask(const Corpus& c, size_t file) {
  vector<char> m(c.recs.size(), 0);
  bool prov = c.provenanceKnown();
  for (size_t i = 0; i < c.recs.size(); i++) m[i] = !prov || file >= 64 || (c.recs[i].src & (1ull << file)) != 0;
  return m;
}

PeriodDiff subsetDiff(const Network& net, const Corpus& c, const vector<char>& inA, const vector<char>& inB, int minDocs) {
  PeriodDiff D;
  for (size_t i = 0; i < c.recs.size(); i++) { if (i < inA.size() && inA[i]) D.nA++; if (i < inB.size() && inB[i]) D.nB++; }
  bool anyRecs = false;
  for (auto& nd : net.nodes) if (!nd.recs.empty()) { anyRecs = true; break; }
  if (!anyRecs) { D.error = "This map was not built from records, so its items cannot be compared by documents."; return D; }
  if (!D.nA || !D.nB) { D.error = "One of the two sides has no documents."; return D; }
  const double up = std::log2(1.5);
  D.items.resize(net.nodes.size());
  for (size_t i = 0; i < net.nodes.size(); i++) {
    ItemChange& it = D.items[i];
    it.node = int(i);
    for (int r : net.nodes[i].recs) {
      if (r < 0 || size_t(r) >= c.recs.size()) continue;
      if (size_t(r) < inA.size() && inA[size_t(r)]) it.a++;
      if (size_t(r) < inB.size() && inB[size_t(r)]) it.b++;
    }
    it.shareA = double(it.a) / D.nA;
    it.shareB = double(it.b) / D.nB;
    it.logRatio = std::log2(((it.b + 0.5) / D.nB) / ((it.a + 0.5) / D.nA));
    if (it.a + it.b < minDocs) { it.few = true; it.status = Change::Stable; }
    else if (it.a == 0) it.status = Change::Appearing;
    else if (it.logRatio >= up) it.status = Change::Growing;
    else if (it.logRatio <= -up) it.status = Change::Fading;
    else it.status = Change::Stable;
    D.counts[int(it.status)]++;
  }
  D.ok = true;
  return D;
}

PeriodDiff periodDiff(const Network& net, const Corpus& c, int a0, int a1, int b0, int b1, int minDocs) {
  PeriodDiff D = subsetDiff(net, c, yearMask(c, a0, a1), yearMask(c, b0, b1), minDocs);
  D.a0 = a0; D.a1 = a1; D.b0 = b0; D.b1 = b1;
  if (!D.ok && D.error.find("no documents") != string::npos) D.error = "One of the periods has no documents.";
  if (!D.ok && D.error.find("cannot be compared") != string::npos) D.error = "This map was not built from records, so its items have no years.";
  return D;
}

Scene compareSideBySide(const Network& net, const ViewStyle& st, const FigureSpec& spec, const Bundles* bundles, const string& methodsShort,
                        const vector<double>& a, const vector<double>& b, double nA, double nB, const string& titleA, const string& titleB,
                        double W, double H) {
  Scene out;
  int n = net.n();
  if (n == 0 || int(a.size()) < n || int(b.size()) < n) return out;
  // shares on one common scale
  vector<double> sa((size_t(n))), sb((size_t(n)));
  double maxS = 0;
  for (int i = 0; i < n; i++) {
    sa[size_t(i)] = nA > 0 ? a[size_t(i)] / nA : a[size_t(i)];
    sb[size_t(i)] = nB > 0 ? b[size_t(i)] / nB : b[size_t(i)];
    maxS = std::max({maxS, sa[size_t(i)], sb[size_t(i)]});
  }
  if (maxS <= 0) maxS = 1;
  Encoder enc;
  enc.prepare(net, st);
  double rMax = 0;
  for (int i = 0; i < n; i++) rMax = std::max(rMax, enc.radius(i));
  if (rMax <= 0) rMax = 1;
  auto panel = [&](const vector<double>& sh) {
    FigPanelDef d;
    d.kind = ViewKind::Network;
    d.rMul.resize(size_t(n));
    d.fade.resize(size_t(n));
    for (int i = 0; i < n; i++) {
      double r0 = std::max(1e-6, enc.radius(i));
      double want = rMax * std::sqrt(sh[size_t(i)] / maxS);  // area proportional to the share
      bool absent = sh[size_t(i)] <= 0;
      d.rMul[size_t(i)] = float(absent ? std::max(0.12, 0.25 * rMax / r0 * 0.5) : std::max(0.15, want / r0));
      d.fade[size_t(i)] = absent ? 0.82f : 0.f;
    }
    return d;
  };
  FigureSpec sp = spec;
  sp.panelNetwork = true;
  sp.panelOverlay = sp.panelDensity = sp.panelTimeline = sp.panelGeo = sp.panel3D = sp.panelMatrix = false;
  sp.letters = false;
  sp.sizeLegend = false;  // sizes are shares on a common scale, not the map's weights
  sp.footer = false;
  sp.pdfPages = false;
  sp.transparent = false;
  sp.caption.clear();
  double halfW = std::max(120.0, W / 2 - 6), hh = std::max(90.0, H);
  sp.wmm = 170;
  sp.hmm = clampv(170 * hh / halfW, 90.0, 260.0);
  FigureSpec spA = sp, spB = sp;
  spA.title = titleA;
  spB.title = titleB;
  spB.legend = false;  // one cluster legend is enough
  vector<FigPanelDef> defsA{panel(sa)}, defsB{panel(sb)};
  Scene A = buildFigure(net, st, spA, bundles, methodsShort, &defsA);
  Scene B = buildFigure(net, st, spB, bundles, methodsShort, &defsB);
  double k = A.W > 0 ? halfW / A.W : 1;
  out.W = W;
  out.H = std::max(A.H, B.H) * k;
  appendScene(out, A, 0, 0, k);
  appendScene(out, B, W / 2 + 6, 0, k);
  Prim div;
  div.type = Prim::Rect;
  div.x = float(W / 2 - 0.5);
  div.y = float(out.H * 0.06);
  div.w = 1;
  div.h = float(out.H * 0.88);
  div.fill = true;
  div.fillC = Color(0.5f, 0.5f, 0.55f, 0.35f);
  div.group = "legend";
  out.items.push_back(div);
  out.warnings.insert(out.warnings.end(), A.warnings.begin(), A.warnings.end());
  out.labels = A.labels + B.labels;
  return out;
}

// ================================================================= main path
namespace {
double logAdd(double a, double b) {
  if (a == -INFINITY) return b;
  if (b == -INFINITY) return a;
  double m = std::max(a, b);
  return m + std::log(std::exp(a - m) + std::exp(b - m));
}
}  // namespace

MainPath mainPath(const Corpus& c, const CitationIndex& X, int keyRoutes) {
  MainPath M;
  size_t N = std::min(c.recs.size(), X.cites.size());
  // order: (year, index); links that go back in time are dropped, so the network is acyclic
  auto before = [&](int u, int v) {
    int yu = c.recs[size_t(u)].year, yv = c.recs[size_t(v)].year;
    return yu != yv ? yu < yv : u < v;
  };
  vector<std::pair<int, int>> E;  // cited -> citing
  for (size_t v = 0; v < N; v++) {
    if (!c.recs[v].year) continue;
    for (int u : X.cites[v]) {
      if (u < 0 || size_t(u) >= N || size_t(u) == v || !c.recs[size_t(u)].year) continue;
      if (before(u, int(v))) E.push_back({u, int(v)});
      else M.dropped++;
    }
  }
  if (E.size() < 2) { M.error = "Too few citations between documents of this corpus for a main path."; return M; }
  vector<int> ids;
  {
    std::unordered_set<int> S;
    for (auto& e : E) { S.insert(e.first); S.insert(e.second); }
    ids.assign(S.begin(), S.end());
  }
  std::sort(ids.begin(), ids.end(), [&](int a, int b) { return before(a, b); });
  std::unordered_map<int, int> pos;
  for (size_t i = 0; i < ids.size(); i++) pos[ids[i]] = int(i);
  size_t n = ids.size();
  vector<vector<int>> pred(n), succ(n);
  for (auto& e : E) { int a = pos[e.first], b = pos[e.second]; succ[size_t(a)].push_back(b); pred[size_t(b)].push_back(a); }
  M.dagNodes = int(n);
  M.dagLinks = int(E.size());
  vector<double> lm(n, -INFINITY), lp(n, -INFINITY);
  for (size_t i = 0; i < n; i++) {
    if (pred[i].empty()) { lm[i] = 0; M.sources++; continue; }
    for (int p : pred[i]) lm[i] = logAdd(lm[i], lm[size_t(p)]);
  }
  for (size_t i = n; i-- > 0;) {
    if (succ[i].empty()) { lp[i] = 0; M.sinks++; continue; }
    for (int s : succ[i]) lp[i] = logAdd(lp[i], lp[size_t(s)]);
  }
  double lt = -INFINITY;
  for (size_t i = 0; i < n; i++) if (succ[i].empty()) lt = logAdd(lt, lm[i]);
  M.logPaths = lt;
  auto spc = [&](int a, int b) { return std::exp(lm[size_t(a)] + lp[size_t(b)] - lt); };
  // best sums into and out of each node
  vector<double> bin(n, 0), bout(n, 0);
  vector<int> prevIn(n, -1), nextOut(n, -1);
  for (size_t i = 0; i < n; i++)
    for (int p : pred[i]) {
      double v = bin[size_t(p)] + spc(p, int(i));
      if (prevIn[i] < 0 || v > bin[i]) { bin[i] = v; prevIn[i] = p; }
    }
  for (size_t i = n; i-- > 0;)
    for (int s : succ[i]) {
      double v = bout[size_t(s)] + spc(int(i), s);
      if (nextOut[i] < 0 || v > bout[i]) { bout[i] = v; nextOut[i] = s; }
    }
  auto forward = [&](int from, vector<int>& out) { for (int k = from; k >= 0; k = nextOut[size_t(k)]) out.push_back(k); };
  auto backward = [&](int to, vector<int>& out) {
    vector<int> rev;
    for (int k = to; k >= 0; k = prevIn[size_t(k)]) rev.push_back(k);
    out.insert(out.end(), rev.rbegin(), rev.rend());
  };
  int start = -1;
  for (size_t i = 0; i < n; i++) if (pred[i].empty() && (start < 0 || bout[i] > bout[size_t(start)])) start = int(i);
  vector<int> g;
  forward(start, g);
  std::set<std::pair<int, int>> pathLinks;
  std::set<int> onPath;
  auto addPath = [&](const vector<int>& p) {
    for (size_t k = 0; k < p.size(); k++) {
      onPath.insert(p[k]);
      if (k) pathLinks.insert({p[k - 1], p[k]});
    }
  };
  addPath(g);
  for (int k : g) M.global.push_back(ids[size_t(k)]);
  // key routes
  vector<std::pair<double, std::pair<int, int>>> ranked;
  ranked.reserve(E.size());
  for (size_t i = 0; i < n; i++) for (int s : succ[i]) ranked.push_back({spc(int(i), s), {int(i), s}});
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
  for (int r = 0; r < keyRoutes && size_t(r) < ranked.size(); r++) {
    vector<int> p;
    backward(ranked[size_t(r)].second.first, p);
    forward(ranked[size_t(r)].second.second, p);
    addPath(p);
  }
  vector<int> nodes(onPath.begin(), onPath.end());  // positions are already in time order
  vector<double> best(n, 0);
  for (auto& l : pathLinks) {
    double v = spc(l.first, l.second);
    M.links.emplace_back(ids[size_t(l.first)], ids[size_t(l.second)], v);
    best[size_t(l.first)] = std::max(best[size_t(l.first)], v);
    best[size_t(l.second)] = std::max(best[size_t(l.second)], v);
  }
  for (int k : nodes) { M.docs.push_back(ids[size_t(k)]); M.spcDoc.push_back(best[size_t(k)]); }
  M.ok = M.global.size() >= 2;
  if (!M.ok) M.error = "No main path was found.";
  return M;
}

// ================================================================= resolution sweep
vector<double> defaultSweepResolutions() { return {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0}; }

Sweep resolutionSweep(const Network& net, const ClusterOpts& base, const vector<double>& res, int seeds, const std::function<bool(int, int)>& progress) {
  Sweep S;
  S.seeds = std::max(2, seeds);
  S.n = net.n();
  Network work = net;
  int total = int(res.size()) * S.seeds, done = 0;
  for (double r : res) {
    SweepPoint p;
    p.resolution = r;
    vector<vector<int>> labs;
    for (int k = 0; k < S.seeds; k++) {
      if (progress && !progress(done, total)) { S.cancelled = true; return S; }
      ClusterOpts o = base;
      o.resolution = r;
      o.seed = base.seed + 1 + 7919 * k;
      o.starts = 1;
      vector<int> lab;
      double q = leiden(work, o, &lab);
      if (k == 0) {
        p.quality = q;
        std::unordered_map<int, int> sz;
        for (int x : lab) sz[x]++;
        int mx = 0;
        for (auto& kv : sz) mx = std::max(mx, kv.second);
        p.largest = S.n ? double(mx) / S.n : 0;
      }
      std::unordered_set<int> distinct(lab.begin(), lab.end());
      int kc = int(distinct.size());
      p.clusters += kc;
      p.clustersMin = k ? std::min(p.clustersMin, kc) : kc;
      p.clustersMax = k ? std::max(p.clustersMax, kc) : kc;
      labs.push_back(std::move(lab));
      done++;
    }
    p.clusters = int(std::lround(double(p.clusters) / S.seeds));
    double sum = 0;
    int pairs = 0;
    for (size_t i = 0; i < labs.size(); i++)
      for (size_t j = i + 1; j < labs.size(); j++) { sum += partitionAgreement(labs[i], labs[j]).ari; pairs++; }
    p.meanAri = pairs ? sum / pairs : 1;
    S.pts.push_back(p);
  }
  if (progress) progress(total, total);
  for (size_t i = 0; i < S.pts.size(); i++) {
    const SweepPoint& p = S.pts[i];
    if (p.clusters < 2) continue;
    if (S.best < 0) { S.best = int(i); continue; }
    const SweepPoint& b = S.pts[size_t(S.best)];
    double d = p.meanAri - b.meanAri;
    auto dist = [&](double x) { return std::fabs(std::log(x / std::max(1e-9, base.resolution))); };
    if (d > 0.01 || (d > -0.01 && dist(p.resolution) < dist(b.resolution))) S.best = int(i);
  }
  return S;
}

string sweepCsv(const Sweep& s) {
  string o = "resolution,clusters,clusters_min,clusters_max,quality,mean_ari,largest_cluster_share\n";
  for (auto& p : s.pts)
    o += fmtNum(p.resolution, 2) + "," + std::to_string(p.clusters) + "," + std::to_string(p.clustersMin) + "," + std::to_string(p.clustersMax) + "," +
         fmtNum(p.quality, 4) + "," + fmtNum(p.meanAri, 4) + "," + fmtNum(p.largest, 4) + "\n";
  return o;
}

// ================================================================= living maps
namespace {
string titleKey(const Record& r) {
  string tk;
  for (char ch : lower(asciiFold(r.title))) if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) tk += ch;
  return tk.size() > 12 ? tk + "|" + std::to_string(r.year) : string();
}
}  // namespace

Placement placeNewPapers(const Network& net, const AnaSpec& spec, const Thesaurus& th, const Corpus& c, const vector<Record>& fetched) {
  Placement P;
  P.fetched = int(fetched.size());
  std::unordered_set<string> dois, titles;
  for (auto& r : c.recs) {
    string d = normDoi(r.doi);
    if (!d.empty()) dois.insert(d);
    string t = titleKey(r);
    if (!t.empty()) titles.insert(t);
  }
  std::unordered_map<string, int> byKey, byDoi;
  int K = 0;
  for (size_t i = 0; i < net.nodes.size(); i++) {
    const Node& nd = net.nodes[i];
    if (!nd.key.empty()) byKey[nd.key] = int(i);
    string d = normDoi(nd.doi);
    if (!d.empty()) byDoi[d] = int(i);
    K = std::max(K, nd.cluster + 1);
  }
  P.perCluster.assign(size_t(K), 0);
  // keywords already in the data (documents per keyword)
  std::unordered_map<string, int> known;
  for (auto& r : c.recs) {
    std::unordered_set<string> seen;
    for (auto* v : {&r.keywords, &r.indexTerms}) for (auto& k : *v) { string b = bibKey(k); if (!b.empty() && seen.insert(b).second) known[b]++; }
  }
  std::unordered_map<string, int> fresh;
  std::unordered_map<string, string> label;
  for (size_t i = 0; i < fetched.size(); i++) {
    const Record& r = fetched[i];
    string d = normDoi(r.doi), t = titleKey(r);
    if ((!d.empty() && dois.count(d)) || (!t.empty() && titles.count(t))) { P.known++; continue; }
    if (!d.empty()) dois.insert(d);  // duplicates within the fetched list
    if (!t.empty()) titles.insert(t);
    NewPaper np;
    np.rec = int(i);
    std::unordered_set<int> hit;
    if (spec.unit == Unit::Docs) {
      for (auto& ref : r.refs) {
        string rd = normDoi(refParse(ref).doi);
        auto it = rd.empty() ? byDoi.end() : byDoi.find(rd);
        if (it != byDoi.end()) hit.insert(it->second);
      }
    } else {
      for (auto& k : recordUnitKeys(r, spec.unit, th)) { auto it = byKey.find(k); if (it != byKey.end()) hit.insert(it->second); }
    }
    np.nodes.assign(hit.begin(), hit.end());
    std::sort(np.nodes.begin(), np.nodes.end());
    np.hits = int(np.nodes.size());
    if (np.hits) {
      vector<int> votes(size_t(std::max(K, 1)), 0);
      for (int nd : np.nodes) { int cl = net.nodes[size_t(nd)].cluster; if (cl >= 0 && cl < K) votes[size_t(cl)]++; }
      np.cluster = int(std::max_element(votes.begin(), votes.end()) - votes.begin());
      if (np.cluster < K) P.perCluster[size_t(np.cluster)]++;
    } else {
      P.unplaced++;
    }
    std::unordered_set<string> seen;
    for (auto* v : {&r.keywords, &r.indexTerms})
      for (auto& k : *v) {
        string b = bibKey(k);
        if (b.size() < 3 || !seen.insert(b).second) continue;
        fresh[b]++;
        if (!label.count(b)) label[b] = lower(k);
      }
    P.papers.push_back(std::move(np));
  }
  for (auto& kv : fresh) {
    if (kv.second < 2) continue;
    auto it = known.find(kv.first);
    int before = it == known.end() ? 0 : it->second;
    if (before > 1 || byKey.count(kv.first)) continue;
    P.emerging.push_back({label[kv.first], kv.second});
  }
  std::sort(P.emerging.begin(), P.emerging.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  if (P.emerging.size() > 12) P.emerging.resize(12);
  return P;
}

}  // namespace vs
