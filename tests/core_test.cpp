#include "../src/core/records.h"
#include "../src/core/analysis.h"
#include "../src/core/stats.h"
#include <cstdio>
using namespace vs;
int main(){
  Corpus C; string t = sampleWoSExport(); BibFormat f = detectFormat(t, "s.txt");
  int n = parseRecords(t, f, C.recs); C.format=f; C.duplicatesRemoved = 0;
  printf("duplicate candidates=%zu (retained for explicit review)\\n", recordDuplicateGroups(C.recs).size());
  auto q = qualityReport(C);
  printf("WoS fmt=%s n=%d years=%d-%d sources=%d authors=%d cpd=%.1f doi=%.2f refs=%lld\n", formatLabel(f), n, q.yearMin,q.yearMax,q.sources,q.authors,q.citesPerDoc,q.doiCov,q.totalRefs);
  printf("rec0: %s | %s | %zu refs | %s | aff=%s ctry=%s\n", C.recs[0].title.c_str(), C.recs[0].authors[0].c_str(), C.recs[0].refs.size(), C.recs[0].refs[0].c_str(), C.recs[0].affiliations[0].c_str(), C.recs[0].countries[0].c_str());
  AnalysisEngine E; E.corpus=&C;
  for (auto& ti : anaTypes()) for (auto& u : ti.units){
    AnaSpec s; s.type=ti.type; s.unit=u.first; s.setDefaults();
    Timer tm; auto& R = E.raw(s); auto& S = E.select(s);
    printf("%-9s %-12s items=%4zu meet=%4d sel=%4zu links=%5zu matched=%lld/%lld shared=%lld/%lld  %.0fms\n", ti.id, unitId(u.first), R.items.size(), S.nMeet, S.sel.size(), S.links.size(), R.matched, R.refs, R.sharedRefs, R.distinctRefs, tm.ms());
  }
  AnaSpec s; s.type=AnaType::Cooc; s.unit=Unit::Keywords; s.setDefaults();
  Network net = E.build(s); normalise(net, Norm::Association);
  LayoutOpts lo; Timer t1; double V = vosLayout(net, lo); double lt=t1.ms();
  ClusterOpts co; Timer t2; double Q = leiden(net, co); double ct=t2.ms();
  applyAutoNames(net, &C);
  printf("keywords map: n=%d m=%d V=%.2f layout=%.0fms clusters=%d Q=%.3f leiden=%.0fms\n", net.n(), net.m(), V, lt, net.nClusters, Q, ct);
  for (int c=0;c<net.nClusters;c++) printf("  C%d %s\n", c+1, net.clusterName(c).c_str());
  auto M = itemMetrics(net); printf("metrics: node0 %s deg=%.0f btw=%.4f P=%.2f role=%s\n", net.nodes[0].label.c_str(), M.degree[0], M.betweenness[0], M.participation[0], M.role[0].c_str());
  auto st = clusterStability(net, co, 8); printf("stability ARI=%.3f NMI=%.3f\n", st.meanAri, st.meanNmi);
  auto bu = fdeb(net); printf("fdeb P=%d pts=%zu\n", bu.P, bu.pts.size());
  auto bursts = detectBursts(C, Unit::Keywords); printf("bursts=%zu top=%s %d-%d\n", bursts.size(), bursts.empty()?"":bursts[0].label.c_str(), bursts.empty()?0:bursts[0].start, bursts.empty()?0:bursts[0].end);
  auto ev = thematicEvolution(C, Unit::Keywords, {}); printf("evolution periods=%zu themes=%zu flows=%zu\n", ev.periods.size(), ev.themes.size(), ev.flows.size());
  auto cmp = comparePeriods(C, Unit::Keywords, 2016, 2019, 2021, 2024); printf("compare emerging=%zu declining=%zu\n", cmp.emerging.size(), cmp.declining.size());
  auto tf = threeField(C, Unit::Authors, Unit::Keywords, Unit::Sources); printf("threefield %zu/%zu/%zu flows %zu/%zu\n", tf.left.size(), tf.mid.size(), tf.right.size(), tf.lm.size(), tf.mr.size());
  auto br = bradford(C); auto lk = lotka(C); auto gr = growth(C); printf("bradford z1=%d z2=%d of %zu; lotka n=%.2f r2=%.2f; cagr=%.3f\n", br.zone1, br.zone2, br.sources.size(), lk.exponent, lk.r2, gr.cagr);
  auto act = topActors(C, Unit::Authors, 5); for (auto&a:act) printf("  author %s docs=%d cites=%d h=%d\n", a.label.c_str(), a.docs, a.cites, a.h);
  auto vg = findVariants(C, Unit::AllKeywords, E.thesaurus); printf("variant groups=%zu\n", vg.size()); for (size_t i=0;i<vg.size()&&i<5;i++) printf("  %s <- %s (%s)\n", vg[i].target.c_str(), vg[i].members[0].c_str(), vg[i].reason.c_str());
  // citation docs naming
  AnaSpec s2; s2.type=AnaType::Coupling; s2.unit=Unit::Docs; s2.setDefaults(); Network n2 = E.build(s2); normalise(n2, Norm::Association); vosLayout(n2, lo); leiden(n2, co); applyAutoNames(n2, &C);
  printf("coupling docs n=%d m=%d k=%d: %s | %s\n", n2.n(), n2.m(), n2.nClusters, n2.clusterName(0).c_str(), n2.clusterName(1).c_str());
  printf("%s\n", methodsParagraph(C, s, net, Norm::Association, lo, co, false).substr(0,400).c_str());
  // scopus
  Corpus S; string st2 = sampleScopusExport(); auto f2 = detectFormat(st2,"x.csv"); parseRecords(st2, f2, S.recs); S.format=f2;
  AnalysisEngine E2; E2.corpus=&S; AnaSpec s3; s3.type=AnaType::Coupling; s3.unit=Unit::Docs; s3.setDefaults(); s3.min=1;
  auto& R3 = E2.raw(s3); auto& S3 = E2.select(s3);
  printf("scopus fmt=%s n=%zu refs=%lld shared=%lld/%lld coupling sel=%zu links=%zu; r0 ref: %s\n", formatLabel(f2), S.recs.size(), R3.refs, R3.sharedRefs, R3.distinctRefs, S3.sel.size(), S3.links.size(), S.recs[0].refs[0].c_str());
  auto rp = refParse(S.recs[0].refs[0]); printf("  parsed: %s %s %d src=%s vol=%s page=%s\n", rp.surname.c_str(), rp.init.c_str(), rp.year, rp.src.c_str(), rp.vol.c_str(), rp.page.c_str());
  printf("sha=%s\n", sha256Hex("abc").c_str());
}
