// Compare mode: subset differences (periods, source files) and the side-by-side figure.
// Run:  g++ -std=c++17 -O1 tests/compare_test.cpp src/core/*.cpp -Isrc/core -o build/compare_test -pthread && ./build/compare_test
#include <cstdio>
#include <iostream>

#include "analysis.h"
#include "figure.h"
#include "records.h"
#include "stats.h"
#include "timecmp.h"

using namespace vs;

static int fails = 0;
#define CHECK(c)                                                                     \
  do {                                                                               \
    if (!(c)) { fails++; std::cerr << "FAIL " << __LINE__ << ": " #c << "\n"; }      \
  } while (0)

int main(int argc, char** argv) {
  Corpus C;
  string t = sampleWoSExport();
  parseRecords(t, detectFormat(t, "s.txt"), C.recs);
  C.format = BibFormat::WoS;
  // pretend two source files: even records from file 0, odd from file 1
  C.files.push_back(SourceFile{});
  C.files.push_back(SourceFile{});
  for (size_t i = 0; i < C.recs.size(); i++) C.recs[i].src = 1ull << (i % 2);
  AnalysisEngine E;
  E.corpus = &C;
  AnaSpec s;
  s.type = AnaType::Cooc;
  s.unit = Unit::Keywords;
  s.setDefaults();
  Network net = E.build(s);
  normalise(net, Norm::Association);
  LayoutOpts lo;
  vosLayout(net, lo);
  worldScale(net);
  ClusterOpts co;
  leiden(net, co);
  CHECK(net.n() > 5);
  bool anyRecs = false;
  for (auto& nd : net.nodes) if (!nd.recs.empty()) anyRecs = true;
  CHECK(anyRecs);

  int a0 = 0, a1 = 0, b0 = 0, b1 = 0;
  suggestPeriods(C, a0, a1, b0, b1);
  PeriodDiff pd = periodDiff(net, C, a0, a1, b0, b1);
  CHECK(pd.ok);
  PeriodDiff sd = subsetDiff(net, C, yearMask(C, a0, a1), yearMask(C, b0, b1));
  CHECK(sd.ok && sd.nA == pd.nA && sd.nB == pd.nB && sd.items.size() == pd.items.size());
  for (size_t i = 0; i < sd.items.size() && i < pd.items.size(); i++) CHECK(sd.items[i].a == pd.items[i].a && sd.items[i].status == pd.items[i].status);

  vector<char> f0 = fileMask(C, 0), f1 = fileMask(C, 1);
  int n0 = 0, n1 = 0;
  for (size_t i = 0; i < C.recs.size(); i++) { n0 += f0[i]; n1 += f1[i]; }
  CHECK(n0 + n1 == int(C.recs.size()) && n0 > 0 && n1 > 0);
  PeriodDiff fd = subsetDiff(net, C, f0, f1);
  CHECK(fd.ok && fd.nA == n0 && fd.nB == n1);
  int sum = fd.counts[0] + fd.counts[1] + fd.counts[2] + fd.counts[3];
  CHECK(sum == net.n());
  // an empty side is reported, not computed
  PeriodDiff bad = subsetDiff(net, C, f0, vector<char>(C.recs.size(), 0));
  CHECK(!bad.ok && !bad.error.empty());

  // side-by-side figure
  ViewStyle st;
  FigureSpec sp;
  vector<double> a(size_t(net.n())), b(size_t(net.n()));
  for (int i = 0; i < net.n(); i++) { a[size_t(i)] = pd.items[size_t(i)].a; b[size_t(i)] = pd.items[size_t(i)].b; }
  Scene sc = compareSideBySide(net, st, sp, nullptr, "test methods", a, b, pd.nA, pd.nB, "A: first half", "B: second half", 1200, 700);
  CHECK(sc.W == 1200 && sc.H > 300 && sc.H < 1400);
  int circles = 0, leftHalf = 0, rightHalf = 0, titles = 0;
  for (auto& p : sc.items) {
    if (p.type == Prim::Circle) { circles++; if (p.x < 600) leftHalf++; else rightHalf++; }
    if (p.type == Prim::Text && (p.text == "A: first half" || p.text == "B: second half")) titles++;
  }
  CHECK(circles >= 2 * net.n());  // every item drawn on both sides
  CHECK(leftHalf > 0 && rightHalf > 0);
  CHECK(titles == 2);
  // thresholds: weights as they are (nA = nB = 0), half the items hidden on the right
  vector<double> wa(size_t(net.n())), wb(size_t(net.n()));
  for (int i = 0; i < net.n(); i++) { wa[size_t(i)] = net.weight(i); wb[size_t(i)] = i % 2 ? 0 : net.weight(i); }
  Scene sc2 = compareSideBySide(net, st, sp, nullptr, "", wa, wb, 0, 0, "A", "B", 900, 500);
  CHECK(sc2.items.size() > 10);
  if (argc > 1) {
    string svg = toSVG(sc);
    FILE* fp = fopen(argv[1], "wb");
    if (fp) { fwrite(svg.data(), 1, svg.size(), fp); fclose(fp); std::cout << "wrote " << argv[1] << "\n"; }
  }
  std::cout << (fails ? "FAILED" : "compare_test OK") << " (" << fails << " failures)\n";
  return fails ? 1 : 0;
}
