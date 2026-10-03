#include "../src/core/algo.h"

#include <cmath>
#include <cstdio>

using namespace vs;

int main() {
  // Each component is a triangle. At CPM resolution 1 the exact move gain is zero, so Leiden
  // keeps singleton communities; minSize cannot merge them because none has an eligible neighbor.
  // This exercises the minimum-size post-pass on a sparse, large graph without a wall-clock gate.
  constexpr int triangles = 2000;
  Network g;
  g.nodes.resize(size_t(triangles) * 3);
  for (int c = 0; c < triangles; c++) {
    int a = c * 3, b = a + 1, d = a + 2;
    g.links.push_back({a, b, 1.0, 1.0});
    g.links.push_back({b, d, 1.0, 1.0});
    g.links.push_back({d, a, 1.0, 1.0});
  }
  ClusterOpts o;
  o.starts = 1;
  o.iterations = 1;
  o.minSize = 4;
  double q = leiden(g, o);
  if (!std::isfinite(q) || g.nClusters != g.n()) {
    std::fprintf(stderr, "min-size merge changed isolated small-cluster behavior: n=%d clusters=%d Q=%g\n", g.n(), g.nClusters, q);
    return 1;
  }
  std::puts("algo_perf_test: sparse min-size clustering passed");
  return 0;
}
