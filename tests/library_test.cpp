// library_test: the reading library (attachments, annotations, coding, linking, exports) — pure core, no PDFium.
#include <cmath>
#include <iostream>

#include "library.h"

using namespace vs;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; std::cerr << "FAIL " << __LINE__ << ": " #c << "\n"; } } while (0)

int main() {
  PdfLibrary lib;
  // attach, dedup by path, ids
  PdfItem& a = lib.add("C:\\papers\\Smith_2021 - deep learning.pdf");
  CHECK(a.id == "p1" && a.title == "Smith 2021 - deep learning" && !a.added.empty());
  PdfItem& again = lib.add("c:/papers/smith_2021 - deep learning.pdf");
  CHECK(&again == &lib.items[0] && lib.items.size() == 1);
  PdfItem& b = lib.add("/home/x/other.pdf");
  CHECK(b.id == "p2" && lib.items.size() == 2);

  // annotations: ids, order by page/position, code colours, removal
  PdfAnnot h;
  h.kind = 0; h.page = 3; h.quads = {{72, 300, 300, 312}}; h.text = "second on page 3"; h.code = Code::Finding;
  PdfAnnot& h1 = lib.addAnnot(lib.items[0], h);
  CHECK(h1.id == "a1" && h1.color == codeColor(Code::Finding) && !h1.created.empty());
  PdfAnnot h2;
  h2.kind = 0; h2.page = 1; h2.quads = {{72, 100, 200, 112}}; h2.text = "first on page 1";
  PdfAnnot& h2Added = lib.addAnnot(lib.items[0], h2);
  CHECK(h2Added.id == "a2" && h2Added.page == 1);  // sorted to the front, but addAnnot still returns the new mark
  PdfAnnot h3;
  h3.kind = 3; h3.page = 3; h3.rect = {72, 100, 92, 120}; h3.note = "a sticky";
  lib.addAnnot(lib.items[0], h3);
  CHECK(lib.items[0].annots.size() == 3 && lib.items[0].annots[0].page == 1 && lib.items[0].annots[1].kind == 3 && lib.items[0].annots[2].id == "a1");
  CHECK(lib.annot(lib.items[0], "a2") && lib.annot(lib.items[0], "a2")->text == "first on page 1");
  CHECK(lib.removeAnnot(lib.items[0], "a2") && lib.items[0].annots.size() == 2 && !lib.removeAnnot(lib.items[0], "a2"));
  lib.items[0].status = ReadStatus::Reading;
  lib.items[0].rating = 4;
  lib.items[0].tags = {"core", "Review"};
  lib.items[0].notes = "Key paper.\nSecond paragraph.";
  lib.items[0].lastPage = 3.25;
  lib.items[0].rot = 3;
  lib.items[1].tags = {"review"};
  PdfAnnot ink;
  ink.kind = 5; ink.page = 2; ink.color = 0xFF2255AA; ink.strokeWidth = 2.5f;
  pdf::InkStroke path; path.points = {{12.25f, 18.5f}, {20.75f, 22.0f}, {31.5f, 19.25f}};
  ink.strokes.push_back(path); ink.rect = {12.25f, 18.5f, 31.5f, 22.0f};
  lib.addAnnot(lib.items[1], ink);

  // round trip through JSON
  Json j = lib.toJson();
  string text = j.dump();
  Json back = Json::parse(text);
  PdfLibrary lib2;
  lib2.fromJson(back);
  CHECK(lib2.items.size() == 2 && lib2.nextId == 3 && lib2.items[0].id == "p1" && lib2.items[0].nextAnnot == 4);
  CHECK(lib2.items[0].annots.size() == 2 && lib2.items[0].annots[1].code == Code::Finding && lib2.items[0].annots[1].quads.size() == 1 && std::fabs(lib2.items[0].annots[1].quads[0].x1 - 300) < 0.01);
  CHECK(lib2.items[0].annots[0].kind == 3 && lib2.items[0].annots[0].note == "a sticky" && std::fabs(lib2.items[0].annots[0].rect.y1 - 120) < 0.01);
  CHECK(lib2.items[1].annots.size() == 1 && lib2.items[1].annots[0].kind == 5 && std::fabs(lib2.items[1].annots[0].strokeWidth - 2.5f) < 0.01);
  CHECK(lib2.items[1].annots[0].strokes.size() == 1 && lib2.items[1].annots[0].strokes[0].points.size() == 3 && std::fabs(lib2.items[1].annots[0].strokes[0].points[1].x - 20.75f) < 0.01f);
  CHECK(lib2.items[0].status == ReadStatus::Reading && lib2.items[0].rating == 4 && lib2.items[0].tags.size() == 2 && lib2.items[0].notes.find("Second") != string::npos && std::fabs(lib2.items[0].lastPage - 3.25) < 1e-6 && lib2.items[0].rot == 3);
  CHECK(lib2.toJson().dump() == text);
  // a hand-edited file whose counters lag behind: ids still unique
  Json edited = Json::parse(text);
  edited.set("nextId", 1);
  PdfLibrary lib3;
  lib3.fromJson(edited);
  CHECK(lib3.nextId == 3 && lib3.add("/new.pdf").id == "p3");
  vector<string> tags = lib2.allTags();
  CHECK(tags.size() == 2 && tags[0] == "core" && tags[1] == "Review");  // "review" deduplicated case-insensitively
  LibraryStats st = lib2.stats([](const string& p) { return p.find("other") == string::npos; });
  CHECK(st.total == 2 && st.reading == 1 && st.toRead == 1 && st.highlights == 1 && st.areas == 0 && st.drawings == 1 && st.notes == 1 && st.withNotes == 1 && st.missing == 1 && st.linked == 0);

  // linking to records
  Corpus c;
  Record r1; r1.id = "rec:library-1"; r1.title = "Deep Learning for Bibliometrics: A Review"; r1.doi = "https://doi.org/10.1000/ABC.123"; r1.year = 2021; r1.authors = {"Smith, J.", "Doe, A."}; r1.source = "Scientometrics";
  Record r2; r2.id = "rec:library-2"; r2.title = "Another paper about something else entirely"; r2.year = 2019;
  c.recs = {r1, r2};
  CHECK(PdfLibrary::matchRecord(c, "10.1000/abc.123", "", 0) == 0);
  CHECK(PdfLibrary::matchRecord(c, "", "deep learning for bibliometrics: a review", 2021) == 0);
  CHECK(PdfLibrary::matchRecord(c, "", "Deep learning for bibliometrics", 0) == 0);   // a long prefix (subtitle dropped) still matches
  CHECK(PdfLibrary::matchRecord(c, "", "Deep learning for", 0) == -1);                // a short one does not
  CHECK(PdfLibrary::matchRecord(c, "", "Another paper about something else entirely", 2010) == -1);  // year too far
  CHECK(PdfLibrary::matchRecord(c, "", "short", 0) == -1);
  lib2.items[0].doi = "10.1000/abc.123";
  lib2.items[1].title = "another paper about something else entirely";
  CHECK(lib2.relinkAll(c) == 2 && lib2.items[0].recordKey == PdfLibrary::keyOf(r1) && lib2.items[0].author == "Smith" && lib2.items[0].year == 2021 && lib2.items[0].title == r1.title);
  CHECK(lib2.recordIndex(c, lib2.items[1]) == 1 && lib2.byRecord(PdfLibrary::keyOf(r2)) == &lib2.items[1]);
  CHECK(lib2.relinkAll(c) == 0);  // already linked: nothing to do
  CHECK(PdfLibrary::keyOf(r1) == r1.id && PdfLibrary::keyOf(r2) == r2.id);
  {
    PdfLibrary oldLib;
    Record paper = r1;
    string legacy = PdfLibrary::legacyKeyOf(paper);
    PdfItem& oldItem = oldLib.add("C:\\\\legacy\\\\paper.pdf"); oldItem.recordKey = legacy; oldItem.doi = paper.doi; oldItem.title = paper.title; oldItem.year = paper.year;
    string oldCollection = oldLib.addCollection("Legacy set"); oldLib.setCollectionMember(oldCollection, legacy, true);
    oldLib.setTagsForRecord(legacy, {"legacy-tag"});
    oldLib.fetch[legacy] = oa::FetchStatus(); oldLib.fetch[legacy].state = oa::FetchState::Closed;
    Corpus current; current.recs.push_back(paper);
    CHECK(oldLib.migrateRecordKeys(current) >= 3);
    CHECK(oldItem.recordKey == paper.id && oldLib.collections[0].contains(paper.id));
    CHECK(oldLib.tagsForRecord(paper.id) == vector<string>{"legacy-tag"});
    CHECK(oldLib.fetch.count(paper.id) == 1 && oldLib.fetch.count(legacy) == 0);
  }
  {
    Corpus duplicate;
    Record a = r1, b = r1; b.id = "rec:library-duplicate"; b.title = "A different title for this same DOI"; duplicate.recs = {a, b};
    CHECK(PdfLibrary::matchRecord(duplicate, r1.doi, "", 0) == -1);  // duplicate DOI alone is ambiguous
    CHECK(PdfLibrary::matchRecord(duplicate, r1.doi, b.title, b.year) == 1);  // title can disambiguate
  }

  // identity from the PDF's text
  CHECK(PdfLibrary::guessDoi("Scientometrics (2021) 126:1-20\nhttps://doi.org/10.1007/s11192-020-03690-4\nAbstract") == "10.1007/s11192-020-03690-4");
  CHECK(PdfLibrary::guessDoi("DOI: 10.1000/xyz123. Received") == "10.1000/xyz123");
  CHECK(PdfLibrary::guessDoi("Version 10.2 of the software") == "");
  CHECK(PdfLibrary::fileTitle("C:\\x\\Hello_World%20paper.PDF") == "Hello World paper");
  {
    pdf::TextPage fp;
    auto put = [&](const string& s, float y, float h) {
      int first = int(fp.chars.size());
      float x = 72;
      for (char ch : s) { pdf::Char c; c.cp = char32_t(ch); c.b = {x, y, x + h * 0.5f, y + h}; x += h * 0.5f; fp.chars.push_back(c); }
      pdf::Char nl; nl.cp = '\n'; nl.generated = true; fp.chars.push_back(nl);
      fp.lines.push_back({first, int(fp.chars.size()) - 1});
    };
    put("Journal of Things 12(3)", 40, 8);
    put("A Large Title That", 100, 20);
    put("Spans Two Lines", 124, 20);
    put("John Smith, Jane Doe", 160, 10);
    fp.text = "x";
    CHECK(PdfLibrary::guessTitle(fp) == "A Large Title That Spans Two Lines");
  }

  // exports
  string md = lib2.exportMarkdown(lib2.items[0], &c.recs[0]);
  CHECK(md.find("## Deep Learning for Bibliometrics") == 0 && md.find("Smith, J.; Doe, A. (2021). *Scientometrics*") != string::npos && md.find("https://doi.org/10.1000/abc.123") != string::npos);
  CHECK(md.find("Status: Reading") != string::npos && md.find("Rating: ****") != string::npos && md.find("Tags: core, Review") != string::npos);
  CHECK(md.find("### Notes\n\nKey paper.") != string::npos && md.find("**Finding** \u201Csecond on page 3\u201D (p. 4)") != string::npos && md.find("**Note** (p. 4): a sticky") != string::npos);
  string all = lib2.exportMarkdownAll(&c);
  CHECK(all.find("# Reading notes") == 0 && all.find("2 papers") != string::npos && all.find("## Another paper") != string::npos);
  string csv = lib2.codingCsv(&c);
  CHECK(csv.find("Title,Year,DOI,Status,Rating,Tags,Aim,Method,Finding,Theory,Gap,Quote,Question,Other highlights,Areas,Drawings,Notes\n") == 0);
  CHECK(csv.find("Deep Learning for Bibliometrics: A Review,2021,10.1000/abc.123,Reading,4,core; Review,0,0,1,0,0,0,0,0,0,0,1\n") != string::npos);
  CHECK(csv.find("Another paper about something else entirely,2019,,To read,0,review,0,0,0,0,0,0,0,0,0,1,0\n") != string::npos);
  CHECK(codeFromName("results") == Code::Finding && codeFromName("METHOD") == Code::Method && codeFromName("nope") == Code::None && string(codeName(Code::Gap)) == "Gap");

  // 1.18: where a fetched file came from, and the outcome of Get PDF per record, survive the project round trip
  {
    PdfLibrary org;
    Record paper; paper.title = "Shared tags without a PDF"; paper.doi = "10.5555/shared.tags"; paper.year = 2024;
    const string key = PdfLibrary::keyOf(paper);
    PdfItem& linked = org.add("C:\\proj\\shared-tags.pdf");
    linked.tags = {"Legacy", "methods"};
    org.link(linked, paper);  // linking migrates old PDF tags into the shared record tag set
    CHECK(linked.recordKey == key && org.tagsForRecord(key).size() == 2);
    bool annotationPresentBeforeAdding = false;
    for (const string& tag : org.allTags()) if (tag == "annotation-code") annotationPresentBeforeAdding = true;
    PdfAnnot codeTag; codeTag.tag = "annotation-code"; string codeTagId = org.addAnnot(linked, codeTag).id;
    vector<string> organizationTags = org.paperTags();
    bool annotationAppearsInBrowseTags = false, annotationAppearsInEditTags = false;
    for (const string& tag : org.allTags()) if (tag == "annotation-code") annotationAppearsInBrowseTags = true;
    for (const string& tag : organizationTags) if (tag == "annotation-code") annotationAppearsInEditTags = true;
    CHECK(!annotationPresentBeforeAdding && annotationAppearsInBrowseTags && !annotationAppearsInEditTags && organizationTags.size() == 2);
    CHECK(org.removeAnnot(linked, codeTagId));
    bool annotationStillInBrowseTags = false;
    for (const string& tag : org.allTags()) if (tag == "annotation-code") annotationStillInBrowseTags = true;
    CHECK(!annotationStillInBrowseTags);
    string collection = org.addCollection("Screening");
    CHECK(collection == "c1" && org.addCollection("screening").empty());
    CHECK(org.setCollectionMember(collection, key, true) && !org.setCollectionMember(collection, key, true));
    CHECK(org.collection(collection) && org.collection(collection)->contains(key));
    SavedPaperView view; view.name = "Recent screening"; view.query = "soil"; view.status = int(ReadStatus::Reading);
    view.pdf = 1; view.collectionId = collection; view.tag = "methods";
    string viewId = org.addSavedView(view);
    CHECK(viewId == "v1" && org.addSavedView(view).empty());
    CHECK(!org.removeCollection(collection));  // a saved view still depends on it
    org.setTagsForRecord(key, {"Legacy", "review", "REVIEW"});
    CHECK(org.tagsForRecord(key).size() == 2 && linked.tags.size() == 2 && linked.tags[1] == "review");
    PaperCollection& second = org.collections[0];
    CHECK(second.recordKeys.size() == 1 && second.contains(key));
    Json orgJson = Json::parse(org.toJson().dump());
    PdfLibrary org2; org2.fromJson(orgJson);
    CHECK(org2.collections.size() == 1 && org2.collections[0].name == "Screening" && org2.collections[0].contains(key));
    CHECK(org2.savedViews.size() == 1 && org2.savedViews[0].query == "soil" && org2.savedViews[0].status == int(ReadStatus::Reading) && org2.savedViews[0].pdf == 1);
    CHECK(org2.tagsForRecord(key).size() == 2 && org2.items[0].tags == linked.tags);
    CHECK(org2.toJson().dump() == orgJson.dump());
    CHECK(org2.removeSavedView(viewId) && org2.removeCollection(collection));
    CHECK(org2.collections.empty() && org2.savedViews.empty());

    // A legacy library with only per-PDF tags remains loadable and can be explicitly promoted to shared tags.
    PdfLibrary legacy; legacy.fromJson(Json::parse(R"({"items":[{"id":"p1","path":"legacy.pdf","record":"rk","tags":["old"]}]})"));
    CHECK(legacy.collections.empty() && legacy.savedViews.empty() && legacy.tagsForRecord("rk").size() == 1);
    legacy.setTagsForRecord("rk", {"old", "shared"});
    CHECK(legacy.items[0].tags.size() == 2 && legacy.tagsForRecord("rk").size() == 2);
    PdfLibrary detached; detached.fromJson(Json::parse(R"({"items":[{"id":"p1","path":"legacy.pdf","record":"rk","tags":["keep"]}]})"));
    CHECK(detached.remove("p1") && detached.tagsForRecord("rk").size() == 1 && detached.tagsForRecord("rk")[0] == "keep");
  }

  {
    PdfLibrary l3;
    PdfItem& f = l3.add("C:\\proj\\proj attachments\\Smith_2021_deep-learning_ab12.pdf");
    f.source.url = "https://content.openalex.org/works/W123.pdf";
    f.source.host = "OpenAlex";
    f.source.kind = "cache";
    f.source.version = "publishedVersion";
    f.source.license = "cc-by";
    f.source.oaStatus = "gold";
    f.source.fetched = "2026-09-30T10:00:00Z";
    CHECK(f.source.versionLabel() == "published version" && !f.source.empty() && PdfSource().empty());
    oa::FetchStatus st;
    st.state = oa::FetchState::Refused;
    st.when = "2026-09-30T10:01:00Z";
    st.detail = "onlinelibrary.wiley.com: refused (HTTP 403)";
    st.landing = "https://doi.org/10.1002/asi.24345";
    st.oaLink = "https://onlinelibrary.wiley.com/doi/pdf/10.1002/asi.24345";
    st.oaStatus = "hybrid";
    st.cachedAvailable = true;
    l3.fetch["k1"] = st;
    oa::FetchStatus none;  // a None status is not written
    l3.fetch["k2"] = none;
    string e3;
    Json b3 = Json::parse(l3.toJson().dump(), &e3);
    CHECK(e3.empty());
    PdfLibrary l4;
    l4.fromJson(b3);
    CHECK(l4.items.size() == 1 && l4.items[0].source.kind == "cache" && l4.items[0].source.host == "OpenAlex" && l4.items[0].source.license == "cc-by" && l4.items[0].source.version == "publishedVersion");
    CHECK(l4.fetch.size() == 1 && l4.fetch.count("k1") == 1 && l4.fetch["k1"].state == oa::FetchState::Refused && l4.fetch["k1"].cachedAvailable && l4.fetch["k1"].oaLink == st.oaLink && l4.fetch["k1"].needsFile());
    CHECK(contains(l4.fetch["k1"].message(false), "add a free OpenAlex API key") && !contains(l4.fetch["k1"].message(true), "add a free"));
    l4.fromJson(Json::parse("{\"items\":[]}"));
    CHECK(l4.fetch.empty() && l4.items.empty());
  }

  if (fails) { std::cerr << fails << " check(s) failed\n"; return 1; }
  std::cout << "library_test: all checks passed\n";
  return 0;
}
