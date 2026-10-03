#include "stats.h"

#include <array>
#include <tuple>

namespace vs {

static const std::set<Unit> KW_UNITS = {Unit::AllKeywords, Unit::Keywords, Unit::IndexTerms, Unit::Terms};

vector<ClusterInfo> clusterInfo(const Network& net, const Corpus* corpus) {
  int K = 0;
  for (auto& nd : net.nodes) K = std::max(K, nd.cluster + 1);
  vector<ClusterInfo> C(static_cast<size_t>(K));
  for (int c = 0; c < K; c++) C[c].c = c;
  for (int i = 0; i < net.n(); i++) { C[net.nodes[i].cluster].items.push_back(i); C[net.nodes[i].cluster].size++; }
  vector<double> internal(static_cast<size_t>(K), 0), external(static_cast<size_t>(K), 0);
  for (auto& l : net.links) {
    int a = net.nodes[l.a].cluster, b = net.nodes[l.b].cluster;
    if (a == b) internal[a] += l.w;
    else { external[a] += l.w; external[b] += l.w; }
  }
  bool kwUnit = net.unitNoun == "keyword" || net.unitNoun == "term" || net.unitNoun == "item";
  for (auto& ci : C) {
    std::sort(ci.items.begin(), ci.items.end(), [&](int a, int b) { return net.weight(a) > net.weight(b); });
    ci.centrality = external[ci.c];
    ci.density = ci.size ? internal[ci.c] / ci.size : 0;
    double ys = 0, cs = 0;
    int yn = 0, cn = 0;
    for (int i : ci.items) {
      ci.weight += net.weight(i);
      if (net.nodes[i].sc.size() > 0 && std::isfinite(net.nodes[i].sc[0])) { ys += net.nodes[i].sc[0]; yn++; }
      if (net.nodes[i].sc.size() > 1 && std::isfinite(net.nodes[i].sc[1])) { cs += net.nodes[i].sc[1]; cn++; }
    }
    ci.avgYear = yn ? ys / yn : NAN;
    ci.avgCites = cn ? cs / cn : NAN;
    string name;
    if (!kwUnit && corpus && !corpus->recs.empty()) {
      vector<int> recs;
      for (int i : ci.items) recs.insert(recs.end(), net.nodes[i].recs.begin(), net.nodes[i].recs.end());
      std::sort(recs.begin(), recs.end());
      recs.erase(std::unique(recs.begin(), recs.end()), recs.end());
      name = keywordName(*corpus, recs, 2);
    }
    if (name.empty()) {
      vector<string> top;
      for (size_t k = 0; k < ci.items.size() && k < 2; k++) top.push_back(net.nodes[ci.items[k]].label);
      name = join(top, " \xC2\xB7 ");
    }
    ci.autoName = name;
  }
  // strategic diagram quadrants (medians)
  vector<double> cen, den;
  for (auto& ci : C) { cen.push_back(ci.centrality); den.push_back(ci.density); }
  auto median = [](vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2; };
  double mc = median(cen), md = median(den);
  for (auto& ci : C) {
    bool hc = ci.centrality >= mc, hd = ci.density >= md;
    ci.quadrant = hc && hd ? "Motor themes" : (!hc && hd ? "Niche themes" : (!hc && !hd ? "Emerging or declining" : "Basic themes"));
  }
  return C;
}

void applyAutoNames(Network& net, const Corpus* corpus) {
  auto C = clusterInfo(net, corpus);
  net.clusterNames.assign(C.size(), "");
  for (auto& ci : C) net.clusterNames[ci.c] = truncate(ci.autoName, 44);
}

// ------------------------------------------------------------ actors
vector<string> unitList(const Record& r, Unit u) {
  switch (u) {
    case Unit::Authors: return r.authors;
    case Unit::Orgs: return r.affiliations;
    case Unit::Countries: return r.countries;
    case Unit::Sources: return r.source.empty() ? vector<string>{} : vector<string>{r.source};
    case Unit::Keywords: return r.keywords;
    case Unit::IndexTerms: return r.indexTerms;
    case Unit::AllKeywords: { auto v = r.keywords; v.insert(v.end(), r.indexTerms.begin(), r.indexTerms.end()); return v; }
    case Unit::Terms: return extractTerms(r);
    default: return {};
  }
}
string unitKey(const string& s, Unit u) { return u == Unit::Sources ? sourceKey(s) : bibKey(s); }

vector<Actor> topActors(const Corpus& c, Unit unit, int top) {
  std::unordered_map<string, Actor> A;
  std::unordered_map<string, vector<int>> cites;
  for (auto& r : c.recs) {
    std::unordered_set<string> seen;
    for (auto& x : unitList(r, unit)) {
      string lab = bibClean(x), k = unitKey(lab, unit);
      if (k.empty() || !seen.insert(k).second) continue;
      auto& a = A[k];
      if (a.label.empty()) a.label = lab;
      a.docs++;
      a.cites += r.cites;
      if (r.year) { a.firstYear = a.firstYear ? std::min(a.firstYear, r.year) : r.year; a.lastYear = std::max(a.lastYear, r.year); }
      cites[k].push_back(r.cites);
    }
  }
  vector<Actor> out;
  for (auto& kv : A) {
    Actor a = kv.second;
    auto& cv = cites[kv.first];
    std::sort(cv.begin(), cv.end(), std::greater<int>());
    int h = 0;
    for (size_t i = 0; i < cv.size(); i++) if (cv[i] >= int(i + 1)) h = int(i + 1);
    a.h = h;
    a.citesPerDoc = a.docs ? double(a.cites) / a.docs : 0;
    out.push_back(a);
  }
  std::sort(out.begin(), out.end(), [](const Actor& a, const Actor& b) { return a.docs != b.docs ? a.docs > b.docs : (a.cites != b.cites ? a.cites > b.cites : a.label < b.label); });
  if (int(out.size()) > top) out.resize(static_cast<size_t>(top));
  return out;
}

Bradford bradford(const Corpus& c) {
  Bradford B;
  std::unordered_map<string, std::pair<string, int>> m;
  int total = 0;
  for (auto& r : c.recs) {
    if (r.source.empty()) continue;
    auto& e = m[sourceKey(r.source)];
    if (e.first.empty()) e.first = r.source;
    e.second++;
    total++;
  }
  for (auto& kv : m) B.sources.emplace_back(kv.second.first, kv.second.second);
  std::sort(B.sources.begin(), B.sources.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  int acc = 0;
  B.zone1 = B.zone2 = int(B.sources.size());
  bool z1 = false;
  for (size_t i = 0; i < B.sources.size(); i++) {
    acc += B.sources[i].second;
    if (!z1 && acc >= total / 3.0) { B.zone1 = int(i + 1); z1 = true; }
    if (acc >= total * 2 / 3.0) { B.zone2 = int(i + 1); break; }
  }
  int n1 = B.zone1, n2 = B.zone2 - B.zone1, n3 = int(B.sources.size()) - B.zone2;
  B.k = n1 > 0 && n2 > 0 ? std::sqrt(double(n3 > 0 ? n3 : n2) / n1) : 0;
  return B;
}

Lotka lotka(const Corpus& c) {
  Lotka L;
  std::unordered_map<string, int> per;
  for (auto& r : c.recs) {
    std::unordered_set<string> seen;
    for (auto& a : r.authors) { string k = bibKey(a); if (!k.empty() && seen.insert(k).second) per[k]++; }
  }
  std::map<int, int> dist;
  for (auto& kv : per) dist[kv.second]++;
  for (auto& kv : dist) L.dist.emplace_back(kv.first, kv.second);
  // log-log least squares: log(y) = log(C) - n log(x)
  double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
  int n = 0;
  for (auto& p : L.dist) { double x = std::log(double(p.first)), y = std::log(double(p.second)); sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; n++; }
  if (n >= 2) {
    double b = (n * sxy - sx * sy) / std::max(1e-12, n * sxx - sx * sx);
    double a = (sy - b * sx) / n;
    L.exponent = -b;
    L.c = std::exp(a);
    double r = (n * sxy - sx * sy) / std::sqrt(std::max(1e-12, (n * sxx - sx * sx) * (n * syy - sy * sy)));
    L.r2 = r * r;
  }
  return L;
}

Growth growth(const Corpus& c) {
  Growth G;
  std::map<int, int> py;
  std::map<int, double> cy;
  for (auto& r : c.recs) if (r.year) { py[r.year]++; cy[r.year] += r.cites; }
  if (py.empty()) return G;
  G.y0 = py.begin()->first;
  G.y1 = py.rbegin()->first;
  for (int y = G.y0; y <= G.y1; y++) { G.perYear.emplace_back(y, py.count(y) ? py[y] : 0); G.citesPerYear.emplace_back(y, py.count(y) ? cy[y] / py[y] : 0); }
  int first = G.perYear.front().second, last = G.perYear.back().second;
  if (G.y1 > G.y0 && first > 0 && last > 0) G.cagr = std::pow(double(last) / first, 1.0 / (G.y1 - G.y0)) - 1;
  return G;
}

// ------------------------------------------------------------ bursts (Kleinberg, 2-state)
vector<Burst> detectBursts(const Corpus& c, Unit unit, int minOcc, double s, double gamma) {
  std::map<int, int> docsPerYear;
  std::unordered_map<string, std::map<int, int>> kwYear;
  std::unordered_map<string, string> labels;
  std::unordered_map<string, int> tot;
  for (auto& r : c.recs) {
    if (!r.year) continue;
    docsPerYear[r.year]++;
    std::unordered_set<string> seen;
    for (auto& x : unitList(r, unit)) {
      string lab = bibClean(x), k = unitKey(lab, unit);
      if (k.empty() || !seen.insert(k).second) continue;
      kwYear[k][r.year]++;
      labels.emplace(k, lab);
      tot[k]++;
    }
  }
  vector<Burst> out;
  if (docsPerYear.size() < 3) return out;
  vector<int> years;
  for (auto& kv : docsPerYear) years.push_back(kv.first);
  int T = int(years.size());
  double D = 0;
  for (auto& kv : docsPerYear) D += kv.second;
  for (auto& kv : kwYear) {
    if (tot[kv.first] < minOcc) continue;
    vector<double> r(static_cast<size_t>(T)), d(static_cast<size_t>(T));
    double R = 0;
    for (int t = 0; t < T; t++) { d[t] = docsPerYear[years[t]]; r[t] = kv.second.count(years[t]) ? kv.second.at(years[t]) : 0; R += r[t]; }
    double p0 = R / D, p1 = std::min(0.9999, s * p0);
    if (p0 <= 0) continue;
    auto cost = [&](int state, int t) {
      double p = state ? p1 : p0;
      // -log binomial likelihood (up to a constant)
      return -(r[t] * std::log(p) + (d[t] - r[t]) * std::log(1 - p));
    };
    double trans = gamma * std::log(double(T));
    vector<std::array<double, 2>> C(static_cast<size_t>(T));
    vector<std::array<int, 2>> back(static_cast<size_t>(T));
    C[0] = {cost(0, 0), cost(1, 0) + trans};
    for (int t = 1; t < T; t++)
      for (int j = 0; j < 2; j++) {
        double a = C[t - 1][0] + (j == 1 ? trans : 0), b = C[t - 1][1];
        if (a <= b) { C[t][j] = a + cost(j, t); back[t][j] = 0; }
        else { C[t][j] = b + cost(j, t); back[t][j] = 1; }
      }
    vector<int> st(static_cast<size_t>(T));
    Burst best;
    st[T - 1] = C[T - 1][0] <= C[T - 1][1] ? 0 : 1;
    for (int t = T - 1; t > 0; t--) st[t - 1] = back[t][st[t]];
    for (int t = 0; t < T; t++) {
      if (!st[t]) continue;
      int e = t;
      while (e + 1 < T && st[e + 1]) e++;
      double strength = 0;
      for (int k = t; k <= e; k++) strength += cost(0, k) - cost(1, k);
      if (strength > 0 && (best.label.empty() || strength > best.strength)) {
        Burst& b = best;
        b.label = labels[kv.first];
        b.start = years[t];
        b.end = years[e];
        b.strength = strength;
        b.total = tot[kv.first];
      }
      t = e;
    }
    if (!best.label.empty()) out.push_back(best);
  }
  std::sort(out.begin(), out.end(), [](const Burst& a, const Burst& b) { return a.strength > b.strength; });
  return out;
}

// ------------------------------------------------------------ thematic evolution
Evolution thematicEvolution(const Corpus& c, Unit unit, const vector<int>& cuts, int minOcc, const Thesaurus* th) {
  Evolution E;
  int y0 = 9999, y1 = 0;
  for (auto& r : c.recs) if (r.year) { y0 = std::min(y0, r.year); y1 = std::max(y1, r.year); }
  if (y1 < y0) return E;
  vector<int> cs = cuts;
  if (cs.empty()) {
    int span = y1 - y0 + 1;
    cs = {y0 + span / 3, y0 + 2 * span / 3};
  }
  int start = y0;
  for (int cut : cs) { if (cut > start && cut <= y1) { E.periods.emplace_back(start, cut - 1); start = cut; } }
  E.periods.emplace_back(start, y1);
  vector<std::unordered_map<string, int>> themeOfKw(E.periods.size());
  vector<int> themeIdx0;
  AnalysisEngine eng;
  eng.corpus = &c;
  if (th) eng.thesaurus = *th;
  for (size_t p = 0; p < E.periods.size(); p++) {
    AnaSpec s;
    s.type = AnaType::Cooc;
    s.unit = unit;
    s.setDefaults();
    s.min = minOcc;
    s.y0 = E.periods[p].first;
    s.y1 = E.periods[p].second;
    s.largest = false;
    s.maxItems = 250;
    Network net;
    try { net = eng.build(s); } catch (...) { themeIdx0.push_back(int(E.themes.size())); continue; }
    normalise(net, Norm::Association);
    ClusterOpts co;
    co.resolution = 1.0;
    co.starts = 3;
    co.minSize = 2;
    leiden(net, co);
    auto C = clusterInfo(net, nullptr);
    themeIdx0.push_back(int(E.themes.size()));
    for (auto& ci : C) {
      EvoTheme t;
      t.period = int(p);
      t.size = ci.size;
      t.weight = ci.weight;
      for (size_t k = 0; k < ci.items.size(); k++) {
        if (k < 8) t.keywords.push_back(net.nodes[ci.items[k]].label);
        themeOfKw[p][net.nodes[ci.items[k]].key] = int(E.themes.size());
      }
      t.name = ci.items.empty() ? "" : net.nodes[ci.items[0]].label;
      E.themes.push_back(t);
    }
  }
  // flows between consecutive periods: shared keywords (inclusion index weight)
  for (size_t p = 0; p + 1 < E.periods.size(); p++) {
    std::map<std::pair<int, int>, double> fl;
    for (auto& kv : themeOfKw[p]) {
      auto it = themeOfKw[p + 1].find(kv.first);
      if (it != themeOfKw[p + 1].end()) fl[{kv.second, it->second}] += 1;
    }
    for (auto& kv : fl) {
      double inc = kv.second / std::max(1, std::min(E.themes[kv.first.first].size, E.themes[kv.first.second].size));
      if (inc < 0.05) continue;
      E.flows.push_back({kv.first.first, kv.first.second, kv.second});
    }
  }
  return E;
}

// ------------------------------------------------------------ compare periods
Comparison comparePeriods(const Corpus& c, Unit unit, int a0, int a1, int b0, int b1, int minOcc) {
  Comparison R;
  R.a0 = a0; R.a1 = a1; R.b0 = b0; R.b1 = b1;
  std::unordered_map<string, std::pair<int, int>> cnt;
  std::unordered_map<string, string> lab;
  for (auto& r : c.recs) {
    int side = (r.year >= a0 && r.year <= a1) ? 0 : ((r.year >= b0 && r.year <= b1) ? 1 : -1);
    if (side < 0) continue;
    (side ? R.nB : R.nA)++;
    std::unordered_set<string> seen;
    for (auto& x : unitList(r, unit)) {
      string l = bibClean(x), k = unitKey(l, unit);
      if (k.empty() || !seen.insert(k).second) continue;
      (side ? cnt[k].second : cnt[k].first)++;
      lab.emplace(k, l);
    }
  }
  for (auto& kv : cnt) {
    if (kv.second.first + kv.second.second < minOcc) continue;
    CompareRow row;
    row.label = lab[kv.first];
    row.a = kv.second.first;
    row.b = kv.second.second;
    row.shareA = R.nA ? double(row.a) / R.nA : 0;
    row.shareB = R.nB ? double(row.b) / R.nB : 0;
    row.logRatio = std::log2((row.shareB + 0.5 / std::max(1, R.nB)) / (row.shareA + 0.5 / std::max(1, R.nA)));
    if (row.logRatio > 0.58) R.emerging.push_back(row);
    else if (row.logRatio < -0.58) R.declining.push_back(row);
    else R.stable.push_back(row);
  }
  auto by = [](const CompareRow& x, const CompareRow& y) { return std::fabs(x.logRatio) * (x.a + x.b) > std::fabs(y.logRatio) * (y.a + y.b); };
  std::sort(R.emerging.begin(), R.emerging.end(), by);
  std::sort(R.declining.begin(), R.declining.end(), by);
  std::sort(R.stable.begin(), R.stable.end(), [](auto& x, auto& y) { return x.a + x.b > y.a + y.b; });
  return R;
}

// ------------------------------------------------------------ three-field plot
ThreeField threeField(const Corpus& c, Unit l, Unit m, Unit r, int topN) {
  ThreeField F;
  auto topOf = [&](Unit u, vector<string>& names, vector<int>& counts, std::unordered_map<string, int>& idx) {
    auto act = topActors(c, u, topN);
    for (auto& a : act) { idx[unitKey(a.label, u)] = int(names.size()); names.push_back(a.label); counts.push_back(a.docs); }
  };
  std::unordered_map<string, int> il, im, ir;
  topOf(l, F.left, F.leftN, il);
  topOf(m, F.mid, F.midN, im);
  topOf(r, F.right, F.rightN, ir);
  std::map<std::pair<int, int>, int> lm, mr;
  for (auto& rec : c.recs) {
    std::set<int> L, M, R;
    for (auto& x : unitList(rec, l)) { auto it = il.find(unitKey(bibClean(x), l)); if (it != il.end()) L.insert(it->second); }
    for (auto& x : unitList(rec, m)) { auto it = im.find(unitKey(bibClean(x), m)); if (it != im.end()) M.insert(it->second); }
    for (auto& x : unitList(rec, r)) { auto it = ir.find(unitKey(bibClean(x), r)); if (it != ir.end()) R.insert(it->second); }
    for (int a : L) for (int b : M) lm[{a, b}]++;
    for (int a : M) for (int b : R) mr[{a, b}]++;
  }
  for (auto& kv : lm) F.lm.emplace_back(kv.first.first, kv.first.second, kv.second);
  for (auto& kv : mr) F.mr.emplace_back(kv.first.first, kv.first.second, kv.second);
  return F;
}

// ------------------------------------------------------------ stability
Stability clusterStability(const Network& net, const ClusterOpts& base, int runs) {
  Stability S;
  S.runs = runs;
  int n = net.n();
  vector<int> ref(static_cast<size_t>(n));
  if (net.clusterTag != clusterTag(base)) {
    Network c = net;
    leiden(c, base, &ref);
    S.freshReference = true;
  } else {
    for (int i = 0; i < n; i++) ref[i] = net.nodes[i].cluster;
  }
  S.itemStability.assign(static_cast<size_t>(n), 0);
  S.minAri = 1;
  Network work = net;
  for (int r = 0; r < runs; r++) {
    ClusterOpts o = base;
    o.seed = base.seed + 1000 + r * 17;
    o.starts = 1;
    vector<int> lab;
    leiden(work, o, &lab);
    auto ag = partitionAgreement(ref, lab);
    S.meanAri += ag.ari;
    S.meanNmi += ag.nmi;
    S.minAri = std::min(S.minAri, ag.ari);
    // item stability: share of its reference-cluster mates it stays with
    std::unordered_map<int, std::unordered_map<int, int>> overlap;
    std::unordered_map<int, int> refSize;
    for (int i = 0; i < n; i++) { overlap[ref[i]][lab[i]]++; refSize[ref[i]]++; }
    for (int i = 0; i < n; i++) S.itemStability[i] += double(overlap[ref[i]][lab[i]]) / std::max(1, refSize[ref[i]]);
  }
  if (runs > 0) { S.meanAri /= runs; S.meanNmi /= runs; for (auto& v : S.itemStability) v /= runs; }
  return S;
}

NetSummary netSummary(const Network& net) {
  NetSummary s;
  s.n = net.n();
  s.m = net.m();
  s.clusters = net.nClusters;
  s.q = net.quality;
  vector<int> par(static_cast<size_t>(s.n)), deg(static_cast<size_t>(s.n), 0);
  for (int i = 0; i < s.n; i++) par[i] = i;
  std::function<int(int)> find = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
  for (auto& l : net.links) { s.totalStrength += l.w; deg[l.a]++; deg[l.b]++; int a = find(l.a), b = find(l.b); if (a != b) par[a] = b; }
  std::unordered_set<int> roots;
  for (int i = 0; i < s.n; i++) { roots.insert(find(i)); if (!deg[i]) s.isolated++; }
  s.components = int(roots.size());
  s.density = s.n > 1 ? 2.0 * s.m / (double(s.n) * (s.n - 1)) : 0;
  s.avgDegree = s.n ? 2.0 * s.m / s.n : 0;
  return s;
}

// ------------------------------------------------------------ methods text
string methodsParagraph(const Corpus& c, const AnaSpec& spec, const Network& net, Norm norm, const LayoutOpts& lo, const ClusterOpts& co, bool bundled) {
  auto q = qualityReport(c);
  string src;
  std::set<string> fm;
  for (auto& f : c.files) fm.insert(formatLabel(f.format));
  if (fm.empty() && c.format != BibFormat::Unknown) fm.insert(formatLabel(c.format));
  vector<string> fv(fm.begin(), fm.end());
  src = fv.empty() ? "a bibliographic database" : join(fv, " and ");
  string o;
  if (!c.recs.empty()) {
    o += "Bibliographic records were retrieved from " + src + ", yielding " + fmtInt(q.records) + " documents";
    if (q.yearMin) o += " published " + std::to_string(q.yearMin) + "\xE2\x80\x93" + std::to_string(q.yearMax);
    o += ".";
    if (c.duplicatesRemoved) o += " " + fmtInt(c.duplicatesRemoved) + " duplicate records were removed (matched by DOI, then by normalised title and year).";
    o += " ";
    Measure M = anaMeasure(spec);
    string type = lower(typeInfo(spec.type).label);
    string art = (type[0] == 'a' || type[0] == 'e' || type[0] == 'i' || type[0] == 'o' || type[0] == 'u') ? "An " : "A ";
    o += art + type + " map of " + lower(unitLabel(spec.type, spec.unit)) + " was built with " + (spec.fractional ? "fractional" : "full") + " counting, retaining " +
         unitNounPlural(spec.unit) + " with at least " + std::to_string(spec.min) + " " + M.label;
    if (M.secondary && spec.minCites > 0) o += " and " + std::to_string(spec.minCites) + " citations";
    if (spec.type == AnaType::Coauth && spec.maxAuth > 0) o += ", ignoring documents with more than " + std::to_string(spec.maxAuth) + " authors";
    if (spec.y0 || spec.y1) o += ", for documents published " + (spec.y0 ? std::to_string(spec.y0) : string("")) + "\xE2\x80\x93" + (spec.y1 ? std::to_string(spec.y1) : string(""));
    o += " (" + fmtInt(net.n()) + " " + unitNounPlural(spec.unit, net.n()) + ", " + fmtInt(net.m()) + " links).";
  } else {
    o += "The map contains " + fmtInt(net.n()) + " items and " + fmtInt(net.m()) + " links.";
  }
  o += " Link strengths were normalised with the ";
  o += norm == Norm::Association ? "association strength method (van Eck & Waltman, 2009)." : (norm == Norm::Fractionalization ? "fractionalization method." : (norm == Norm::LinLog ? "LinLog/modularity normalisation." : "raw counts (no normalisation)."));
  o += " Items were positioned with the VOS mapping technique (attraction " + fmtNum(lo.attraction, 1) + ", repulsion " + fmtNum(lo.repulsion, 1) + "; best of " +
       std::to_string(lo.starts) + " random starts; van Eck et al., 2010).";
  o += " Clusters were identified with the Leiden algorithm (Traag et al., 2019), optimising " +
       string(co.vosQuality ? "the VOS clustering quality function (Waltman et al., 2010)" : "modularity") + " on the " + string(co.normalizedWeights ? "normalised" : "raw") +
       " link strengths, with resolution " + fmtNum(co.resolution, 2) + ", " + std::to_string(co.starts) + " random starts and seed " + std::to_string(co.seed) +
       (co.minSize > 1 ? ", merging clusters smaller than " + std::to_string(co.minSize) + " items" : string("")) + ", giving " + std::to_string(net.nClusters) +
       " clusters (modularity Q = " + fmtFixed(net.quality, 3) + ").";
  if (bundled) o += " Links were drawn with force-directed edge bundling (Holten & van Wijk, 2009).";
  o += " The analysis was performed with VOSStudio Native " + appVersion() + " (VOSStudio Native, " + currentYear() + ").";
  o += string("\n\nReferences\n"
       "Holten, D., & van Wijk, J. J. (2009). Force-directed edge bundling for graph visualization. Computer Graphics Forum, 28(3), 983\xE2\x80\x93" "990.\n"
       "Traag, V. A., Waltman, L., & van Eck, N. J. (2019). From Louvain to Leiden: guaranteeing well-connected communities. Scientific Reports, 9, 5233.\n"
       "van Eck, N. J., & Waltman, L. (2009). How to normalize cooccurrence data? Journal of the American Society for Information Science and Technology, 60(8), 1635\xE2\x80\x93" "1651.\n"
       "van Eck, N. J., Waltman, L., Dekker, R., & van den Berg, J. (2010). A comparison of two techniques for bibliometric mapping: Multidimensional scaling and VOS. Journal of the American Society for Information Science and Technology, 61(12), 2405\xE2\x80\x93" "2416.\n"
       "Waltman, L., van Eck, N. J., & Noyons, E. C. M. (2010). A unified approach to mapping and clustering of bibliometric networks. Journal of Informetrics, 4(4), 629\xE2\x80\x93" "635.\n") + softwareReference(false);
  if (!bundled) {
    size_t p = o.find("Holten, D.");
    size_t e = o.find('\n', p);
    if (p != string::npos) o.erase(p, e - p + 1);
  }
  return o;
}

}  // namespace vs
