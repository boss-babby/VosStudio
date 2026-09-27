// 1.2 analyses: trend topics, RPYS, production, SCP/MCP, local citations, citation summary, cluster years
#include "../src/core/project.h"
#include "../src/core/charts.h"
#include <cstdio>
using namespace vs;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main() {
  Project P; P.addSample(false); P.spec.type = AnaType::Cooc; P.spec.unit = Unit::Keywords; P.spec.setDefaults(); P.build();
  auto t = ChartTheme::make(false);
  auto tt = trendTopics(P.corpus, Unit::Keywords, 3, 3);
  printf("trend topics %zu\n", tt.size());
  CHECK(tt.size() >= 5);
  for (size_t i = 1; i < tt.size(); i++) CHECK(tt[i - 1].med <= tt[i].med);
  for (auto& x : tt) CHECK(x.q1 <= x.med && x.med <= x.q3);
  CHECK(refYear("Johnson T, 2010, PATTE RECOG, V54, P118") == 2010);
  CHECK(refYear("Silva, S., Vosviewer revisited, (2005) Quantitative Science Studies, 21") == 2005);
  CHECK(refYear("no year here") == 0);
  auto rp = rpys(P.corpus);
  printf("rpys %d-%d dated %lld/%lld peaks %zu\n", rp.y0, rp.y1, rp.dated, rp.refs, rp.peaks.size());
  CHECK(rp.dated > 0 && !rp.counts.empty() && rp.counts.size() == rp.dev.size());
  auto pr = productionOverTime(P.corpus, Unit::Authors, 10);
  CHECK(pr.labels.size() == 10 && pr.docs.size() == 10);
  auto cc = countryCollaboration(P.corpus, 12);
  CHECK(!cc.empty());
  int mcp = 0; for (auto& c : cc) mcp += c.mcp;
  CHECK(mcp > 0);
  auto X = localCitations(P.corpus);
  int lcsMax = 0; for (auto& v : X.citedBy) lcsMax = std::max(lcsMax, int(v.size()));
  printf("local citation links %d, max LCS %d\n", X.links, lcsMax);
  CHECK(X.links > 0);
  auto cs = citationSummary(P.corpus);
  printf("citations total %lld h %d g %d median %.1f bins %zu\n", cs.total, cs.h, cs.g, cs.median, cs.bins.size());
  CHECK(cs.h > 0 && cs.g >= cs.h);
  auto cy = clusterYears(P.net, P.corpus);
  CHECK(int(cy.docs.size()) == P.net.nClusters);
  const Record& r0 = P.corpus.recs[0];
  printf("shortCite: %s\napa: %s\n", shortCite(r0).c_str(), apaCitation(r0).c_str());
  CHECK(apaCitation(r0).find("(" + std::to_string(r0.year) + ")") != string::npos);
  Record rr; rr.authors = {"Boris A. Petrov", "Lee, J", "Smith AB"}; rr.year = 2020; rr.title = "T"; 
  printf("apa2: %s\n", apaCitation(rr).c_str());
  CHECK(apaCitation(rr).find("Petrov, B. A., Lee, J., & Smith, A. B.") == 0);
  vector<Color> cols; Encoder e; e.prepare(P.net, P.style); for (int c = 0; c < P.net.nClusters; c++) cols.push_back(e.clusterColor(c));
  vector<string> names; for (int c = 0; c < P.net.nClusters; c++) names.push_back(P.net.clusterName(c));
  vector<DocBar> db; for (int i = 0; i < 8; i++) db.push_back({i, shortCite(P.corpus.recs[size_t(i)]), double(P.corpus.recs[size_t(i)].cites), ""});
  vector<Scene> v = {chartTrendTopics(tt, 380, 320, t), chartRpys(rp, 380, 220, t), chartProduction(pr, 380, 260, t), chartCollab(cc, 380, 260, t),
                     chartHistoriograph(P.corpus, X, 24, 420, 300, t), chartClusterYears(cy, names, cols, false, 380, 220, t), chartClusterYears(cy, names, cols, true, 380, 220, t), chartDocBars(db, 380, 220, t, " cites")};
  for (size_t i = 0; i < v.size(); i++) {
    writeFile("/tmp/ins" + std::to_string(i) + ".svg", toSVG(v[i]));
    int tips = 0; for (auto& p : v[i].items) if (!p.tip.empty()) tips++;
    CHECK(tips > 0);
  }
  CHECK(tipAt(v[5], 200, 100) != nullptr);
  printf("fails=%d\n", fails);
  return fails != 0;
}
