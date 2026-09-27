// VOSStudio Native — descriptive statistics, themes, trends
#pragma once
#include "algo.h"
#include "analysis.h"

namespace vs {

struct ClusterInfo {
  int c = 0, size = 0;
  string autoName;
  vector<int> items;      // node indices, by weight desc
  double centrality = 0;  // external link strength (Callon)
  double density = 0;     // internal link strength / size
  double avgYear = NAN, avgCites = NAN, weight = 0;
  string quadrant;        // Motor / Niche / Emerging or declining / Basic
};
vector<ClusterInfo> clusterInfo(const Network& net, const Corpus* corpus);
void applyAutoNames(Network& net, const Corpus* corpus);

vector<string> unitList(const Record& r, Unit u);  // raw values of a unit in one record
string unitKey(const string& s, Unit u);            // matching key for a value of that unit

struct Actor {
  string label;
  int docs = 0, cites = 0, h = 0, firstYear = 0, lastYear = 0;
  double citesPerDoc = 0;
};
vector<Actor> topActors(const Corpus& c, Unit unit, int top = 50);

struct Bradford { vector<std::pair<string, int>> sources; int zone1 = 0, zone2 = 0; double k = 0; };
Bradford bradford(const Corpus& c);
struct Lotka { vector<std::pair<int, int>> dist; double exponent = 0, c = 0, r2 = 0; };
Lotka lotka(const Corpus& c);
struct Growth { vector<std::pair<int, int>> perYear; vector<std::pair<int, double>> citesPerYear; double cagr = 0; int y0 = 0, y1 = 0; };
Growth growth(const Corpus& c);

struct Burst { string label; int start = 0, end = 0; double strength = 0; int total = 0; };
vector<Burst> detectBursts(const Corpus& c, Unit unit, int minOcc = 3, double s = 2.0, double gamma = 1.0);

struct EvoTheme { int period = 0; string name; int size = 0; vector<string> keywords; double weight = 0; };
struct EvoFlow { int from = 0, to = 0; double w = 0; };
struct Evolution { vector<std::pair<int, int>> periods; vector<EvoTheme> themes; vector<EvoFlow> flows; };
Evolution thematicEvolution(const Corpus& c, Unit unit, const vector<int>& cuts, int minOcc = 2, const Thesaurus* th = nullptr);

struct CompareRow { string label; int a = 0, b = 0; double shareA = 0, shareB = 0, logRatio = 0; };
struct Comparison { int a0 = 0, a1 = 0, b0 = 0, b1 = 0, nA = 0, nB = 0; vector<CompareRow> emerging, declining, stable; };
Comparison comparePeriods(const Corpus& c, Unit unit, int a0, int a1, int b0, int b1, int minOcc = 2);

struct ThreeField {
  vector<string> left, mid, right;
  vector<int> leftN, midN, rightN;
  vector<std::tuple<int, int, int>> lm, mr;  // (i, j, weight)
};
ThreeField threeField(const Corpus& c, Unit l, Unit m, Unit r, int topN = 12);

struct Stability {
  int runs = 0;
  double meanAri = 0, minAri = 0, meanNmi = 0;
  vector<double> itemStability;
  bool freshReference = false;  // the map's clusters came from other settings: runs were compared with a fresh clustering
};
// Re-runs the clustering (the full pipeline, including the small-cluster merge) with different seeds and compares each
// run with the map's clusters, or with a fresh clustering when the map's clusters were made with other settings.
Stability clusterStability(const Network& net, const ClusterOpts& base, int runs);

struct NetSummary { int n = 0, m = 0, clusters = 0, components = 0, isolated = 0; double density = 0, avgDegree = 0, totalStrength = 0, q = 0; };
NetSummary netSummary(const Network& net);

string methodsParagraph(const Corpus& c, const AnaSpec& spec, const Network& net, Norm norm, const LayoutOpts& lo, const ClusterOpts& co, bool bundled);

}  // namespace vs
