// Data-cleaning studio 2.0: author identifiers from WoS / Scopus / OpenAlex, author disambiguation, acronym expansion
// from text, irregular plurals, confidence ranking, BibTeX export.
// Run:  g++ -std=c++17 -O1 tests/clean_test.cpp src/core/*.cpp -Isrc/core -o build/clean_test -pthread && ./build/clean_test
#include <iostream>

#include "analysis.h"
#include "project.h"
#include "records.h"

using namespace vs;

static int fails = 0;
#define CHECK(c)                                                                     \
  do {                                                                               \
    if (!(c)) { fails++; std::cerr << "FAIL " << __LINE__ << ": " #c << "\n"; }      \
  } while (0)

static Record rec(const string& title, int year, vector<string> authors, vector<string> affs, vector<string> kws, vector<string> ids = {}) {
  Record r;
  r.title = title;
  r.year = year;
  r.authors = authors;
  r.affiliations = affs;
  r.keywords = kws;
  r.authorIds = ids;
  r.source = "Journal of Tests";
  return r;
}

int main() {
  // --- WoS identifiers (OI = ORCID, RI = ResearcherID) matched to the author list by surname + initial
  string wos = "FN Clarivate Analytics Web of Science\nVR 1.0\nPT J\nAU Smith, JA\n   Doe, B\nAF Smith, John A.\n   Doe, Bob\nTI A first paper\n"
               "SO J TEST\nRI Doe, Bob/A-1234-2010\nOI Smith, John/0000-0001-2345-6789\nPY 2020\nDI 10.1000/abc\nER\n\nEF\n";
  vector<Record> w;
  CHECK(parseRecords(wos, BibFormat::WoS, w) == 1);
  CHECK(w.size() == 1 && w[0].authorIds.size() == 2);
  if (w.size() == 1 && w[0].authorIds.size() == 2) {
    CHECK(w[0].authorIds[0] == "0000-0001-2345-6789");
    CHECK(w[0].authorIds[1] == "rid:A-1234-2010");
  }
  // --- project JSON round trip keeps the ids
  if (!w.empty()) {
    Record back = recordFromJson(recordToJson(w[0]));
    CHECK(back.authorIds == w[0].authorIds);
  }
  // --- OpenAlex ORCID / author id
  Json oa = Json::parse(R"({"results":[{"id":"https://openalex.org/W1","title":"Deep work","publication_year":2021,
    "authorships":[{"author":{"id":"https://openalex.org/A1","display_name":"Ada Lovelace","orcid":"https://orcid.org/0000-0002-1111-2222"},"institutions":[]},
                   {"author":{"id":"https://openalex.org/A2","display_name":"Charles Babbage","orcid":null},"institutions":[]}]}]})");
  vector<Record> o;
  CHECK(parseOpenAlex(oa, o) == 1);
  CHECK(o.size() == 1 && o[0].authorIds.size() == 2 && o[0].authorIds[0] == "0000-0002-1111-2222" && o[0].authorIds[1] == "openalex:A2");

  // --- author disambiguation
  Corpus c;
  c.recs.push_back(rec("Paper one", 2019, {"Smith, J.", "Wang, Y."}, {"Univ Oxford", "Peking Univ"}, {"machine learning"}, {"0000-0001-2345-6789", ""}));
  c.recs.push_back(rec("Paper two", 2020, {"Smith, John A.", "Lee, K."}, {"Univ Oxford"}, {"ML"}, {"0000-0001-2345-6789", ""}));
  c.recs.push_back(rec("Paper three", 2021, {"Smith, J. A.", "Lee, K."}, {"Univ Oxford"}, {"machine-learning"}));
  c.recs.push_back(rec("Paper four", 2021, {"Smith, Jane"}, {"MIT"}, {"analyses"}, {"0000-0009-9999-9999"}));  // different person: other ORCID
  c.recs.push_back(rec("Paper five", 2022, {"Wang, Yong", "Li, X."}, {"Peking Univ"}, {"analysis"}));
  c.recs.push_back(rec("Paper six", 2022, {"Wang, Yan"}, {"Tsinghua Univ"}, {"statistical analysis"}));
  vector<VariantGroup> av = findAuthorVariants(c);
  const VariantGroup* smith = nullptr;
  const VariantGroup* wang = nullptr;
  for (auto& g : av) {
    string all = g.target;
    for (auto& m : g.members) all += "|" + m;
    if (all.find("Smith") != string::npos) smith = &g;
    if (all.find("Wang") != string::npos) wang = &g;
  }
  CHECK(smith != nullptr);
  if (smith) {
    CHECK(smith->members.size() == 2);  // Smith, J. + Smith, J. A. join Smith, John A.; Smith, Jane (other ORCID) stays out
    bool jane = false;
    for (auto& m : smith->members) if (m == "Smith, Jane") jane = true;
    CHECK(!jane && smith->target != "Smith, Jane");
    CHECK(smith->score >= 0.8);
    CHECK(!smith->evidence.empty());
  }
  CHECK(wang != nullptr);
  if (wang) {
    CHECK(wang->members.size() == 1);  // Wang, Y. + Wang, Yong (shared affiliation); Wang, Yan differs in given name and must not ride along
    bool yan = wang->target == "Wang, Yan";
    for (auto& m : wang->members) if (m == "Wang, Yan") yan = true;
    CHECK(!yan);
    CHECK(wang->score < 0.99 && !wang->safe);
  }
  CHECK(av.size() >= 2 && av[0].score >= av.back().score);  // ranked

  // --- keyword variants: irregular plural, hyphen, acronym from text
  c.recs[1].abstract_ = "We apply machine learning (ML) to citation data. Machine learning (ML) works.";
  auto dict = acronymDictionary(c);
  CHECK(dict.count("ml") && dict["ml"] == "machine learning");
  vector<VariantGroup> kv = findVariants(c, Unit::Keywords, Thesaurus());
  bool plural = false, acr = false, hyphen = false;
  for (auto& g : kv) {
    for (auto& m : g.members) {
      if ((g.target == "analysis" && m == "analyses") || (g.target == "analyses" && m == "analysis")) plural = true;
      if ((g.target == "machine learning" || g.target == "machine-learning") && m == "ML") acr = true;
      if ((g.target == "machine learning" && m == "machine-learning") || (g.target == "machine-learning" && m == "machine learning")) hyphen = true;
    }
    CHECK(g.score > 0);
  }
  CHECK(plural);
  CHECK(hyphen);
  CHECK(acr);
  CHECK(variantScore("hyphen") > variantScore("acronym (text)") && variantScore("acronym (text)") > variantScore("typo"));

  // --- BibTeX
  string bib = writeRecords({c.recs[0], c.recs[1]}, RecordExport::BibTeX);
  CHECK(bib.find("@article{smith2019paper,") != string::npos);
  CHECK(bib.find("author = {Smith, J. and Wang, Y.}") != string::npos || bib.find("author = {Smith, J and Wang, Y}") != string::npos);
  CHECK(bib.find("title = {{Paper one}}") != string::npos);
  CHECK(bib.find("journal = {Journal of Tests}") != string::npos);
  CHECK(bib.find("keywords = {machine learning}") != string::npos);
  CHECK(bib.find(",\n}") == string::npos);  // no trailing comma before the closing brace
  Record amp = c.recs[0];
  amp.title = "Salt & pepper: 100% coverage";
  string bib2 = writeRecords({amp}, RecordExport::BibTeX);
  CHECK(bib2.find("Salt \\& pepper: 100\\% coverage") != string::npos);

  std::cout << (fails ? "FAILED" : "clean_test OK") << " (" << fails << " failures)\n";
  return fails ? 1 : 0;
}
