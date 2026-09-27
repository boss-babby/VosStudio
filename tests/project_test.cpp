#include "../src/core/project.h"
#include "../src/core/records.h"
#include <cstdio>
using namespace vs;
int main(){
  Project P; P.addSample(false); P.spec.type=AnaType::Cooc; P.spec.unit=Unit::Keywords; P.spec.setDefaults();
  auto R = P.build([](double){}); printf("build n=%d m=%d k=%d Q=%.3f ms=%.0f (layout %.0f, cluster %.0f)\n", R.n,R.m,R.clusters,R.Q,R.ms,R.layoutMs,R.clusterMs);
  P.style.bundle=true; P.computeBundles(); P.fig.panelOverlay=true; P.net.scoreIdx=0; P.fig.wmm=180; P.fig.hmm=120;
  P.engine.excluded["cooc:keywords"].insert("chatgpt");
  if(!P.save("/tmp/t.vosproj")) return 1;
  Project Q; string err; bool ok=Q.open("/tmp/t.vosproj",&err); printf("reopen ok=%d err='%s' n=%d m=%d k=%d recs=%zu excl=%zu names=%s\n", ok, err.c_str(), Q.net.n(), Q.net.m(), Q.net.nClusters, Q.corpus.recs.size(), Q.engine.excluded.size(), Q.net.clusterName(0).c_str());
  auto j1 = P.toJson(), j2 = Q.toJson(); printf("map sha equal: %d\n", j1["manifest"]["map_sha256"].str()==j2["manifest"]["map_sha256"].str());
  printf("corpus sha equal: %d\n", j1["corpus"]["sha256"].str()==j2["corpus"]["sha256"].str());
  Scene s = buildFigure(Q.net, Q.style, P.fig, &P.bundles, Q.methodsShort()); writeFile("/tmp/p.pdf", toPDF(s)); writeFile("/tmp/p.svg", toSVG(s));
  printf("%s\n", Q.methodsShort().c_str());
  // vosviewer roundtrip
  string mp = writeVosMap(Q.net), nw = writeVosNetwork(Q.net); Project V; bool vok = V.loadVosviewer(mp, nw, &err); printf("vos roundtrip ok=%d n=%d m=%d k=%d\n", vok, V.net.n(), V.net.m(), V.net.nClusters);
  // rebuild after exclusion
  auto R2 = P.build(); printf("after exclusion n=%d\n", R2.n);
  // all types quick build
  int fails=0; for (auto& ti: anaTypes()) for (auto& u: ti.units){ P.spec.type=ti.type; P.spec.unit=u.first; P.spec.setDefaults(); try{ auto r=P.build(); printf("  %s/%s n=%d k=%d Q=%.2f %.0fms\n", ti.id, unitId(u.first), r.n, r.clusters, r.Q, r.ms);}catch(std::exception&e){fails++; printf("  %s/%s FAIL %s\n", ti.id, unitId(u.first), e.what());} }
  printf("fails=%d\n", fails);
  // 1.5: map history saved with the project
  {
    Project H; H.addSample(false);
    H.spec.type=AnaType::Cooc; H.spec.unit=Unit::Keywords; H.spec.setDefaults(); H.build();
    SavedMap m0; m0.net=H.net; m0.spec=H.spec; m0.params=H.params; m0.mapSource=H.mapSource; m0.title="Keywords"; m0.when="10:00";
    H.spec.type=AnaType::Coauth; H.spec.unit=Unit::Countries; H.spec.setDefaults(); H.build();
    SavedMap m1; m1.title="Countries"; m1.when="10:05";  // current: stored as a marker only
    H.maps={m0,m1}; H.mapCur=1;
    int mapsFails=0;
    if(!H.save("/tmp/h.vosproj")) mapsFails++;
    Project R; string e2; if(!R.open("/tmp/h.vosproj",&e2)) mapsFails++;
    if(R.maps.size()!=2 || R.mapCur!=1) mapsFails++;
    else {
      if(R.maps[0].net.n()!=m0.net.n() || R.maps[0].net.m()!=m0.net.m() || R.maps[0].title!="Keywords" || R.maps[0].spec.unit!=Unit::Keywords) mapsFails++;
      if(R.maps[1].net.n()!=H.net.n() || R.maps[1].spec.unit!=Unit::Countries || R.net.n()!=H.net.n()) mapsFails++;
    }
    // files without a history (1.4 and older) open with an empty list
    Project O; O.open("/tmp/t.vosproj",&e2); if(!O.maps.empty() || O.mapCur!=-1) mapsFails++;
    printf("map history: saved=%zu reopened=%zu cur=%d n0=%d n1=%d mapsFails=%d\n", H.maps.size(), R.maps.size(), R.mapCur, R.maps.empty()?0:R.maps[0].net.n(), R.maps.size()>1?R.maps[1].net.n():0, mapsFails);
    if(mapsFails) return 2;
  }
}
