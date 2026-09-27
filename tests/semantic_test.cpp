// expand-and-re-rank: candidates close to the seeds in wording rank first, off-topic works are dropped
#include <cstdio>
#include "../src/core/semantic.h"
using namespace vs;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { fails++; printf("FAIL %s\n", msg); } } while (0)
static Record R(const char* t, const char* a) { Record r; r.title = t; r.abstract_ = a; return r; }
int main() {
  vector<Record> c = {
    R("Graph neural networks for molecular property prediction", "We train message passing graph neural networks on molecular graphs to predict quantum chemical properties."),
    R("Message passing neural networks for quantum chemistry", "Molecular graphs are encoded with learned message functions; we predict DFT properties of small molecules."),
    R("Deep learning on molecular graphs: a review", "Graph convolution and message passing architectures for molecules, drug discovery and property prediction."),
    // candidates
    R("Equivariant graph networks for predicting molecular energies", "An E(3)-equivariant message passing network predicts energies and forces of molecules."),
    R("Self-supervised pretraining of graph neural networks for drug discovery", "Pretraining molecular graph encoders improves property prediction on small labelled datasets."),
    R("Supply chain resilience in the automotive industry", "A survey of automotive suppliers on disruptions, inventory buffers and risk management practices."),
    R("Tourism demand forecasting with seasonal ARIMA", "Monthly tourist arrivals are forecast with seasonal time-series models for island destinations."),
    R("Protein structure prediction with attention", "Attention networks predict three-dimensional protein structures from amino acid sequences."),
    R("", ""),
  };
  vector<char> seed = {1, 1, 1, 0, 0, 0, 0, 0, 0};
  auto rr = rerankCandidates(c, seed, 6, 0.05);
  for (size_t i = 0; i < c.size(); i++) printf("  %.3f %s %s\n", rr.score[i], seed[i] ? "seed" : "    ", c[i].title.c_str());
  printf("kept:"); for (int k : rr.keep) printf(" %d", k); printf("  cutoff=%.3f\n", rr.cutoff);
  CHECK(rr.keep.size() >= 5, "seeds + two on-topic kept");
  CHECK(rr.keep[0] <= 2 && rr.keep[1] <= 2 && rr.keep[2] <= 2, "seeds first");
  CHECK(rr.score[3] > rr.score[5] && rr.score[4] > rr.score[6], "on-topic above off-topic");
  CHECK(rr.score[3] > rr.score[7] && rr.score[4] > rr.score[7], "molecular GNN above protein");
  bool offKept = false; for (int k : rr.keep) if (k == 5 || k == 6 || k == 8) offKept = true;
  CHECK(!offKept, "off-topic and empty dropped");
  auto r2 = rerankCandidates(c, seed, 4, 0.0);
  CHECK(r2.keep.size() == 4, "maxKeep respected");
  auto t = simTokens("Graph Neural Networks, and large-scale studies of molecules");
  bool bi = false; for (auto& x : t) if (x == "graph_neural") bi = true;
  CHECK(bi, "bigrams");
  printf("fails=%d\n", fails);
  return fails ? 1 : 0;
}
