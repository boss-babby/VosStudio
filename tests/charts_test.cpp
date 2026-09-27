#include "../src/core/project.h"
#include "../src/core/charts.h"
#include <cstdio>
using namespace vs;
int main(){
  Project P; P.addSample(false); P.spec.type=AnaType::Cooc; P.spec.unit=Unit::Keywords; P.spec.setDefaults(); P.build();
  auto t = ChartTheme::make(true); vector<Scene> v;
  auto C = clusterInfo(P.net, &P.corpus); vector<Color> cols; Encoder e; e.prepare(P.net,P.style); for(int c=0;c<P.net.nClusters;c++) cols.push_back(e.clusterColor(c));
  v.push_back(chartGrowth(growth(P.corpus), 360, 200, t));
  v.push_back(chartStrategic(C, cols, 360, 260, t));
  v.push_back(chartBursts(detectBursts(P.corpus, Unit::Keywords, 3, 2.0, 0.5), 2016, 2024, 12, 360, 220, t));
  v.push_back(chartSankey(thematicEvolution(P.corpus, Unit::Keywords, {}), 420, 260, t));
  v.push_back(chartThreeField(threeField(P.corpus, Unit::Countries, Unit::Keywords, Unit::Sources, 8), "Countries","Keywords","Sources", 420, 260, t));
  v.push_back(chartBradford(bradford(P.corpus), 360, 200, t));
  v.push_back(chartLotka(lotka(P.corpus), 360, 200, t));
  v.push_back(chartCompare(comparePeriods(P.corpus, Unit::Keywords, 2016, 2019, 2021, 2024), 8, 360, 260, t));
  v.push_back(chartSensitivity(P.engine.sensitivity(P.spec), 5, "", 300, 90, t));
  v.push_back(chartDensityMatrix(P.net, cols, 420, 420, t));
  for (size_t i=0;i<v.size();i++){ writeFile("/tmp/ch"+std::to_string(i)+".svg", toSVG(v[i])); }
  writeFile("/tmp/ch3.pdf", toPDF(v[3]));
  printf("bursts=%zu\n", detectBursts(P.corpus, Unit::Keywords, 3, 2.0, 0.5).size());
}
