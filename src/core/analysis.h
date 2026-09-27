// VOSStudio Native — analysis builder engine (VOSviewer-style types × units)
#pragma once
#include "model.h"

namespace vs {

enum class AnaType { Cooc, Coauth, Citation, Coupling, Cocit };
enum class Unit { AllKeywords, Keywords, IndexTerms, Terms, Authors, Orgs, Countries, Docs, Sources, Refs, CSources, CAuthors };

struct TypeInfo {
  AnaType type;
  const char* id;
  const char* label;
  const char* hint;
  vector<std::pair<Unit, const char*>> units;
};
const vector<TypeInfo>& anaTypes();
const TypeInfo& typeInfo(AnaType t);
const char* unitId(Unit u);
const char* unitNoun(Unit u);         // singular: "keyword"
string unitNounPlural(Unit u, int n = 2);
string unitLabel(AnaType t, Unit u);  // "Author keywords"
bool unitFromId(const string& s, Unit& u);
bool typeFromId(const string& s, AnaType& t);

struct AnaSpec {
  AnaType type = AnaType::Cooc;
  Unit unit = Unit::Keywords;
  bool fractional = false;
  int min = 5;           // main threshold (occurrences / documents / citations)
  int minCites = 0;      // secondary threshold (documents-measured units)
  int maxItems = 1000;
  double minLink = 0;
  int maxAuth = 25;      // ignore documents with more authors (co-authorship)
  int y0 = 0, y1 = 0;    // year filter (0 = open)
  bool largest = true;   // only the largest connected set
  double termKeep = 0.6; // terms: keep the most relevant share
  void setDefaults();    // VOSviewer defaults for type/unit
};

struct Measure { bool cites; const char* label; bool secondary; };
Measure anaMeasure(const AnaSpec& s);
string thresholdLabel(const AnaSpec& s);

struct Thesaurus {
  std::map<string, string> replace;  // key → replacement label ("" = ignore)
  bool empty() const { return replace.empty(); }
  string sig() const;
  bool load(const string& text, string* err = nullptr);  // VOSviewer thesaurus file
  string save() const;
};

struct RawItem {
  string key, label, title, doi;
  double occ = 0, cites = 0, yearSum = 0;
  int yearN = 0;
  vector<int> recs;
};
struct PairW { uint32_t a, b; double w; };  // a < b
struct RawNet {
  string sig;
  vector<RawItem> items;
  // Links are only counted among items that meet the threshold (as VOSviewer does): counting every pair of every
  // item does not fit in memory for large data sets (e.g. terms of 10,000 abstracts). pairs holds the links among
  // items whose measure is >= pairFloor; AnalysisEngine::pairsFor recomputes them when the threshold changes.
  vector<PairW> pairs;
  double pairFloor = -1;
  int pairMaxItems = -1;
  int pairsLimited = 0;  // > 0: links were counted only among this many strongest items (memory guard)
  int hiddenSingles = 0;  // items that occur once, not kept when the threshold is 2 or more (counted for totals)
  // per-document data kept to recount links
  vector<vector<int>> docSets;             // co-occurrence types: item ids of each used document
  vector<double> docW;                     // weight of each document's pairs (fractional counting)
  vector<vector<int>> docRefs;             // coupling: shared reference ids per document
  vector<int> refDocOff, refDocs;          // coupling: documents per shared reference (CSR)
  vector<double> refW;                     // coupling: weight per shared reference
  vector<std::pair<int, int>> citeLinks;   // citation: document -> cited document
  vector<vector<int>> docUnits;            // coupling/citation of non-document units: unit ids per document
  int nRecs = 0, used = 0;
  long long refs = 0, matched = 0, distinctRefs = 0, sharedRefs = 0;
  double ms = 0;
};
struct Selection {
  vector<int> sel;           // raw item indices, ranked
  struct L { int a, b; double w; };
  vector<L> links;           // indices into sel
  vector<double> tls;        // per raw item (among passing)
  int nTotal = 0, nMeet = 0, nPass = 0, preCap = 0, dropped = 0, isolated = 0;
};

// matching keys of a record's items for a unit (the same keys as Node::key of maps built for that unit; empty for documents)
vector<string> recordUnitKeys(const Record& r, Unit unit, const Thesaurus& th);

struct RefParts { string doi, surname, init, vol, page, src; int year = 0; };
RefParts refParse(const string& ref);
string refKey(const RefParts& p);
string refLabel(const RefParts& p);
string surnameOf(const string& author);

class AnalysisEngine {
 public:
  const Corpus* corpus = nullptr;
  Thesaurus thesaurus;
  std::map<string, std::set<string>> excluded;  // "type:unit" → keys

  const RawNet& raw(const AnaSpec& s);
  const Selection& select(const AnaSpec& s);
  vector<std::pair<int, int>> sensitivity(const AnaSpec& s, int maxM = 20);
  int suggest(const AnaSpec& s, int target);
  std::set<string>& exclSet(const AnaSpec& s);
  // Builds the network (no layout/clustering). Throws std::runtime_error with a readable reason.
  Network build(const AnaSpec& s);
  void invalidate() { rawCache_.sig.clear(); selSig_.clear(); }
  const RawNet& pairsFor(const AnaSpec& s);  // raw(s) with links counted among the items passing s.min

 private:
  RawNet rawCache_;
  Selection selCache_;
  string selSig_;
  const vector<Record>* rawRecs_ = nullptr;
  size_t rawCount_ = 0;
};

// title/abstract noun-phrase-like terms of a record (lower-case)
vector<string> extractTerms(const Record& r);

// keyword variants (cleaning studio)
struct VariantGroup {
  string target;               // suggested preferred label
  vector<string> members;      // labels merged into target (excluding target)
  vector<int> counts;          // occurrences of each member
  int targetCount = 0;
  string reason;               // "spelling", "plural", "hyphen", "acronym", "typo"
  bool safe = true;
  bool apply = true;
  bool ai = false;             // proposed by the AI (Data → Clean terms → AI)
  bool ignore = false;         // generic term: remove target from maps instead of merging
  string note;                 // the AI's short reason
};
vector<VariantGroup> findVariants(const Corpus& c, Unit unit, const Thesaurus& th);
// labels of a field with their occurrences after the thesaurus (most frequent first); "" keys (ignored) are left out
vector<std::pair<string, int>> termCounts(const Corpus& c, Unit unit, const Thesaurus& th);
string thesaurusKey(Unit unit, const string& label);  // key used by the thesaurus for this unit

// keywords most over-represented among the given records (for naming clusters of documents/authors…)
string keywordName(const Corpus& c, const vector<int>& recs, int top = 2);

}  // namespace vs
