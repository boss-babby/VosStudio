// Per-file provenance and conservative duplicate handling: imports retain every record, candidates are reviewable,
// and removing a source removes only the records that source contributed.
#include <cassert>
#include <cstdio>
#include <set>
#include <string>

#include "project.h"
#include "records.h"

using namespace vs;

static int fails = 0;
#define CHECK(c)                                                   \
  do {                                                             \
    if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
  } while (0)

static Record rec(const string& title, int year, const string& doi = "") {
  Record r;
  r.title = title;
  r.year = year;
  r.doi = doi;
  r.source = "Journal of Tests";
  r.authors = {"Doe, J"};
  r.keywords = {"testing", "provenance"};
  return r;
}

int main() {
  Project P;
  {
    vector<Record> a = {rec("Alpha study of something long enough", 2020, "10.1/a"), rec("Beta study of something long enough", 2021, "10.1/b"), rec("Gamma study of something long enough", 2022)};
    CHECK(P.addRecords("first.txt", BibFormat::WoS, a) == 3);
    vector<Record> b = {rec("Beta study of something long enough", 2021, "10.1/b"), rec("Delta study of something long enough", 2023, "10.1/d"), rec("Gamma Study Of Something Long Enough", 2022)};
    CHECK(P.addRecords("second.csv", BibFormat::Scopus, b) == 3);  // duplicate candidates are retained, never silently merged
  }
  CHECK(P.corpus.recs.size() == 6);
  CHECK(P.corpus.files.size() == 2);
  CHECK(P.corpus.duplicatesRemoved == 0);
  CHECK(P.corpus.provenanceKnown());
  Corpus::FileStat f0 = P.corpus.fileStat(0), f1 = P.corpus.fileStat(1);
  CHECK(f0.unique == 3 && f0.shared == 0);
  CHECK(f1.unique == 3 && f1.shared == 0);
  int first = 0, second = 0;
  std::set<string> ids;
  for (const auto& r : P.corpus.recs) {
    if (r.src == 1) first++;
    if (r.src == 2) second++;
    CHECK(!r.id.empty());
    ids.insert(r.id);
  }
  CHECK(first == 3 && second == 3);
  CHECK(ids.size() == P.corpus.recs.size());  // even the duplicated DOI gets distinct local identity
  vector<RecordDuplicateGroup> groups = recordDuplicateGroups(P.corpus.recs);
  CHECK(groups.size() == 2);
  CHECK(groups[0].indices.size() == 2 && groups[0].hasDoiMatch);
  CHECK(groups[1].indices.size() == 2);
  // round trip through project JSON retains identities, added metadata and review flags
  {
    P.corpus.recs[0].url = "https://example.org/alpha";
    P.corpus.recs[0].issue = "4";
    P.corpus.recs[0].extra["PMID"] = "123456";
    P.corpus.recs[0].duplicateReviewed = true;
    const string id = P.corpus.recs[0].id;
    string js = P.toJson(true).dump();
    Project Q;
    string err;
    CHECK(Q.fromJson(Json::parse(js), &err));
    CHECK(err.empty());
    CHECK(Q.corpus.recs.size() == 6);
    CHECK(Q.corpus.provenanceKnown());
    CHECK(Q.corpus.recs[0].id == id);
    CHECK(Q.corpus.recs[0].url == "https://example.org/alpha");
    CHECK(Q.corpus.recs[0].issue == "4");
    CHECK(Q.corpus.recs[0].extra["PMID"] == "123456");
    CHECK(Q.corpus.recs[0].duplicateReviewed);
  }
  // a map built on the corpus keeps valid record indices after a file is removed
  P.spec.type = AnaType::Cooc;
  P.spec.unit = Unit::Keywords;
  P.spec.setDefaults();
  P.spec.min = 1;
  BuildReport rep;
  P.commitBuild(P.computeNetwork(rep, nullptr, nullptr), rep);
  CHECK(P.net.n() >= 1);
  // Removing one source never deletes records imported from the other, even when they are duplicate candidates.
  CHECK(P.removeFile(0) == 3);
  CHECK(P.corpus.recs.size() == 3);
  CHECK(P.corpus.files.size() == 1);
  CHECK(P.corpus.files[0].name == "second.csv");
  CHECK(P.corpus.duplicatesRemoved == 0);
  for (auto& r : P.corpus.recs) CHECK(r.src == 1);  // remaining file is renumbered to bit zero
  for (auto& nd : P.net.nodes) for (int ri : nd.recs) CHECK(ri >= 0 && ri < int(P.corpus.recs.size()));
  bool alphaGone = true;
  for (auto& r : P.corpus.recs) if (r.title.rfind("Alpha", 0) == 0) alphaGone = false;
  CHECK(alphaGone);
  CHECK(recordDuplicateGroups(P.corpus.recs).size() == 0);
  CHECK(P.removeFile(0) == 3);
  CHECK(P.corpus.empty() && P.corpus.files.empty());
  CHECK(P.removeFile(0) == -1);
  // unknown provenance (old project) refuses
  {
    Project R;
    vector<Record> a = {rec("Alpha study of something long enough", 2020, "10.1/a")};
    R.addRecords("old.txt", BibFormat::WoS, a);
    R.corpus.recs[0].src = 0;
    CHECK(!R.corpus.provenanceKnown());
    CHECK(R.removeFile(0) == -1);
  }
  // An explicit merge reports field conflicts, honors per-field choices, keeps the chosen stable ID, and unions list metadata.
  {
    Record keep = rec("Short title", 2020, "10.1000/old");
    keep.id = "rec:keep"; keep.abstract_ = "abstract A"; keep.keywords = {"alpha"}; keep.src = 1; keep.authors = {"Doe, J"}; keep.authorIds = {"orcid:old"};
    Record other = rec("Corrected title", 2021, "10.1000/new");
    other.id = "rec:other"; other.abstract_ = "abstract B"; other.keywords = {"beta"}; other.src = 2; other.authors = keep.authors; other.authorIds = {"orcid:new"};
    vector<RecordFieldConflict> conflicts = recordMergeConflicts(keep, other);
    CHECK(conflicts.size() >= 5);
    std::unordered_map<string, bool> choose = {{"title", true}, {"year", true}, {"DOI", false}, {"abstract", true}, {"author identifiers", true}};
    Record merged = mergeRecords(keep, other, choose);
    CHECK(merged.id == "rec:keep");
    CHECK(merged.title == "Corrected title" && merged.year == 2021);
    CHECK(merged.doi == "10.1000/old" && merged.abstract_ == "abstract B");
    CHECK(merged.src == 3 && merged.duplicateReviewed);
    CHECK(merged.authorIds == other.authorIds);
    CHECK(merged.keywords.size() == 2);
    CHECK(merged.extra["vosstudio.merged-record-ids"].find("rec:other") != string::npos);
  }
  // Reused stable IDs from an import are made unique against the existing project without merging records.
  {
    Project R;
    vector<Record> a = {rec("Repeat record with enough title characters", 2024, "10.999/repeat")};
    R.addRecords("one.ris", BibFormat::RIS, a);
    string id = R.corpus.recs[0].id;
    vector<Record> b = {R.corpus.recs[0]};
    CHECK(R.addRecords("two.ris", BibFormat::RIS, b) == 1);
    CHECK(R.lastRecordIdConflicts == 1);
    CHECK(R.corpus.recs.size() == 2);
    CHECK(R.corpus.recs[0].id == id && R.corpus.recs[1].id != id);
    CHECK(recordDuplicateGroups(R.corpus.recs).size() == 1);
  }
  std::printf(fails ? "corpus_test: %d FAILED\n" : "corpus_test: all checks passed\n", fails);
  return fails ? 1 : 0;
}
