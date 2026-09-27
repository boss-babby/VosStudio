#include "semantic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace vs {

namespace {
const std::unordered_set<string>& stopWords() {
  static const std::unordered_set<string> s = {
      "a", "about", "above", "across", "after", "again", "against", "all", "almost", "also", "although", "among", "an", "and", "another",
      "any", "are", "around", "as", "at", "based", "be", "because", "been", "before", "being", "below", "between", "both", "but", "by",
      "can", "could", "did", "do", "does", "doing", "done", "due", "during", "each", "either", "et", "etc", "even", "ever", "every",
      "few", "for", "found", "from", "further", "had", "has", "have", "having", "he", "her", "here", "hers", "high", "him", "his", "how",
      "however", "i", "if", "in", "into", "is", "it", "its", "itself", "just", "less", "low", "made", "make", "many", "may", "more",
      "most", "much", "must", "my", "new", "no", "nor", "not", "novel", "now", "of", "off", "often", "on", "once", "one", "only", "or",
      "other", "our", "ours", "out", "over", "own", "paper", "per", "present", "proposed", "rather", "result", "results", "same",
      "several", "she", "should", "show", "shown", "shows", "since", "so", "some", "study", "studies", "such", "than", "that", "the",
      "their", "theirs", "them", "then", "there", "these", "they", "this", "those", "through", "thus", "to", "too", "two", "under",
      "until", "up", "upon", "use", "used", "uses", "using", "various", "very", "via", "was", "we", "well", "were", "what", "when",
      "where", "whether", "which", "while", "who", "whom", "whose", "why", "will", "with", "within", "without", "would", "yet", "you",
      "your", "article", "research", "approach", "method", "methods", "analysis", "data", "three", "first", "second", "however",
      "elsevier", "rights", "reserved", "copyright", "ltd", "inc", "springer", "wiley", "published", "author", "authors"};
  return s;
}

// very light stemming: plural and a few common suffixes, never below 4 characters
string stem(string w) {
  auto ends = [&](const char* suf) { size_t n = strlen(suf); return w.size() >= n + 3 && w.compare(w.size() - n, n, suf) == 0; };
  if (ends("ies")) { w.resize(w.size() - 3); w += 'y'; }
  else if (ends("sses")) w.resize(w.size() - 2);
  else if (ends("ss")) {}
  else if (ends("s") && !ends("us") && !ends("is")) w.resize(w.size() - 1);
  if (ends("ing") && w.size() > 6) w.resize(w.size() - 3);
  else if (ends("ed") && w.size() > 5) w.resize(w.size() - 2);
  if (ends("ation") && w.size() > 8) w.resize(w.size() - 3);  // optimisation -> optimis(ation) ~ optimise
  return w;
}
}  // namespace

vector<string> simTokens(const string& text) {
  vector<string> words;
  string cur;
  auto flush = [&]() {
    if (cur.size() >= 3 && !stopWords().count(cur)) {
      bool digits = std::all_of(cur.begin(), cur.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
      words.push_back(digits ? string() : stem(cur));  // numbers break bigrams but are not terms
    } else if (!cur.empty()) words.push_back(string());  // stop word: bigram break
    cur.clear();
  };
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) cur += char(c);
    else if (c >= 'A' && c <= 'Z') cur += char(c - 'A' + 'a');
    else if (c == '-' && !cur.empty()) {}  // "large-scale" -> "largescale"
    else flush();
  }
  flush();
  vector<string> out;
  out.reserve(words.size() * 2);
  for (size_t i = 0; i < words.size(); i++) {
    if (words[i].empty()) continue;
    out.push_back(words[i]);
    if (i + 1 < words.size() && !words[i + 1].empty()) out.push_back(words[i] + "_" + words[i + 1]);
  }
  return out;
}

string simText(const Record& r) {
  string t = r.title + ". " + r.title + ". " + r.abstract_;  // title counts twice: it is the most specific text
  for (auto& k : r.keywords) t += ". " + k;
  return t;
}

vector<double> similarityToSeeds(const vector<string>& seedTexts, const vector<string>& candTexts) {
  size_t ns = seedTexts.size(), nc = candTexts.size(), N = ns + nc;
  vector<double> out(nc, 0.0);
  if (!ns || !nc) return out;
  // vocabulary + term counts per document
  std::unordered_map<string, int> vocab;
  vector<vector<std::pair<int, double>>> docs(N);
  vector<int> df;
  for (size_t d = 0; d < N; d++) {
    const string& text = d < ns ? seedTexts[d] : candTexts[d - ns];
    std::unordered_map<int, int> cnt;
    for (auto& t : simTokens(text)) {
      auto it = vocab.find(t);
      int id;
      if (it == vocab.end()) { id = int(vocab.size()); vocab.emplace(t, id); df.push_back(0); }
      else id = it->second;
      cnt[id]++;
    }
    for (auto& kv : cnt) { df[size_t(kv.first)]++; docs[d].push_back({kv.first, double(kv.second)}); }
  }
  // tf-idf, L2 normalised
  vector<double> idf(df.size());
  for (size_t t = 0; t < df.size(); t++) idf[t] = std::log((double(N) + 1) / (double(df[t]) + 1)) + 1;
  for (auto& d : docs) {
    double n2 = 0;
    for (auto& e : d) { e.second = (1 + std::log(e.second)) * idf[size_t(e.first)]; n2 += e.second * e.second; }
    double inv = n2 > 0 ? 1 / std::sqrt(n2) : 0;
    for (auto& e : d) e.second *= inv;
  }
  // seed centroid (dense over the vocabulary), normalised
  vector<double> cen(vocab.size(), 0.0);
  for (size_t d = 0; d < ns; d++) for (auto& e : docs[d]) cen[size_t(e.first)] += e.second;
  double n2 = 0;
  for (double v : cen) n2 += v * v;
  if (n2 <= 0) return out;
  double inv = 1 / std::sqrt(n2);
  for (double& v : cen) v *= inv;
  for (size_t c = 0; c < nc; c++) {
    double dot = 0;
    for (auto& e : docs[ns + c]) dot += e.second * cen[size_t(e.first)];
    out[c] = std::max(0.0, std::min(1.0, dot));
  }
  return out;
}

RerankResult rerankCandidates(const vector<Record>& cands, const vector<char>& isSeed, int maxKeep, double minSim) {
  RerankResult R;
  vector<string> seedT, candT;
  for (size_t i = 0; i < cands.size(); i++) {
    candT.push_back(simText(cands[i]));
    if (i < isSeed.size() && isSeed[i]) seedT.push_back(candT.back());
  }
  R.score = similarityToSeeds(seedT, candT);
  vector<int> order(cands.size());
  std::iota(order.begin(), order.end(), 0);
  auto seed = [&](int i) { return size_t(i) < isSeed.size() && isSeed[size_t(i)]; };
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    if (seed(a) != seed(b)) return seed(a);
    return R.score[size_t(a)] > R.score[size_t(b)];
  });
  R.cutoff = 1;
  for (int i : order) {
    if (int(R.keep.size()) >= std::max(1, maxKeep) && !seed(i)) break;
    if (!seed(i) && R.score[size_t(i)] < minSim) break;
    // a candidate without any text (no title/abstract) cannot be judged: dropped unless it is a seed
    R.keep.push_back(i);
    if (!seed(i)) R.cutoff = std::min(R.cutoff, R.score[size_t(i)]);
  }
  if (R.cutoff == 1) R.cutoff = 0;
  return R;
}

}  // namespace vs
