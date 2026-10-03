// VOSStudio Native — time and comparison (1.8): period difference of map items, main path analysis,
// resolution sweep and the placement of new papers on an existing map ("living maps").
#pragma once
#include <functional>

#include "algo.h"
#include "analysis.h"
#include "figure.h"
#include "insights.h"
#include "model.h"

namespace vs {

// ---------------------------------------------------------------- difference between two periods
enum class Change { Appearing = 0, Growing = 1, Stable = 2, Fading = 3 };
const char* changeLabel(Change c);

struct ItemChange {
  int node = 0;
  int a = 0, b = 0;               // documents of the item in period A and B
  double shareA = 0, shareB = 0;  // share of the period's documents (0..1)
  double logRatio = 0;            // log2 of the smoothed share ratio B / A
  Change status = Change::Stable;
  bool few = false;               // too few documents to classify (kept as stable)
};
struct PeriodDiff {
  int a0 = 0, a1 = 0, b0 = 0, b1 = 0;
  int nA = 0, nB = 0;            // documents of the corpus in each period
  vector<ItemChange> items;      // one per node, in node order
  int counts[4] = {0, 0, 0, 0};  // per Change
  bool ok = false;
  string error;
};
// Share of each map item in two periods, from the records behind the item (Node::recs) and their years.
// The ratio uses additive smoothing ((b+0.5)/nB) / ((a+0.5)/nA); growing / fading means a change of at least 1.5x.
PeriodDiff periodDiff(const Network& net, const Corpus& c, int a0, int a1, int b0, int b1, int minDocs = 2);
// The same comparison for any two record subsets (masks over c.recs): two source files, two queries, two periods.
PeriodDiff subsetDiff(const Network& net, const Corpus& c, const vector<char>& inA, const vector<char>& inB, int minDocs = 2);
// Records that came from source file `file` (provenance bits); all records when provenance is unknown
vector<char> fileMask(const Corpus& c, size_t file);
vector<char> yearMask(const Corpus& c, int y0, int y1);

// ---------------------------------------------------------------- side-by-side comparison figure
// Two network panels of the same layout: node areas follow the item's share of documents in A (left) and in B (right)
// on one common scale, items absent from a side are faded, cluster colours and positions stay put so the eye can compare.
// a/b: per-node document counts (or weights), nA/nB: totals the shares are taken from (0 = use the counts as they are).
// W x H is the requested size in points; the returned scene keeps the aspect the panels needed.
Scene compareSideBySide(const Network& net, const ViewStyle& st, const FigureSpec& spec, const Bundles* bundles, const string& methodsShort,
                        const vector<double>& a, const vector<double>& b, double nA, double nB, const string& titleA, const string& titleB,
                        double W, double H);
// split of the corpus years into two halves of about the same number of documents
void suggestPeriods(const Corpus& c, int& a0, int& a1, int& b0, int& b1);

// ---------------------------------------------------------------- main path analysis
struct MainPath {
  bool ok = false;
  string error;
  int dagNodes = 0, dagLinks = 0, sources = 0, sinks = 0, dropped = 0;
  double logPaths = 0;                        // natural log of the number of source-to-sink paths
  vector<int> global;                         // documents on the global main path, oldest first
  vector<int> docs;                           // documents on the global path and the key-route paths
  vector<std::tuple<int, int, double>> links; // (cited, citing, normalised SPC) among docs
  vector<double> spcDoc;                      // per doc in docs: largest SPC of its links
};
// Search path count (Batagelj 2003) on the citation network of the corpus: knowledge flows from the cited to the
// citing document. SPC(u,v) = N-(u) * N+(v) / total paths. Global main path: the source-to-sink path with the largest
// sum of SPC (Liu & Lu 2012). Key routes: the keyRoutes links with the largest SPC, each extended both ways by the
// same criterion. Counts are kept as logarithms, so large networks do not overflow.
MainPath mainPath(const Corpus& c, const CitationIndex& x, int keyRoutes = 5);

// ---------------------------------------------------------------- resolution sweep
struct SweepPoint {
  double resolution = 1;
  int clusters = 0, clustersMin = 0, clustersMax = 0;
  double quality = 0;    // objective of the first run
  double meanAri = 0;    // agreement among the seeds (1 = the same partition every time)
  double largest = 0;    // share of items in the largest cluster
};
struct Sweep {
  vector<SweepPoint> pts;
  int seeds = 0, n = 0;
  int best = -1;         // suggested point: the most reproducible with at least two clusters
  bool cancelled = false;
};
vector<double> defaultSweepResolutions();
// progress(done, total) returns false to stop. Works on its own copy of the network.
Sweep resolutionSweep(const Network& net, const ClusterOpts& base, const vector<double>& res, int seeds, const std::function<bool(int, int)>& progress = {});
string sweepCsv(const Sweep& s);

// ---------------------------------------------------------------- living maps: new papers on an existing map
struct NewPaper {
  int rec = 0;           // index into the fetched list
  int cluster = -1;      // map cluster the paper matches best (-1 = none)
  int hits = 0;          // map items it mentions / cites
  vector<int> nodes;     // matched nodes
};
struct Placement {
  int fetched = 0, known = 0;       // fetched, already in the data
  vector<NewPaper> papers;          // the new ones
  vector<int> perCluster;           // new papers per map cluster
  int unplaced = 0;
  vector<std::pair<string, int>> emerging;  // frequent keywords of the new papers that are not on the map
};
// Removes papers already in the corpus (DOI or title and year), then places each new paper in the map cluster whose
// items it mentions most (keywords, authors, sources or cited works, depending on the map's unit).
Placement placeNewPapers(const Network& net, const AnaSpec& spec, const Thesaurus& th, const Corpus& c, const vector<Record>& fetched);

}  // namespace vs
