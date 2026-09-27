#pragma once
// Text similarity for the OpenAlex "expand and re-rank" search.
//
// OpenAlex semantic search returns at most 50 works per query. To build a corpus that is large enough for
// science mapping, the seed works are expanded with their references and the works that cite them, and every
// candidate is then scored by TF-IDF cosine similarity (title, abstract and keywords) to the centroid of the
// seeds. Candidates that are close to the seeds in wording are kept; the rest are dropped.
#include "model.h"

namespace vs {

// lower-case word tokens with stop words removed and light suffix stripping, plus adjacent-word bigrams
vector<string> simTokens(const string& text);
// the text a record contributes: title, abstract, author keywords
string simText(const Record& r);

// cosine similarity (0..1) of every candidate to the centroid of the seed texts (TF-IDF over seeds + candidates)
vector<double> similarityToSeeds(const vector<string>& seedTexts, const vector<string>& candTexts);

struct RerankResult {
  vector<int> keep;       // candidate indices kept, best first
  vector<double> score;   // similarity per candidate (same order as the input)
  double cutoff = 0;      // lowest similarity that was kept
};
// Keep at most maxKeep candidates with similarity >= minSim, best first. Candidates flagged as seeds are always kept
// (they are the semantic search results themselves) and count toward maxKeep.
RerankResult rerankCandidates(const vector<Record>& cands, const vector<char>& isSeed, int maxKeep, double minSim = 0.05);

}  // namespace vs
