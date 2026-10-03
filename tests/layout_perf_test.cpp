#include "../src/core/algo.h"

#include <cmath>
#include <cstdio>

using namespace vs;

namespace {
Network sparseRing(int n) {
  Network g;
  g.nodes.resize(size_t(n));
  for (int i = 0; i < n; i++) {
    g.nodes[size_t(i)].id = std::to_string(i);
    g.nodes[size_t(i)].label = "Node " + std::to_string(i);
    const int links[] = {(i + 1) % n, (i + 13) % n, (i + 71) % n};
    const double weights[] = {1.0, 0.7, 0.2};
    for (int k = 0; k < 3; k++) if (links[k] != i) g.links.push_back({i, links[k], weights[k], weights[k]});
  }
  return g;
}

bool finiteLayout(const Network& g, double energy) {
  if (!std::isfinite(energy)) return false;
  for (const Node& v : g.nodes) if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
  return true;
}

double exactVosEnergy(const Network& g, bool threeD) {
  double mean = 0;
  for (const Link& e : g.links) mean += e.s;
  mean = g.links.empty() ? 1 : mean / double(g.links.size());
  if (!(mean > 0)) mean = 1;
  double attraction = 0, repulsion = 0;
  for (const Link& e : g.links) {
    double dx = g.nodes[size_t(e.a)].x - g.nodes[size_t(e.b)].x;
    double dy = g.nodes[size_t(e.a)].y - g.nodes[size_t(e.b)].y;
    double dz = threeD ? g.nodes[size_t(e.a)].z - g.nodes[size_t(e.b)].z : 0;
    attraction += (e.s / mean) * (std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-12);
  }
  for (size_t i = 0; i < g.nodes.size(); i++) for (size_t j = i + 1; j < g.nodes.size(); j++) {
    double dx = g.nodes[i].x - g.nodes[j].x, dy = g.nodes[i].y - g.nodes[j].y;
    double dz = threeD ? g.nodes[i].z - g.nodes[j].z : 0;
    repulsion += std::log(std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-12);
  }
  return attraction - repulsion;
}
}

int main() {
  // Compare approximate and exact objectives on moderate 2D and 3D oracle graphs. The Barnes–Hut run may differ
  // because large maps use synchronous updates, but must not drift far from the exact optimizer on these fixtures.
  for (bool threeD : {false, true}) {
    Network exact = sparseRing(384), approximate = exact;
    LayoutOpts oracleOpts;
    oracleOpts.starts = 1; oracleOpts.iterations = 500; oracleOpts.seed = 21; oracleOpts.exactRepulsionLimit = 1000; oracleOpts.threeD = threeD;
    LayoutOpts bhOpts = oracleOpts;
    bhOpts.exactRepulsionLimit = 0; bhOpts.barnesHutTheta = 0.8;
    double oracleReported = vosLayout(exact, oracleOpts), bhReported = vosLayout(approximate, bhOpts);
    double oracleEnergy = exactVosEnergy(exact, threeD), bhEnergy = exactVosEnergy(approximate, threeD);
    if (!finiteLayout(exact, oracleReported) || !finiteLayout(approximate, bhReported) || bhEnergy > oracleEnergy + std::fabs(oracleEnergy) * 0.15) {
      std::fprintf(stderr, "Barnes-Hut %s oracle gap too large: exact=%.6f approximate=%.6f\n", threeD ? "3D" : "2D", oracleEnergy, bhEnergy);
      return 1;
    }
  }

  // Exercise every supported repulsion exponent through the approximate path, including the 3D octree traversal.
  for (double repulsion : {0.0, 1.0, -1.0, 0.5}) {
    Network g = sparseRing(96);
    LayoutOpts o;
    o.starts = 1; o.iterations = 20; o.seed = 17; o.exactRepulsionLimit = 0; o.barnesHutTheta = 0.45;
    o.repulsion = repulsion;
    o.threeD = (repulsion == -1.0 || repulsion == 0.5);
    double energy = vosLayout(g, o);
    if (!finiteLayout(g, energy)) { std::fprintf(stderr, "non-finite layout for repulsion %.2f\n", repulsion); return 1; }
  }

  // Sparse synthetic 10k and 20k networks exercise the production Barnes-Hut path in both dimensions.
  // This is a correctness/performance smoke test (not a hardware-specific wall-clock gate); timings are printed.
  for (int n : {10000, 20000}) for (bool threeD : {false, true}) {
    Network g = sparseRing(n);
    LayoutOpts o;
    o.starts = 1; o.iterations = 20; o.seed = 29; o.threeD = threeD;
    Timer timer;
    double energy = vosLayout(g, o);
    if (!finiteLayout(g, energy)) { std::fprintf(stderr, "non-finite %s %dk layout\n", threeD ? "3D" : "2D", n / 1000); return 1; }
    std::printf("layout_perf_test: %s n=%d m=%d theta=%.2f energy=%.5f time=%.0fms\n", threeD ? "3D" : "2D", g.n(), g.m(), o.barnesHutTheta, energy, timer.ms());
  }
  std::puts("layout_perf_test: all checks passed");
  return 0;
}
