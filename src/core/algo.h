// VOSStudio Native — network algorithms
#pragma once
#include <atomic>

#include "model.h"

namespace vs {

using Progress = std::function<void(double)>;

enum class Norm { Association, Fractionalization, LinLog, None };
const char* normLabel(Norm n);
// fills Link::s from Link::w
void normalise(Network& net, Norm how);

struct LayoutOpts {
  // a=1, r=0 is what CWTS uses for its larger reference maps: clearer cluster separation, no sparse fringe
  double attraction = 1, repulsion = 0;
  int starts = 3, iterations = 0, seed = 0;
  bool threeD = false;
  // Keep the exact VOS repulsion for small maps; use a Barnes–Hut spatial approximation above this size.
  // These are algorithm tuning values (not document/UI settings) and may be overridden by the benchmark harness.
  // theta=0.8 is the current measured speed/quality compromise; oracle tests compare it to exact small-map layouts.
  int exactRepulsionLimit = 512;
  double barnesHutTheta = 0.8;
};
// VOS mapping technique (van Eck & Waltman). Returns final energy. Writes x,y(,z) into nodes (≈ unit scale).
double vosLayout(Network& net, const LayoutOpts& o, Progress prog = nullptr, const std::atomic<bool>* cancel = nullptr);

struct ClusterOpts {
  double resolution = 1.0, randomness = 0.01;
  int starts = 10, iterations = 10, seed = 0, minSize = 1;
  bool normalizedWeights = true;  // use Link::s (true) or Link::w
  // Objective. true: VOS clustering quality as in VOSviewer (Waltman, van Eck & Noyons 2010): constant Potts model on the
  // normalised weights with unit node weights, i.e. sum over same-cluster pairs of (s_ij - resolution).
  // false: modularity on the same weights (VOSviewer's LinLog/modularity option; VOSStudio 1.5 and earlier).
  bool vosQuality = true;
};
// identifies the objective and settings that produced a partition ("vos:1:1" = VOS quality, resolution 1, min size 1)
string clusterTag(const ClusterOpts& o);
// Leiden (Traag, Waltman & van Eck 2019). Writes Node::cluster (sorted by size, largest = 0). Returns modularity.
double leiden(Network& net, const ClusterOpts& o, vector<int>* labelsOut = nullptr);
vector<int> leidenLabels(int n, const vector<int>& ea, const vector<int>& eb, const vector<double>& ew, const ClusterOpts& o, double* q = nullptr);
double modularity(const Network& net, const vector<int>& labels, double gamma = 1, bool normalized = true);

struct Agreement { double nmi = 1, ari = 1; };
Agreement partitionAgreement(const vector<int>& a, const vector<int>& b);

// force-directed edge bundling; returns polylines (P+2 points per link) in world coordinates
struct Bundles { int P = 0; vector<float> pts; bool valid() const { return P > 0 && !pts.empty(); } };
Bundles fdeb(const Network& net, double compat = 0.6, int cycles = 5, int iterations = 60, Progress prog = nullptr, int maxLinks = 4000);

// metrics
struct ItemMetrics {
  vector<double> degree, strength, betweenness, participation, withinZ, closeness;
  vector<string> role;
};
ItemMetrics itemMetrics(const Network& net);

// geometry
struct P2 { double x, y; };
vector<P2> convexHull(vector<P2> pts);
vector<P2> smoothHull(const vector<P2>& hull, double pad, int segPerCorner = 6);

// declutter: push overlapping nodes apart (radius per node in world units)
void removeOverlap(Network& net, const vector<double>& radius, int iters = 60);
// one step of live physics (VOS gradient + weak gravity); returns movement
double physicsStep(Network& net, double step);

// rescale positions so that the layout spans roughly [-span/2, span/2]
void normaliseSpan(Network& net, double span = 1000);
void rotateToPCA(Network& net);
// scale layout coordinates to world units (RMS radius = k*sqrt(n)); matches the web app
void worldScale(Network& net, double k = 22);

}  // namespace vs
