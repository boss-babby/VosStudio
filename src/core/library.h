// The reading library: the PDFs attached to a project's records and everything the reader adds to them —
// highlights, notes, codes (tags), reading status, position — kept in the project file, never in the PDF unless the
// user asks for it. This is the data side of the bibliographic-analysis workflow (Read stage); the reader UI in
// src/win/reader.cpp draws it, the writer quotes from it, the assistant summarises it.
#pragma once
#include "common.h"
#include "json.h"
#include "model.h"
#include "oa.h"
#include "pdfdoc.h"

namespace vs {

// what a highlight means in the analysis — the coding scheme. Fixed set so colours, filters and exports agree;
// "custom" codes live in PdfAnnot::tag as free text.
enum class Code { None = 0, Aim, Method, Finding, Theory, Gap, Quote, Question, Count };
const char* codeName(Code c);       // "Aim", "Method"…
uint32_t codeColor(Code c);         // ARGB highlight colour
Code codeFromName(const string& s); // tolerant (case-insensitive), None when unknown

struct PdfAnnot {
  string id;              // unique within the item ("a12")
  int kind = 0;           // 0 highlight, 1 underline, 2 strikeout, 3 note (sticky), 4 area, 5 freehand Ink
  int page = 0;
  vector<pdf::Box> quads; // text markup rectangles in display points (one per line)
  vector<pdf::InkStroke> strokes; // freehand paths in display points
  float strokeWidth = 1.8f; // points
  pdf::Box rect;          // note anchor / area rectangle / ink bounds
  uint32_t color = 0xFFFFE066;
  Code code = Code::None;
  string tag;             // free-text code when Code::None does not cover it
  string text;            // the highlighted passage (verbatim, hyphenation joined) — the quote
  string note;            // the user's comment
  int start = -1, count = 0;  // character range on the page (re-anchoring, "next highlight")
  string created, modified;   // ISO 8601
  bool inFile = false;    // already written into the PDF file
  bool isMarkup() const { return kind <= 2; }
};

enum class ReadStatus { ToRead = 0, Reading, Read, Excluded, Count };
const char* statusName(ReadStatus s);

// where a fetched file came from (Get PDF, 1.18): shown in the Paper pane so the reader knows whether it is the
// version of record or an author manuscript
struct PdfSource {
  string url, host, kind, version, license, oaStatus, fetched;  // kind: cache | publisher | repository | arxiv | pmc
  bool empty() const { return url.empty() && host.empty(); }
  string versionLabel() const;  // "published version", "accepted manuscript", "preprint", ""
  Json toJson() const;
  static PdfSource fromJson(const Json& j);
};

struct PdfItem {
  string id;             // stable within the project ("p3")
  string path;           // where the file is (absolute); the file may be missing
  string recordKey;      // stable Record.id; pre-upgrade DOI/title keys are migrated when the match is unambiguous
  string title, doi;     // from the record when linked, else from the PDF / file name
  string author;         // first author (display)
  int year = 0;
  int pages = 0;
  long long size = 0;    // file size when attached (identity)
  ReadStatus status = ReadStatus::ToRead;
  int rating = 0;        // 0..5
  vector<string> tags;   // item-level tags ("core", "review", "exclude?"…)
  string notes;          // reading notes (plain text, paragraphs)
  vector<PdfAnnot> annots;
  int nextAnnot = 1;
  double lastPage = 0;   // reading position: page index + fraction
  double zoom = 0;       // 0 = fit width
  int rot = 0;           // view rotation in the reader, quarter turns clockwise (scans that lie on their side)
  string added, opened;  // ISO 8601
  PdfSource source;      // set when the file was fetched by Get PDF
  int annotCount(int kindMask = 0xFF) const { int n = 0; for (auto& a : annots) if (kindMask & (1 << a.kind)) n++; return n; }
  bool hasTag(const string& t) const;
};

struct LibraryStats { int total = 0, toRead = 0, reading = 0, read = 0, excluded = 0, highlights = 0, areas = 0, drawings = 0, notes = 0, withNotes = 0, linked = 0, missing = 0; };

// Library organization is keyed to the same normalized reference key used to link PDFs. Existing projects that do
// not contain these fields load with empty organization metadata and keep their original items unchanged.
struct PaperCollection {
  string id, name;
  vector<string> recordKeys;                 // stable serialization/order
  std::unordered_set<string> recordIndex;    // membership lookup for large libraries
  bool contains(const string& key) const;
};
struct PaperTagSet { string recordKey; vector<string> tags; };
struct SavedPaperView {
  string id, name, query, collectionId, tag;
  int status = -1;  // -1 = any; otherwise ReadStatus value
  int pdf = -1;     // -1 = any; 0 = no linked PDF; 1 = linked PDF
};

class PdfLibrary {
 public:
  vector<PdfItem> items;
  vector<PaperCollection> collections;
  vector<PaperTagSet> recordTags;
  vector<SavedPaperView> savedViews;
  int nextId = 1, nextCollectionId = 1, nextSavedViewId = 1;
  uint64_t organizationVersion = 1;  // cache invalidation for shared tags, Library filters, and organization views
  mutable uint64_t recordTagIndexVersion = 0;
  mutable std::unordered_map<string, vector<string>> recordTagIndex;
  mutable uint64_t allTagsCacheVersion = 0;
  mutable vector<string> allTagsCache;
  std::map<string, oa::FetchStatus> fetch;  // Get PDF outcomes per record key (attached, closed, refused, ...)

  PdfItem* find(const string& id);
  const PdfItem* find(const string& id) const;
  PdfItem* byPath(const string& path);
  PdfItem* byRecord(const string& key);
  int indexOf(const string& id) const;
  PdfItem& add(const string& path);  // returns the existing item when the path is already attached
  bool remove(const string& id);
  PdfAnnot& addAnnot(PdfItem& it, PdfAnnot a);  // assigns id/created; keeps annots sorted by page, then position
  bool removeAnnot(PdfItem& it, const string& annotId);
  PdfAnnot* annot(PdfItem& it, const string& annotId);
  void sortAnnots(PdfItem& it);

  PaperCollection* collection(const string& id);
  const PaperCollection* collection(const string& id) const;
  string addCollection(const string& name);
  bool removeCollection(const string& id);  // removes only the grouping; records/PDFs remain
  bool setCollectionMember(const string& id, const string& recordKey, bool member);
  vector<string> tagsForRecord(const string& recordKey) const;  // shared record tags plus legacy linked-PDF tags
  void setTagsForRecord(const string& recordKey, const vector<string>& tags);  // synchronizes linked PDFs on explicit edit
  string addSavedView(const SavedPaperView& view);
  bool removeSavedView(const string& id);
  void touchOrganization() { ++organizationVersion; }

  Json toJson() const;
  void fromJson(const Json& j);
  bool empty() const { return items.empty(); }  // true when no PDFs are attached
  bool hasData() const { return !items.empty() || !collections.empty() || !recordTags.empty() || !savedViews.empty() || !fetch.empty(); }
  LibraryStats stats(const std::function<bool(const string&)>& exists = nullptr) const;
  const vector<string>& allTags() const;  // cached paper/item tags + free-text annotation tags, sorted, unique
  vector<string> paperTags() const;  // organization tags only (record-level and legacy item tags)

  // linking to the corpus: by DOI, else by normalised title (+ year when both known). Returns the record index or -1.
  static int matchRecord(const Corpus& c, const string& doi, const string& title, int year);
  static string keyOf(const Record& r);        // durable Record.id; legacy ref-key fallback only for unsaved/transient records
  static string legacyKeyOf(const Record& r);  // refKeyFor(doi, title, year), for cautious migration
  int recordIndex(const Corpus& c, const PdfItem& it) const;  // -1 when unlinked or the record is gone
  void link(PdfItem& it, const Record& r);
  int relinkAll(const Corpus& c);  // (re)links every item that has a DOI or title; ambiguous matches remain unlinked
  int migrateRecordKeys(const Corpus& c);  // adopts pre-upgrade DOI/title keys only when they identify one record
  void mergeRecordKeys(const string& fromId, const string& keepId, const Record& keep);  // called only after an explicit duplicate merge

  // pulling identity out of the PDF itself
  static string guessDoi(const string& text);                 // first DOI-looking token in the first pages' text
  static string guessTitle(const pdf::TextPage& firstPage);   // the largest text on the first page
  static string fileTitle(const string& path);                // "Smith 2021 - deep learning.pdf" -> "Smith 2021 - deep learning"

  // exports (Zotero-style notes export): one item or all, Markdown
  string exportMarkdown(const PdfItem& it, const Record* rec) const;
  string exportMarkdownAll(const Corpus* c) const;
  // the coding matrix: one row per item, one column per code, the count of highlights (for the CSV/dashboard)
  string codingCsv(const Corpus* c) const;
};

}  // namespace vs
