#include "../src/core/records.h"
#include "../src/core/analysis.h"
#include "../src/core/stats.h"
#include "../src/core/figure.h"
#include <cstdio>
using namespace vs;
int main(){
  Corpus C; string t = sampleWoSExport(); parseRecords(t, detectFormat(t,"s.txt"), C.recs); C.format=BibFormat::WoS;
  AnalysisEngine E; E.corpus=&C; AnaSpec s; s.type=AnaType::Cooc; s.unit=Unit::Keywords; s.setDefaults();
  Network net = E.build(s); normalise(net, Norm::Association); LayoutOpts lo; vosLayout(net, lo); worldScale(net); ClusterOpts co; leiden(net, co); applyAutoNames(net,&C);
  net.scoreIdx = 0;
  ViewStyle st; FigureSpec fs; fs.panelOverlay=true; fs.panelDensity=true; fs.wmm=260; fs.hmm=110; fs.title="Author keyword co-occurrence"; fs.caption="Figure 1. Co-occurrence network (a), overlay by average publication year (b) and item density (c)."; fs.footer=true;
  st.clusterNames=true; st.hulls=true;
  Timer tm; Scene sc = buildFigure(net, st, fs, nullptr, "VOS layout, Leiden clustering (resolution 1.00), association strength normalisation."); double bt=tm.ms();
  printf("scene items=%zu labels=%d/%d warnings=%zu build=%.0fms\n", sc.items.size(), sc.labels, sc.candidates, sc.warnings.size(), bt);
  for (auto& w: sc.warnings) printf("  warn: %s\n", w.c_str());
  writeFile("/tmp/fig.svg", toSVG(sc)); writeFile("/tmp/fig.pdf", toPDF(sc, "Keyword map"));
  // vosviewer look + bundles, single panel
  applyLook(st, "vosviewer"); st.bundle=true; st.hulls=false; st.clusterNames=false; auto B = fdeb(net);
  FigureSpec f2; f2.wmm=180; f2.hmm=130; Scene s2 = buildFigure(net, st, f2, &B);
  writeFile("/tmp/fig2.svg", toSVG(s2)); writeFile("/tmp/fig2.pdf", toPDF(s2));
  printf("fig2 items=%zu labels=%d\n", s2.items.size(), s2.labels);
}
