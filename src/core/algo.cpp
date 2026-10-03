#include "algo.h"

#include <atomic>
#include <thread>

#include <deque>
#include <queue>

namespace vs {

const char* normLabel(Norm n) {
  switch (n) {
    case Norm::Association: return "Association strength";
    case Norm::Fractionalization: return "Fractionalization";
    case Norm::LinLog: return "LinLog / modularity";
    default: return "No normalization";
  }
}

void normalise(Network& net, Norm how) {
  int n = net.n();
  vector<double> k(static_cast<size_t>(n), 0);
  double m2 = 0;
  for (auto& l : net.links) { k[l.a] += l.w; k[l.b] += l.w; m2 += 2 * l.w; }
  for (auto& l : net.links) {
    double ka = std::max(1e-12, k[l.a]), kb = std::max(1e-12, k[l.b]);
    switch (how) {
      case Norm::Association: l.s = m2 * l.w / (ka * kb); break;
      case Norm::Fractionalization: l.s = l.w / ka + l.w / kb; break;
      case Norm::LinLog: l.s = l.w / std::sqrt(ka * kb); break;
      default: l.s = l.w;
    }
  }
}

// ------------------------------------------------------------ CSR
namespace {
struct CSR {
  int n = 0;
  vector<int> off, nb;
  vector<double> w;
};
CSR makeCSR(int n, const vector<int>& ea, const vector<int>& eb, const vector<double>& ew) {
  CSR g;
  g.n = n;
  g.off.assign(static_cast<size_t>(n + 1), 0);
  for (size_t e = 0; e < ea.size(); e++) { if (ea[e] == eb[e]) continue; g.off[ea[e] + 1]++; g.off[eb[e] + 1]++; }
  for (int i = 0; i < n; i++) g.off[i + 1] += g.off[i];
  g.nb.assign(static_cast<size_t>(g.off[n]), 0);
  g.w.assign(static_cast<size_t>(g.off[n]), 0);
  vector<int> pos(g.off.begin(), g.off.end() - 1);
  for (size_t e = 0; e < ea.size(); e++) {
    int a = ea[e], b = eb[e];
    if (a == b) continue;
    g.nb[pos[a]] = b; g.w[pos[a]++] = ew[e];
    g.nb[pos[b]] = a; g.w[pos[b]++] = ew[e];
  }
  return g;
}
int renumber(vector<int>& part) {
  int mx = 0;
  for (int c : part) mx = std::max(mx, c);
  vector<int> map(static_cast<size_t>(mx + 1), -1);
  int k = 0;
  for (auto& c : part) { if (map[c] < 0) map[c] = k++; c = map[c]; }
  return k;
}
vector<int> shuffled(int len, Rng& rnd) {
  vector<int> o(static_cast<size_t>(len));
  for (int i = 0; i < len; i++) o[i] = i;
  for (int i = len - 1; i > 0; i--) { int j = int(std::floor(rnd() * (i + 1))); std::swap(o[i], o[j]); }
  return o;
}
}  // namespace

// ------------------------------------------------------------ Leiden
vector<int> leidenLabels(int n, const vector<int>& ea, const vector<int>& eb, const vector<double>& ew, const ClusterOpts& o, double* qOut) {
  double gamma = o.resolution, theta = o.randomness;
  int maxIter = std::max(1, o.iterations), starts = std::max(1, o.starts);
  if (!n) return {};
  CSR G0 = makeCSR(n, ea, eb, ew);
  vector<double> k0(static_cast<size_t>(n), 0);
  double m2 = 0;
  for (int i = 0; i < n; i++) { double s = 0; for (int e = G0.off[i]; e < G0.off[i + 1]; e++) s += G0.w[e]; k0[i] = s; m2 += s; }
  // VOS quality: node weights 1, the resolution applies to the normalised link weights directly.
  // Modularity: node weights are strengths and the resolution is scaled by 1/2m.
  const bool cpm = o.vosQuality;
  if (cpm) std::fill(k0.begin(), k0.end(), 1.0);
  const double sc = cpm ? 1.0 : 1.0 / std::max(1e-300, m2);
  if (m2 <= 0) { vector<int> l(static_cast<size_t>(n)); for (int i = 0; i < n; i++) l[i] = i; if (qOut) *qOut = 0; return l; }

  auto quality = [&](const vector<int>& labels) {
    vector<double> K(static_cast<size_t>(n), 0), IN(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; i++) {
      int c = labels[i];
      K[c] += k0[i];
      for (int e = G0.off[i]; e < G0.off[i + 1]; e++) if (labels[G0.nb[e]] == c) IN[c] += G0.w[e];
    }
    double q = 0;
    for (int c = 0; c < n; c++) if (K[c] > 0) q += IN[c] - gamma * K[c] * K[c] * sc;
    return q / m2;
  };
  auto moveNodesFast = [&](const CSR& g, const vector<double>& kw, int nn, vector<int>& part, Rng& rnd) {
    vector<double> K(static_cast<size_t>(nn), 0);
    vector<int> size(static_cast<size_t>(nn), 0);
    for (int v = 0; v < nn; v++) { K[part[v]] += kw[v]; size[part[v]]++; }
    vector<int> empty;
    for (int c = nn - 1; c >= 0; c--) if (size[c] == 0) empty.push_back(c);
    vector<int> queue = shuffled(nn, rnd);
    vector<char> inQ(static_cast<size_t>(nn), 1);
    int qh = 0, qt = 0, qlen = nn;
    vector<double> wTo(static_cast<size_t>(nn), 0);
    vector<int> mark(static_cast<size_t>(nn), -1), touched(static_cast<size_t>(nn), 0);
    int stamp = 0;
    while (qlen > 0) {
      int v = queue[qh];
      qh = (qh + 1) % nn;
      qlen--;
      inQ[v] = 0;
      int cv = part[v];
      stamp++;
      int nt = 0;
      for (int e = g.off[v]; e < g.off[v + 1]; e++) {
        int c = part[g.nb[e]];
        if (mark[c] != stamp) { mark[c] = stamp; wTo[c] = 0; touched[nt++] = c; }
        wTo[c] += g.w[e];
      }
      K[cv] -= kw[v];
      size[cv]--;
      if (size[cv] == 0) empty.push_back(cv);
      int best = cv;
      double bestGain = (mark[cv] == stamp ? wTo[cv] : 0) - gamma * kw[v] * K[cv] * sc;
      for (int t = 0; t < nt; t++) {
        int c = touched[t];
        if (c == cv) continue;
        double gain = wTo[c] - gamma * kw[v] * K[c] * sc;
        if (gain > bestGain) { bestGain = gain; best = c; }
      }
      if (bestGain < 0 && !empty.empty()) { best = empty.back(); bestGain = 0; }
      if (size[best] == 0 && !empty.empty() && empty.back() == best) empty.pop_back();
      else if (size[best] == 0) empty.erase(std::remove(empty.begin(), empty.end(), best), empty.end());
      part[v] = best;
      K[best] += kw[v];
      size[best]++;
      if (best != cv) {
        for (int e = g.off[v]; e < g.off[v + 1]; e++) {
          int u = g.nb[e];
          if (!inQ[u] && part[u] != best) { inQ[u] = 1; queue[qt] = u; qt = (qt + 1) % nn; qlen++; }
        }
      }
    }
  };
  auto refine = [&](const CSR& g, const vector<double>& kw, int nn, const vector<int>& part, int ncom, Rng& rnd) {
    vector<int> ref(static_cast<size_t>(nn)), rsize(static_cast<size_t>(nn), 1);
    for (int v = 0; v < nn; v++) ref[v] = v;
    vector<double> Kc(static_cast<size_t>(ncom), 0);
    for (int v = 0; v < nn; v++) Kc[part[v]] += kw[v];
    vector<double> Kr = kw, ext(static_cast<size_t>(nn), 0);
    for (int v = 0; v < nn; v++) { double s = 0; for (int e = g.off[v]; e < g.off[v + 1]; e++) if (part[g.nb[e]] == part[v]) s += g.w[e]; ext[v] = s; }
    vector<int> order = shuffled(nn, rnd);
    vector<double> wTo(static_cast<size_t>(nn), 0);
    vector<int> mark(static_cast<size_t>(nn), -1), touched(static_cast<size_t>(nn), 0);
    vector<int> cand;
    vector<double> cw;
    for (int oi = 0; oi < nn; oi++) {
      int v = order[oi], rv = ref[v];
      if (rsize[rv] != 1) continue;
      int C = part[v];
      if (ext[v] < gamma * kw[v] * (Kc[C] - kw[v]) * sc) continue;
      int nt = 0;
      for (int e = g.off[v]; e < g.off[v + 1]; e++) {
        int u = g.nb[e];
        if (part[u] != C) continue;
        int r = ref[u];
        if (mark[r] != v) { mark[r] = v; wTo[r] = 0; touched[nt++] = r; }
        wTo[r] += g.w[e];
      }
      cand.clear(); cw.clear();
      cand.push_back(rv); cw.push_back(0);
      double gmax = 0;
      for (int t = 0; t < nt; t++) {
        int r = touched[t];
        if (r == rv) continue;
        if (ext[r] < gamma * Kr[r] * (Kc[C] - Kr[r]) * sc) continue;
        double gain = wTo[r] - gamma * kw[v] * Kr[r] * sc;
        if (gain < 0) continue;
        cand.push_back(r); cw.push_back(gain);
        if (gain > gmax) gmax = gain;
      }
      if (cand.size() == 1) continue;
      double tot = 0;
      for (size_t i = 0; i < cand.size(); i++) { cw[i] = std::exp((cpm ? (cw[i] - gmax) : 2 * (cw[i] - gmax) / m2) / theta); tot += cw[i]; }
      double x = rnd() * tot;
      int pick = cand.back();
      for (size_t i = 0; i < cand.size(); i++) { x -= cw[i]; if (x <= 0) { pick = cand[i]; break; } }
      if (pick == rv) continue;
      double wvr = mark[pick] == v ? wTo[pick] : 0;
      ext[pick] = ext[pick] + ext[v] - 2 * wvr;
      Kr[pick] += kw[v];
      rsize[pick]++;
      rsize[rv] = 0; Kr[rv] = 0; ext[rv] = 0;
      ref[v] = pick;
    }
    return ref;
  };
  auto aggregate = [&](const CSR& g, const vector<double>& kw, int nn, const vector<int>& ref, int nref, vector<double>& kOut) {
    kOut.assign(static_cast<size_t>(nref), 0);
    for (int v = 0; v < nn; v++) kOut[ref[v]] += kw[v];
    std::unordered_map<uint64_t, double> acc;
    for (int v = 0; v < nn; v++) {
      int a = ref[v];
      for (int e = g.off[v]; e < g.off[v + 1]; e++) {
        int b = ref[g.nb[e]];
        if (a >= b) continue;
        acc[(uint64_t(a) << 32) | uint32_t(b)] += g.w[e];
      }
    }
    vector<int> A, B;
    vector<double> W;
    A.reserve(acc.size()); B.reserve(acc.size()); W.reserve(acc.size());
    // deterministic order
    vector<std::pair<uint64_t, double>> es(acc.begin(), acc.end());
    std::sort(es.begin(), es.end());
    for (auto& kv : es) { A.push_back(int(kv.first >> 32)); B.push_back(int(kv.first & 0xffffffffu)); W.push_back(kv.second); }
    return makeCSR(nref, A, B, W);
  };
  auto leidenOnce = [&](const vector<int>& init, Rng& rnd) {
    CSR g = G0;
    vector<double> kw = k0;
    int nn = n;
    vector<int> part = init;
    renumber(part);
    vector<int> map(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) map[i] = i;
    for (int level = 0; level < 40; level++) {
      moveNodesFast(g, kw, nn, part, rnd);
      int ncom = renumber(part);
      if (ncom == nn) break;
      vector<int> ref = refine(g, kw, nn, part, ncom, rnd);
      int nref = renumber(ref);
      if (nref == nn) { ref = part; nref = ncom; }
      vector<double> kNew;
      CSR agg = aggregate(g, kw, nn, ref, nref, kNew);
      vector<int> np(static_cast<size_t>(nref), 0);
      for (int v = 0; v < nn; v++) np[ref[v]] = part[v];
      for (int i = 0; i < n; i++) map[i] = ref[map[i]];
      g = std::move(agg);
      kw = std::move(kNew);
      nn = nref;
      part = std::move(np);
    }
    vector<int> labels(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) labels[i] = part[map[i]];
    renumber(labels);
    return labels;
  };
  vector<int> best;
  double bestQ = -1e300;
  for (int s = 0; s < starts; s++) {
    Rng rnd(uint32_t((o.seed + 1) * 7919 + s * 104729));
    vector<int> labels(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) labels[i] = i;
    double q = -1e300;
    for (int it = 0; it < maxIter; it++) {
      vector<int> next = leidenOnce(labels, rnd);
      double qn = quality(next);
      if (qn <= q + 1e-12) break;
      labels = next;
      q = qn;
    }
    if (q > bestQ) { bestQ = q; best = labels; }
  }
  if (qOut) *qOut = bestQ;
  return best;
}

double modularity(const Network& net, const vector<int>& labels, double gamma, bool normalized) {
  int n = net.n();
  vector<double> K(static_cast<size_t>(n + 1), 0), IN(static_cast<size_t>(n + 1), 0), k(static_cast<size_t>(n), 0);
  double m2 = 0;
  for (auto& l : net.links) { double w = normalized ? l.s : l.w; k[l.a] += w; k[l.b] += w; m2 += 2 * w; if (labels[l.a] == labels[l.b]) IN[labels[l.a]] += 2 * w; }
  if (m2 <= 0) return 0;
  for (int i = 0; i < n; i++) K[labels[i]] += k[i];
  double q = 0;
  for (int c = 0; c <= n; c++) if (K[c] > 0) q += IN[c] - gamma * K[c] * K[c] / m2;
  return q / m2;
}

string clusterTag(const ClusterOpts& o) {
  return string(o.vosQuality ? "vos" : "mod") + ":" + fmtNum(o.resolution, 3) + ":" + std::to_string(o.minSize);
}

double leiden(Network& net, const ClusterOpts& o, vector<int>* labelsOut) {
  int n = net.n();
  vector<int> ea, eb;
  vector<double> ew;
  for (auto& l : net.links) { ea.push_back(l.a); eb.push_back(l.b); ew.push_back(o.normalizedWeights ? l.s : l.w); }
  double q = 0;
  vector<int> lab = leidenLabels(n, ea, eb, ew, o, &q);
  // Merge clusters smaller than minSize into their most-connected eligible neighbour.
  // Build adjacency once: scanning the entire edge list for every small node made this O(VE)
  // on large sparse maps, even though only incident links can affect a node's destination.
  if (o.minSize > 1) {
    vector<vector<std::pair<int, double>>> adj(static_cast<size_t>(n));
    for (size_t e = 0; e < ea.size(); e++) {
      int a = ea[e], b = eb[e];
      if (a == b) continue;
      adj[size_t(a)].push_back({b, ew[e]});
      adj[size_t(b)].push_back({a, ew[e]});
    }
    vector<int> size(static_cast<size_t>(n), 0), seen(static_cast<size_t>(n), -1), touched;
    vector<double> conn(static_cast<size_t>(n), 0);
    int stamp = 0;
    for (int pass = 0; pass < 3; pass++) {
      std::fill(size.begin(), size.end(), 0);
      for (int c : lab) if (c >= 0 && c < n) size[size_t(c)]++;
      bool changed = false;
      for (int i = 0; i < n; i++) {
        int own = lab[size_t(i)];
        if (own < 0 || own >= n || size[size_t(own)] >= o.minSize) continue;
        ++stamp;
        touched.clear();
        for (const auto& edge : adj[size_t(i)]) {
          int c = lab[size_t(edge.first)];
          if (c == own || c < 0 || c >= n) continue;
          if (seen[size_t(c)] != stamp) { seen[size_t(c)] = stamp; conn[size_t(c)] = 0; touched.push_back(c); }
          conn[size_t(c)] += edge.second;
        }
        int best = -1;
        double bw = -1;
        for (int c : touched) if (size[size_t(c)] >= o.minSize && conn[size_t(c)] > bw) { bw = conn[size_t(c)]; best = c; }
        if (best >= 0) { size[size_t(own)]--; lab[size_t(i)] = best; size[size_t(best)]++; changed = true; }
      }
      if (!changed) break;
    }
  }
  // order clusters by size (largest first), ties by first appearance
  std::unordered_map<int, int> size;
  for (int c : lab) size[c]++;
  vector<int> ids;
  for (auto& kv : size) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end(), [&](int a, int b) { return size[a] != size[b] ? size[a] > size[b] : a < b; });
  std::unordered_map<int, int> remap;
  for (size_t i = 0; i < ids.size(); i++) remap[ids[i]] = int(i);
  for (int i = 0; i < n; i++) { lab[i] = remap[lab[i]]; net.nodes[i].cluster = lab[i]; }
  net.countClusters();
  net.quality = modularity(net, lab, o.resolution, o.normalizedWeights);
  net.clusterTag = clusterTag(o);
  if (labelsOut) *labelsOut = lab;
  return net.quality;
}

Agreement partitionAgreement(const vector<int>& a, const vector<int>& b) {
  Agreement r;
  size_t n = a.size();
  if (!n || b.size() != n) return r;
  std::map<std::pair<int, int>, double> ct;
  std::map<int, double> ca, cb;
  for (size_t i = 0; i < n; i++) { ct[{a[i], b[i]}]++; ca[a[i]]++; cb[b[i]]++; }
  double N = double(n), I = 0, Ha = 0, Hb = 0;
  for (auto& kv : ct) I += kv.second / N * std::log(kv.second * N / (ca[kv.first.first] * cb[kv.first.second]));
  for (auto& kv : ca) Ha -= kv.second / N * std::log(kv.second / N);
  for (auto& kv : cb) Hb -= kv.second / N * std::log(kv.second / N);
  r.nmi = (Ha + Hb) > 0 ? 2 * I / (Ha + Hb) : 1;
  auto c2 = [](double x) { return x * (x - 1) / 2; };
  double sij = 0, sa = 0, sb = 0;
  for (auto& kv : ct) sij += c2(kv.second);
  for (auto& kv : ca) sa += c2(kv.second);
  for (auto& kv : cb) sb += c2(kv.second);
  double ex = sa * sb / c2(N), mx = (sa + sb) / 2;
  r.ari = mx - ex == 0 ? 1 : (sij - ex) / (mx - ex);
  return r;
}

// ------------------------------------------------------------ VOS layout
namespace {
constexpr int kBHLeafCapacity = 8;
constexpr int kBHMaxDepth = 28;

struct BHNode {
  double cx = 0, cy = 0, cz = 0, half = 0;
  double sum[3] = {0, 0, 0};
  int mass = 0, begin = 0, end = 0;
  int child[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  bool leaf = true;
};

// Rebuilt from a coordinate snapshot once per large-map iteration. Queries use exact leaf interactions and a
// monopole (count + centre of mass) for sufficiently distant cells. The target-containing branch is never collapsed,
// so the target is excluded exactly rather than exerting a force on itself.
class BarnesHutTree {
 public:
  BarnesHutTree(const vector<double>& xy, int n, int dimensions) : xy_(xy), n_(n), d_(dimensions) {
    order_.resize(size_t(n_));
    tmp_.resize(size_t(n_));
    for (int i = 0; i < n_; i++) order_[size_t(i)] = i;
    double lo[3] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    double hi[3] = {-lo[0], -lo[1], -lo[2]};
    for (int i = 0; i < n_; i++) for (int k = 0; k < d_; k++) {
      double v = xy_[size_t(i) * d_ + k];
      lo[k] = std::min(lo[k], v); hi[k] = std::max(hi[k], v);
    }
    double c[3] = {0, 0, 0}, span = 0;
    for (int k = 0; k < d_; k++) { c[k] = (lo[k] + hi[k]) * 0.5; span = std::max(span, hi[k] - lo[k]); }
    double half = std::max(1e-9, span * 0.5000001 + 1e-12);
    nodes_.reserve(size_t(n_) * 2);
    root_ = n_ ? build(0, n_, c[0], c[1], c[2], half, 0) : -1;
  }

  // Adds -sum_j ((x_i-x_j) / d^(2-r)) to gradient and the unweighted repulsion potential sum_j U(d).
  void repulsion(int i, double exponent, double theta, double gradient[3], double& potential) const {
    if (root_ >= 0) visit(root_, i, true, exponent, theta, gradient, potential);
  }

 private:
  const vector<double>& xy_;
  int n_ = 0, d_ = 2, root_ = -1;
  vector<BHNode> nodes_;
  vector<int> order_, tmp_;

  int octant(int i, double cx, double cy, double cz) const {
    const size_t p = size_t(i) * d_;
    return (xy_[p] >= cx ? 1 : 0) | (xy_[p + 1] >= cy ? 2 : 0) | (d_ == 3 && xy_[p + 2] >= cz ? 4 : 0);
  }

  int build(int begin, int end, double cx, double cy, double cz, double half, int depth) {
    const int ix = int(nodes_.size());
    nodes_.push_back(BHNode{});
    BHNode& first = nodes_[size_t(ix)];
    first.cx = cx; first.cy = cy; first.cz = cz; first.half = half;
    first.begin = begin; first.end = end; first.mass = end - begin;
    for (int q = begin; q < end; q++) {
      int i = order_[size_t(q)];
      for (int k = 0; k < d_; k++) first.sum[k] += xy_[size_t(i) * d_ + k];
    }
    if (end - begin <= kBHLeafCapacity || depth >= kBHMaxDepth) return ix;

    int counts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int occupied = 0, only = -1;
    for (int q = begin; q < end; q++) {
      int b = octant(order_[size_t(q)], cx, cy, cz);
      if (!counts[b]++) { occupied++; only = b; }
    }
    // Coincident points have no useful subdivision. They are handled as one aggregate (excluding the target below).
    if (occupied <= 1) {
      bool identical = true;
      const int p0 = order_[size_t(begin)];
      for (int q = begin + 1; q < end && identical; q++) {
        const int p = order_[size_t(q)];
        for (int k = 0; k < d_; k++) if (xy_[size_t(p) * d_ + k] != xy_[size_t(p0) * d_ + k]) { identical = false; break; }
      }
      if (identical) return ix;
      (void)only;
    }

    int offset[8], cursor[8];
    int at = begin;
    for (int b = 0; b < 8; b++) { offset[b] = cursor[b] = at; at += counts[b]; }
    for (int q = begin; q < end; q++) {
      int i = order_[size_t(q)], b = octant(i, cx, cy, cz);
      tmp_[size_t(cursor[b]++)] = i;
    }
    std::copy(tmp_.begin() + begin, tmp_.begin() + end, order_.begin() + begin);

    nodes_[size_t(ix)].leaf = false;
    const double ch = half * 0.5;
    for (int b = 0; b < 8; b++) if (counts[b]) {
      double x = cx + (b & 1 ? ch : -ch), y = cy + (b & 2 ? ch : -ch);
      double z = d_ == 3 ? cz + (b & 4 ? ch : -ch) : 0;
      int child = build(offset[b], offset[b] + counts[b], x, y, z, ch, depth + 1);
      nodes_[size_t(ix)].child[b] = child;
    }
    return ix;
  }

  static double potentialAt(double d, double exponent) {
    return exponent == 0 ? std::log(d) : exponent == 1 ? d : std::pow(d, exponent) / exponent;
  }
  static double forceFactor(double d, double exponent) { return exponent == 0 ? 1 / (d * d) : exponent == 1 ? 1 / d : std::pow(d, exponent - 2); }

  void aggregate(const BHNode& node, int target, bool contains, double exponent, double gradient[3], double& potential) const {
    int mass = node.mass - (contains ? 1 : 0);
    if (mass <= 0) return;
    const size_t ti = size_t(target) * d_;
    double dx[3] = {0, 0, 0}, center[3] = {0, 0, 0}, d2 = 1e-24;
    for (int k = 0; k < d_; k++) {
      double sum = node.sum[k] - (contains ? xy_[ti + k] : 0.0);
      center[k] = sum / mass;
      dx[k] = xy_[ti + k] - center[k];
      d2 += dx[k] * dx[k];
    }
    double d = std::sqrt(d2), f = double(mass) * forceFactor(d, exponent);
    for (int k = 0; k < d_; k++) gradient[k] -= f * dx[k];
    potential += double(mass) * potentialAt(d, exponent);
  }

  void visit(int at, int target, bool contains, double exponent, double theta, double gradient[3], double& potential) const {
    const BHNode& node = nodes_[size_t(at)];
    if (node.mass <= 0) return;
    if (node.leaf) {
      if (node.mass > kBHLeafCapacity) { aggregate(node, target, contains, exponent, gradient, potential); return; }
      const size_t ti = size_t(target) * d_;
      for (int q = node.begin; q < node.end; q++) {
        int j = order_[size_t(q)];
        if (j == target) continue;
        const size_t tj = size_t(j) * d_;
        double d2 = 1e-24, dx[3] = {0, 0, 0};
        for (int k = 0; k < d_; k++) { dx[k] = xy_[ti + k] - xy_[tj + k]; d2 += dx[k] * dx[k]; }
        double d = std::sqrt(d2), f = forceFactor(d, exponent);
        for (int k = 0; k < d_; k++) gradient[k] -= f * dx[k];
        potential += potentialAt(d, exponent);
      }
      return;
    }
    const size_t ti = size_t(target) * d_;
    double center[3] = {node.sum[0] / node.mass, node.sum[1] / node.mass, d_ == 3 ? node.sum[2] / node.mass : 0};
    double d2 = 1e-24;
    for (int k = 0; k < d_; k++) { double v = xy_[ti + k] - center[k]; d2 += v * v; }
    double d = std::sqrt(d2);
    if (!contains && d > 0 && (node.half * 2) / d < theta) { aggregate(node, target, false, exponent, gradient, potential); return; }
    int targetChild = contains ? octant(target, node.cx, node.cy, node.cz) : -1;
    for (int b = 0; b < 8; b++) if (node.child[b] >= 0)
      visit(node.child[b], target, contains && b == targetChild, exponent, theta, gradient, potential);
  }
};
}  // namespace

double vosLayout(Network& net, const LayoutOpts& o, Progress prog, const std::atomic<bool>* cancel) {
  int n = net.n();
  int D = o.threeD ? 3 : 2;
  double A = o.attraction, R = o.repulsion;
  int starts = std::max(1, o.starts);
  int iters = std::max(20, o.iterations ? o.iterations : int(std::clamp(1000.0 * sq(1000.0 / std::max(1, n)), 150.0, 1000.0)));
  if (n == 0) return 0;
  if (n == 1) { net.nodes[0].x = net.nodes[0].y = net.nodes[0].z = 0; return 0; }
  int m = net.m();
  vector<int> ea(static_cast<size_t>(m)), eb(static_cast<size_t>(m));
  vector<double> s(static_cast<size_t>(m));
  double sMean = 0;
  for (int e = 0; e < m; e++) { ea[e] = net.links[e].a; eb[e] = net.links[e].b; s[e] = net.links[e].s; sMean += s[e]; }
  sMean = m ? sMean / m : 1;
  if (!(sMean > 0)) sMean = 1;
  for (auto& v : s) v /= sMean;
  struct { vector<int> off, nb; } csr;
  vector<double> cs;
  csr.off.assign(size_t(n) + 1, 0);
  for (int e = 0; e < m; e++) { csr.off[ea[e] + 1]++; csr.off[eb[e] + 1]++; }
  for (int i = 0; i < n; i++) csr.off[i + 1] += csr.off[i];
  csr.nb.resize(size_t(csr.off[n])); cs.resize(size_t(csr.off[n]));
  { vector<int> p(csr.off.begin(), csr.off.end() - 1); for (int e = 0; e < m; e++) { csr.nb[p[ea[e]]] = eb[e]; cs[p[ea[e]]++] = s[e]; csr.nb[p[eb[e]]] = ea[e]; cs[p[eb[e]]++] = s[e]; } }
  double pairs = double(n) * (n - 1) / 2;
  const bool useBarnesHut = n > std::max(0, o.exactRepulsionLimit);
  const double bhTheta = std::clamp(o.barnesHutTheta, 0.0, 1.5);
  auto dist = [&](const vector<double>& X, int i, int j) {
    double d2 = 0;
    for (int k = 0; k < D; k++) { double t = X[size_t(i) * D + k] - X[size_t(j) * D + k]; d2 += t * t; }
    return std::sqrt(d2) + 1e-12;
  };
  auto energyPartsExact = [&](const vector<double>& X, double& At, double& Bt) {
    At = 0; Bt = 0;
    for (int e = 0; e < m; e++) At += s[e] * std::pow(dist(X, ea[e], eb[e]), A) / A;
    if (R == 0) {  // sum of log d = 0.5 log of products of d^2 (one log per ~50 pairs)
      double prod = 1, logs = 0;
      for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
          double d2 = 0;
          for (int k = 0; k < D; k++) { double t = X[size_t(i) * D + k] - X[size_t(j) * D + k]; d2 += t * t; }
          prod *= d2 + 1e-24;
          if (prod > 1e120 || prod < 1e-120) { logs += std::log(prod); prod = 1; }
        }
      Bt = 0.5 * (logs + std::log(prod));
      return;
    }
    for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++) { double d = dist(X, i, j); Bt += R == 1 ? d : std::pow(d, R) / R; }
  };
  auto energyParts = [&](const vector<double>& X, double& At, double& Bt) {
    if (!useBarnesHut) { energyPartsExact(X, At, Bt); return; }
    At = 0; Bt = 0;
    for (int e = 0; e < m; e++) At += s[e] * std::pow(dist(X, ea[e], eb[e]), A) / A;
    BarnesHutTree tree(X, n, D);
    for (int i = 0; i < n; i++) {
      double g[3] = {0, 0, 0}, potential = 0;
      tree.repulsion(i, R, bhTheta, g, potential);
      Bt += potential * 0.5;
    }
  };
  auto rescale = [&](vector<double>& X) {
    double At, Bt;
    energyParts(X, At, Bt);
    if (!(At > 0)) return;
    double lam = R == 0 ? std::pow(pairs / (A * At), 1 / A) : std::pow((R * Bt) / (A * At), 1 / (A - R));
    if (!std::isfinite(lam) || lam <= 0) return;
    for (auto& v : X) v *= lam;
  };
  vector<char> pinned(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) pinned[size_t(i)] = net.nodes[size_t(i)].pinned;
  // one random start; returns the final energy and positions in X
  auto runStart = [&](int st, vector<double>& X, bool report) -> double {
    Rng rnd(uint32_t((o.seed + 1) * 31337 + st * 7777));
    X.assign(static_cast<size_t>(n) * D, 0.0);
    for (int i = 0; i < n; i++) {
      if (D == 2) { double a = rnd() * M_PI * 2, r = std::sqrt(rnd()); X[i * 2] = std::cos(a) * r; X[i * 2 + 1] = std::sin(a) * r; }
      else { for (int k = 0; k < 3; k++) X[i * 3 + k] = rnd() * 2 - 1; }
    }
    rescale(X);
    // CWTS gradient-descent schedule (networkanalysis GradientDescentVOSLayoutAlgorithm): exact small-map runs
    // move sequentially in random order; the large-map approximation updates one snapshot synchronously. Both use
    // step x0.75 on a non-improving objective and /0.75 after 5 consecutive improvements.
    double meanD = 0;
    for (int i = 0; i < n; i++) { double d2 = 0; for (int k = 0; k < D; k++) d2 += sq(X[i * D + k]); meanD += std::sqrt(d2); }
    meanD = meanD / n;
    if (!(meanD > 0)) meanD = 1;
    vector<int> order = shuffled(n, rnd);
    double step = meanD, minStep = meanD * 1e-3, Vprev = 1e300;
    int improv = 0;
    vector<char> done;
    // 2D exact-path cache: structure-of-arrays copies are kept in sync with X.
    vector<double> px, py;
    if (!useBarnesHut) {
      done.resize(static_cast<size_t>(n));
      if (D == 2) { px.resize(size_t(n)); py.resize(size_t(n)); for (int i = 0; i < n; i++) { px[size_t(i)] = X[i * 2]; py[size_t(i)] = X[i * 2 + 1]; } }
    }
    for (int it = 0; it < iters && step >= minStep; it++) {
      if (cancel && cancel->load()) return 1e300;
      double V = 0;
      if (useBarnesHut) {
        // Large maps use a Jacobi update from one immutable coordinate snapshot. The tree energy is the same
        // VOS objective with distant groups represented by their centre of mass; edge attraction stays exact.
        BarnesHutTree tree(X, n, D);
        vector<double> next = X;
        double attractionEnergy = 0, repulsionEnergy = 0;
        for (int e = 0; e < m; e++) {
          double d = dist(X, ea[e], eb[e]);
          attractionEnergy += s[e] * (A == 2 ? d * d * 0.5 : A == 1 ? d : std::pow(d, A) / A);
        }
        for (int i = 0; i < n; i++) {
          if ((i & 255) == 0 && cancel && cancel->load()) return 1e300;
          double gi[3] = {0, 0, 0};
          for (int e = csr.off[i]; e < csr.off[i + 1]; e++) {
            int j = csr.nb[e];
            double dx[3] = {0, 0, 0}, d2 = 1e-24;
            for (int k = 0; k < D; k++) { dx[k] = X[size_t(i) * D + k] - X[size_t(j) * D + k]; d2 += dx[k] * dx[k]; }
            double d = std::sqrt(d2), w = cs[e];
            double f = w * (A == 2 ? 1 : A == 1 ? 1 / d : std::pow(d, A - 2));
            for (int k = 0; k < D; k++) gi[k] += f * dx[k];
          }
          double repGrad[3] = {0, 0, 0}, potential = 0;
          tree.repulsion(i, R, bhTheta, repGrad, potential);
          for (int k = 0; k < D; k++) gi[k] += repGrad[k];
          repulsionEnergy += potential;
          if (pinned[size_t(i)]) continue;
          double gl = 0;
          for (int k = 0; k < D; k++) gl += gi[k] * gi[k];
          gl = std::sqrt(gl);
          if (gl > 1e-300) for (int k = 0; k < D; k++) next[size_t(i) * D + k] -= step * gi[k] / gl;
        }
        V = attractionEnergy - 0.5 * repulsionEnergy;
        X.swap(next);
      } else {
      std::fill(done.begin(), done.end(), 0);
      for (int oi = 0; oi < n; oi++) {
        int i = order[oi];
        double gi[3] = {0, 0, 0};
        for (int e = csr.off[i]; e < csr.off[i + 1]; e++) {
          int j = csr.nb[e];
          double dx[3], d2 = 1e-24;
          for (int k = 0; k < D; k++) { dx[k] = X[i * D + k] - X[j * D + k]; d2 += dx[k] * dx[k]; }
          double d = std::sqrt(d2), w = cs[e];
          double f = w * (A == 2 ? 1 : A == 1 ? 1 / d : std::pow(d, A - 2));
          for (int k = 0; k < D; k++) gi[k] += f * dx[k];
          if (!done[j]) V += w * (A == 2 ? d2 / 2 : A == 1 ? d : std::pow(d, A) / A);
        }
        if (D == 2 && (R == 0 || R == 1 || R == -1)) {
          const double xi = px[size_t(i)], yi = py[size_t(i)];
          double gx = 0, gy = 0, vr = 0, prod = 1, logs = 0;
          const double* PX = px.data();
          const double* PY = py.data();
          const char* DN = done.data();
          auto seg = [&](int j0, int j1) {
            if (R == 0) {
              // force dx / d^2; energy log d = 0.5 log d^2, taken on running products (one log per ~50 pairs)
              for (int j = j0; j < j1; j++) {
                double dx = xi - PX[j], dy = yi - PY[j], d2 = dx * dx + dy * dy + 1e-24;
                double f = 1 / d2;
                gx -= f * dx; gy -= f * dy;
                if (!DN[j]) { prod *= d2; if (prod > 1e120 || prod < 1e-120) { logs += std::log(prod); prod = 1; } }
              }
            } else if (R == 1) {
              for (int j = j0; j < j1; j++) {
                double dx = xi - PX[j], dy = yi - PY[j], d2 = dx * dx + dy * dy + 1e-24, d = std::sqrt(d2);
                double f = 1 / d;
                gx -= f * dx; gy -= f * dy;
                if (!DN[j]) vr += d;
              }
            } else {
              for (int j = j0; j < j1; j++) {
                double dx = xi - PX[j], dy = yi - PY[j], d2 = dx * dx + dy * dy + 1e-24, d = std::sqrt(d2);
                double f = 1 / (d2 * d);
                gx -= f * dx; gy -= f * dy;
                if (!DN[j]) vr -= 1 / d;  // pow(d, -1) / -1
              }
            }
          };
          seg(0, i);
          seg(i + 1, n);
          if (R == 0) { logs += std::log(prod); V -= 0.5 * logs; }
          else V -= vr;
          gi[0] += gx; gi[1] += gy;
        } else {
          for (int j = 0; j < n; j++) {
            if (j == i) continue;
            double dx[3], d2 = 1e-24;
            for (int k = 0; k < D; k++) { dx[k] = X[i * D + k] - X[j * D + k]; d2 += dx[k] * dx[k]; }
            double d = std::sqrt(d2), f;
            if (R == 1) f = 1 / d;
            else if (R == 0) f = 1 / d2;
            else if (R == -1) f = 1 / (d2 * d);
            else f = std::pow(d, R - 2);
            for (int k = 0; k < D; k++) gi[k] -= f * dx[k];
            if (!done[j]) V -= R == 1 ? d : R == 0 ? std::log(d) : std::pow(d, R) / R;
          }
        }
        done[i] = 1;
        if (pinned[size_t(i)]) continue;
        double gl = 0;
        for (int k = 0; k < D; k++) gl += gi[k] * gi[k];
        gl = std::sqrt(gl);
        if (gl > 1e-300) {
          for (int k = 0; k < D; k++) X[i * D + k] -= step * gi[k] / gl;
          if (D == 2) { px[size_t(i)] = X[i * 2]; py[size_t(i)] = X[i * 2 + 1]; }
        }
      }
      }
      if (V < Vprev) { if (++improv >= 5) { improv = 0; step /= 0.75; } }
      else { improv = 0; step *= 0.75; }
      Vprev = V;
      if (report && prog && it % 5 == 0) prog(double(it) / iters);
    }
    rescale(X);
    double At, Bt;
    energyParts(X, At, Bt);
    return At - Bt;
  };
  // random starts run in parallel (one thread per core); the lowest energy wins, ties go to the lower start index
  vector<vector<double>> Xs(static_cast<size_t>(starts));
  vector<double> Vs(static_cast<size_t>(starts), 1e300);
  {
    int hw = std::max(1, int(std::thread::hardware_concurrency()));
    int nth = std::min(starts, hw);
    std::atomic<int> next{0};
    auto worker = [&](int t) {
      for (int st; (st = next.fetch_add(1)) < starts;) Vs[size_t(st)] = runStart(st, Xs[size_t(st)], t == 0 && st == 0);
    };
    vector<std::thread> th;
    for (int t = 1; t < nth; t++) th.emplace_back(worker, t);
    worker(0);
    for (auto& t : th) t.join();
  }
  if (cancel && cancel->load()) return 1e300;
  int bi = 0;
  for (int st = 1; st < starts; st++) if (Vs[size_t(st)] < Vs[size_t(bi)]) bi = st;
  double bestV = Vs[size_t(bi)];
  vector<double> best = std::move(Xs[size_t(bi)]);
  for (int i = 0; i < n; i++) {
    net.nodes[i].x = best[i * D];
    net.nodes[i].y = best[i * D + 1];
    net.nodes[i].z = D == 3 ? best[i * D + 2] : 0;
  }
  bool anyPinned = false;
  for (auto& nd : net.nodes) anyPinned |= nd.pinned;
  if (!anyPinned) {
    rotateToPCA(net);
    // VOSviewer convention: reflect so that the medians of x and y are not positive
    vector<double> xs, ys;
    for (auto& nd : net.nodes) { xs.push_back(nd.x); ys.push_back(nd.y); }
    std::nth_element(xs.begin(), xs.begin() + n / 2, xs.end());
    std::nth_element(ys.begin(), ys.begin() + n / 2, ys.end());
    bool fx = xs[n / 2] > 0, fy = ys[n / 2] > 0;
    for (auto& nd : net.nodes) { if (fx) nd.x = -nd.x; if (fy) nd.y = -nd.y; }
  }
  if (prog) prog(1);
  return bestV;
}

void rotateToPCA(Network& net) {
  int n = net.n();
  if (!n) return;
  double cx = 0, cy = 0, cz = 0;
  for (auto& nd : net.nodes) { cx += nd.x; cy += nd.y; cz += nd.z; }
  cx /= n; cy /= n; cz /= n;
  double sxx = 0, syy = 0, sxy = 0;
  for (auto& nd : net.nodes) { nd.x -= cx; nd.y -= cy; nd.z -= cz; sxx += nd.x * nd.x; syy += nd.y * nd.y; sxy += nd.x * nd.y; }
  double ang = 0.5 * std::atan2(2 * sxy, sxx - syy), c = std::cos(-ang), sn = std::sin(-ang);
  for (auto& nd : net.nodes) { double xr = nd.x * c - nd.y * sn, yr = nd.x * sn + nd.y * c; nd.x = xr; nd.y = yr; }
}

void normaliseSpan(Network& net, double span) {
  if (net.nodes.empty()) return;
  double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300, minz = 1e300, maxz = -1e300;
  for (auto& nd : net.nodes) {
    minx = std::min(minx, nd.x); maxx = std::max(maxx, nd.x);
    miny = std::min(miny, nd.y); maxy = std::max(maxy, nd.y);
    minz = std::min(minz, nd.z); maxz = std::max(maxz, nd.z);
  }
  double ext = std::max({maxx - minx, maxy - miny, 1e-9});
  double k = span / ext, cx = (minx + maxx) / 2, cy = (miny + maxy) / 2, cz = (minz + maxz) / 2;
  for (auto& nd : net.nodes) { nd.x = (nd.x - cx) * k; nd.y = (nd.y - cy) * k; nd.z = (nd.z - cz) * k; }
}

// ------------------------------------------------------------ FDEB
Bundles fdeb(const Network& net, double threshold, int cycles, int I, Progress prog, int maxLinks) {
  Bundles out;
  vector<int> idx;
  for (int e = 0; e < net.m(); e++) idx.push_back(e);
  if (int(idx.size()) > maxLinks) {
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return net.links[a].w > net.links[b].w; });
    idx.resize(static_cast<size_t>(maxLinks));
    std::sort(idx.begin(), idx.end());
  }
  int m = int(idx.size());
  if (!m) return out;
  cycles = clampv(cycles, 1, 6);
  const double K = 0.1;
  double S = 0.1;
  double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
  for (auto& nd : net.nodes) { minx = std::min(minx, nd.x); maxx = std::max(maxx, nd.x); miny = std::min(miny, nd.y); maxy = std::max(maxy, nd.y); }
  double scale = 1000 / std::max(1e-9, std::max(maxx - minx, maxy - miny));
  vector<double> sx(static_cast<size_t>(m)), sy(static_cast<size_t>(m)), tx(static_cast<size_t>(m)), ty(static_cast<size_t>(m)), len(static_cast<size_t>(m));
  for (int e = 0; e < m; e++) {
    auto& l = net.links[idx[e]];
    sx[e] = (net.nodes[l.a].x - minx) * scale; sy[e] = (net.nodes[l.a].y - miny) * scale;
    tx[e] = (net.nodes[l.b].x - minx) * scale; ty[e] = (net.nodes[l.b].y - miny) * scale;
    len[e] = std::hypot(tx[e] - sx[e], ty[e] - sy[e]);
  }
  auto visibility = [&](int p, int q) {
    double px = tx[p] - sx[p], py = ty[p] - sy[p], L2 = px * px + py * py;
    if (L2 < 1e-9) return 0.0;
    auto proj = [&](double x, double y, double& ox, double& oy) { double t = ((x - sx[p]) * px + (y - sy[p]) * py) / L2; ox = sx[p] + t * px; oy = sy[p] + t * py; };
    double i0x, i0y, i1x, i1y;
    proj(sx[q], sy[q], i0x, i0y);
    proj(tx[q], ty[q], i1x, i1y);
    double mix = (i0x + i1x) / 2, miy = (i0y + i1y) / 2, mpx = (sx[p] + tx[p]) / 2, mpy = (sy[p] + ty[p]) / 2;
    double il = std::hypot(i1x - i0x, i1y - i0y);
    if (il < 1e-9) return 0.0;
    return std::max(0.0, 1 - 2 * std::hypot(mpx - mix, mpy - miy) / il);
  };
  vector<vector<int>> compat(static_cast<size_t>(m));
  vector<vector<char>> flip(static_cast<size_t>(m));
  for (int p = 0; p < m; p++) {
    if (len[p] < 1e-6) continue;
    for (int q = p + 1; q < m; q++) {
      if (len[q] < 1e-6) continue;
      double pxv = tx[p] - sx[p], pyv = ty[p] - sy[p], qxv = tx[q] - sx[q], qyv = ty[q] - sy[q];
      double dot = pxv * qxv + pyv * qyv;
      double ca = std::fabs(dot / (len[p] * len[q]));
      if (ca < threshold) continue;
      double lavg = (len[p] + len[q]) / 2;
      double cs = 2 / (lavg / std::min(len[p], len[q]) + std::max(len[p], len[q]) / lavg);
      if (ca * cs < threshold) continue;
      double mdx = (sx[p] + tx[p] - sx[q] - tx[q]) / 2, mdy = (sy[p] + ty[p] - sy[q] - ty[q]) / 2;
      double cp = lavg / (lavg + std::hypot(mdx, mdy));
      if (ca * cs * cp < threshold) continue;
      double cv = std::min(visibility(p, q), visibility(q, p));
      if (ca * cs * cp * cv < threshold) continue;
      char f = dot < 0 ? 1 : 0;
      compat[p].push_back(q); flip[p].push_back(f);
      compat[q].push_back(p); flip[q].push_back(f);
    }
    if (prog && p % 200 == 0) prog(0.25 * p / m);
  }
  int P = 1;
  vector<vector<double>> pts(static_cast<size_t>(m));
  for (int e = 0; e < m; e++) pts[e] = {sx[e], sy[e], (sx[e] + tx[e]) / 2, (sy[e] + ty[e]) / 2, tx[e], ty[e]};
  auto subdivide = [&](int newP) {
    for (int e = 0; e < m; e++) {
      auto& old = pts[e];
      int np = int(old.size() / 2);
      double total = 0;
      for (int i = 1; i < np; i++) total += std::hypot(old[i * 2] - old[i * 2 - 2], old[i * 2 + 1] - old[i * 2 - 1]);
      double seg = total / (newP + 1);
      vector<double> o2(static_cast<size_t>((newP + 2) * 2));
      o2[0] = old[0]; o2[1] = old[1];
      int cur = 1, k = 1;
      double acc = 0, px = old[0], py = old[1];
      while (k <= newP && cur < np) {
        double nx2 = old[cur * 2], ny2 = old[cur * 2 + 1];
        double d = std::hypot(nx2 - px, ny2 - py);
        if (acc + d >= seg - 1e-9 && d > 0) {
          double t = (seg - acc) / d;
          px = px + (nx2 - px) * t; py = py + (ny2 - py) * t;
          o2[k * 2] = px; o2[k * 2 + 1] = py;
          k++; acc = 0;
        } else { acc += d; px = nx2; py = ny2; cur++; }
      }
      for (; k <= newP; k++) { o2[k * 2] = old[old.size() - 2]; o2[k * 2 + 1] = old[old.size() - 1]; }
      o2[(newP + 1) * 2] = old[old.size() - 2]; o2[(newP + 1) * 2 + 1] = old[old.size() - 1];
      pts[e] = std::move(o2);
    }
  };
  for (int cyc = 0; cyc < cycles; cyc++) {
    if (cyc > 0) { P *= 2; subdivide(P); S /= 2; I = std::max(4, int(std::round(I * 2.0 / 3))); }
    for (int it = 0; it < I; it++) {
      vector<vector<double>> next(static_cast<size_t>(m));
      for (int e = 0; e < m; e++) {
        auto& pe = pts[e];
        if (len[e] < 1e-6 || compat[e].empty()) { next[e] = pe; continue; }
        double kP = K / (len[e] * (P + 1));
        vector<double> o2 = pe;
        for (int i = 1; i <= P; i++) {
          double x = pe[i * 2], y = pe[i * 2 + 1];
          double fxs = kP * (pe[i * 2 - 2] - x + pe[i * 2 + 2] - x);
          double fys = kP * (pe[i * 2 - 1] - y + pe[i * 2 + 3] - y);
          auto& lst = compat[e];
          auto& fl = flip[e];
          for (size_t c = 0; c < lst.size(); c++) {
            auto& q = pts[lst[c]];
            int qi = fl[c] ? (P + 1 - i) : i;
            double dx = q[qi * 2] - x, dy = q[qi * 2 + 1] - y;
            double d = std::sqrt(dx * dx + dy * dy);
            if (d > 1e-4) { fxs += dx / d; fys += dy / d; }
          }
          o2[i * 2] = x + S * fxs; o2[i * 2 + 1] = y + S * fys;
        }
        next[e] = std::move(o2);
      }
      pts = std::move(next);
    }
    if (prog) prog(0.25 + 0.75 * (cyc + 1) / cycles);
  }
  // pack for all links (links not bundled keep straight polylines)
  int stride = (P + 2) * 2;
  out.P = P;
  out.pts.assign(static_cast<size_t>(net.m()) * stride, 0.f);
  vector<int> slot(static_cast<size_t>(net.m()), -1);
  for (int e = 0; e < m; e++) slot[idx[e]] = e;
  for (int L = 0; L < net.m(); L++) {
    float* dst = &out.pts[size_t(L) * stride];
    if (slot[L] < 0) {
      auto& l = net.links[L];
      for (int i = 0; i < P + 2; i++) {
        double t = double(i) / (P + 1);
        dst[i * 2] = float(net.nodes[l.a].x + (net.nodes[l.b].x - net.nodes[l.a].x) * t);
        dst[i * 2 + 1] = float(net.nodes[l.a].y + (net.nodes[l.b].y - net.nodes[l.a].y) * t);
      }
      continue;
    }
    auto& pe = pts[slot[L]];
    vector<double> sm = pe;
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 1; i <= P; i++) {
        sm[i * 2] = 0.25 * pe[i * 2 - 2] + 0.5 * pe[i * 2] + 0.25 * pe[i * 2 + 2];
        sm[i * 2 + 1] = 0.25 * pe[i * 2 - 1] + 0.5 * pe[i * 2 + 1] + 0.25 * pe[i * 2 + 3];
      }
      pe = sm;
    }
    for (int i = 0; i < P + 2; i++) { dst[i * 2] = float(sm[i * 2] / scale + minx); dst[i * 2 + 1] = float(sm[i * 2 + 1] / scale + miny); }
  }
  return out;
}

// ------------------------------------------------------------ metrics
ItemMetrics itemMetrics(const Network& net) {
  int n = net.n();
  ItemMetrics M;
  M.degree.assign(static_cast<size_t>(n), 0);
  M.strength.assign(static_cast<size_t>(n), 0);
  M.betweenness.assign(static_cast<size_t>(n), 0);
  M.participation.assign(static_cast<size_t>(n), 0);
  M.withinZ.assign(static_cast<size_t>(n), 0);
  M.closeness.assign(static_cast<size_t>(n), 0);
  M.role.assign(static_cast<size_t>(n), "");
  vector<vector<std::pair<int, double>>> adj(static_cast<size_t>(n));
  for (auto& l : net.links) {
    adj[l.a].push_back({l.b, l.w});
    adj[l.b].push_back({l.a, l.w});
    M.degree[l.a]++; M.degree[l.b]++;
    M.strength[l.a] += l.w; M.strength[l.b] += l.w;
  }
  // Brandes betweenness (unweighted, exact for n ≤ 3000, sampled beyond)
  int srcN = n <= 3000 ? n : 600;
  Rng rnd(99);
  vector<int> order(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) order[i] = i;
  if (srcN < n) for (int i = n - 1; i > 0; i--) std::swap(order[i], order[rnd.below(i + 1)]);
  vector<double> sigma(static_cast<size_t>(n)), delta(static_cast<size_t>(n));
  vector<int> dist(static_cast<size_t>(n));
  vector<vector<int>> pred(static_cast<size_t>(n));
  for (int si = 0; si < srcN; si++) {
    int s = order[si];
    vector<int> stack;
    for (int i = 0; i < n; i++) { pred[i].clear(); sigma[i] = 0; dist[i] = -1; delta[i] = 0; }
    sigma[s] = 1; dist[s] = 0;
    std::deque<int> q{s};
    long long sumD = 0;
    int reach = 0;
    while (!q.empty()) {
      int v = q.front(); q.pop_front();
      stack.push_back(v);
      sumD += dist[v]; reach++;
      for (auto& e : adj[v]) {
        int w = e.first;
        if (dist[w] < 0) { dist[w] = dist[v] + 1; q.push_back(w); }
        if (dist[w] == dist[v] + 1) { sigma[w] += sigma[v]; pred[w].push_back(v); }
      }
    }
    if (sumD > 0) M.closeness[s] = double(reach - 1) / double(sumD);
    while (!stack.empty()) {
      int w = stack.back(); stack.pop_back();
      for (int v : pred[w]) delta[v] += sigma[v] / sigma[w] * (1 + delta[w]);
      if (w != s) M.betweenness[w] += delta[w];
    }
  }
  double norm = n > 2 ? 1.0 / ((n - 1.0) * (n - 2.0)) : 1;
  double sampleK = double(n) / srcN;
  for (auto& b : M.betweenness) b = b * norm * sampleK;
  // participation coefficient & within-module z (Guimerà & Amaral)
  int K = 0;
  for (auto& nd : net.nodes) K = std::max(K, nd.cluster + 1);
  vector<double> kin(static_cast<size_t>(n), 0);
  for (int i = 0; i < n; i++) {
    std::unordered_map<int, double> byC;
    for (auto& e : adj[i]) byC[net.nodes[e.first].cluster] += e.second;
    double s = M.strength[i], sum = 0;
    for (auto& kv : byC) sum += sq(kv.second / std::max(1e-12, s));
    M.participation[i] = s > 0 ? 1 - sum : 0;
    kin[i] = byC[net.nodes[i].cluster];
  }
  vector<double> mean(static_cast<size_t>(K + 1), 0), var(static_cast<size_t>(K + 1), 0);
  vector<int> cnt(static_cast<size_t>(K + 1), 0);
  for (int i = 0; i < n; i++) { mean[net.nodes[i].cluster] += kin[i]; cnt[net.nodes[i].cluster]++; }
  for (int c = 0; c <= K; c++) if (cnt[c]) mean[c] /= cnt[c];
  for (int i = 0; i < n; i++) var[net.nodes[i].cluster] += sq(kin[i] - mean[net.nodes[i].cluster]);
  for (int i = 0; i < n; i++) {
    int c = net.nodes[i].cluster;
    double sd = cnt[c] > 1 ? std::sqrt(var[c] / cnt[c]) : 0;
    M.withinZ[i] = sd > 0 ? (kin[i] - mean[c]) / sd : 0;
    double z = M.withinZ[i], P = M.participation[i];
    if (z >= 2.5) M.role[i] = P < 0.3 ? "Provincial hub" : (P < 0.75 ? "Connector hub" : "Kinless hub");
    else M.role[i] = P < 0.05 ? "Ultra-peripheral" : (P < 0.62 ? "Peripheral" : (P < 0.8 ? "Connector" : "Kinless"));
  }
  return M;
}

// ------------------------------------------------------------ geometry
vector<P2> convexHull(vector<P2> p) {
  if (p.size() < 3) return p;
  std::sort(p.begin(), p.end(), [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  auto cross = [](const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
  vector<P2> h(p.size() * 2);
  size_t k = 0;
  for (size_t i = 0; i < p.size(); i++) { while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) k--; h[k++] = p[i]; }
  for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) { while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) k--; h[k++] = p[i - 1]; }
  h.resize(k - 1);
  return h;
}
vector<P2> smoothHull(const vector<P2>& hull, double pad, int seg) {
  vector<P2> out;
  if (hull.empty()) return out;
  if (hull.size() < 3) {
    // circle(s) around 1–2 points
    double cx = 0, cy = 0;
    for (auto& p : hull) { cx += p.x; cy += p.y; }
    cx /= hull.size(); cy /= hull.size();
    double r = pad + (hull.size() == 2 ? std::hypot(hull[0].x - hull[1].x, hull[0].y - hull[1].y) / 2 : 0);
    for (int i = 0; i < 32; i++) { double a = i * M_PI * 2 / 32; out.push_back({cx + std::cos(a) * r, cy + std::sin(a) * r}); }
    return out;
  }
  size_t n = hull.size();
  for (size_t i = 0; i < n; i++) {
    const P2& prev = hull[(i + n - 1) % n];
    const P2& cur = hull[i];
    const P2& next = hull[(i + 1) % n];
    double a0 = std::atan2(cur.y - prev.y, cur.x - prev.x) - M_PI / 2;
    double a1 = std::atan2(next.y - cur.y, next.x - cur.x) - M_PI / 2;
    // hull is CCW in math coords; outward normal = direction - 90°
    while (a1 < a0) a1 += 2 * M_PI;
    if (a1 - a0 > M_PI * 1.999) a1 -= 2 * M_PI;
    for (int k = 0; k <= seg; k++) {
      double a = a0 + (a1 - a0) * k / seg;
      out.push_back({cur.x + std::cos(a) * pad, cur.y + std::sin(a) * pad});
    }
  }
  return out;
}

void removeOverlap(Network& net, const vector<double>& radius, int iters) {
  int n = net.n();
  if (n < 2) return;
  for (int it = 0; it < iters; it++) {
    // spatial hash
    double cell = 0;
    for (double r : radius) cell = std::max(cell, r * 2);
    if (cell <= 0) return;
    std::unordered_map<int64_t, vector<int>> grid;
    auto key = [&](double x, double y) { return (int64_t(std::floor(x / cell)) << 32) ^ int64_t(uint32_t(int(std::floor(y / cell)))); };
    for (int i = 0; i < n; i++) grid[key(net.nodes[i].x, net.nodes[i].y)].push_back(i);
    bool moved = false;
    for (int i = 0; i < n; i++) {
      auto& a = net.nodes[i];
      int gx = int(std::floor(a.x / cell)), gy = int(std::floor(a.y / cell));
      for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
          auto it2 = grid.find((int64_t(gx + dx) << 32) ^ int64_t(uint32_t(gy + dy)));
          if (it2 == grid.end()) continue;
          for (int j : it2->second) {
            if (j <= i) continue;
            auto& b = net.nodes[j];
            double ddx = b.x - a.x, ddy = b.y - a.y, d = std::sqrt(ddx * ddx + ddy * ddy);
            double minD = (radius[i] + radius[j]) * 1.05;
            if (d < minD) {
              if (d < 1e-6) { ddx = 1e-3 * (i % 7 - 3); ddy = 1e-3 * (j % 5 - 2); d = std::hypot(ddx, ddy); }
              double push = (minD - d) / 2 / d;
              if (!a.pinned) { a.x -= ddx * push; a.y -= ddy * push; }
              if (!b.pinned) { b.x += ddx * push; b.y += ddy * push; }
              moved = true;
            }
          }
        }
    }
    if (!moved) break;
  }
}

double physicsStep(Network& net, double step) {
  int n = net.n();
  if (n < 2) return 0;
  vector<double> gx(static_cast<size_t>(n), 0), gy(static_cast<size_t>(n), 0);
  double sMean = 0;
  for (auto& l : net.links) sMean += l.s;
  sMean = net.m() ? sMean / net.m() : 1;
  if (!(sMean > 0)) sMean = 1;
  // work in a unit where the layout span is ~ 10
  double scale = 0;
  for (auto& nd : net.nodes) scale = std::max(scale, std::max(std::fabs(nd.x), std::fabs(nd.y)));
  scale = scale > 0 ? 5.0 / scale : 1;
  for (auto& l : net.links) {
    double dx = (net.nodes[l.a].x - net.nodes[l.b].x) * scale, dy = (net.nodes[l.a].y - net.nodes[l.b].y) * scale;
    double f = l.s / sMean;
    gx[l.a] += f * dx; gy[l.a] += f * dy; gx[l.b] -= f * dx; gy[l.b] -= f * dy;
  }
  int stride = n > 2500 ? n / 1200 : 1;
  for (int i = 0; i < n; i++) {
    double xi = net.nodes[i].x * scale, yi = net.nodes[i].y * scale;
    for (int j = (i + 1) % stride; j < n; j += stride) {
      if (j == i) continue;
      double dx = xi - net.nodes[j].x * scale, dy = yi - net.nodes[j].y * scale, d2 = dx * dx + dy * dy + 1e-6, d = std::sqrt(d2);
      double f = stride / d * 0.5;
      gx[i] -= f * dx; gy[i] -= f * dy;
    }
  }
  double moved = 0;
  for (int i = 0; i < n; i++) {
    if (net.nodes[i].pinned) continue;
    double gl = std::hypot(gx[i], gy[i]);
    if (gl < 1e-12) continue;
    double s = std::min(step, gl * step * 0.05);
    net.nodes[i].x -= s * gx[i] / gl / scale;
    net.nodes[i].y -= s * gy[i] / gl / scale;
    moved += s;
  }
  return moved / n;
}

void worldScale(Network& net, double k) {
  int n = net.n();
  if (!n) return;
  double cx = 0, cy = 0, cz = 0;
  for (auto& nd : net.nodes) { cx += nd.x; cy += nd.y; cz += nd.z; }
  cx /= n; cy /= n; cz /= n;
  double rms = 0;
  for (auto& nd : net.nodes) rms += sq(nd.x - cx) + sq(nd.y - cy);
  rms = std::sqrt(rms / n);
  if (rms < 1e-12) rms = 1;
  double sc = k * std::sqrt(double(n)) / rms;
  for (auto& nd : net.nodes) { nd.x = (nd.x - cx) * sc; nd.y = (nd.y - cy) * sc; nd.z = (nd.z - cz) * sc; }
}

}  // namespace vs
