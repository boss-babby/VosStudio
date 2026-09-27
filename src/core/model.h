// VOSStudio Native — data model: bibliographic records and the item network
#pragma once
#include "common.h"

namespace vs {

// ------------------------------------------------------------ records
struct Record {
  string title, abstract_, source, doi, volume, pages, docType, publisher, language, key;
  int year = 0;   // 0 = unknown
  int cites = 0;
  vector<string> authors, keywords, indexTerms, affiliations, countries, refs;
};

enum class BibFormat { Unknown, WoS, WoSTab, Scopus, RIS, BibTeX, OpenAlex, VOSviewer, Bundle };
const char* formatLabel(BibFormat f);

struct SourceFile {
  string name;
  BibFormat format = BibFormat::Unknown;
  int records = 0;
};

struct Corpus {
  vector<Record> recs;
  BibFormat format = BibFormat::Unknown;
  vector<SourceFile> files;
  int duplicatesRemoved = 0;
  bool empty() const { return recs.empty(); }
  void clear() { recs.clear(); files.clear(); format = BibFormat::Unknown; duplicatesRemoved = 0; }
};

struct QualityReport {
  int records = 0, yearMin = 0, yearMax = 0, sources = 0, authors = 0;
  double citesPerDoc = 0;
  double doiCov = 0, absCov = 0, kwCov = 0, idCov = 0, refCov = 0, affCov = 0, yearCov = 0;
  long long totalRefs = 0, totalCites = 0;
};
QualityReport qualityReport(const Corpus& c);

// ------------------------------------------------------------ network
struct Node {
  string id, label, title, doi, key;
  double x = 0, y = 0, z = 0;
  int cluster = 0;
  vector<double> w;   // weights (see Network::weightNames)
  vector<double> sc;  // scores  (see Network::scoreNames)
  vector<int> recs;   // record indices behind the item (bibliographic maps)
  bool pinned = false;
};
struct Link {
  int a = 0, b = 0;
  double w = 1;   // raw strength
  double s = 1;   // normalised similarity used for layout/clustering
};
struct Network {
  vector<Node> nodes;
  vector<Link> links;
  vector<string> weightNames{"Links", "Total link strength"};
  vector<string> scoreNames;
  vector<string> clusterNames;  // optional user/auto names
  int weightIdx = 0, scoreIdx = -1;
  int nClusters = 0;
  double quality = 0;
  string clusterTag;             // settings that produced the clusters (see clusterTag()); "" = unknown (imported)
  string unitNoun = "item";      // e.g. "keyword"
  string description;            // "co-occurrence of author keywords"
  int n() const { return int(nodes.size()); }
  int m() const { return int(links.size()); }
  double weight(int i) const { const auto& w = nodes[i].w; return weightIdx >= 0 && weightIdx < int(w.size()) ? w[weightIdx] : 1; }
  double score(int i) const { const auto& s = nodes[i].sc; return scoreIdx >= 0 && scoreIdx < int(s.size()) ? s[scoreIdx] : NAN; }
  int weightIndex(const string& name) const;
  int scoreIndex(const string& name) const;
  void recomputeLinkWeights();   // "Links" and "Total link strength" from links
  void countClusters();
  string clusterName(int c) const;
};

}  // namespace vs
