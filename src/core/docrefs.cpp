// VOSStudio Native: references and citation styles of the editable document.
//
// The bibliography is structured (RefEntry: authors, year, title, container, volume, issue, pages, DOI). Everything
// the reader sees is generated from it and the document's citation style: the in-text citations (citation fields,
// Span::cite) and the Reference paragraphs under the "References" heading (Para::refKey). Changing the style
// regenerates both — like Word's own citations, which the Word export also produces (see docx.cpp).
#include <algorithm>
#include <map>
#include <set>

#include "doc.h"
#include "model.h"

namespace vs {

// ---------------------------------------------------------------- fonts
bool PageSetup::serifFamily(const string& f0) {
  string f = lower(f0);
  static const char* serifs[] = {"cambria", "georgia", "times", "garamond", "book antiqua", "palatino", "constantia", "baskerville",
                                 "century", "bookman", "minion", "caslon", "didot", "bodoni", "sylfaen", "perpetua", "goudy",
                                 "rockwell", "charter", "merriweather", "lora", "libre baskerville", "crimson", "eb garamond",
                                 "source serif", "noto serif", "dejavu serif", "liberation serif", "pt serif", "playfair"};
  for (const char* s : serifs) if (f.find(s) != string::npos) return true;
  return f.find("serif") != string::npos && f.find("sans") == string::npos;
}

bool PageSetup::monoFamily(const string& f0) {
  string f = lower(f0);
  static const char* monos[] = {"consolas", "courier", "cascadia", "lucida console", "monaco", "menlo", "fira code", "fira mono",
                                "source code", "jetbrains", "roboto mono", "ubuntu mono", "dejavu sans mono", "liberation mono", "inconsolata", "mono"};
  for (const char* s : monos) if (f.find(s) != string::npos) return true;
  return false;
}

// ---------------------------------------------------------------- styles
const vector<CiteStyleInfo>& citeStyles() {
  static const vector<CiteStyleInfo> v = {
      {"apa", "APA 7th (Author, Year)", false, "\\APASixthEditionOfficeOnline.xsl", "APA"},
      {"mla", "MLA 9th (Author page)", false, "\\MLASeventhEditionOfficeOnline.xsl", "MLA"},
      {"chicago", "Chicago author-date (Author Year)", false, "\\CHICAGO.XSL", "Chicago"},
      {"harvard", "Harvard (Author, Year)", false, "\\HarvardAnglia2008OfficeOnline.xsl", "Harvard - Anglia"},
      {"ieee", "IEEE [1]", true, "\\IEEE2006OfficeOnline.xsl", "IEEE"},
      {"vancouver", "Vancouver (1)", true, "\\ISO690Nmerical.XSL", "ISO 690 - Numerical Reference"},
      {"nature", "Nature, superscript", true, "\\ISO690Nmerical.XSL", "ISO 690 - Numerical Reference"},
  };
  return v;
}

const CiteStyleInfo& citeStyleInfo(const string& id) {
  for (auto& s : citeStyles()) if (id == s.id) return s;
  return citeStyles()[0];
}

// ---------------------------------------------------------------- names
RefName splitAuthor(const string& name0) {
  RefName n;
  string name = collapseWs(trim(name0));
  if (name.empty()) return n;
  size_t comma = name.find(',');
  if (comma != string::npos) {
    n.last = trim(name.substr(0, comma));
    n.given = trim(name.substr(comma + 1));
  } else {
    // "John A. Smith" -> Smith / John A.;  "Smith JA" (WoS / Vancouver) -> Smith / JA
    vector<string> parts = split(name, ' ');
    if (parts.size() == 1) n.last = parts[0];
    else {
      const string& lastTok = parts.back();
      bool initialsLast = lastTok.size() <= 3 && std::all_of(lastTok.begin(), lastTok.end(), [](char c) { return isupper(uint8_t(c)) || c == '.'; });
      if (initialsLast) { n.last = join(vector<string>(parts.begin(), parts.end() - 1), " "); n.given = lastTok; }
      else { n.last = lastTok; n.given = join(vector<string>(parts.begin(), parts.end() - 1), " "); }
    }
  }
  // initials from the given names: "John A." -> "J. A.", "JA" -> "J. A.", "J.-P." -> "J.-P."
  string g = n.given;
  if (!g.empty()) {
    bool compact = g.find(' ') == string::npos && g.find('.') == string::npos && g.size() <= 3 && std::all_of(g.begin(), g.end(), [](char c) { return isupper(uint8_t(c)); });
    vector<string> toks;
    if (compact) for (char c : g) toks.push_back(string(1, c));
    else toks = splitAny(g, " .");
    string ini;
    for (auto& t : toks) {
      if (t.empty()) continue;
      if (!ini.empty()) ini += ' ';
      size_t hy = t.find('-');
      if (hy != string::npos && hy + 1 < t.size()) ini += string(1, t[0]) + ".-" + string(1, t[hy + 1]) + ".";
      else ini += string(1, t[0]) + ".";
    }
    n.initials = ini;
  }
  return n;
}

namespace {

string initialsBare(const RefName& n) { string o; for (char c : n.initials) if (isalpha(uint8_t(c))) o += c; return o; }  // "JA"

// "Last, F. M." (APA / Harvard / Nature)
string nameLastInitials(const RefName& n) { return n.initials.empty() ? n.last : n.last + ", " + n.initials; }
// "F. M. Last" (IEEE)
string nameInitialsLast(const RefName& n) { return n.initials.empty() ? n.last : n.initials + " " + n.last; }
// "Last FM" (Vancouver)
string nameVancouver(const RefName& n) { string i = initialsBare(n); return i.empty() ? n.last : n.last + " " + i; }
// "Last, First" / "First Last" (MLA, Chicago): the given names as recorded, initials when that is all we have
string nameLastGiven(const RefName& n) { return n.given.empty() ? n.last : n.last + ", " + n.given; }
string nameGivenLast(const RefName& n) { return n.given.empty() ? n.last : n.given + " " + n.last; }

string joinNames(const vector<string>& names, const string& sep, const string& lastSep) {
  if (names.empty()) return "";
  if (names.size() == 1) return names[0];
  string o;
  for (size_t i = 0; i + 1 < names.size(); i++) { if (i) o += sep; o += names[i]; }
  return o + lastSep + names.back();
}

string endDot(const string& s) {
  if (s.empty()) return s;
  char c = s.back();
  return (c == '.' || c == '?' || c == '!') ? s : s + ".";
}
string noEndDot(string s) { while (!s.empty() && s.back() == '.') s.pop_back(); return s; }

string doiUrl(const RefEntry& e) {
  if (!e.doi.empty()) { string d = e.doi; if (startsWith(lower(d), "http")) return d; return "https://doi.org/" + d; }
  return e.url;
}

struct Out {  // builds the spans of one entry
  vector<Span> spans;
  void t(const string& s, uint16_t f = 0) { if (!s.empty()) spans.push_back({s, f, ""}); }
  void i(const string& s) { t(s, F_ITALIC); }
  void b(const string& s) { t(s, F_BOLD); }
  void link(const string& text, const string& url) { if (!text.empty()) spans.push_back({text, 0, url}); }
};

// a free-text entry (assistant strings, imported references): the raw text, a DOI/URL in it becomes a link
vector<Span> rawSpans(const RefEntry& e) {
  vector<Span> sp = spansFromMarkdown(e.raw, false, true);
  if (!e.url.empty()) {
    bool linked = false;
    for (auto& s : sp) if (!s.link.empty()) linked = true;
    if (!linked) { sp.push_back({" ", 0, ""}); sp.push_back({e.url, 0, e.url}); }
  }
  return sp;
}

// author / year guessed from a free-text entry ("Smith, J., & Lee, K. (2020). …")
void rawAuthorYear(const RefEntry& e, string& last, string& year, int& nAuthors) {
  string r = e.raw;
  size_t c = r.find_first_of(",.(");
  last = trim(c == string::npos ? r.substr(0, std::min<size_t>(r.size(), 24)) : r.substr(0, c));
  {  // "Smith J" / "Smith JA" / "Doe J A" -> "Smith": trailing initials go
    vector<string> w = splitAny(last, " ");
    while (w.size() > 1 && (w.back().size() <= 2 || w.back().find('.') != string::npos) && isupper(uint8_t(w.back()[0]))) w.pop_back();
    if (!w.empty()) last = join(w, " ");
  }
  year.clear();
  for (size_t i = 0; i + 3 < r.size(); i++) {
    if (isdigit(uint8_t(r[i])) && isdigit(uint8_t(r[i + 1])) && isdigit(uint8_t(r[i + 2])) && isdigit(uint8_t(r[i + 3])) &&
        (i == 0 || !isdigit(uint8_t(r[i - 1]))) && (i + 4 >= r.size() || !isdigit(uint8_t(r[i + 4])))) {
      int y = toInt(r.substr(i, 4), 0);
      if (y >= 1500 && y <= 2100) { year = r.substr(i, 4); break; }
    }
  }
  nAuthors = 1;
  size_t amp = r.find(" & ");
  if (amp != string::npos || r.find(" and ") != string::npos) nAuthors = 2;
  if (r.find("et al") != string::npos) nAuthors = 3;
}

}  // namespace

string refKeyFor(const string& doi, const string& title, const string& year) {
  string d = normDoi(doi);
  if (!d.empty()) return "doi:" + lower(d);
  string t = lower(asciiFold(collapseWs(trim(title))));
  return "t:" + sha256Hex(t + "|" + year).substr(0, 16);
}

string refSortKey(const RefEntry& e) {
  if (!e.structured()) { string last, year; int n; rawAuthorYear(e, last, year, n); return lower(asciiFold(last)) + "|" + year + "|" + lower(e.raw); }
  string k;
  for (auto& a : e.authors) { RefName n = splitAuthor(a); k += lower(asciiFold(n.last)) + " " + lower(asciiFold(n.initials)) + ";"; }
  if (e.authors.empty()) k = lower(asciiFold(e.title));
  return k + "|" + e.year + "|" + lower(asciiFold(e.title));
}

string wordSourceType(const string& kind) {
  if (kind == "book") return "Book";
  if (kind == "chapter") return "BookSection";
  if (kind == "conference") return "ConferenceProceedings";
  if (kind == "report") return "Report";
  if (kind == "web") return "InternetSite";
  if (kind == "thesis") return "Report";
  if (kind == "software") return "Misc";
  if (kind == "other") return "Misc";
  return "JournalArticle";
}

int migrateDocRecordLinks(Document& d, const Corpus& corpus) {
  int changed = 0;
  auto titleKey = [](const string& value) { return lower(asciiFold(collapseWs(trim(value)))); };
  for (RefEntry& e : d.refs) {
    int match = -1;
    const bool hadRecordId = !e.recordId.empty();
    if (hadRecordId) {
      for (size_t i = 0; i < corpus.recs.size(); i++) if (corpus.recs[i].id == e.recordId) { match = int(i); break; }
      if (match < 0) continue;  // never redirect a stale durable link by metadata alone
    }
    if (match < 0 && !e.doi.empty()) {
      string wanted = lower(normDoi(e.doi));
      int only = -1, count = 0;
      for (size_t i = 0; i < corpus.recs.size(); i++) if (lower(normDoi(corpus.recs[i].doi)) == wanted) { only = int(i); count++; }
      if (count == 1) match = only;
    }
    if (match < 0 && !trim(e.title).empty()) {
      string wanted = titleKey(e.title);
      int only = -1, count = 0;
      for (size_t i = 0; i < corpus.recs.size(); i++) {
        const Record& r = corpus.recs[i];
        if (titleKey(r.title) != wanted) continue;
        if (!e.year.empty() && r.year > 0 && std::to_string(r.year) != e.year) continue;
        only = int(i); count++;
      }
      if (count == 1) match = only;
    }
    if (match < 0 && e.doi.empty() && trim(e.title).empty() && e.rec >= 0 && size_t(e.rec) < corpus.recs.size()) match = e.rec;
    if (match < 0 || corpus.recs[size_t(match)].id.empty()) continue;
    const string& id = corpus.recs[size_t(match)].id;
    if (e.recordId != id || e.rec != match) {
      e.recordId = id;
      e.rec = match;
      changed++;
    }
  }
  return changed;
}

RefEntry refFromRecord(const Record& r) {
  RefEntry e;
  e.rec = -1;
  e.recordId = r.id;
  e.authors = r.authors;
  e.year = r.year > 0 ? std::to_string(r.year) : "";
  e.title = bibClean(r.title);
  e.container = bibClean(r.source);
  e.volume = trim(r.volume);
  e.issue = trim(r.issue);
  e.pages = trim(r.pages);
  e.doi = normDoi(r.doi);
  e.url = trim(r.url);
  e.publisher = trim(r.publisher);
  string dt = lower(r.docType);
  if (dt.find("book chapter") != string::npos || dt.find("chapter") != string::npos) e.kind = "chapter";
  else if (dt.find("book") != string::npos) e.kind = "book";
  else if (dt.find("proceeding") != string::npos || dt.find("conference") != string::npos) e.kind = "conference";
  else if (dt.find("thesis") != string::npos || dt.find("dissertation") != string::npos) e.kind = "thesis";
  else if (dt.find("report") != string::npos) e.kind = "report";
  else e.kind = "article";
  // "12(3)" volumes carry the issue
  size_t par = e.volume.find('(');
  if (par != string::npos) {
    size_t close = e.volume.find(')', par);
    e.issue = e.volume.substr(par + 1, close == string::npos ? string::npos : close - par - 1);
    e.volume = trim(e.volume.substr(0, par));
  }
  e.key = refKeyFor(e.doi, e.title, e.year);
  return e;
}

// ---------------------------------------------------------------- one bibliography entry
vector<Span> formatReference(const RefEntry& e, const string& style, int number) {
  Out o;
  const CiteStyleInfo& st = citeStyleInfo(style);
  string id = st.id;
  if (!e.structured()) {
    if (st.numeric) o.t(id == "ieee" ? "[" + std::to_string(number) + "] " : std::to_string(number) + ". ");
    vector<Span> r = rawSpans(e);
    o.spans.insert(o.spans.end(), r.begin(), r.end());
    return o.spans;
  }
  vector<RefName> names;
  for (auto& a : e.authors) names.push_back(splitAuthor(a));
  string year = e.year.empty() ? "n.d." : e.year;
  string url = doiUrl(e);
  bool book = e.kind == "book";
  string pages = replaceAll(e.pages, "-", "\xE2\x80\x93");  // en dash
  auto volIssue = [&](const string& sep, bool italicVol) {
    if (e.volume.empty() && e.issue.empty()) return;
    if (!e.volume.empty()) { if (italicVol) o.i(e.volume); else o.t(e.volume); }
    if (!e.issue.empty()) o.t("(" + e.issue + ")");
    o.t(sep);
  };
  if (id == "apa" || id == "harvard") {
    // Smith, J. A., Lee, K., & Wang, M. (2020). Title. Journal, 12(3), 1–10. https://doi.org/…
    vector<string> ns;
    size_t maxN = 20;
    for (size_t i = 0; i < names.size() && i < maxN; i++) ns.push_back(nameLastInitials(names[i]));
    string authors;
    if (names.size() > maxN) authors = join(vector<string>(ns.begin(), ns.begin() + 19), ", ") + ", … " + nameLastInitials(names.back());
    else authors = joinNames(ns, ", ", id == "apa" ? ", & " : (ns.size() > 2 ? ", and " : " and "));
    if (authors.empty()) authors = e.title;
    if (id == "apa") {
      o.t(endDot(authors) + " (" + year + "). ");
      if (book) { o.i(endDot(e.title)); o.t(" "); if (!e.publisher.empty()) o.t(endDot(e.publisher) + " "); }
      else {
        o.t(endDot(e.title) + " ");
        if (!e.container.empty()) { o.i(e.container); o.t(!e.volume.empty() || !e.issue.empty() || !pages.empty() ? ", " : ". "); }
        if (!e.volume.empty()) o.i(e.volume);
        if (!e.issue.empty()) o.t("(" + e.issue + ")");
        if (!pages.empty()) o.t((e.volume.empty() && e.issue.empty() ? "" : ", ") + pages);
        if (!e.volume.empty() || !e.issue.empty() || !pages.empty()) o.t(". ");
        if (e.container.empty() && e.volume.empty() && pages.empty() && !e.publisher.empty()) o.t(endDot(e.publisher) + " ");
      }
    } else {  // Harvard: Smith, J.A. and Lee, K. (2020) 'Title', Journal, 12(3), pp. 1–10. doi: …
      o.t(authors + " (" + year + ") ");
      if (book) { o.i(e.title); o.t(". "); if (!e.publisher.empty()) o.t((e.place.empty() ? "" : e.place + ": ") + endDot(e.publisher) + " "); }
      else {
        o.t("\xE2\x80\x98" + noEndDot(e.title) + "\xE2\x80\x99, ");
        if (!e.container.empty()) { o.i(e.container); o.t(", "); }
        volIssue(", ", false);
        if (!pages.empty()) o.t("pp. " + pages + ". ");
        else if (!e.container.empty() || !e.volume.empty()) { if (!o.spans.empty() && o.spans.back().text.size() >= 2 && o.spans.back().text.substr(o.spans.back().text.size() - 2) == ", ") o.spans.back().text = o.spans.back().text.substr(0, o.spans.back().text.size() - 2) + ". "; }
      }
    }
    if (!url.empty()) o.link(id == "harvard" && !e.doi.empty() ? "doi: " + e.doi : url, url);
    return o.spans;
  }
  if (id == "mla") {
    // Smith, John A., and Kim Lee. "Title." Journal, vol. 12, no. 3, 2020, pp. 1–10. https://doi.org/…
    string authors;
    if (names.size() == 1) authors = nameLastGiven(names[0]);
    else if (names.size() == 2) authors = nameLastGiven(names[0]) + ", and " + nameGivenLast(names[1]);
    else if (names.size() > 2) authors = nameLastGiven(names[0]) + ", et al";
    if (!authors.empty()) o.t(endDot(authors) + " ");
    if (book) { o.i(endDot(e.title)); o.t(" "); if (!e.publisher.empty()) o.t(e.publisher + ", "); o.t(year + ". "); }
    else {
      o.t("\xE2\x80\x9C" + endDot(e.title) + "\xE2\x80\x9D ");
      if (!e.container.empty()) { o.i(e.container); o.t(", "); }
      if (!e.volume.empty()) o.t("vol. " + e.volume + ", ");
      if (!e.issue.empty()) o.t("no. " + e.issue + ", ");
      o.t(year);
      if (!pages.empty()) o.t(", pp. " + pages);
      o.t(". ");
    }
    if (!url.empty()) { o.link(url, url); o.t("."); }
    return o.spans;
  }
  if (id == "chicago") {
    // Smith, John A., and Kim Lee. 2020. "Title." Journal 12 (3): 1–10. https://doi.org/…
    vector<string> ns;
    for (size_t i = 0; i < names.size() && i < 10; i++) ns.push_back(i == 0 ? nameLastGiven(names[i]) : nameGivenLast(names[i]));
    string authors = names.size() > 10 ? join(vector<string>(ns.begin(), ns.begin() + 7), ", ") + ", et al" : joinNames(ns, ", ", ns.size() > 2 ? ", and " : " and ");
    if (!authors.empty()) o.t(endDot(authors) + " ");
    o.t(year + ". ");
    if (book) { o.i(endDot(e.title)); o.t(" "); if (!e.publisher.empty()) o.t((e.place.empty() ? "" : e.place + ": ") + endDot(e.publisher) + " "); }
    else {
      o.t("\xE2\x80\x9C" + endDot(e.title) + "\xE2\x80\x9D ");
      if (!e.container.empty()) { o.i(e.container); o.t(" "); }
      if (!e.volume.empty()) o.t(e.volume);
      if (!e.issue.empty()) o.t(" (" + e.issue + ")");
      if (!pages.empty()) o.t(": " + pages);
      if (!e.container.empty() || !e.volume.empty() || !pages.empty()) o.t(". ");
    }
    if (!url.empty()) { o.link(url, url); o.t("."); }
    return o.spans;
  }
  if (id == "ieee") {
    // [1] J. A. Smith, K. Lee, and M. Wang, "Title," Journal, vol. 12, no. 3, pp. 1–10, 2020, doi: …
    o.t("[" + std::to_string(number) + "] ");
    vector<string> ns;
    for (size_t i = 0; i < names.size() && i < 6; i++) ns.push_back(nameInitialsLast(names[i]));
    string authors = names.size() > 6 ? nameInitialsLast(names[0]) + " et al." : joinNames(ns, ", ", ns.size() > 2 ? ", and " : " and ");
    if (!authors.empty()) o.t(authors + ", ");
    if (book) { o.i(e.title); o.t(". "); if (!e.publisher.empty()) o.t((e.place.empty() ? "" : e.place + ": ") + e.publisher + ", "); o.t(year + "."); }
    else {
      o.t("\xE2\x80\x9C" + noEndDot(e.title) + ",\xE2\x80\x9D ");
      if (!e.container.empty()) { o.i(e.container); o.t(", "); }
      if (!e.volume.empty()) o.t("vol. " + e.volume + ", ");
      if (!e.issue.empty()) o.t("no. " + e.issue + ", ");
      if (!pages.empty()) o.t("pp. " + pages + ", ");
      o.t(year);
      if (!e.doi.empty()) { o.t(", doi: "); o.link(e.doi, url); }
      o.t(".");
    }
    return o.spans;
  }
  if (id == "vancouver") {
    // 1. Smith JA, Lee K, Wang M. Title. Journal. 2020;12(3):1-10. doi:…
    o.t(std::to_string(number) + ". ");
    vector<string> ns;
    for (size_t i = 0; i < names.size() && i < 6; i++) ns.push_back(nameVancouver(names[i]));
    string authors = join(ns, ", ") + (names.size() > 6 ? ", et al" : "");
    if (!authors.empty()) o.t(endDot(authors) + " ");
    o.t(endDot(e.title) + " ");
    if (book) { if (!e.publisher.empty()) o.t((e.place.empty() ? "" : e.place + ": ") + e.publisher + "; "); o.t(year + "."); }
    else {
      if (!e.container.empty()) o.t(endDot(e.container) + " ");
      o.t(year);
      if (!e.volume.empty()) o.t(";" + e.volume);
      if (!e.issue.empty()) o.t("(" + e.issue + ")");
      if (!e.pages.empty()) o.t(":" + e.pages);
      o.t(". ");
      if (!e.doi.empty()) { o.t("doi:"); o.link(e.doi, url); }
    }
    return o.spans;
  }
  // nature: 1. Smith, J. A., Lee, K. & Wang, M. Title. Journal 12, 1–10 (2020).
  o.t(std::to_string(number) + ". ");
  vector<string> ns;
  for (size_t i = 0; i < names.size() && i < 5; i++) ns.push_back(nameLastInitials(names[i]));
  string authors = names.size() > 5 ? nameLastInitials(names[0]) + " et al." : joinNames(ns, ", ", " & ");
  if (!authors.empty()) o.t(authors + " ");
  if (book) { o.i(e.title); o.t(". "); if (!e.publisher.empty()) o.t("(" + e.publisher + ", " + year + ")."); else o.t("(" + year + ")."); }
  else {
    o.t(endDot(e.title) + " ");
    if (!e.container.empty()) { o.i(e.container); o.t(" "); }
    if (!e.volume.empty()) { o.b(e.volume); o.t(pages.empty() ? " " : ", "); }
    if (!pages.empty()) o.t(pages + " ");
    o.t("(" + year + ").");
    if (!e.doi.empty()) { o.t(" "); o.link(url, url); }
  }
  return o.spans;
}

// ---------------------------------------------------------------- in-text citation
string formatCitation(const vector<const RefEntry*>& refs, const vector<int>& numbers, const string& style, uint16_t& fmt) {
  const CiteStyleInfo& st = citeStyleInfo(style);
  string id = st.id;
  fmt = 0;
  if (refs.empty()) return "";
  if (st.numeric) {
    // sorted, ranges of three or more collapsed: 1–3, 5
    vector<int> ns = numbers;
    std::sort(ns.begin(), ns.end());
    ns.erase(std::unique(ns.begin(), ns.end()), ns.end());
    string o;
    for (size_t i = 0; i < ns.size();) {
      size_t j = i;
      while (j + 1 < ns.size() && ns[j + 1] == ns[j] + 1) j++;
      if (!o.empty()) o += id == "ieee" ? ", " : ",";
      if (j - i >= 2) o += std::to_string(ns[i]) + "\xE2\x80\x93" + std::to_string(ns[j]);
      else { o += std::to_string(ns[i]); if (j > i) o += (id == "ieee" ? ", " : ",") + std::to_string(ns[j]); }
      i = j + 1;
    }
    if (id == "ieee") return "[" + o + "]";
    if (id == "vancouver") return "(" + o + ")";
    fmt = F_SUP;
    return o;
  }
  vector<string> parts;
  for (auto* e : refs) {
    string last, year;
    int n = 0;
    if (!e->structured()) rawAuthorYear(*e, last, year, n);
    else {
      n = int(e->authors.size());
      year = e->year.empty() ? "n.d." : e->year;
      if (n == 0) last = e->title.size() > 30 ? e->title.substr(0, 30) + "\xE2\x80\xA6" : e->title;
    }
    auto L = [&](size_t i) { return e->structured() && i < e->authors.size() ? splitAuthor(e->authors[i]).last : last; };
    string who;
    if (n <= 1) who = L(0);
    else if (n == 2) who = L(0) + (id == "apa" ? " & " : " and ") + L(1);
    else if (n == 3 && id == "chicago") who = L(0) + ", " + L(1) + ", and " + L(2);
    else who = L(0) + " et al.";
    if (id == "mla") parts.push_back(who);
    else if (id == "chicago") parts.push_back(who + " " + year);
    else parts.push_back(who + ", " + year);
  }
  return "(" + join(parts, "; ") + ")";
}

// ---------------------------------------------------------------- the document's bibliography
const RefEntry* Document::findRef(const string& key) const {
  for (auto& r : refs) if (r.key == key) return &r;
  return nullptr;
}

int docRefIndexOf(const Document& d, const string& key) {
  for (size_t i = 0; i < d.refs.size(); i++) if (d.refs[i].key == key) return int(i);
  return -1;
}

vector<string> Document::citedKeys() const {
  vector<string> out;
  std::set<string> seen;
  auto scan = [&](const Para& p) {
    for (auto& s : p.spans) {
      if (s.cite.empty()) continue;
      for (auto& k : split(s.cite, ';')) { string t = trim(k); if (!t.empty() && seen.insert(t).second) out.push_back(t); }
    }
  };
  for (auto& b : blocks) {
    if (b.kind == Block::Paragraph) scan(b.p);
    else if (b.kind == Block::FigureBlock) scan(b.fig.caption);
    else if (b.kind == Block::TableBlock) for (auto& r : b.tbl.cells) for (auto& c : r) scan(c);
  }
  return out;
}

namespace {
int docRefIndexByRecordId(const Document& d, const string& id) {
  if (id.empty()) return -1;
  for (size_t i = 0; i < d.refs.size(); i++) if (d.refs[i].recordId == id) return int(i);
  return -1;
}

void rewriteRefKey(Document& d, const string& from, const string& to) {
  auto fix = [&](Para& p) {
    if (p.refKey == from) p.refKey = to;
    for (Span& s : p.spans) {
      if (s.cite.empty()) continue;
      vector<string> keys = split(s.cite, ';');
      bool changed = false;
      for (string& key : keys) if (trim(key) == from) { key = to; changed = true; }
      if (changed) s.cite = join(keys, ";");
    }
  };
  for (Block& b : d.blocks) {
    if (b.kind == Block::Paragraph) fix(b.p);
    else if (b.kind == Block::FigureBlock) fix(b.fig.caption);
    else if (b.kind == Block::TableBlock) for (auto& row : b.tbl.cells) for (Para& p : row) fix(p);
  }
}

void replaceDocReference(Document& d, int at, RefEntry e) {
  if (at < 0 || size_t(at) >= d.refs.size()) return;
  const RefEntry old = d.refs[size_t(at)];
  if (e.recordId.empty()) e.recordId = old.recordId;
  if (e.rec < 0) e.rec = old.rec;
  string base = e.key.empty() ? old.key : e.key;
  int collision = docRefIndexOf(d, base);
  if (collision >= 0 && collision != at && d.refs[size_t(collision)].recordId != e.recordId) {
    string suffix = e.recordId.empty() ? sha256Hex(base + "|" + e.title + "|" + e.year) : e.recordId;
    base += "#" + suffix.substr(0, std::min<size_t>(12, suffix.size()));
    while ((collision = docRefIndexOf(d, base)) >= 0 && collision != at) base += "-2";
  }
  e.key = base;
  d.refs[size_t(at)] = std::move(e);
  if (old.key != d.refs[size_t(at)].key) rewriteRefKey(d, old.key, d.refs[size_t(at)].key);
  docRefreshCitations(d);
}
}  // namespace

int docAddReference(Document& d, const RefEntry& e0) {
  RefEntry e = e0;
  if (e.key.empty()) e.key = e.structured() ? refKeyFor(e.doi, e.title, e.year) : "raw:" + sha256Hex(lower(collapseWs(trim(e.raw)))).substr(0, 16);
  int at = docRefIndexByRecordId(d, e.recordId);
  if (at < 0 && e.rec >= 0) for (size_t i = 0; i < d.refs.size(); i++) if (d.refs[i].rec == e.rec && d.refs[i].recordId.empty()) { at = int(i); break; }
  if (at < 0) {
    int byKey = docRefIndexOf(d, e.key);
    if (byKey >= 0) {
      const RefEntry& old = d.refs[size_t(byKey)];
      bool sameRecord = e.recordId.empty() || old.recordId.empty() || e.recordId == old.recordId || (e.rec >= 0 && old.rec == e.rec);
      if (sameRecord) at = byKey;
      else {
        string suffix = e.recordId.empty() ? sha256Hex(e.key + "|" + e.title + "|" + e.year) : e.recordId;
        e.key += "#" + suffix.substr(0, std::min<size_t>(12, suffix.size()));
        while (docRefIndexOf(d, e.key) >= 0) e.key += "-2";
      }
    }
  }
  if (at >= 0) {
    replaceDocReference(d, at, std::move(e));
    return at;
  }
  d.refs.push_back(std::move(e));
  return int(d.refs.size()) - 1;
}

void docUpdateRecordReference(Document& d, const Record& r, int recIndex) {
  RefEntry e = refFromRecord(r);
  e.rec = recIndex;
  int at = docRefIndexByRecordId(d, r.id);
  if (at < 0 && recIndex >= 0) for (size_t i = 0; i < d.refs.size(); i++) if (d.refs[i].rec == recIndex && d.refs[i].recordId.empty()) { at = int(i); break; }
  if (at >= 0) replaceDocReference(d, at, std::move(e));
}

void docMergeRecordLinks(Document& d, const Record& keep, const string& removedRecordId, int keepIndex, int removedIndex) {
  if (removedRecordId.empty() || keep.id.empty() || removedRecordId == keep.id) return;
  for (RefEntry& e : d.refs) {
    if (e.rec == removedIndex) e.rec = keepIndex;
    else if (e.rec > removedIndex) e.rec--;
  }
  int keepRef = docRefIndexByRecordId(d, keep.id);
  vector<int> discarded;
  for (size_t i = 0; i < d.refs.size(); i++) if (d.refs[i].recordId == removedRecordId) discarded.push_back(int(i));
  if (keepRef < 0 && !discarded.empty()) {
    int adopt = discarded.front();
    d.refs[size_t(adopt)].recordId = keep.id;
    d.refs[size_t(adopt)].rec = keepIndex;
    keepRef = adopt;
  }
  for (auto it = discarded.rbegin(); it != discarded.rend(); ++it) {
    int at = *it;
    if (keepRef == at) continue;
    if (at < 0 || size_t(at) >= d.refs.size() || d.refs[size_t(at)].recordId != removedRecordId) continue;
    string oldKey = d.refs[size_t(at)].key;
    if (keepRef >= 0 && size_t(keepRef) < d.refs.size()) rewriteRefKey(d, oldKey, d.refs[size_t(keepRef)].key);
    d.refs.erase(d.refs.begin() + at);
    if (keepRef > at) keepRef--;
  }
  docUpdateRecordReference(d, keep, keepIndex);
  docRefreshCitations(d);
}

namespace {
uint64_t textHash(const vector<Span>& sp) {
  uint64_t h = 1469598103934665603ull;
  for (auto& s : sp) for (unsigned char c : s.text) { h ^= c; h *= 1099511628211ull; }
  return h ? h : 1;
}
bool isRefsHeadingBlock(const Block& b) {
  if (b.kind != Block::Paragraph || !isHeading(b.p.style)) return false;
  string t = lower(trim(b.p.text()));
  return t == "references" || t == "bibliography" || t == "works cited" || t == "reference list";
}
}  // namespace

void docRefreshCitations(Document& d, DocPos* caret, DocPos* anchor) {
  const CiteStyleInfo& st = citeStyleInfo(d.citeStyle);
  // ---- order and numbers
  vector<string> cited = d.citedKeys();
  vector<int> order;  // indices into d.refs in list order
  {
    std::set<int> used;
    if (st.numeric) {
      for (auto& k : cited) { int i = docRefIndexOf(d, k); if (i >= 0 && used.insert(i).second) order.push_back(i); }
      for (size_t i = 0; i < d.refs.size(); i++) if (used.insert(int(i)).second) order.push_back(int(i));
    } else {
      for (size_t i = 0; i < d.refs.size(); i++) order.push_back(int(i));
      std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return refSortKey(d.refs[size_t(a)]) < refSortKey(d.refs[size_t(b)]); });
    }
  }
  std::map<string, int> numberOf;  // key -> 1-based number
  for (size_t i = 0; i < order.size(); i++) numberOf[d.refs[size_t(order[i])].key] = int(i) + 1;
  // ---- citation texts
  auto refresh = [&](Para& p) {
    bool changed = false;
    for (auto& s : p.spans) {
      if (s.cite.empty()) continue;
      vector<const RefEntry*> es;
      vector<int> ns;
      vector<string> keep;
      for (auto& k0 : split(s.cite, ';')) {
        string k = trim(k0);
        const RefEntry* e = d.findRef(k);
        if (!e) continue;
        es.push_back(e);
        ns.push_back(numberOf[k]);
        keep.push_back(k);
      }
      uint16_t f = 0;
      string text = es.empty() ? "[?]" : formatCitation(es, ns, d.citeStyle, f);
      uint16_t nf = uint16_t((s.fmt & ~uint16_t(F_SUP)) | f);
      if (s.text != text || s.fmt != nf) { s.text = text; s.fmt = nf; changed = true; }
      string joined = join(keep, ";");
      if (!keep.empty() && joined != s.cite) s.cite = joined;
    }
    if (changed) p.normalize();
  };
  for (auto& b : d.blocks) {
    if (b.kind == Block::Paragraph) refresh(b.p);
    else if (b.kind == Block::FigureBlock) refresh(b.fig.caption);
    else if (b.kind == Block::TableBlock) for (auto& r : b.tbl.cells) for (auto& c : r) refresh(c);
  }
  // ---- Reference paragraphs: the generated ones follow the list order; hand-written ones stay after them
  vector<Block>& bl = d.blocks;
  int firstRef = -1, heading = -1;
  std::map<string, Para> existing;
  vector<Block> manual;
  for (size_t k = 0; k < bl.size(); k++) {
    if (isRefsHeadingBlock(bl[k]) && heading < 0) heading = int(k);
    if (bl[k].kind == Block::Paragraph && bl[k].p.style == PStyle::Reference) {
      if (firstRef < 0) firstRef = int(k);
      if (!bl[k].p.refKey.empty() && docRefIndexOf(d, bl[k].p.refKey) >= 0) existing[bl[k].p.refKey] = bl[k].p;
      else if (!bl[k].p.empty() && bl[k].p.refKey.empty()) manual.push_back(bl[k]);
    }
  }
  if (order.empty() && manual.empty() && firstRef < 0) return;
  vector<Block> list;
  for (int i : order) {
    const RefEntry& e = d.refs[size_t(i)];
    vector<Span> gen = formatReference(e, d.citeStyle, numberOf[e.key]);
    uint64_t gh = textHash(gen);
    Block nb;
    nb.p.style = PStyle::Reference;
    nb.p.refKey = e.key;
    auto it = existing.find(e.key);
    if (it != existing.end()) {
      nb.p = it->second;
      bool untouched = nb.p.refGen == 0 || textHash(nb.p.spans) == nb.p.refGen;
      if (untouched) { nb.p.spans = gen; nb.p.refGen = gh; }
    } else { nb.p.spans = gen; nb.p.refGen = gh; }
    list.push_back(nb);
  }
  for (auto& m : manual) list.push_back(m);
  // fast path (every edit ends here while typing): the Reference paragraphs already sit together in list order -> update in place
  if (firstRef >= 0 && !list.empty()) {
    bool same = firstRef + int(list.size()) <= int(bl.size());
    for (size_t i = 0; same && i < list.size(); i++) {
      const Block& cur = bl[size_t(firstRef) + i];
      same = cur.kind == Block::Paragraph && cur.p.style == PStyle::Reference && cur.p.refKey == list[i].p.refKey && (!list[i].p.refKey.empty() || (!cur.p.empty()));
    }
    if (same) {
      for (size_t k = size_t(firstRef) + list.size(); same && k < bl.size(); k++) same = !(bl[k].kind == Block::Paragraph && bl[k].p.style == PStyle::Reference);
      for (int k = 0; same && k < firstRef; k++) same = !(bl[size_t(k)].kind == Block::Paragraph && bl[size_t(k)].p.style == PStyle::Reference);
    }
    if (same) {
      for (size_t i = 0; i < list.size(); i++) {
        Para& cur = bl[size_t(firstRef) + i].p;
        if (cur.refKey.empty()) continue;
        if (cur.refGen != list[i].p.refGen || cur.spans.size() != list[i].p.spans.size() || cur.text() != list[i].p.text()) {
          bool untouched = cur.refGen == 0 || textHash(cur.spans) == cur.refGen;
          if (untouched) { cur.spans = list[i].p.spans; cur.refGen = list[i].p.refGen; }
        }
      }
      return;
    }
  }
  // remove every Reference paragraph, then insert the list where the first one was (or after the heading, or at the end)
  size_t before = bl.size();
  auto isRefBlk = [&](const Block& b) { return b.kind == Block::Paragraph && b.p.style == PStyle::Reference; };
  bool caretWasRef = caret && caret->blk >= 0 && caret->blk < int(before) && isRefBlk(bl[size_t(caret->blk)]);
  bool anchorWasRef = anchor && anchor->blk >= 0 && anchor->blk < int(before) && isRefBlk(bl[size_t(anchor->blk)]);
  int removedBeforeCaret = 0, removedBeforeAnchor = 0;
  vector<Block> kept;
  kept.reserve(bl.size());
  int insertAt = -1;
  for (size_t k = 0; k < bl.size(); k++) {
    bool isRef = bl[k].kind == Block::Paragraph && bl[k].p.style == PStyle::Reference;
    if (isRef) {
      if (insertAt < 0) insertAt = int(kept.size());
      if (caret && int(k) < caret->blk) removedBeforeCaret++;
      if (anchor && int(k) < anchor->blk) removedBeforeAnchor++;
      continue;
    }
    kept.push_back(std::move(bl[k]));
  }
  if (insertAt < 0) {
    if (heading >= 0) {
      int h = 0;
      for (size_t k = 0; k < before && int(k) <= heading; k++) if (!(bl[k].kind == Block::Paragraph && bl[k].p.style == PStyle::Reference)) h++;
      insertAt = h;  // right after the heading
    } else if (!list.empty()) {
      if (!kept.empty() && kept.back().kind == Block::Paragraph && kept.back().p.empty() && kept.back().p.style == PStyle::Body) kept.pop_back();
      kept.push_back(Block::para("References", PStyle::H1));
      insertAt = int(kept.size());
    } else insertAt = int(kept.size());
  }
  bl = std::move(kept);
  bl.insert(bl.begin() + insertAt, list.begin(), list.end());
  auto fix = [&](DocPos* p, int removedBefore, bool wasRef) {
    if (!p) return;
    int b;
    if (wasRef) { b = list.empty() ? std::max(0, insertAt - 1) : insertAt; p->off = 0; p->cell = -1; }
    else { b = p->blk - removedBefore; if (b >= insertAt) b += int(list.size()); }
    p->blk = clampv(b, 0, std::max(0, int(bl.size()) - 1));
  };
  fix(caret, removedBeforeCaret, caretWasRef);
  fix(anchor, removedBeforeAnchor, anchorWasRef);
  if (bl.empty()) bl.push_back(Block::para(""));
}

}  // namespace vs
