#include "analysis.h"
#include <thread>

#include "records.h"
#include <stdexcept>

namespace vs {

// ------------------------------------------------------------ tables
const vector<TypeInfo>& anaTypes() {
  static const vector<TypeInfo> T = {
      {AnaType::Cooc, "cooc", "Co-occurrence", "Keywords or terms are linked when they appear in the same documents.",
       {{Unit::AllKeywords, "All keywords"}, {Unit::Keywords, "Author keywords"}, {Unit::IndexTerms, "Index keywords (Keywords Plus)"}, {Unit::Terms, "Title & abstract terms"}}},
      {AnaType::Coauth, "coauth", "Co-authorship", "Authors, organisations or countries are linked when they publish documents together.",
       {{Unit::Authors, "Authors"}, {Unit::Orgs, "Organisations"}, {Unit::Countries, "Countries"}}},
      {AnaType::Citation, "citation", "Citation",
       "Items are linked when one cites the other inside this dataset (references are matched to records by DOI or author–year–volume–page).",
       {{Unit::Docs, "Documents"}, {Unit::Sources, "Sources"}, {Unit::Authors, "Authors"}, {Unit::Orgs, "Organisations"}, {Unit::Countries, "Countries"}}},
      {AnaType::Coupling, "coupling", "Bibliographic coupling", "Items are linked by the number of references they share: a view of the current research front.",
       {{Unit::Docs, "Documents"}, {Unit::Sources, "Sources"}, {Unit::Authors, "Authors"}, {Unit::Orgs, "Organisations"}, {Unit::Countries, "Countries"}}},
      {AnaType::Cocit, "cocit", "Co-citation", "Cited works are linked when they are cited together by the same documents: a view of the intellectual base.",
       {{Unit::Refs, "Cited references"}, {Unit::CSources, "Cited sources"}, {Unit::CAuthors, "Cited authors"}}}};
  return T;
}
const TypeInfo& typeInfo(AnaType t) {
  for (auto& ti : anaTypes()) if (ti.type == t) return ti;
  return anaTypes()[0];
}
const char* unitId(Unit u) {
  static const char* ids[] = {"allKeywords", "keywords", "indexTerms", "terms", "authors", "orgs", "countries", "docs", "sources", "refs", "csources", "cauthors"};
  return ids[int(u)];
}
const char* unitNoun(Unit u) {
  static const char* n[] = {"keyword", "keyword", "keyword", "term", "author", "organisation", "country", "document", "source", "cited reference", "cited source", "cited author"};
  return n[int(u)];
}
string unitNounPlural(Unit u, int n) {
  string s = unitNoun(u);
  if (n == 1) return s;
  if (s == "country") return "countries";
  return s + "s";
}
string unitLabel(AnaType t, Unit u) {
  for (auto& p : typeInfo(t).units) if (p.first == u) return p.second;
  return unitNoun(u);
}
bool unitFromId(const string& s, Unit& u) {
  for (int i = 0; i < 12; i++) if (s == unitId(Unit(i))) { u = Unit(i); return true; }
  return false;
}
bool typeFromId(const string& s, AnaType& t) {
  for (auto& ti : anaTypes()) if (s == ti.id) { t = ti.type; return true; }
  return false;
}

void AnaSpec::setDefaults() {
  fractional = false;
  min = 5;
  minCites = 0;
  maxItems = 1000;
  minLink = 0;
  maxAuth = 25;
  largest = true;
  if (type == AnaType::Cooc && unit == Unit::Terms) min = 10;
  if ((type == AnaType::Citation || type == AnaType::Coupling) && unit == Unit::Docs) min = 20;
  if (type == AnaType::Cocit) min = 20;
}
Measure anaMeasure(const AnaSpec& s) {
  if ((s.type == AnaType::Citation || s.type == AnaType::Coupling) && s.unit == Unit::Docs) return {true, "citations", false};
  if (s.type == AnaType::Cocit) return {false, "citations", false};
  if (s.type == AnaType::Cooc) return {false, "occurrences", false};
  return {false, "documents", true};
}
string thresholdLabel(const AnaSpec& s) {
  string noun = unitNoun(s.unit);
  bool vowel = !noun.empty() && strchr("aeiou", noun[0]);
  return string("Min. ") + anaMeasure(s).label + " of " + (vowel ? "an " : "a ") + noun;
}

// ------------------------------------------------------------ thesaurus
string Thesaurus::sig() const {
  string s = std::to_string(replace.size());
  for (auto& kv : replace) s += "|" + kv.first + ">" + kv.second;
  return sha256Hex(s).substr(0, 16);
}
bool Thesaurus::load(const string& text, string* err) {
  auto rows = parseCsv(text, '\t');
  int n = 0;
  for (size_t i = 0; i < rows.size(); i++) {
    auto& r = rows[i];
    if (r.empty()) continue;
    string a = trim(r[0]), b = r.size() > 1 ? trim(r[1]) : "";
    if (i == 0 && lower(a) == "label") continue;
    if (a.empty()) continue;
    replace[bibKey(a)] = b;
    n++;
  }
  if (!n && err) *err = "no thesaurus rows found";
  return n > 0;
}
string Thesaurus::save() const {
  string o = "label\treplace by\n";
  for (auto& kv : replace) o += kv.first + "\t" + kv.second + "\n";
  return o;
}

// ------------------------------------------------------------ references
string surnameOf(const string& author0) {
  string s = trim(author0);
  if (s.empty()) return "";
  size_t c = s.find(',');
  if (c != string::npos) s = s.substr(0, c);
  else {
    auto p = split(s, ' ');
    if (p.size() > 1) {
      string last = replaceAll(p.back(), ".", "");
      bool ini = !last.empty() && last.size() <= 3;
      for (char ch : last) if (!(ch >= 'A' && ch <= 'Z')) ini = false;
      if (ini) { p.pop_back(); s = join(p, " "); }
      else s = p[0];
    }
  }
  s = asciiFold(s);
  string r;
  for (char ch : s) if (isalpha((unsigned char)ch) || ch == '\'' || ch == ' ' || ch == '-') r += ch;
  return trim(r);
}
static string initialOf(const string& s) {
  size_t c = s.find(',');
  if (c != string::npos) {
    for (size_t i = c + 1; i < s.size(); i++) if (isalpha((unsigned char)s[i])) return string(1, char(toupper(s[i])));
    return "";
  }
  auto p = split(trim(s), ' ');
  if (p.size() > 1) {
    string last = replaceAll(p.back(), ".", "");
    if (!last.empty() && last.size() <= 3 && isupper((unsigned char)last[0])) return string(1, last[0]);
  }
  return "";
}
static bool isYear(const string& s) {
  string t = s;
  if (!t.empty() && t[0] == '(') t = t.substr(1);
  if (!t.empty() && t.back() == ')') t.pop_back();
  if (t.size() != 4 || !isDigits(t)) return false;
  int y = toInt(t);
  return y >= 1800 && y <= 2099;
}
RefParts refParse(const string& ref) {
  RefParts p;
  string s = ref;
  // DOI
  size_t d = lower(s).find("10.");
  while (d != string::npos) {
    size_t sl = s.find('/', d);
    if (sl != string::npos && sl - d >= 7 && sl - d <= 13 && isDigits(s.substr(d + 3, sl - d - 3))) {
      size_t e = sl;
      while (e < s.size() && s[e] != ' ' && s[e] != ',' && s[e] != ';') e++;
      p.doi = normDoi(s.substr(d, e - d));
      break;
    }
    d = lower(s).find("10.", d + 3);
  }
  if (startsWith(s, "DOI openalex:")) { p.doi = "openalex:" + s.substr(13); return p; }
  auto parts = split(s, ',');
  for (auto& x : parts) x = trim(x);
  parts.erase(std::remove(parts.begin(), parts.end(), string()), parts.end());
  string first = parts.empty() ? "" : parts[0];
  string author = first;
  if (parts.size() > 1) {
    string p1 = replaceAll(replaceAll(parts[1], ".", ""), " ", "");
    bool ini = !p1.empty() && p1.size() <= 3;
    for (char ch : p1) if (!(ch >= 'A' && ch <= 'Z')) ini = false;
    if (ini) author = first + ", " + parts[1];
  }
  p.surname = surnameOf(author);
  p.init = initialOf(author);
  // year: "(2019)" first, else any 4-digit year token
  size_t lp = s.find('(');
  while (lp != string::npos) {
    if (lp + 5 < s.size() && s[lp + 5] == ')' && isYear(s.substr(lp + 1, 4))) { p.year = toInt(s.substr(lp + 1, 4)); break; }
    lp = s.find('(', lp + 1);
  }
  int yi = -1;
  for (size_t i = 0; i < parts.size(); i++) if (isYear(parts[i])) { yi = int(i); if (!p.year) p.year = toInt(replaceAll(replaceAll(parts[i], "(", ""), ")", "")); break; }
  if (!p.year) {
    for (size_t i = 0; i + 4 <= s.size(); i++)
      if (isdigit((unsigned char)s[i]) && (i == 0 || !isdigit((unsigned char)s[i - 1])) && (i + 4 == s.size() || !isdigit((unsigned char)s[i + 4])) && isYear(s.substr(i, 4))) {
        p.year = toInt(s.substr(i, 4));
        break;
      }
  }
  // WoS "V12" "P345"
  for (auto& x : parts) {
    if (x.size() > 1 && x[0] == 'V' && isDigits(x.substr(1)) && p.vol.empty()) p.vol = x.substr(1);
    if (x.size() > 1 && x[0] == 'P' && isDigits(x.substr(1)) && p.page.empty()) p.page = x.substr(1);
    if (startsWith(x, "pp. ") && p.page.empty()) { string q = x.substr(4); string num; for (char ch : q) { if (isdigit((unsigned char)ch)) num += ch; else break; } p.page = num; }
  }
  if (yi >= 0) {
    if (yi + 1 < int(parts.size())) p.src = parts[yi + 1];
    if (p.vol.empty() && yi + 2 < int(parts.size()) && isDigits(parts[yi + 2])) p.vol = parts[yi + 2];
  } else if (p.year) {
    // Scopus: "Smith, J., Title, (2019) Journal Name, 12, pp. 1-10"
    for (size_t i = 0; i < parts.size(); i++) {
      size_t q = parts[i].find("(" + std::to_string(p.year) + ")");
      if (q != string::npos) {
        p.src = trim(parts[i].substr(q + 6));
        if (p.vol.empty() && i + 1 < parts.size() && isDigits(parts[i + 1])) p.vol = parts[i + 1];
        break;
      }
    }
  }
  if ((p.src.size() > 1 && (p.src[0] == 'V' || p.src[0] == 'P') && isDigits(p.src.substr(1))) || startsWith(upper(p.src), "DOI")) p.src.clear();
  p.src = bibClean(p.src);
  return p;
}
string refKey(const RefParts& p) {
  if (!p.doi.empty()) return "doi:" + p.doi;
  if (p.surname.empty() || !p.year) return "";
  return lower(p.surname) + "|" + p.init + "|" + std::to_string(p.year) + "|" + sourceKey(p.src) + "|" + p.vol + "|" + p.page;
}
string refLabel(const RefParts& p) {
  string a = "anon.";
  if (!p.surname.empty()) a = string(1, char(toupper(p.surname[0]))) + lower(p.surname.substr(1)) + (p.init.empty() ? "" : " " + p.init);
  return a + " (" + (p.year ? std::to_string(p.year) : string("n.d.")) + ")" + (p.src.empty() ? "" : ", " + lower(p.src));
}

// ------------------------------------------------------------ terms
static const std::unordered_set<string>& stopwords() {
  static const std::unordered_set<string> S = {
      "a", "about", "above", "across", "after", "again", "against", "all", "almost", "also", "although", "among", "an", "and", "another", "any", "are", "as", "at",
      "based", "be", "because", "been", "before", "being", "between", "both", "but", "by", "can", "could", "did", "do", "does", "during", "each", "either", "et",
      "etc", "even", "ever", "every", "few", "for", "from", "further", "had", "has", "have", "having", "he", "her", "here", "hers", "him", "his", "how", "however",
      "i", "if", "in", "into", "is", "it", "its", "itself", "just", "least", "less", "may", "more", "most", "much", "must", "my", "neither", "no", "nor", "not",
      "of", "often", "on", "once", "one", "only", "or", "other", "our", "out", "over", "own", "per", "rather", "same", "several", "she", "should", "since", "so",
      "some", "such", "than", "that", "the", "their", "them", "then", "there", "these", "they", "this", "those", "though", "through", "thus", "to", "too", "two",
      "under", "until", "up", "upon", "us", "use", "used", "using", "very", "via", "was", "we", "well", "were", "what", "when", "where", "whether", "which",
      "while", "who", "whom", "whose", "why", "will", "with", "within", "without", "would", "yet", "you", "your", "study", "paper", "results", "result", "show",
      "shows", "findings", "research", "article", "investigates", "examine", "examines", "discuss", "implications", "approach", "method", "methods", "new",
      "different", "various", "associated", "measurable", "changes", "indicate", "evidence", "growing", "attention", "limitations", "purpose", "aim", "aims",
      "present", "proposed", "propose", "provide", "provides", "significant", "significantly", "large", "high", "low", "first", "second", "three", "four"};
  return S;
}
vector<string> extractTerms(const Record& r) {
  string text = lower(asciiFold(r.title + ". " + r.abstract_));
  vector<string> out;
  std::unordered_set<string> seen;
  vector<string> run;
  auto flushRun = [&]() {
    // all sub-phrases of length 1..3 ending at noun-ish words; prefer 2-3 word phrases
    for (size_t i = 0; i < run.size(); i++)
      for (size_t len = 1; len <= 3 && i + len <= run.size(); len++) {
        string ph;
        for (size_t k = i; k < i + len; k++) ph += (k > i ? " " : "") + run[k];
        if (len == 1 && ph.size() < 5) continue;
        if (seen.insert(ph).second) out.push_back(ph);
      }
    run.clear();
  };
  string tok;
  auto endTok = [&](char c) {
    if (!tok.empty()) {
      bool num = true;
      for (char ch : tok) if (!isdigit((unsigned char)ch) && ch != ',') num = false;
      while (!tok.empty() && tok.back() == '-') tok.pop_back();
      if (num || stopwords().count(tok) || tok.size() < 3) flushRun();
      else run.push_back(tok);
      tok.clear();
    }
    if (c == '.' || c == ',' || c == ';' || c == ':' || c == '(' || c == ')' || c == '?' || c == '!') flushRun();
  };
  for (char c : text) {
    if ((c >= 'a' && c <= 'z') || c == '-' || (c >= '0' && c <= '9')) tok += c;
    else endTok(c);
  }
  endTok('.');
  return out;
}

// ------------------------------------------------------------ unit extraction
static void addUnit(std::vector<std::pair<string, string>>& out, std::unordered_set<string>& seen, const string& raw, Unit unit, const Thesaurus& th) {
  string label = bibClean(raw);
  if (label.empty()) return;
  string key = unit == Unit::Sources ? sourceKey(label) : bibKey(label);
  auto it = th.replace.find(unit == Unit::Sources ? bibKey(label) : key);
  if (it != th.replace.end()) {
    if (it->second.empty()) return;  // ignored
    label = it->second;
    key = unit == Unit::Sources ? sourceKey(label) : bibKey(label);
  }
  if (key.size() < 2 || !seen.insert(key).second) return;
  out.emplace_back(key, label);
}
static vector<std::pair<string, string>> recUnits(const Record& r, Unit unit, const Thesaurus& th) {
  vector<std::pair<string, string>> out;
  std::unordered_set<string> seen;
  auto addAll = [&](const vector<string>& v) { for (auto& x : v) addUnit(out, seen, x, unit, th); };
  switch (unit) {
    case Unit::Keywords: addAll(r.keywords); break;
    case Unit::IndexTerms: addAll(r.indexTerms); break;
    case Unit::AllKeywords: addAll(r.keywords); addAll(r.indexTerms); break;
    case Unit::Terms: addAll(extractTerms(r)); break;
    case Unit::Authors: addAll(r.authors); break;
    case Unit::Orgs: addAll(r.affiliations); break;
    case Unit::Countries: addAll(r.countries); break;
    case Unit::Sources: if (!r.source.empty()) addUnit(out, seen, r.source, unit, th); break;
    default: break;
  }
  return out;
}
static vector<std::pair<string, string>> citedUnits(const Record& r, Unit unit) {
  vector<std::pair<string, string>> out;
  std::unordered_set<string> seen;
  for (auto& ref : r.refs) {
    RefParts p = refParse(ref);
    string key, label;
    if (unit == Unit::Refs) { key = refKey(p); label = refLabel(p); }
    else if (unit == Unit::CSources) { if (!p.src.empty()) { key = sourceKey(p.src); label = upper(p.src); } }
    else if (unit == Unit::CAuthors) {
      if (!p.surname.empty()) {
        key = lower(p.surname) + (p.init.empty() ? "" : " " + lower(p.init));
        label = string(1, char(toupper(p.surname[0]))) + lower(p.surname.substr(1)) + (p.init.empty() ? "" : " " + p.init);
      }
    }
    if (key.size() > 1 && seen.insert(key).second) out.emplace_back(key, label);
  }
  return out;
}
vector<string> recordUnitKeys(const Record& r, Unit unit, const Thesaurus& th) {
  vector<std::pair<string, string>> v = (unit == Unit::Refs || unit == Unit::CSources || unit == Unit::CAuthors) ? citedUnits(r, unit) : recUnits(r, unit, th);
  vector<string> out;
  out.reserve(v.size());
  for (auto& kv : v) out.push_back(kv.first);
  return out;
}
static vector<string> docLabels(const vector<const Record*>& recs) {
  vector<string> base;
  for (auto r : recs) {
    string s = r->authors.empty() ? "" : surnameOf(r->authors[0]);
    if (s.empty()) { auto w = split(r->title, ' '); s = w.size() > 1 ? w[0] + " " + w[1] : (w.empty() ? "untitled" : w[0]); }
    base.push_back(s + " (" + (r->year ? std::to_string(r->year) : string("n.d.")) + ")");
  }
  std::unordered_map<string, int> cnt, seen;
  for (auto& b : base) cnt[b]++;
  for (auto& b : base) {
    if (cnt[b] < 2) continue;
    int k = seen[b]++;
    string orig = b;
    b = orig.substr(0, orig.size() - 1) + char('a' + k % 26) + ")";
  }
  return base;
}
static uint64_t pk(int a, int b) { return a < b ? (uint64_t(a) << 32) | uint32_t(b) : (uint64_t(b) << 32) | uint32_t(a); }

// ------------------------------------------------------------ raw network
const RawNet& AnalysisEngine::raw(const AnaSpec& s) {
  static RawNet empty;
  if (!corpus || corpus->recs.empty()) return empty;
  string sig = string(typeInfo(s.type).id) + "|" + unitId(s.unit) + "|" + (s.fractional ? "f" : "F") + "|" + std::to_string(s.y0) + "|" + std::to_string(s.y1) + "|" +
               std::to_string(s.type == AnaType::Coauth ? s.maxAuth : 0) + "|" + thesaurus.sig() + (s.min >= 2 ? "|k2" : "|k1");
  if (rawCache_.sig == sig && rawRecs_ == &corpus->recs && rawCount_ == corpus->recs.size()) return rawCache_;
  Timer tm;
  RawNet R;
  R.sig = sig;
  vector<const Record*> recs;
  vector<int> recIdx;
  for (size_t i = 0; i < corpus->recs.size(); i++) {
    auto& r = corpus->recs[i];
    if (s.y0 && r.year && r.year < s.y0) continue;
    if (s.y1 && r.year && r.year > s.y1) continue;
    recs.push_back(&r);
    recIdx.push_back(int(i));
  }
  R.nRecs = int(recs.size());
  std::unordered_map<string, int> idx;
  auto item = [&](const string& k, const string& label) {
    auto it = idx.find(k);
    if (it != idx.end()) return it->second;
    int i = int(R.items.size());
    idx[k] = i;
    RawItem e;
    e.key = k;
    e.label = label;
    R.items.push_back(e);
    return i;
  };
  auto touch = [&](int i, int ri) {
    auto& e = R.items[i];
    const Record& r = *recs[ri];
    e.recs.push_back(recIdx[ri]);
    e.occ += 1;
    e.cites += r.cites;
    if (r.year) { e.yearSum += r.year; e.yearN++; }
  };
  if (s.type == AnaType::Cooc || s.type == AnaType::Coauth || s.type == AnaType::Cocit) {
    // pass 1: integer id and occurrence count per key (first-appearance order)
    int keepMin = s.min >= 2 ? 2 : 1;
    std::unordered_map<string, int> kid;
    vector<int> kcnt;
    vector<string> klabel;
    vector<const string*> kkey;
    vector<vector<int>> docK;
    vector<int> docRi;
    // Unit extraction (term parsing, key normalisation, reference parsing) is pure: run it in parallel on
    // blocks of records and merge serially in record order, so results do not depend on the thread count.
    const size_t BLK = 1024;
    unsigned nth = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    vector<vector<std::pair<string, string>>> blk;
    for (size_t b0 = 0; b0 < recs.size(); b0 += BLK) {
      size_t b1 = std::min(recs.size(), b0 + BLK);
      blk.assign(b1 - b0, {});
      auto work = [&](unsigned t) {
        for (size_t ri = b0 + t; ri < b1; ri += nth) {
          const Record& r = *recs[ri];
          if (s.type == AnaType::Coauth && s.maxAuth > 0 && int(r.authors.size()) > s.maxAuth) continue;
          blk[ri - b0] = s.type == AnaType::Cocit ? citedUnits(r, s.unit) : recUnits(r, s.unit, thesaurus);
        }
      };
      if (nth > 1 && b1 - b0 > 64) {
        vector<std::thread> th;
        for (unsigned t = 1; t < nth; t++) th.emplace_back(work, t);
        work(0);
        for (auto& x : th) x.join();
      } else for (unsigned t = 0; t < nth; t++) work(t);
    for (size_t ri = b0; ri < b1; ri++) {
      auto& u = blk[ri - b0];
      if (u.empty()) continue;
      R.used++;
      vector<int> ids;
      ids.reserve(u.size());
      for (auto& kv : u) {
        auto ins = kid.emplace(kv.first, int(kcnt.size()));
        if (ins.second) { kcnt.push_back(0); klabel.push_back(kv.second); kkey.push_back(&ins.first->first); }
        kcnt[size_t(ins.first->second)]++;
        ids.push_back(ins.first->second);
      }
      docK.push_back(std::move(ids));
      docRi.push_back(int(ri));
      vector<std::pair<string, string>>().swap(u);
    }
    }
    // pass 2: items for keys that can meet the threshold
    vector<int> toItem(kcnt.size(), -1);
    for (size_t k = 0; k < kcnt.size(); k++) {
      if (kcnt[k] < keepMin) { R.hiddenSingles++; continue; }
      toItem[k] = item(*kkey[k], klabel[k]);
    }
    klabel.clear(); klabel.shrink_to_fit();
    R.docSets.reserve(docK.size());
    R.docW.reserve(docK.size());
    for (size_t d = 0; d < docK.size(); d++) {
      vector<int> ids;
      for (int k : docK[d]) { int i = toItem[size_t(k)]; if (i >= 0) { touch(i, docRi[d]); ids.push_back(i); } }
      // fractional weight from all units of the document, as before
      R.docW.push_back(s.fractional && docK[d].size() > 1 ? 1.0 / double(docK[d].size() - 1) : 1.0);
      R.docSets.push_back(std::move(ids));
      vector<int>().swap(docK[d]);
    }
  } else {
    if (s.type == AnaType::Citation) {
      // match references to records
      std::unordered_map<string, int> byDoi, byAYVP, byAYP, byAYV;
      auto put = [](std::unordered_map<string, int>& m, const string& k, int i) {
        if (k.empty()) return;
        auto it = m.find(k);
        if (it == m.end()) m[k] = i;
        else if (it->second != i) it->second = -1;
      };
      for (size_t i = 0; i < recs.size(); i++) {
        auto& r = *recs[i];
        string d = normDoi(r.doi);
        if (!d.empty()) put(byDoi, d, int(i));
        string sn = lower(r.authors.empty() ? "" : surnameOf(r.authors[0]));
        if (sn.empty() || !r.year) continue;
        string v, p;
        for (char c : r.volume) if (isdigit((unsigned char)c)) v += c;
        for (char c : r.pages) { if (isdigit((unsigned char)c)) p += c; else if (!p.empty()) break; }
        string y = std::to_string(r.year);
        if (!v.empty() && !p.empty()) put(byAYVP, sn + "|" + y + "|" + v + "|" + p, int(i));
        if (!p.empty()) put(byAYP, sn + "|" + y + "|" + p, int(i));
        if (!v.empty()) put(byAYV, sn + "|" + y + "|" + v, int(i));
      }
      for (size_t i = 0; i < recs.size(); i++) {
        std::unordered_set<int> seen;
        for (auto& ref : recs[i]->refs) {
          R.refs++;
          RefParts p = refParse(ref);
          int j = -2;
          auto get = [&](std::unordered_map<string, int>& m, const string& k) { auto it = m.find(k); return it == m.end() ? -2 : it->second; };
          if (!p.doi.empty() && byDoi.count(p.doi)) j = byDoi[p.doi];
          else if (!p.surname.empty() && p.year) {
            string sn = lower(p.surname), y = std::to_string(p.year);
            if (!p.vol.empty() && !p.page.empty()) j = get(byAYVP, sn + "|" + y + "|" + p.vol + "|" + p.page);
            if (j == -2 && !p.page.empty()) j = get(byAYP, sn + "|" + y + "|" + p.page);
            if (j == -2 && !p.vol.empty() && p.page.empty()) j = get(byAYV, sn + "|" + y + "|" + p.vol);
          }
          if (j >= 0 && j != int(i) && seen.insert(j).second) { R.citeLinks.push_back({int(i), j}); R.matched++; }
        }
      }
    } else {
      // coupling: documents per shared reference
      std::unordered_map<string, int> refId;
      vector<vector<int>> inv;
      R.docRefs.assign(recs.size(), {});
      for (size_t i = 0; i < recs.size(); i++) {
        std::unordered_set<string> seen;
        for (auto& ref : recs[i]->refs) {
          string k = refKey(refParse(ref));
          if (k.empty() || !seen.insert(k).second) continue;
          R.refs++;
          auto it = refId.find(k);
          int id;
          if (it == refId.end()) { id = int(inv.size()); refId.emplace(k, id); inv.emplace_back(); }
          else id = it->second;
          inv[size_t(id)].push_back(int(i));
          R.docRefs[i].push_back(id);
        }
      }
      R.distinctRefs = (long long)inv.size();
      R.refDocOff.assign(inv.size() + 1, 0);
      R.refW.assign(inv.size(), 0);
      for (size_t k = 0; k < inv.size(); k++) {
        auto& ds = inv[k];
        bool use = ds.size() >= 2 && ds.size() <= 2000;
        if (use) { R.sharedRefs++; R.refW[k] = s.fractional ? 1.0 / double(ds.size() - 1) : 1.0; }
        R.refDocOff[k + 1] = R.refDocOff[k] + (use ? int(ds.size()) : 0);
      }
      R.refDocs.reserve(size_t(R.refDocOff.back()));
      for (size_t k = 0; k < inv.size(); k++) if (R.refDocOff[k + 1] > R.refDocOff[k]) R.refDocs.insert(R.refDocs.end(), inv[k].begin(), inv[k].end());
    }
    if (s.unit == Unit::Docs) {
      auto labels = docLabels(recs);
      for (size_t i = 0; i < recs.size(); i++) {
        int it = item("d" + std::to_string(i), labels[i]);
        touch(it, int(i));
        R.items[it].title = recs[i]->title;
        R.items[it].doi = recs[i]->doi;
      }
      R.used = int(recs.size());
    } else {
      R.docUnits.assign(recs.size(), {});
      for (size_t i = 0; i < recs.size(); i++) {
        for (auto& kv : recUnits(*recs[i], s.unit, thesaurus)) { int it = item(kv.first, kv.second); touch(it, int(i)); R.docUnits[i].push_back(it); }
        if (!R.docUnits[i].empty()) R.used++;
      }
    }
  }
  R.ms = tm.ms();
  rawCache_ = std::move(R);
  rawRecs_ = &corpus->recs;
  rawCount_ = corpus->recs.size();
  selSig_.clear();
  return rawCache_;
}

// Links among the items that meet the threshold. Rows are accumulated in a dense scratch array (a sparse
// A'A product), so memory is proportional to the links that are kept rather than to all item pairs.
const RawNet& AnalysisEngine::pairsFor(const AnaSpec& s) {
  raw(s);
  RawNet& R = rawCache_;
  Measure M = anaMeasure(s);
  double floor = std::max(0.0, double(s.min));
  if (R.pairFloor == floor && R.pairMaxItems == s.maxItems) return R;
  Timer tm;
  int n = int(R.items.size());
  vector<char> pass(size_t(n), 0);
  for (int i = 0; i < n; i++) pass[size_t(i)] = (M.cites ? R.items[size_t(i)].cites : R.items[size_t(i)].occ) >= floor;
  // Memory guard. The number of pair updates bounds the number of links; when it is too large, links are counted
  // only among the strongest items. Their total link strength is exact without building any pair.
  R.pairsLimited = 0;
  bool docsAreItems = R.docSets.empty() && R.docUnits.empty() && R.citeLinks.empty() && !R.docRefs.empty();
  if (!R.docSets.empty() || docsAreItems) {
    const double BUDGET = 16e6;
    auto work = [&](const vector<char>& ps) {
      double wsum = 0;
      if (!R.docSets.empty()) {
        for (auto& ds : R.docSets) { double k = 0; for (int i : ds) k += ps[size_t(i)]; wsum += k * (k - 1) / 2; }
      } else {
        for (size_t r = 0; r + 1 < R.refDocOff.size(); r++) { double k = 0; for (int q = R.refDocOff[r]; q < R.refDocOff[r + 1]; q++) k += ps[size_t(R.refDocs[size_t(q)])]; wsum += k * (k - 1) / 2; }
      }
      return wsum;
    };
    if (work(pass) > BUDGET) {
      vector<double> tls(size_t(n), 0.0);
      if (!R.docSets.empty()) {
        for (size_t d = 0; d < R.docSets.size(); d++) {
          double k = 0;
          for (int i : R.docSets[d]) k += pass[size_t(i)];
          for (int i : R.docSets[d]) if (pass[size_t(i)]) tls[size_t(i)] += R.docW[d] * (k - 1);
        }
      } else {
        for (size_t r = 0; r + 1 < R.refDocOff.size(); r++) {
          double k = 0;
          for (int q = R.refDocOff[r]; q < R.refDocOff[r + 1]; q++) k += pass[size_t(R.refDocs[size_t(q)])];
          for (int q = R.refDocOff[r]; q < R.refDocOff[r + 1]; q++) { int d = R.refDocs[size_t(q)]; if (pass[size_t(d)]) tls[size_t(d)] += R.refW[r] * (k - 1); }
        }
      }
      vector<int> cand;
      for (int i = 0; i < n; i++) if (pass[size_t(i)]) cand.push_back(i);
      std::stable_sort(cand.begin(), cand.end(), [&](int a, int b) { return tls[size_t(a)] > tls[size_t(b)]; });
      auto topK = [&](size_t K) { vector<char> ps(size_t(n), 0); for (size_t k = 0; k < K && k < cand.size(); k++) ps[size_t(cand[k])] = 1; return ps; };
      size_t lo = std::min(cand.size(), size_t(std::max(2, s.maxItems))), hi = cand.size();
      while (lo < hi) {  // largest K within the budget (never below maxItems)
        size_t mid = (lo + hi + 1) / 2;
        if (work(topK(mid)) <= BUDGET) lo = mid; else hi = mid - 1;
      }
      pass = topK(lo);
      R.pairsLimited = int(lo);
    }
  }
  vector<PairW> out;
  vector<double> acc(size_t(n), 0.0);
  vector<int> touched;
  auto flushRow = [&](int a) {
    std::sort(touched.begin(), touched.end());
    for (int b : touched) { if (acc[size_t(b)] != 0) out.push_back({uint32_t(a), uint32_t(b), acc[size_t(b)]}); acc[size_t(b)] = 0; }
    touched.clear();
  };
  auto add = [&](int b, double w) { if (acc[size_t(b)] == 0) touched.push_back(b); acc[size_t(b)] += w; };
  if (!R.docSets.empty()) {
    // item -> documents (only passing items)
    vector<int> off(size_t(n) + 1, 0);
    for (auto& ds : R.docSets) for (int i : ds) if (pass[size_t(i)]) off[size_t(i) + 1]++;
    for (int i = 0; i < n; i++) off[size_t(i) + 1] += off[size_t(i)];
    vector<int> docs(static_cast<size_t>(off[size_t(n)]));
    { vector<int> p(off.begin(), off.end() - 1); for (size_t d = 0; d < R.docSets.size(); d++) for (int i : R.docSets[d]) if (pass[size_t(i)]) docs[size_t(p[size_t(i)]++)] = int(d); }
    for (int a = 0; a < n; a++) {
      if (!pass[size_t(a)]) continue;
      for (int k = off[size_t(a)]; k < off[size_t(a) + 1]; k++) {
        int d = docs[size_t(k)];
        double w = R.docW[size_t(d)];
        for (int b : R.docSets[size_t(d)]) if (b > a && pass[size_t(b)]) add(b, w);
      }
      flushRow(a);
    }
  } else {
    // document-level links: coupling (shared references) or citation
    size_t nd = R.docRefs.size();
    if (!R.citeLinks.empty()) nd = std::max(nd, R.docUnits.size());
    auto docRow = [&](size_t d, auto&& emit) {  // emit(otherDoc > d, weight)
      for (int ref : R.docRefs[d])
        for (int k = R.refDocOff[size_t(ref)]; k < R.refDocOff[size_t(ref) + 1]; k++) { int e = R.refDocs[size_t(k)]; if (size_t(e) > d) emit(e, R.refW[size_t(ref)]); }
    };
    if (R.docUnits.empty()) {
      // items are documents: item index == document index (items were created in document order)
      if (!R.citeLinks.empty()) {
        for (auto& l : R.citeLinks) { int a = std::min(l.first, l.second), b = std::max(l.first, l.second); if (pass[size_t(a)] && pass[size_t(b)]) out.push_back({uint32_t(a), uint32_t(b), 1.0}); }
        std::sort(out.begin(), out.end(), [](const PairW& x, const PairW& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
        vector<PairW> merged;  // a citation in both directions counts twice
        for (auto& p : out) { if (!merged.empty() && merged.back().a == p.a && merged.back().b == p.b) merged.back().w += p.w; else merged.push_back(p); }
        out.swap(merged);
      } else {
        for (size_t a = 0; a < nd; a++) {
          if (!pass[a]) continue;
          docRow(a, [&](int b, double w) { if (pass[size_t(b)]) add(b, w); });
          flushRow(int(a));
        }
      }
    } else {
      // units (authors, sources, ...) linked through their documents: unit pair weight = sum of document link weights
      auto unitPairs = [&](int da, int db, double w, std::unordered_map<uint64_t, double>& m) {
        auto& A = R.docUnits[size_t(da)];
        auto& B = R.docUnits[size_t(db)];
        if (A.empty() || B.empty()) return;
        double ww = s.fractional ? w / double(A.size() * B.size()) : w;
        for (int u : A) for (int v : B) if (u != v && pass[size_t(u)] && pass[size_t(v)]) m[pk(u, v)] += ww;
      };
      std::unordered_map<uint64_t, double> m;  // unit pairs among passing units only (small)
      if (!R.citeLinks.empty()) {
        for (auto& l : R.citeLinks) unitPairs(l.first, l.second, 1.0, m);
      } else {
        vector<double> dacc(nd, 0.0);
        vector<int> dt;
        vector<char> docOn(nd, 0);  // documents with at least one passing unit
        for (size_t d = 0; d < nd; d++) for (int u : R.docUnits[d]) if (pass[size_t(u)]) { docOn[d] = 1; break; }
        for (size_t a = 0; a < nd; a++) {
          if (!docOn[a]) continue;
          docRow(a, [&](int b, double w) { if (!docOn[size_t(b)]) return; if (dacc[size_t(b)] == 0) dt.push_back(b); dacc[size_t(b)] += w; });
          for (int b : dt) { unitPairs(int(a), b, dacc[size_t(b)], m); dacc[size_t(b)] = 0; }
          dt.clear();
        }
      }
      out.reserve(m.size());
      for (auto& kv : m) out.push_back({uint32_t(kv.first >> 32), uint32_t(kv.first & 0xffffffffu), kv.second});
      std::sort(out.begin(), out.end(), [](const PairW& x, const PairW& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
    }
  }
  R.pairs.swap(out);
  R.pairs.shrink_to_fit();
  R.pairFloor = floor;
  R.pairMaxItems = s.maxItems;
  R.ms += tm.ms();
  selSig_.clear();
  return R;
}

std::set<string>& AnalysisEngine::exclSet(const AnaSpec& s) { return excluded[string(typeInfo(s.type).id) + ":" + unitId(s.unit)]; }

const Selection& AnalysisEngine::select(const AnaSpec& s) {
  const RawNet& R = pairsFor(s);
  auto& excl = exclSet(s);
  string sig = R.sig + "|" + std::to_string(s.min) + "," + std::to_string(s.minCites) + "," + std::to_string(s.maxItems) + "," + fmtFixed(s.minLink, 4) + "," +
               (s.largest ? "L" : "l") + "," + fmtFixed(s.termKeep, 2) + "|" + std::to_string(excl.size());
  for (auto& k : excl) sig += "," + k;
  if (sig == selSig_) return selCache_;
  Selection S;
  Measure M = anaMeasure(s);
  int n = int(R.items.size());
  S.nTotal = n + R.hiddenSingles;
  vector<char> pass(n, 0);
  for (int i = 0; i < n; i++) {
    auto& e = R.items[i];
    double v = M.cites ? e.cites : e.occ;
    if (v >= s.min && (!M.secondary || e.cites >= s.minCites)) {
      S.nMeet++;
      if (!excl.count(e.key)) { pass[i] = 1; S.nPass++; }
    }
  }
  // terms: keep the most relevant share (VOSviewer relevance heuristic)
  if (s.unit == Unit::Terms && s.termKeep < 0.999 && S.nPass > 10) {
    vector<double> occ(n, 0), rowSum(n, 0);
    double tot = 0;
    for (auto& kv : R.pairs) {
      int a = int(kv.a), b = int(kv.b);
      if (!pass[a] || !pass[b]) continue;
      rowSum[a] += kv.w; rowSum[b] += kv.w; tot += 2 * kv.w;
    }
    vector<double> rel(n, 0);
    for (auto& kv : R.pairs) {
      int a = int(kv.a), b = int(kv.b);
      if (!pass[a] || !pass[b] || tot <= 0) continue;
      double pa = kv.w / std::max(1e-9, rowSum[a]), pb = kv.w / std::max(1e-9, rowSum[b]);
      double qa = rowSum[b] / tot, qb = rowSum[a] / tot;
      rel[a] += pa * std::log(pa / std::max(1e-12, qa));
      rel[b] += pb * std::log(pb / std::max(1e-12, qb));
    }
    vector<int> cand;
    for (int i = 0; i < n; i++) if (pass[i]) cand.push_back(i);
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return rel[a] > rel[b]; });
    size_t keep = std::max<size_t>(2, size_t(std::ceil(cand.size() * s.termKeep)));
    for (size_t k = keep; k < cand.size(); k++) { pass[cand[k]] = 0; S.nPass--; }
  }
  S.tls.assign(n, 0);
  for (auto& kv : R.pairs) {
    int a = int(kv.a), b = int(kv.b);
    if (pass[a] && pass[b]) { S.tls[a] += kv.w; S.tls[b] += kv.w; }
  }
  for (int i = 0; i < n; i++) if (pass[i]) S.sel.push_back(i);
  std::sort(S.sel.begin(), S.sel.end(), [&](int a, int b) {
    if (S.tls[a] != S.tls[b]) return S.tls[a] > S.tls[b];
    double va = M.cites ? R.items[a].cites : R.items[a].occ, vb = M.cites ? R.items[b].cites : R.items[b].occ;
    if (va != vb) return va > vb;
    return R.items[a].label < R.items[b].label;
  });
  S.preCap = int(S.sel.size());
  if (int(S.sel.size()) > s.maxItems) S.sel.resize(static_cast<size_t>(std::max(0, s.maxItems)));
  std::unordered_map<int, int> pos;
  for (size_t j = 0; j < S.sel.size(); j++) pos[S.sel[j]] = int(j);
  for (auto& kv : R.pairs) {
    if (kv.w < s.minLink || kv.w <= 0) continue;
    auto ia = pos.find(int(kv.a)), ib = pos.find(int(kv.b));
    if (ia != pos.end() && ib != pos.end()) S.links.push_back({std::min(ia->second, ib->second), std::max(ia->second, ib->second), kv.w});
  }
  std::sort(S.links.begin(), S.links.end(), [](const Selection::L& x, const Selection::L& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
  {
    vector<int> deg(S.sel.size(), 0);
    for (auto& l : S.links) { deg[l.a]++; deg[l.b]++; }
    for (int d : deg) if (!d) S.isolated++;
  }
  if (s.largest && S.sel.size() > 2) {
    vector<int> par(S.sel.size());
    for (size_t i = 0; i < par.size(); i++) par[i] = int(i);
    std::function<int(int)> find = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
    for (auto& l : S.links) { int ra = find(l.a), rb = find(l.b); if (ra != rb) par[ra] = rb; }
    std::unordered_map<int, int> size;
    for (size_t i = 0; i < par.size(); i++) size[find(int(i))]++;
    int best = -1, bs = -1;
    for (size_t i = 0; i < par.size(); i++) { int r = find(int(i)); if (size[r] > bs || (size[r] == bs && r < best)) { bs = size[r]; best = r; } }
    if (bs < int(S.sel.size())) {
      vector<int> remap(S.sel.size(), -1), s2;
      for (size_t i = 0; i < S.sel.size(); i++) if (find(int(i)) == best) { remap[i] = int(s2.size()); s2.push_back(S.sel[i]); }
      S.dropped = int(S.sel.size() - s2.size());
      S.sel = s2;
      vector<Selection::L> l2;
      for (auto& l : S.links) if (remap[l.a] >= 0 && remap[l.b] >= 0) l2.push_back({remap[l.a], remap[l.b], l.w});
      S.links = l2;
    }
  }
  selCache_ = std::move(S);
  selSig_ = sig;
  return selCache_;
}

vector<std::pair<int, int>> AnalysisEngine::sensitivity(const AnaSpec& s, int maxM) {
  const RawNet& R = raw(s);
  Measure M = anaMeasure(s);
  vector<int> cnt(static_cast<size_t>(maxM + 1), 0);
  for (auto& e : R.items) {
    if (M.secondary && e.cites < s.minCites) continue;
    int v = std::min(maxM, int(std::floor(M.cites ? e.cites : e.occ)));
    for (int m = 1; m <= v; m++) cnt[m]++;
  }
  if (maxM >= 1 && !M.secondary) cnt[1] += R.hiddenSingles;
  vector<std::pair<int, int>> out;
  for (int m = 1; m <= maxM; m++) out.emplace_back(m, cnt[m]);
  return out;
}
int AnalysisEngine::suggest(const AnaSpec& s, int target) {
  const RawNet& R = raw(s);
  Measure M = anaMeasure(s);
  vector<double> v;
  for (auto& e : R.items) if (!M.secondary || e.cites >= s.minCites) v.push_back(M.cites ? e.cites : e.occ);
  std::sort(v.begin(), v.end(), std::greater<double>());
  if (int(v.size()) <= target) return 1;
  return std::max(1, int(std::ceil(v[target - 1])));
}

Network AnalysisEngine::build(const AnaSpec& s) {
  if (!corpus || corpus->recs.empty()) throw std::runtime_error("load bibliographic records first");
  const RawNet& R = raw(s);
  if (R.items.empty()) {
    if (s.type == AnaType::Citation) throw std::runtime_error("no references could be matched to documents in this dataset (citation analysis needs full records with cited references)");
    if (s.type == AnaType::Coupling || s.type == AnaType::Cocit) throw std::runtime_error("the records contain no cited references. Export them with “Full record and cited references”");
    throw std::runtime_error("the records have no " + lower(unitLabel(s.type, s.unit)));
  }
  const Selection& S = select(s);
  if (S.sel.size() < 2)
    throw std::runtime_error("only " + std::to_string(S.sel.size()) + (S.sel.size() == 1 ? " item" : " items") + " left. Lower the “" + lower(thresholdLabel(s)) + "” threshold");
  Network net;
  Measure M = anaMeasure(s);
  string noun = unitNoun(s.unit);
  net.unitNoun = noun;
  net.description = lower(typeInfo(s.type).label) + " of " + lower(unitLabel(s.type, s.unit));
  bool isCit = s.type == AnaType::Citation || s.type == AnaType::Coupling || s.type == AnaType::Cocit;
  net.weightNames = {"Links", "Total link strength", M.label == string("occurrences") ? "Occurrences" : (M.cites ? "Citations" : (s.type == AnaType::Cocit ? "Citations" : "Documents"))};
  if (!M.cites && s.type != AnaType::Cocit) net.weightNames.push_back("Citations");
  net.scoreNames = {"Avg. pub. year", "Avg. citations", "Norm. citations"};
  (void)isCit;
  // normalised citations: citations / mean citations of records of the same year
  std::unordered_map<int, double> yearMean;
  {
    std::unordered_map<int, std::pair<double, int>> acc;
    for (auto& r : corpus->recs) { auto& a = acc[r.year]; a.first += r.cites; a.second++; }
    for (auto& kv : acc) yearMean[kv.first] = kv.second.second ? kv.second.first / kv.second.second : 0;
  }
  for (size_t j = 0; j < S.sel.size(); j++) {
    const RawItem& e = R.items[S.sel[j]];
    Node nd;
    nd.id = std::to_string(j + 1);
    nd.label = e.label;
    nd.key = e.key;
    nd.title = e.title;
    nd.doi = e.doi;
    nd.recs = e.recs;
    double main = M.cites ? e.cites : e.occ;
    nd.w = {0, 0, main};
    if (net.weightNames.size() > 3) nd.w.push_back(e.cites);
    double nc = 0;
    int ncN = 0;
    for (int ri : e.recs) {
      auto& r = corpus->recs[ri];
      double m = yearMean[r.year];
      if (m > 0) { nc += r.cites / m; ncN++; }
    }
    nd.sc = {e.yearN ? e.yearSum / e.yearN : NAN, e.occ > 0 ? e.cites / e.occ : 0, ncN ? nc / ncN : 0};
    net.nodes.push_back(nd);
  }
  for (auto& l : S.links) {
    Link L;
    L.a = l.a;
    L.b = l.b;
    L.w = l.w;
    L.s = l.w;
    net.links.push_back(L);
  }
  net.recomputeLinkWeights();
  net.weightIdx = 2;
  net.scoreIdx = 0;
  return net;
}

// ------------------------------------------------------------ variants
static string singular(const string& w) {
  static const std::map<string, string> irregular = {
      {"analyses", "analysis"}, {"hypotheses", "hypothesis"}, {"theses", "thesis"}, {"diagnoses", "diagnosis"}, {"prognoses", "prognosis"},
      {"syntheses", "synthesis"}, {"crises", "crisis"}, {"bases", "basis"}, {"axes", "axis"}, {"criteria", "criterion"}, {"phenomena", "phenomenon"},
      {"indices", "index"}, {"matrices", "matrix"}, {"vertices", "vertex"}, {"appendices", "appendix"}, {"taxa", "taxon"}, {"strata", "stratum"},
      {"spectra", "spectrum"}, {"curricula", "curriculum"}, {"media", "medium"}, {"bacteria", "bacterium"}, {"fungi", "fungus"}, {"nuclei", "nucleus"},
      {"stimuli", "stimulus"}, {"radii", "radius"}, {"foci", "focus"}, {"loci", "locus"}, {"alumni", "alumnus"}, {"children", "child"}, {"women", "woman"},
      {"men", "man"}, {"people", "person"}, {"mice", "mouse"}, {"feet", "foot"}, {"teeth", "tooth"}, {"schemata", "schema"}, {"corpora", "corpus"},
      {"genera", "genus"}, {"larvae", "larva"}, {"algae", "alga"}, {"antennae", "antenna"}, {"formulae", "formula"}, {"vertebrae", "vertebra"},
      {"leaves", "leaf"}, {"lives", "life"}, {"knives", "knife"}, {"wolves", "wolf"}, {"halves", "half"}, {"shelves", "shelf"}, {"calves", "calf"},
      {"quizzes", "quiz"}, {"heroes", "hero"}, {"potatoes", "potato"}, {"tomatoes", "tomato"}, {"echoes", "echo"}, {"cargoes", "cargo"}, {"volcanoes", "volcano"}};
  auto it = irregular.find(w);
  if (it != irregular.end()) return it->second;
  size_t n = w.size();
  if (n > 4 && endsWith(w, "ies")) return w.substr(0, n - 3) + "y";
  if (n > 4 && (endsWith(w, "sses") || endsWith(w, "xes") || endsWith(w, "ches") || endsWith(w, "shes"))) return w.substr(0, n - 2);
  if (n > 3 && w.back() == 's' && !endsWith(w, "ss") && !endsWith(w, "us") && !endsWith(w, "is") && !endsWith(w, "ics")) return w.substr(0, n - 1);
  return w;
}
static string usSpelling(const string& w) {
  static const vector<std::pair<string, string>> R = {{"isation", "ization"}, {"isations", "izations"}, {"ised", "ized"}, {"ising", "izing"}, {"ise", "ize"},
                                                      {"ises", "izes"},       {"yse", "yze"},           {"ysed", "yzed"}, {"ysing", "yzing"}, {"elling", "eling"},
                                                      {"elled", "eled"},      {"centre", "center"},     {"centres", "centers"}};
  string r = w;
  for (auto& p : R) if (endsWith(r, p.first) && r.size() > p.first.size() + 2) { r = r.substr(0, r.size() - p.first.size()) + p.second; break; }
  static const vector<std::pair<string, string>> OUR = {{"behaviour", "behavior"}, {"colour", "color"}, {"favour", "favor"}, {"labour", "labor"}, {"honour", "honor"}, {"neighbour", "neighbor"}, {"humour", "humor"}, {"programme", "program"}, {"analogue", "analog"}, {"catalogue", "catalog"}};
  for (auto& p : OUR) if (startsWith(r, p.first)) { r = p.second + r.substr(p.first.size()); break; }
  return r;
}
static int lev(const string& a, const string& b, int maxD) {
  int n = int(a.size()), m = int(b.size());
  if (std::abs(n - m) > maxD) return maxD + 1;
  vector<int> prev(static_cast<size_t>(m + 1)), cur(static_cast<size_t>(m + 1));
  for (int j = 0; j <= m; j++) prev[j] = j;
  for (int i = 1; i <= n; i++) {
    cur[0] = i;
    int best = cur[0];
    for (int j = 1; j <= m; j++) {
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
      best = std::min(best, cur[j]);
    }
    if (best > maxD) return maxD + 1;
    std::swap(prev, cur);
  }
  return prev[m];
}
string thesaurusKey(Unit unit, const string& label) { return unit == Unit::Sources ? sourceKey(label) : bibKey(label); }

vector<std::pair<string, int>> termCounts(const Corpus& c, Unit unit, const Thesaurus& th) {
  std::unordered_map<string, int> cnt;
  std::unordered_map<string, string> lab;
  for (auto& r : c.recs) {
    vector<string> list;
    if (unit == Unit::Keywords || unit == Unit::AllKeywords) list.insert(list.end(), r.keywords.begin(), r.keywords.end());
    if (unit == Unit::IndexTerms || unit == Unit::AllKeywords) list.insert(list.end(), r.indexTerms.begin(), r.indexTerms.end());
    if (unit == Unit::Authors) list = r.authors;
    if (unit == Unit::Orgs) list = r.affiliations;
    if (unit == Unit::Countries) list = r.countries;
    if (unit == Unit::Sources && !r.source.empty()) list = {r.source};
    std::set<string> seen;
    for (auto& l : list) {
      string label = bibClean(l);
      if (label.empty()) continue;
      string k = thesaurusKey(unit, label);
      auto it = th.replace.find(k);
      if (it != th.replace.end()) {
        if (it->second.empty()) continue;  // ignored
        label = it->second;
        k = thesaurusKey(unit, label);
      }
      if (!seen.insert(k).second) continue;
      if (!lab.count(k)) lab[k] = label;
      cnt[k]++;
    }
  }
  vector<std::pair<string, int>> out;
  out.reserve(cnt.size());
  for (auto& kv : cnt) out.push_back({lab[kv.first], kv.second});
  std::sort(out.begin(), out.end(), [](const std::pair<string, int>& a, const std::pair<string, int>& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  return out;
}

double variantScore(const string& reason) {
  if (reason == "hyphen") return 0.98;
  if (reason == "spelling") return 0.95;
  if (reason == "plural") return 0.9;
  if (reason == "author id") return 0.99;
  if (reason == "acronym") return 0.7;
  if (reason == "acronym (text)") return 0.65;
  if (reason == "typo") return 0.5;
  if (reason == "variant") return 0.6;
  return 0.6;
}

void rankVariants(vector<VariantGroup>& v) {
  for (auto& g : v) if (g.score <= 0) g.score = variantScore(g.reason);
  std::stable_sort(v.begin(), v.end(), [](const VariantGroup& a, const VariantGroup& b) {
    if (a.ai != b.ai) return a.ai;  // AI proposals first (they were asked for), each block ranked by confidence
    if (std::fabs(a.score - b.score) > 1e-9) return a.score > b.score;
    return a.targetCount > b.targetCount;
  });
}

std::map<string, string> acronymDictionary(const Corpus& c) {
  std::map<string, std::map<string, int>> votes;  // abbr -> long form -> count
  auto scan = [&](const string& text) {
    size_t p = 0;
    while ((p = text.find('(', p)) != string::npos) {
      size_t e = text.find(')', p);
      if (e == string::npos) break;
      string ab = text.substr(p + 1, e - p - 1);
      p = e + 1;
      if (ab.size() < 2 || ab.size() > 8) continue;
      bool caps = true;
      int letters = 0;
      for (char ch : ab) {
        if (isupper(uint8_t(ch))) letters++;
        else if (!isdigit(uint8_t(ch)) && ch != '-' && ch != 's') caps = false;
      }
      if (!caps || letters < 2) continue;
      if (ab.back() == 's' && letters >= 2) ab.pop_back();  // "(LLMs)"
      // the long form: the preceding words, as many as the abbreviation has letters (allowing small function words)
      size_t start = text.substr(0, p - ab.size() - 3).size();  // index of '('
      size_t at = start;
      while (at > 0 && text[at - 1] == ' ') at--;
      vector<string> words;
      size_t wend = at;
      int need = 0;
      for (char ch : ab) if (isalpha(uint8_t(ch))) need++;
      while (int(words.size()) < need + 3 && wend > 0) {
        size_t ws = wend;
        while (ws > 0 && text[ws - 1] != ' ') ws--;
        string w = text.substr(ws, wend - ws);
        bool ok = !w.empty();
        for (char ch : w) if (!isalnum(uint8_t(ch)) && ch != '-' && ch != '\'') ok = false;
        if (!ok) break;
        words.insert(words.begin(), w);
        // does the initial-letter sequence of the collected words match the abbreviation?
        string init;
        for (auto& x : words) {
          string lx = lower(x);
          if (lx == "of" || lx == "and" || lx == "the" || lx == "for" || lx == "in" || lx == "on" || lx == "to" || lx == "with") continue;
          init += char(toupper(uint8_t(x[0])));
          for (size_t q = 0; q + 1 < x.size(); q++) if (x[q] == '-' && isalpha(uint8_t(x[q + 1]))) init += char(toupper(uint8_t(x[q + 1])));
        }
        string abl;
        for (char ch : ab) if (isalpha(uint8_t(ch))) abl += char(toupper(uint8_t(ch)));
        if (init == abl && words.size() >= 2) { votes[lower(ab)][lower(join(words, " "))]++; break; }
        wend = ws;
        while (wend > 0 && text[wend - 1] == ' ') wend--;
      }
    }
  };
  for (auto& r : c.recs) { scan(r.title); scan(r.abstract_); }
  std::map<string, string> out;
  for (auto& kv : votes) {
    string best;
    int bc = 0, tot = 0;
    for (auto& f : kv.second) { tot += f.second; if (f.second > bc) { bc = f.second; best = f.first; } }
    if (bc >= 2 || (bc == 1 && tot == 1 && best.size() > 8)) out[kv.first] = best;
  }
  return out;
}

namespace {
struct AuthorName { string sur, ini, given; };
AuthorName parseAuthor(const string& label) {
  AuthorName a;
  string l = lower(asciiFold(trim(label)));
  size_t comma = l.find(',');
  string given;
  if (comma != string::npos) { a.sur = trim(l.substr(0, comma)); given = trim(l.substr(comma + 1)); }
  else {
    vector<string> w = split(l, ' ');
    if (w.empty()) return a;
    a.sur = w.back();
    w.pop_back();
    given = join(w, " ");
  }
  string sur;
  for (char ch : a.sur) if (isalpha(uint8_t(ch))) sur += ch;
  a.sur = sur;
  for (auto& w : split(replaceAll(replaceAll(given, ".", " "), "-", " "), ' ')) {
    if (w.empty() || !isalpha(uint8_t(w[0]))) continue;
    a.ini += w[0];
    if (w.size() > 1) a.given += (a.given.empty() ? "" : " ") + w;
  }
  return a;
}
}  // namespace

vector<VariantGroup> findAuthorVariants(const Corpus& c) {
  struct Info { string label; int count = 0; std::set<string> ids, affs, countries, coauthors; AuthorName name; };
  std::map<string, Info> info;  // lower label -> info
  for (auto& r : c.recs) {
    std::set<string> affs, ctry;
    for (auto& a : r.affiliations) affs.insert(lower(asciiFold(a)));
    for (auto& a : r.countries) ctry.insert(lower(a));
    for (size_t i = 0; i < r.authors.size(); i++) {
      string lab = bibClean(r.authors[i]);
      if (lab.empty()) continue;
      Info& in = info[lower(lab)];
      if (in.count == 0) { in.label = lab; in.name = parseAuthor(lab); }
      in.count++;
      if (i < r.authorIds.size() && !r.authorIds[i].empty()) in.ids.insert(r.authorIds[i]);
      in.affs.insert(affs.begin(), affs.end());
      in.countries.insert(ctry.begin(), ctry.end());
      for (size_t j = 0; j < r.authors.size(); j++) if (j != i) in.coauthors.insert(lower(bibClean(r.authors[j])));
    }
  }
  // candidates share a surname
  std::map<string, vector<const Info*>> bySur;
  for (auto& kv : info) if (!kv.second.name.sur.empty() && kv.second.name.sur.size() >= 2) bySur[kv.second.name.sur].push_back(&kv.second);
  vector<VariantGroup> out;
  for (auto& kv : bySur) {
    auto& v = kv.second;
    if (v.size() < 2 || v.size() > 400) continue;
    size_t n = v.size();
    vector<size_t> parent(n);
    for (size_t i = 0; i < n; i++) parent[i] = i;
    std::function<size_t(size_t)> find = [&](size_t x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    vector<double> best(n, 0);
    vector<string> why(n);
    struct Pair { size_t i, j; double sc; string ev; };
    vector<Pair> pairs;
    vector<char> incompat(n * n, 0);  // explicitly different people (other identifiers, other given names)
    for (size_t i = 0; i < n; i++)
      for (size_t j = i + 1; j < n; j++) {
        const Info &A = *v[i], &B = *v[j];
        // identifiers decide when both have them
        bool shared = false, conflict = false;
        for (auto& id : A.ids) if (B.ids.count(id)) shared = true;
        if (!shared && !A.ids.empty() && !B.ids.empty()) conflict = true;
        if (shared) {
          string id;
          for (auto& x : A.ids) if (B.ids.count(x)) { id = x; break; }
          pairs.push_back({i, j, 0.99, id.compare(0, 7, "scopus:") == 0 ? "Scopus id " + id.substr(7) : id.compare(0, 9, "openalex:") == 0 ? "OpenAlex id " + id.substr(9) : id.compare(0, 4, "rid:") == 0 ? "ResearcherID " + id.substr(4) : "ORCID " + id});
          continue;
        }
        if (conflict) { incompat[i * n + j] = incompat[j * n + i] = 1; continue; }
        const string &ia = A.name.ini, &ib = B.name.ini;
        if (ia.empty() || ib.empty()) continue;
        bool same = ia == ib, prefix = !same && (ia.compare(0, std::min(ia.size(), ib.size()), ib.substr(0, std::min(ia.size(), ib.size()))) == 0);
        if (!same && !prefix) { incompat[i * n + j] = incompat[j * n + i] = 1; continue; }
        // full given names that differ ("John" vs "James") are different people even with equal initials
        if (!A.name.given.empty() && !B.name.given.empty()) {
          string ga = split(A.name.given, ' ')[0], gb = split(B.name.given, ' ')[0];
          if (ga != gb && !(ga.size() >= 2 && gb.size() >= 2 && (startsWith(ga, gb) || startsWith(gb, ga)))) { incompat[i * n + j] = incompat[j * n + i] = 1; continue; }
        }
        int affs = 0, co = 0, ctry = 0;
        for (auto& x : A.affs) if (B.affs.count(x)) affs++;
        for (auto& x : A.coauthors) if (B.coauthors.count(x) && x != lower(A.label) && x != lower(B.label)) co++;
        for (auto& x : A.countries) if (B.countries.count(x)) ctry++;
        string ev = same ? "same initials" : "compatible initials";
        double sc = 0;
        if (affs > 0) { sc = same ? 0.9 : 0.8; ev += ", " + std::to_string(affs) + (affs == 1 ? " shared affiliation" : " shared affiliations"); }
        else if (co > 0) { sc = same ? 0.85 : 0.72; ev += ", " + std::to_string(co) + (co == 1 ? " shared co-author" : " shared co-authors"); }
        else if (ctry > 0 && same) { sc = 0.6; ev += ", same country"; }
        else if (same && A.name.given.size() > 1 && B.name.given.size() > 1) { sc = 0.7; ev += ", same given name"; }
        else if (same) { sc = 0.45; ev += " only"; }
        else continue;  // prefix initials without any shared context: too weak to show
        pairs.push_back({i, j, sc, ev});
      }
    // strongest evidence first; a link is dropped when it would put two explicitly different people in one group
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.sc > b.sc; });
    vector<vector<size_t>> members(n);
    for (size_t i = 0; i < n; i++) members[i] = {i};
    for (auto& p : pairs) {
      size_t a = find(p.i), b = find(p.j);
      if (a != b) {
        bool clash = false;
        for (size_t x : members[a]) { for (size_t y : members[b]) if (incompat[x * n + y]) { clash = true; break; } if (clash) break; }
        if (clash) continue;
        parent[a] = b;
        members[b].insert(members[b].end(), members[a].begin(), members[a].end());
        members[a].clear();
      }
      if (p.sc > best[p.i]) { best[p.i] = p.sc; why[p.i] = p.ev; }
      if (p.sc > best[p.j]) { best[p.j] = p.sc; why[p.j] = p.ev; }
    }
    std::map<size_t, vector<size_t>> groups;
    for (size_t i = 0; i < n; i++) groups[find(i)].push_back(i);
    for (auto& g : groups) {
      if (g.second.size() < 2) continue;
      VariantGroup vg;
      // target: the most frequent label; fuller given names win ties
      size_t bi = g.second[0];
      for (size_t i : g.second)
        if (v[i]->count > v[bi]->count || (v[i]->count == v[bi]->count && v[i]->label.size() > v[bi]->label.size())) bi = i;
      vg.target = v[bi]->label;
      vg.targetCount = v[bi]->count;
      double sc = 1;
      for (size_t i : g.second) {
        if (i == bi) continue;
        vg.members.push_back(v[i]->label);
        vg.counts.push_back(v[i]->count);
        sc = std::min(sc, best[i]);
        if (vg.evidence.empty() || best[i] < sc + 1e-9) vg.evidence = why[i];
      }
      vg.score = sc;
      vg.reason = sc >= 0.99 ? "author id" : sc >= 0.8 ? "name + affiliation" : sc >= 0.7 ? "name + context" : "initials";
      vg.safe = sc >= 0.99;
      vg.apply = vg.safe;
      out.push_back(vg);
    }
  }
  rankVariants(out);
  return out;
}

vector<VariantGroup> findVariants(const Corpus& c, Unit unit, const Thesaurus& th) {
  (void)th;
  if (unit == Unit::Authors) return findAuthorVariants(c);
  std::map<string, int> cnt;       // label → count (first-seen label per key)
  std::map<string, string> keyLabel;
  for (auto& r : c.recs) {
    vector<string> list;
    if (unit == Unit::Keywords || unit == Unit::AllKeywords) list.insert(list.end(), r.keywords.begin(), r.keywords.end());
    if (unit == Unit::IndexTerms || unit == Unit::AllKeywords) list.insert(list.end(), r.indexTerms.begin(), r.indexTerms.end());
    if (unit == Unit::Authors) list = r.authors;
    if (unit == Unit::Orgs) list = r.affiliations;
    if (unit == Unit::Countries) list = r.countries;
    if (unit == Unit::Sources && !r.source.empty()) list = {r.source};
    for (auto& l : list) {
      string lab = bibClean(l);
      if (lab.empty()) continue;
      string k = lower(lab);
      if (!keyLabel.count(k)) keyLabel[k] = lab;
      cnt[k]++;
    }
  }
  // canonical form: us spelling + singular + no hyphen/space
  auto canon = [](const string& k, string* why) {
    string a = replaceAll(replaceAll(k, "-", " "), "  ", " ");
    vector<string> w = split(a, ' ');
    string s1 = join(w, " ");
    for (auto& x : w) x = usSpelling(x);
    string s2 = join(w, " ");
    if (!w.empty()) w.back() = singular(w.back());
    string s3 = join(w, " ");
    string s4 = replaceAll(s3, " ", "");
    if (why) *why = s1 != k ? "hyphen" : (s2 != s1 ? "spelling" : (s3 != s2 ? "plural" : "hyphen"));
    return s4;
  };
  std::map<string, vector<string>> groups;
  for (auto& kv : cnt) groups[canon(kv.first, nullptr)].push_back(kv.first);
  // acronyms: "long form (ABBR)" joins "long form" and "abbr"
  std::map<string, string> acr;
  for (auto& kv : cnt) {
    const string& k = kv.first;
    size_t o = k.rfind('('), e = k.rfind(')');
    if (o != string::npos && e == k.size() - 1 && o > 2 && e - o >= 3 && e - o <= 8) {
      string longf = trim(k.substr(0, o)), ab = k.substr(o + 1, e - o - 1);
      string cl = canon(longf, nullptr), ca = canon(ab, nullptr), ck = canon(k, nullptr);
      if (groups.count(cl) && cl != ck) { for (auto& x : groups[ck]) groups[cl].push_back(x); groups.erase(ck); acr[cl] = "acronym"; }
      if (groups.count(ca) && ca != cl && groups.count(cl)) { for (auto& x : groups[ca]) groups[cl].push_back(x); groups.erase(ca); acr[cl] = "acronym"; }
    }
  }
  vector<VariantGroup> out;
  std::set<string> inGroup;
  for (auto& g : groups) {
    if (g.second.size() < 2) continue;
    VariantGroup vg;
    string best;
    int bc = -1;
    for (auto& k : g.second) if (cnt[k] > bc || (cnt[k] == bc && k.size() < best.size())) { bc = cnt[k]; best = k; }
    vg.target = keyLabel[best];
    vg.targetCount = bc;
    string why;
    auto stepForms = [&](const string& k, string& h, string& sp, string& pl) {
      string a = replaceAll(k, "-", " ");
      vector<string> w = split(a, ' ');
      h = replaceAll(join(w, " "), " ", "");
      for (auto& x : w) x = usSpelling(x);
      sp = replaceAll(join(w, " "), " ", "");
      if (!w.empty()) w.back() = singular(w.back());
      pl = replaceAll(join(w, " "), " ", "");
    };
    string bh, bs, bp;
    stepForms(best, bh, bs, bp);
    for (auto& k : g.second) {
      inGroup.insert(k);
      if (k == best) continue;
      vg.members.push_back(keyLabel[k]);
      vg.counts.push_back(cnt[k]);
      if (why.empty()) {
        string h, sp, pl;
        stepForms(k, h, sp, pl);
        why = h == bh ? "hyphen" : (sp == bs ? "spelling" : (pl == bp ? "plural" : "variant"));
      }
    }
    vg.reason = acr.count(g.first) ? "acronym" : (why.empty() ? "variant" : why);
    vg.safe = vg.reason != "acronym";
    vg.apply = vg.safe;
    out.push_back(vg);
  }
  // typos: edit distance 1 between long labels not already grouped
  vector<string> keys;
  for (auto& kv : cnt) if (kv.first.size() >= 8 && !inGroup.count(kv.first)) keys.push_back(kv.first);
  if (keys.size() < 3000) {
    for (size_t i = 0; i < keys.size(); i++)
      for (size_t j = i + 1; j < keys.size(); j++) {
        if (keys[i][0] != keys[j][0]) continue;
        if (lev(keys[i], keys[j], 1) == 1) {
          // skip digit differences ("web 2.0" vs "web 3.0")
          bool digit = false;
          for (size_t q = 0; q < std::min(keys[i].size(), keys[j].size()); q++) if (keys[i][q] != keys[j][q] && (isdigit((unsigned char)keys[i][q]) || isdigit((unsigned char)keys[j][q]))) digit = true;
          if (digit) continue;
          VariantGroup vg;
          bool iBig = cnt[keys[i]] >= cnt[keys[j]];
          vg.target = keyLabel[iBig ? keys[i] : keys[j]];
          vg.targetCount = cnt[iBig ? keys[i] : keys[j]];
          vg.members = {keyLabel[iBig ? keys[j] : keys[i]]};
          vg.counts = {cnt[iBig ? keys[j] : keys[i]]};
          vg.reason = "typo";
          vg.safe = false;
          vg.apply = false;
          out.push_back(vg);
        }
      }
  }
  // acronyms expanded from the titles and abstracts: "ml" joins "machine learning" when both are terms
  if (unit == Unit::Keywords || unit == Unit::IndexTerms || unit == Unit::AllKeywords) {
    std::map<string, string> dict = acronymDictionary(c);
    std::map<string, string> canonKey;  // canonical form -> a label key present in the data
    for (auto& kv : cnt) canonKey.emplace(canon(kv.first, nullptr), kv.first);
    for (auto& kv : dict) {
      auto a = canonKey.find(canon(kv.first, nullptr)), l = canonKey.find(canon(kv.second, nullptr));
      if (a == canonKey.end() || l == canonKey.end() || a->second == l->second) continue;
      if (inGroup.count(a->second) && inGroup.count(l->second)) continue;
      VariantGroup vg;
      bool longBig = cnt[l->second] >= cnt[a->second];
      vg.target = keyLabel[longBig ? l->second : a->second];
      vg.targetCount = cnt[longBig ? l->second : a->second];
      vg.members = {keyLabel[longBig ? a->second : l->second]};
      vg.counts = {cnt[longBig ? a->second : l->second]};
      vg.reason = "acronym (text)";
      vg.evidence = "\"" + kv.second + " (" + upper(kv.first) + ")\" in titles or abstracts";
      vg.safe = false;
      vg.apply = false;
      out.push_back(vg);
      inGroup.insert(a->second);
      inGroup.insert(l->second);
    }
  }
  rankVariants(out);
  return out;
}

// ------------------------------------------------------------ keyword naming
string keywordName(const Corpus& c, const vector<int>& recs, int top) {
  if (recs.size() < 2 || c.recs.empty()) return "";
  std::unordered_set<int> inC(recs.begin(), recs.end());
  std::unordered_map<string, int> df, cf;
  std::unordered_map<string, string> lab;
  for (size_t i = 0; i < c.recs.size(); i++) {
    std::unordered_set<string> seen;
    for (auto& k : c.recs[i].keywords) {
      string kk = lower(bibClean(k));
      if (kk.empty() || !seen.insert(kk).second) continue;
      df[kk]++;
      if (inC.count(int(i))) cf[kk]++;
      lab.emplace(kk, kk);
    }
  }
  double N = double(c.recs.size()), Nc = double(inC.size());
  vector<std::pair<double, string>> sc;
  for (auto& kv : cf) {
    int cnt = kv.second;
    if (cnt < 2 && Nc > 3) continue;
    double lift = (cnt / Nc) / (double(df[kv.first]) / N);
    sc.emplace_back(cnt * std::log(1 + lift) * (lift > 1 ? 1 : 0.3), kv.first);
  }
  std::sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
  vector<string> names;
  for (size_t i = 0; i < sc.size() && int(i) < top; i++) names.push_back(sc[i].second);
  return join(names, " \xC2\xB7 ");
}

}  // namespace vs
