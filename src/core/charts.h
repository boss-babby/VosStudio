// VOSStudio Native — statistical charts as Scenes (drawn by D2D on screen, exported to SVG/PDF/PNG)
#pragma once
#include "figure.h"
#include "stats.h"
#include "insights.h"
#include "timecmp.h"

namespace vs {

struct ChartTheme {
  Color bg, fg, muted, grid, accent, accent2;
  bool light = true;
  vector<Color> series;
  float font = 9;  // base font size in pt
  bool transparent = false;
  static ChartTheme make(bool light);
};

// every chart item may carry a hit tag (>= 0) through Prim::group = "hit:<n>"
Scene chartGrowth(const Growth& g, double w, double h, const ChartTheme& t);
Scene chartStrategic(const vector<ClusterInfo>& C, const vector<Color>& colors, double w, double h, const ChartTheme& t);
Scene chartBursts(const vector<Burst>& b, int y0, int y1, int top, double w, double h, const ChartTheme& t);
Scene chartSankey(const Evolution& e, double w, double h, const ChartTheme& t);
Scene chartThreeField(const ThreeField& f, const string& lt, const string& mt, const string& rt, double w, double h, const ChartTheme& t);
Scene chartBradford(const Bradford& b, double w, double h, const ChartTheme& t);
Scene chartLotka(const Lotka& l, double w, double h, const ChartTheme& t);
Scene chartCompare(const Comparison& c, int top, double w, double h, const ChartTheme& t);
Scene chartSensitivity(const vector<std::pair<int, int>>& s, int current, const string& xlabel, double w, double h, const ChartTheme& t);
Scene chartBars(const vector<std::pair<string, double>>& rows, const string& title, double w, double h, const ChartTheme& t, const string& fmtSuffix = "");
// 1.2 charts
struct DocBar { int rec = -1; string label; double value = 0; string tip; };
Scene chartTrendTopics(const vector<TrendTopic>& v, double w, double h, const ChartTheme& t);
Scene chartRpys(const Rpys& r, double w, double h, const ChartTheme& t);
Scene chartProduction(const Production& p, double w, double h, const ChartTheme& t);
Scene chartCollab(const vector<CountryCollab>& v, double w, double h, const ChartTheme& t);
Scene chartDocBars(const vector<DocBar>& rows, double w, double h, const ChartTheme& t, const string& suffix);  // hit tag = record index
// main path: documents by year, the global main path highlighted (hit tag = record index)
Scene chartMainPath(const Corpus& c, const MainPath& m, double w, double h, const ChartTheme& t);
// resolution sweep: clusters per resolution (bars, hit tag = point index) and agreement among seeds (line)
Scene chartSweep(const Sweep& sw, double current, double w, double h, const ChartTheme& t);
Scene chartHistoriograph(const Corpus& c, const CitationIndex& x, int top, double w, double h, const ChartTheme& t);  // hit tag = record index
Scene chartClusterYears(const ClusterYears& y, const vector<string>& names, const vector<Color>& colors, bool share, double w, double h, const ChartTheme& t);
Scene chartDensityMatrix(const Network& net, const vector<Color>& colors, double w, double h, const ChartTheme& t);

// PRISMA-style flow of the records from import to the map (Data → Records flow)
struct RecordsFlow {
  vector<std::pair<string, int>> sources;   // imported files / searches with their record counts
  int identified = 0, duplicates = 0, screened = 0;
  vector<std::pair<string, int>> excluded;  // reason → records
  int analysed = 0;
  string unitNoun;                          // plural: "keywords", "authors", ...
  int found = 0, meet = 0, userExcluded = 0, inMap = -1;  // inMap < 0: no map built for this analysis
  string thresholdText;                     // "Min. occurrences of a keyword: 5"
  bool valid = false;
  bool itemsKnown = true;                   // false: item counts not computed (large data set, map not built yet)
};
// The text scales with the height; the width only limits how far it grows (narrow panels stay readable).
Scene chartFlow(const RecordsFlow& f, double w, double h, const ChartTheme& t);

// top-most primitive carrying hover text at (x, y), or nullptr
const Prim* tipAt(const Scene& sc, double x, double y);
int hitTag(const Scene& sc, double x, double y);  // tag of the top-most tagged prim under (x, y) in scene units, -1 if none

}  // namespace vs
