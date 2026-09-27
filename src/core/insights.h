// VOSStudio Native — additional bibliometric analyses (1.2):
// trend topics, reference publication year spectroscopy, production over time, country collaboration,
// local citations (historiograph), citation summary and cluster activity over time.
#pragma once
#include "stats.h"

namespace vs {

// ---- trend topics (Bibliometrix style): for each frequent term, the quartiles of its publication years
struct TrendTopic { string label; int freq = 0; double q1 = 0, med = 0, q3 = 0; };
vector<TrendTopic> trendTopics(const Corpus& c, Unit unit, int minFreq = 3, int perYear = 3, const Thesaurus* th = nullptr);

// ---- RPYS: cited references per publication year and the deviation from the 5-year median
int refYear(const string& ref);  // year inside a cited-reference string (WoS or Scopus style), 0 = none
struct RpysPeak { int year = 0, count = 0, topRefCount = 0; double dev = 0; string topRef; };
struct Rpys {
  int y0 = 0, y1 = 0;
  vector<int> counts;   // [year - y0]
  vector<double> dev;   // counts - median(counts[y-2..y+2])
  vector<RpysPeak> peaks;
  long long refs = 0, dated = 0;
};
Rpys rpys(const Corpus& c, int from = 0, int to = 0);

// ---- production of the top actors over time
struct Production {
  vector<string> labels;
  int y0 = 0, y1 = 0;
  vector<vector<int>> docs;             // [actor][year - y0]
  vector<vector<double>> citesPerYear;  // total citations per year since publication
};
Production productionOverTime(const Corpus& c, Unit unit, int top = 10);

// ---- single- vs multiple-country publications per country
struct CountryCollab { string country; int scp = 0, mcp = 0; };
vector<CountryCollab> countryCollaboration(const Corpus& c, int top = 15);

// ---- local citations: which documents of the corpus cite each other (matched by DOI, WoS key or title)
struct CitationIndex {
  vector<vector<int>> cites;    // [doc] -> docs of the corpus it cites
  vector<vector<int>> citedBy;  // [doc] -> docs of the corpus citing it (size = local citation score)
  int links = 0;
  size_t sig = 0;               // corpus signature this index belongs to
};
CitationIndex localCitations(const Corpus& c);
size_t corpusSignature(const Corpus& c);

struct CitationSummary {
  int docs = 0, uncited = 0, h = 0, g = 0;
  long long total = 0;
  double mean = 0, median = 0;
  vector<std::pair<string, int>> bins;  // citation-count classes
};
CitationSummary citationSummary(const Corpus& c);

// ---- activity of each map cluster per year (documents containing any item of the cluster)
struct ClusterYears { int y0 = 0, y1 = 0; vector<vector<int>> docs; };  // [cluster][year - y0]
ClusterYears clusterYears(const Network& net, const Corpus& c);

// ---- citation strings
string firstAuthorSurname(const Record& r);
string shortCite(const Record& r);    // "Smith et al. (2019)"
string apaCitation(const Record& r);
string sourceCase(const string& s);  // keeps the database's capitalisation; ALL-CAPS sources (WoS) become Title Case  // "Smith, A., & Lee, B. (2019). Title. Source, 12, 1-10. https://doi.org/…"

}  // namespace vs
