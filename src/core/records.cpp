#include "records.h"

#include "world.h"

#include <cstdio>
#include <sstream>

namespace vs {

const char* formatLabel(BibFormat f) {
  switch (f) {
    case BibFormat::WoS: return "Web of Science";
    case BibFormat::WoSTab: return "Web of Science (tab-delimited)";
    case BibFormat::Scopus: return "Scopus";
    case BibFormat::RIS: return "RIS";
    case BibFormat::BibTeX: return "BibTeX";
    case BibFormat::OpenAlex: return "OpenAlex";
    case BibFormat::VOSviewer: return "VOSviewer";
    case BibFormat::Bundle: return "VOSStudio bundle";
    default: return "Unknown";
  }
}

// ------------------------------------------------------------ countries
namespace {
struct CountryInfo { const char* name; double lat, lon; };
const CountryInfo COUNTRIES[] = {
    {"United States", 39.8, -98.6}, {"Canada", 56.1, -106.3}, {"Mexico", 23.6, -102.5}, {"Brazil", -14.2, -51.9}, {"Argentina", -38.4, -63.6},
    {"Chile", -35.7, -71.5}, {"Colombia", 4.6, -74.3}, {"Peru", -9.2, -75.0}, {"Ecuador", -1.8, -78.2}, {"Venezuela", 6.4, -66.6}, {"Uruguay", -32.5, -55.8},
    {"United Kingdom", 54.0, -2.5}, {"Ireland", 53.4, -8.2}, {"France", 46.2, 2.2}, {"Germany", 51.2, 10.4}, {"Netherlands", 52.1, 5.3}, {"Belgium", 50.5, 4.5},
    {"Switzerland", 46.8, 8.2}, {"Austria", 47.5, 14.6}, {"Italy", 41.9, 12.6}, {"Spain", 40.5, -3.7}, {"Portugal", 39.4, -8.2}, {"Denmark", 56.3, 9.5},
    {"Sweden", 60.1, 18.6}, {"Norway", 60.5, 8.5}, {"Finland", 61.9, 25.7}, {"Iceland", 64.9, -19.0}, {"Poland", 51.9, 19.1}, {"Czech Republic", 49.8, 15.5},
    {"Slovakia", 48.7, 19.7}, {"Hungary", 47.2, 19.5}, {"Romania", 45.9, 25.0}, {"Bulgaria", 42.7, 25.5}, {"Greece", 39.1, 21.8}, {"Turkey", 39.0, 35.2},
    {"Russia", 61.5, 105.3}, {"Ukraine", 48.4, 31.2}, {"Serbia", 44.0, 21.0}, {"Croatia", 45.1, 15.2}, {"Slovenia", 46.2, 14.9}, {"Estonia", 58.6, 25.0},
    {"Latvia", 56.9, 24.6}, {"Lithuania", 55.2, 23.9}, {"Cyprus", 35.1, 33.4}, {"Israel", 31.0, 34.9}, {"Iran", 32.4, 53.7}, {"Iraq", 33.2, 43.7},
    {"Saudi Arabia", 23.9, 45.1}, {"United Arab Emirates", 23.4, 53.8}, {"Qatar", 25.4, 51.2}, {"Jordan", 30.6, 36.2}, {"Lebanon", 33.9, 35.9},
    {"Egypt", 26.8, 30.8}, {"Morocco", 31.8, -7.1}, {"Tunisia", 33.9, 9.5}, {"Algeria", 28.0, 1.7}, {"Nigeria", 9.1, 8.7}, {"Ghana", 7.9, -1.0},
    {"Kenya", -0.0, 37.9}, {"Ethiopia", 9.1, 40.5}, {"South Africa", -30.6, 22.9}, {"Tanzania", -6.4, 34.9}, {"Uganda", 1.4, 32.3},
    {"India", 20.6, 78.9}, {"Pakistan", 30.4, 69.3}, {"Bangladesh", 23.7, 90.4}, {"Sri Lanka", 7.9, 80.8}, {"Nepal", 28.4, 84.1}, {"China", 35.9, 104.2},
    {"Japan", 36.2, 138.3}, {"South Korea", 35.9, 127.8}, {"Taiwan", 23.7, 121.0}, {"Hong Kong", 22.3, 114.2}, {"Singapore", 1.35, 103.8},
    {"Malaysia", 4.2, 101.9}, {"Indonesia", -0.8, 113.9}, {"Thailand", 15.9, 101.0}, {"Vietnam", 14.1, 108.3}, {"Philippines", 12.9, 121.8},
    {"Australia", -25.3, 133.8}, {"New Zealand", -40.9, 174.9}, {"Kazakhstan", 48.0, 66.9}, {"Luxembourg", 49.8, 6.1}, {"Malta", 35.9, 14.4}};
}  // namespace

string normCountry(const string& c0) {
  {
    string k = canonicalCountry(bibClean(c0));
    if (!k.empty()) return k;
  }
  string c = bibClean(c0);
  // WoS: "CA 94305 USA", "Peoples R China", "England"
  string lc = lower(c);
  auto has = [&](const char* s) { return lc.find(s) != string::npos; };
  if (lc == "usa" || endsWith(lc, " usa") || lc == "u.s.a" || lc == "united states of america" || lc == "us" || lc == "united states") return "United States";
  if (has("peoples r china") || lc == "pr china" || lc == "p.r. china" || lc == "people's republic of china" || lc == "china") return "China";
  if (lc == "england" || lc == "scotland" || lc == "wales" || lc == "north ireland" || lc == "northern ireland" || lc == "uk" || lc == "u.k" || lc == "great britain") return "United Kingdom";
  if (lc == "south korea" || lc == "korea" || lc == "republic of korea" || lc == "korea, republic of") return "South Korea";
  if (lc == "russian federation" || lc == "russia") return "Russia";
  if (lc == "viet nam") return "Vietnam";
  if (lc == "turkiye") return "Turkey";
  if (lc == "czechia") return "Czech Republic";
  if (lc == "iran, islamic republic of") return "Iran";
  if (lc == "u arab emirates" || lc == "uae") return "United Arab Emirates";
  if (lc == "the netherlands") return "Netherlands";
  // Title-case otherwise, but keep known names exactly
  for (auto& ci : COUNTRIES) if (lower(ci.name) == lc) return ci.name;
  return titleCase(lc);
}
vector<std::pair<string, string>> countryCoords() {
  vector<std::pair<string, string>> v;
  for (auto& c : COUNTRIES) v.emplace_back(c.name, fmtFixed(c.lat, 2) + "," + fmtFixed(c.lon, 2));
  return v;
}
bool countryLatLon(const string& c, double& lat, double& lon) {
  if (countryLonLat(c, lon, lat)) return true;
  string n = normCountry(c);
  for (auto& ci : COUNTRIES) if (n == ci.name) { lat = ci.lat; lon = ci.lon; return true; }
  return false;
}

// ------------------------------------------------------------ helpers
static const char* ORG_WORDS[] = {"univ", "inst", "coll", "school", "acad", "ctr", "center", "centre", "hosp", "lab", "corp", "inc", "ltd", "fdn", "polytech", "minist", "council", "agcy", "agency", "res "};
static string pickOrg(const vector<string>& parts) {
  for (auto& p : parts) {
    string l = lower(p);
    for (auto w : ORG_WORDS) if (l.find(w) != string::npos) return bibClean(p);
  }
  return parts.empty() ? "" : bibClean(parts[0]);
}
static void addUnique(vector<string>& v, const string& s) {
  if (s.empty()) return;
  for (auto& x : v) if (x == s) return;
  v.push_back(s);
}
static string firstNumber(const string& s) {
  string r;
  for (char c : s) {
    if (c >= '0' && c <= '9') r += c;
    else if (!r.empty()) break;
  }
  return r;
}

// WoS C1: "[Smith, J; Doe, A] Stanford Univ, Dept X, Stanford, CA 94305 USA."
static void wosAffiliation(const string& line, Record& r) {
  string s = trim(line);
  if (!s.empty() && s[0] == '[') {
    size_t e = s.find(']');
    if (e != string::npos) s = trim(s.substr(e + 1));
  }
  auto parts = splitAny(s, ",");
  if (parts.empty()) return;
  addUnique(r.affiliations, pickOrg(parts));
  string c = parts.back();
  addUnique(r.countries, normCountry(c));
}

// ------------------------------------------------------------ WoS plain text
static int parseWoS(const string& text, vector<Record>& out) {
  std::istringstream in(text);
  string line, tag;
  Record r;
  bool inRec = false;
  int n = 0;
  vector<string> cur;
  auto flush = [&]() {
    if (tag.empty()) return;
    string joined;
    if (tag == "AU" || tag == "AF" || tag == "CR" || tag == "C1") {
      // multi-line list
      for (auto& v : cur) {
        if (tag == "AU") r.authors.push_back(bibClean(v));
        else if (tag == "CR") r.refs.push_back(trim(v));
        else if (tag == "C1") wosAffiliation(v, r);
      }
    } else {
      for (auto& v : cur) joined += (joined.empty() ? "" : " ") + trim(v);
      if (tag == "TI") r.title = joined;
      else if (tag == "SO") r.source = joined;
      else if (tag == "AB") r.abstract_ = joined;
      else if (tag == "PY") r.year = toInt(joined);
      else if (tag == "DI") r.doi = normDoi(joined);
      else if (tag == "DE") for (auto& k : splitAny(joined, ";")) r.keywords.push_back(bibClean(k));
      else if (tag == "ID") for (auto& k : splitAny(joined, ";")) r.indexTerms.push_back(bibClean(k));
      else if (tag == "TC") r.cites = toInt(joined);
      else if (tag == "Z9" && r.cites == 0) r.cites = toInt(joined);
      else if (tag == "VL") r.volume = joined;
      else if (tag == "BP") r.pages = joined;
      else if (tag == "DT") r.docType = joined;
      else if (tag == "LA") r.language = joined;
      else if (tag == "PU") r.publisher = joined;
      else if (tag == "UT") r.key = joined;
    }
    cur.clear();
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 2 && line[0] != ' ' && (line.size() == 2 || line[2] == ' ')) {
      string t = line.substr(0, 2);
      string v = line.size() > 3 ? line.substr(3) : "";
      if (t == "ER") {
        flush();
        tag.clear();
        if (inRec) {
          if (!r.title.empty() || !r.authors.empty()) { out.push_back(r); n++; }
        }
        r = Record();
        inRec = false;
        continue;
      }
      if (t == "PT") { r = Record(); inRec = true; tag.clear(); cur.clear(); r.docType = v; continue; }
      if (t == "FN" || t == "VR" || t == "EF") continue;
      flush();
      tag = t;
      cur.push_back(v);
      inRec = true;
    } else if (startsWith(line, "   ") && !tag.empty()) {
      cur.push_back(line.substr(3));
    }
  }
  return n;
}

// ------------------------------------------------------------ CSV
vector<vector<string>> parseCsv(const string& text, char sep) {
  vector<vector<string>> rows;
  vector<string> row;
  string cell;
  bool q = false;
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (q) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; i++; }
        else q = false;
      } else cell += c;
    } else {
      if (c == '"') q = true;
      else if (c == sep) { row.push_back(cell); cell.clear(); }
      else if (c == '\n' || c == '\r') {
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
        row.push_back(cell);
        cell.clear();
        if (!(row.size() == 1 && row[0].empty())) rows.push_back(row);
        row.clear();
      } else cell += c;
    }
  }
  if (!cell.empty() || !row.empty()) { row.push_back(cell); rows.push_back(row); }
  return rows;
}

// ------------------------------------------------------------ WoS tab-delimited
static int parseWoSTab(const string& text, vector<Record>& out) {
  auto rows = parseCsv(text, '\t');
  if (rows.empty()) return 0;
  std::map<string, int> col;
  for (size_t i = 0; i < rows[0].size(); i++) col[trim(rows[0][i])] = int(i);
  auto g = [&](const vector<string>& row, const char* k) -> string {
    auto it = col.find(k);
    return it != col.end() && it->second < int(row.size()) ? trim(row[it->second]) : "";
  };
  int n = 0;
  for (size_t r = 1; r < rows.size(); r++) {
    auto& row = rows[r];
    Record rec;
    for (auto& a : splitAny(g(row, "AU"), ";")) rec.authors.push_back(bibClean(a));
    rec.title = g(row, "TI");
    rec.source = g(row, "SO");
    rec.abstract_ = g(row, "AB");
    rec.year = toInt(g(row, "PY"));
    rec.doi = normDoi(g(row, "DI"));
    rec.cites = toInt(g(row, "TC"));
    rec.volume = g(row, "VL");
    rec.pages = g(row, "BP");
    rec.docType = g(row, "DT");
    rec.language = g(row, "LA");
    for (auto& k : splitAny(g(row, "DE"), ";")) rec.keywords.push_back(bibClean(k));
    for (auto& k : splitAny(g(row, "ID"), ";")) rec.indexTerms.push_back(bibClean(k));
    for (auto& c : splitAny(g(row, "CR"), ";")) rec.refs.push_back(c);
    // C1 in tab files: "[A; B] Org, ..., Country; [C] Org2, ..., Country2"
    string c1 = g(row, "C1");
    string curAff;
    int depth = 0;
    for (char ch : c1) {
      if (ch == '[') depth++;
      if (ch == ']') depth--;
      if (ch == ';' && depth == 0) { wosAffiliation(curAff, rec); curAff.clear(); }
      else curAff += ch;
    }
    if (!trim(curAff).empty()) wosAffiliation(curAff, rec);
    if (!rec.title.empty() || !rec.authors.empty()) { out.push_back(rec); n++; }
  }
  return n;
}

// ------------------------------------------------------------ Scopus CSV
static int parseScopus(const string& text, vector<Record>& out) {
  auto rows = parseCsv(text, ',');
  if (rows.empty()) return 0;
  std::map<string, int> col;
  for (size_t i = 0; i < rows[0].size(); i++) col[lower(trim(rows[0][i]))] = int(i);
  auto ci = [&](std::initializer_list<const char*> names) {
    for (auto nm : names) { auto it = col.find(nm); if (it != col.end()) return it->second; }
    return -1;
  };
  int iAu = ci({"authors"}), iTi = ci({"title"}), iYr = ci({"year"}), iSo = ci({"source title"}), iVol = ci({"volume"}), iPs = ci({"page start"}),
      iCb = ci({"cited by"}), iDoi = ci({"doi"}), iAb = ci({"abstract"}), iKw = ci({"author keywords"}), iId = ci({"index keywords"}),
      iAf = ci({"affiliations"}), iRef = ci({"references"}), iDt = ci({"document type"}), iPub = ci({"publisher"}), iLa = ci({"language of original document"}),
      iEid = ci({"eid"});
  auto g = [&](const vector<string>& row, int i) { return i >= 0 && i < int(row.size()) ? trim(row[i]) : string(); };
  int n = 0;
  for (size_t r = 1; r < rows.size(); r++) {
    auto& row = rows[r];
    Record rec;
    string au = g(row, iAu);
    if (au.find(';') != string::npos) for (auto& a : splitAny(au, ";")) rec.authors.push_back(bibClean(a));
    else for (auto& a : splitAny(au, ",")) rec.authors.push_back(bibClean(a));
    if (rec.authors.size() == 1 && lower(rec.authors[0]) == "[no author name available]") rec.authors.clear();
    rec.title = g(row, iTi);
    rec.year = toInt(g(row, iYr));
    rec.source = g(row, iSo);
    rec.volume = g(row, iVol);
    rec.pages = g(row, iPs);
    rec.cites = toInt(g(row, iCb));
    rec.doi = normDoi(g(row, iDoi));
    rec.abstract_ = g(row, iAb);
    if (startsWith(rec.abstract_, "[No abstract available]")) rec.abstract_.clear();
    for (auto& k : splitAny(g(row, iKw), ";")) rec.keywords.push_back(bibClean(k));
    for (auto& k : splitAny(g(row, iId), ";")) rec.indexTerms.push_back(bibClean(k));
    for (auto& a : splitAny(g(row, iAf), ";")) {
      auto parts = splitAny(a, ",");
      if (parts.empty()) continue;
      addUnique(rec.affiliations, pickOrg(parts));
      addUnique(rec.countries, normCountry(parts.back()));
    }
    for (auto& ref : splitAny(g(row, iRef), ";")) rec.refs.push_back(ref);
    rec.docType = g(row, iDt);
    rec.publisher = g(row, iPub);
    rec.language = g(row, iLa);
    rec.key = g(row, iEid);
    if (!rec.title.empty()) { out.push_back(rec); n++; }
  }
  return n;
}

// ------------------------------------------------------------ RIS
static int parseRIS(const string& text, vector<Record>& out) {
  std::istringstream in(text);
  string line;
  Record r;
  bool inRec = false;
  int n = 0;
  string sp, ep;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 5 || line[2] != ' ' || line[3] != ' ' || line[4] != '-') continue;
    string t = line.substr(0, 2), v = trim(line.size() > 6 ? line.substr(6) : "");
    if (t == "TY") { r = Record(); inRec = true; r.docType = v; sp.clear(); ep.clear(); continue; }
    if (!inRec) continue;
    if (t == "ER") {
      r.pages = sp;
      if (!r.title.empty()) { out.push_back(r); n++; }
      inRec = false;
      continue;
    }
    if (t == "AU" || t == "A1" || (t == "A2" && r.authors.empty())) r.authors.push_back(bibClean(v));
    else if (t == "TI" || t == "T1") r.title = r.title.empty() ? v : r.title;
    else if (t == "PY" || t == "Y1" || t == "DA") { if (!r.year) r.year = toInt(firstNumber(v)); }
    else if (t == "JO" || t == "JF" || t == "T2" || t == "JA") { if (r.source.empty()) r.source = v; }
    else if (t == "VL") r.volume = v;
    else if (t == "SP") sp = v;
    else if (t == "EP") ep = v;
    else if (t == "DO") r.doi = normDoi(v);
    else if (t == "N1" && startsWith(lower(v), "times cited:")) r.cites = toInt(trim(v.substr(12)));
    else if (t == "AB" || t == "N2") { if (r.abstract_.empty()) r.abstract_ = v; }
    else if (t == "KW") { for (auto& k : splitAny(v, ";")) r.keywords.push_back(bibClean(k)); }
    else if (t == "AD") {
      auto parts = splitAny(v, ",");
      if (!parts.empty()) { addUnique(r.affiliations, pickOrg(parts)); addUnique(r.countries, normCountry(parts.back())); }
    } else if (t == "PB") r.publisher = v;
    else if (t == "LA") r.language = v;
    else if (t == "N1" && icontains(v, "cited by")) {
      string num = firstNumber(v.substr(lower(v).find("cited by")));
      if (!num.empty()) r.cites = toInt(num);
    } else if (t == "CR") r.refs.push_back(v);
  }
  return n;
}

// ------------------------------------------------------------ BibTeX
static string latexClean(const string& s) {
  string r;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '{' || c == '}') continue;
    if (c == '\\') {
      // \"o, \'e, \&, \textit{...}
      if (i + 1 < s.size() && strchr("\"'`^~=.", s[i + 1])) { i++; continue; }
      if (i + 1 < s.size() && strchr("&%$#_", s[i + 1])) { r += s[++i]; continue; }
      while (i + 1 < s.size() && isalpha((unsigned char)s[i + 1])) i++;
      continue;
    }
    r += c;
  }
  return collapseWs(r);
}
static int parseBibTeX(const string& t, vector<Record>& out) {
  int n = 0;
  size_t i = 0;
  while ((i = t.find('@', i)) != string::npos) {
    size_t br = t.find_first_of("{(", i);
    if (br == string::npos) break;
    string type = lower(trim(t.substr(i + 1, br - i - 1)));
    if (type == "comment" || type == "string" || type == "preamble") { i = br + 1; continue; }
    size_t k = t.find(',', br);
    if (k == string::npos) break;
    Record r;
    r.docType = type;
    r.key = trim(t.substr(br + 1, k - br - 1));
    size_t p = k + 1;
    int depth = 1;
    std::map<string, string> f;
    while (p < t.size() && depth > 0) {
      while (p < t.size() && (isspace((unsigned char)t[p]) || t[p] == ',')) p++;
      if (p >= t.size() || t[p] == '}' || t[p] == ')') { p++; break; }
      size_t eq = t.find('=', p);
      if (eq == string::npos) break;
      string name = lower(trim(t.substr(p, eq - p)));
      p = eq + 1;
      while (p < t.size() && isspace((unsigned char)t[p])) p++;
      string val;
      if (p < t.size() && t[p] == '{') {
        int d = 1;
        size_t s0 = ++p;
        while (p < t.size() && d > 0) { if (t[p] == '{') d++; else if (t[p] == '}') d--; p++; }
        val = t.substr(s0, p - s0 - 1);
      } else if (p < t.size() && t[p] == '"') {
        size_t s0 = ++p;
        int d = 0;
        while (p < t.size() && (t[p] != '"' || d > 0)) { if (t[p] == '{') d++; else if (t[p] == '}') d--; p++; }
        val = t.substr(s0, p - s0);
        p++;
      } else {
        size_t s0 = p;
        while (p < t.size() && t[p] != ',' && t[p] != '}') p++;
        val = t.substr(s0, p - s0);
      }
      f[name] = val;
    }
    i = p;
    auto g = [&](const char* nm) { auto it = f.find(nm); return it == f.end() ? string() : latexClean(it->second); };
    r.title = g("title");
    string au = g("author");
    string auRaw = f.count("author") ? f["author"] : "";
    for (auto& a : split(replaceAll(latexClean(auRaw), " and ", "\x01"), '\x01')) if (!trim(a).empty()) r.authors.push_back(bibClean(a));
    (void)au;
    r.year = toInt(firstNumber(g("year")));
    r.source = g("journal");
    if (r.source.empty()) r.source = g("booktitle");
    r.volume = g("volume");
    r.pages = firstNumber(g("pages"));
    r.doi = normDoi(g("doi"));
    r.abstract_ = g("abstract");
    string kw = g("author_keywords");
    if (kw.empty()) kw = g("keywords");
    for (auto& k : splitAny(kw, kw.find(';') != string::npos ? ";" : ",")) r.keywords.push_back(bibClean(k));
    for (auto& k : splitAny(g("keywords-plus"), ";")) r.indexTerms.push_back(bibClean(k));
    string aff = g("affiliations");
    if (aff.empty()) aff = g("affiliation");
    for (auto& a : splitAny(aff, ";")) {
      auto parts = splitAny(a, ",");
      if (!parts.empty()) { addUnique(r.affiliations, pickOrg(parts)); addUnique(r.countries, normCountry(parts.back())); }
    }
    string refs = f.count("references") ? f["references"] : (f.count("cited-references") ? f["cited-references"] : "");
    if (!refs.empty()) for (auto& rf : splitAny(latexClean(refs), refs.find('\n') != string::npos && refs.find(';') == string::npos ? "\n" : ";")) r.refs.push_back(rf);
    string note = g("note");
    if (icontains(note, "cited by")) r.cites = toInt(firstNumber(note.substr(lower(note).find("cited by"))));
    string tc = g("times-cited");
    if (!tc.empty()) r.cites = toInt(tc);
    r.publisher = g("publisher");
    r.language = g("language");
    if (!r.title.empty()) { out.push_back(r); n++; }
  }
  return n;
}

// ------------------------------------------------------------ OpenAlex
int parseOpenAlex(const Json& j, vector<Record>& out) {
  const Json& res = j.has("results") ? j["results"] : j;
  int n = 0;
  for (auto& w : res.a) {
    Record r;
    r.title = w["title"].str(w["display_name"].str());
    r.year = w["publication_year"].integer();
    r.doi = normDoi(w["doi"].str());
    r.cites = w["cited_by_count"].integer();
    r.key = w["id"].str();
    r.docType = w["type"].str();
    r.language = w["language"].str();
    const Json& loc = w["primary_location"];
    r.source = loc["source"]["display_name"].str();
    r.volume = w["biblio"]["volume"].str();
    r.pages = w["biblio"]["first_page"].str();
    for (auto& a : w["authorships"].a) {
      r.authors.push_back(a["author"]["display_name"].str());
      for (auto& inst : a["institutions"].a) {
        addUnique(r.affiliations, inst["display_name"].str());
        string cc = inst["country_code"].str();
        if (!cc.empty()) {
          string nm = canonicalCountry(cc);
          addUnique(r.countries, nm.empty() ? cc : nm);
        }
      }
    }
    for (auto& k : w["keywords"].a) r.keywords.push_back(k["display_name"].str(k["keyword"].str()));
    for (auto& c : w["concepts"].a) if (c["level"].integer() >= 1 && c["score"].num() > 0.3) r.indexTerms.push_back(lower(c["display_name"].str()));
    for (auto& ref : w["referenced_works"].a) r.refs.push_back("DOI openalex:" + ref.str());
    // abstract_inverted_index → text
    const Json& inv = w["abstract_inverted_index"];
    if (inv.t == Json::Obj) {
      vector<std::pair<int, string>> words;
      for (auto& kv : inv.o) for (auto& p : kv.second.a) words.emplace_back(p.integer(), kv.first);
      std::sort(words.begin(), words.end());
      string ab;
      for (auto& wd : words) { if (!ab.empty()) ab += ' '; ab += wd.second; }
      r.abstract_ = ab;
    }
    if (!r.title.empty()) { out.push_back(r); n++; }
  }
  return n;
}

// ------------------------------------------------------------ detection / dispatch
BibFormat detectFormat(const string& text, const string& name) {
  string head = text.substr(0, std::min<size_t>(text.size(), 4000));
  string ext = fileExt(name);
  string t = trim(head);
  if (!t.empty() && t[0] == '{') {
    if (head.find("\"vosstudio\"") != string::npos || head.find("\"sha256\"") != string::npos) return BibFormat::Bundle;
    if (head.find("\"results\"") != string::npos || head.find("openalex") != string::npos) return BibFormat::OpenAlex;
  }
  if (startsWith(t, "FN ") || (head.find("\nPT ") != string::npos && text.find("\nER") != string::npos) || startsWith(t, "PT ")) return BibFormat::WoS;
  if (startsWith(t, "PT\tAU") || startsWith(t, "PT\t")) return BibFormat::WoSTab;
  if (head.find("TY  - ") != string::npos) return BibFormat::RIS;
  string firstLine = t.substr(0, t.find('\n'));
  string lfl = lower(firstLine);
  if ((lfl.find("authors") != string::npos || lfl.find("author full names") != string::npos) && lfl.find("title") != string::npos &&
      (lfl.find("source title") != string::npos || lfl.find("eid") != string::npos || lfl.find("cited by") != string::npos))
    return BibFormat::Scopus;
  if (t.find('@') != string::npos && (ext == "bib" || head.find("@article") != string::npos || head.find("@ARTICLE") != string::npos || head.find("@inproceedings") != string::npos)) return BibFormat::BibTeX;
  if (lfl.find("id\tlabel") != string::npos || lfl.find("label\tx\ty") != string::npos || (lfl.find("label") != string::npos && lfl.find('\t') != string::npos)) return BibFormat::VOSviewer;
  {
    // VOSviewer network file: lines of "id id [strength]" (tab or space separated numbers only)
    int good = 0, bad = 0;
    size_t p = 0;
    while (p < t.size() && good + bad < 8) {
      size_t e = t.find('\n', p);
      string ln = trim(t.substr(p, e == string::npos ? string::npos : e - p));
      p = e == string::npos ? t.size() : e + 1;
      if (ln.empty()) continue;
      auto f = splitAny(ln, " \t");
      bool ok = f.size() == 2 || f.size() == 3;
      for (size_t k = 0; ok && k < f.size(); k++) { char* end = nullptr; std::strtod(f[k].c_str(), &end); ok = end && *end == 0 && !f[k].empty(); }
      (ok ? good : bad)++;
    }
    if (good >= 1 && bad == 0) return BibFormat::VOSviewer;
  }
  if (ext == "ris") return BibFormat::RIS;
  if (ext == "bib") return BibFormat::BibTeX;
  if (ext == "csv") return BibFormat::Scopus;
  return BibFormat::Unknown;
}

int parseRecords(const string& text, BibFormat fmt, vector<Record>& out, string* err) {
  switch (fmt) {
    case BibFormat::WoS: return parseWoS(text, out);
    case BibFormat::WoSTab: return parseWoSTab(text, out);
    case BibFormat::Scopus: return parseScopus(text, out);
    case BibFormat::RIS: return parseRIS(text, out);
    case BibFormat::BibTeX: return parseBibTeX(text, out);
    case BibFormat::OpenAlex: {
      string e;
      Json j = Json::parse(text, &e);
      if (!e.empty()) { if (err) *err = "JSON: " + e; return 0; }
      return parseOpenAlex(j, out);
    }
    default:
      if (err) *err = "unrecognised file format";
      return 0;
  }
}

// ------------------------------------------------------------ dedup
int deduplicate(vector<Record>& recs) {
  std::unordered_set<string> dois, titles;
  vector<Record> keep;
  keep.reserve(recs.size());
  int removed = 0;
  for (auto& r : recs) {
    string d = normDoi(r.doi);
    string tk;
    for (char c : lower(asciiFold(r.title))) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) tk += c;
    tk += "|" + std::to_string(r.year);
    bool dup = (!d.empty() && dois.count(d)) || (tk.size() > 12 && titles.count(tk));
    if (dup) { removed++; continue; }
    if (!d.empty()) dois.insert(d);
    if (tk.size() > 12) titles.insert(tk);
    keep.push_back(std::move(r));
  }
  recs.swap(keep);
  return removed;
}

// ------------------------------------------------------------ quality
QualityReport qualityReport(const Corpus& c) {
  QualityReport q;
  q.records = int(c.recs.size());
  if (!q.records) return q;
  std::unordered_set<string> so, au;
  int doi = 0, ab = 0, kw = 0, id = 0, ref = 0, aff = 0, yr = 0;
  q.yearMin = 9999;
  q.yearMax = 0;
  for (auto& r : c.recs) {
    if (!r.doi.empty()) doi++;
    if (!r.abstract_.empty()) ab++;
    if (!r.keywords.empty()) kw++;
    if (!r.indexTerms.empty()) id++;
    if (!r.refs.empty()) ref++;
    if (!r.affiliations.empty()) aff++;
    if (r.year) { yr++; q.yearMin = std::min(q.yearMin, r.year); q.yearMax = std::max(q.yearMax, r.year); }
    if (!r.source.empty()) so.insert(sourceKey(r.source));
    for (auto& a : r.authors) au.insert(bibKey(a));
    q.totalCites += r.cites;
    q.totalRefs += (long long)r.refs.size();
  }
  if (!yr) q.yearMin = q.yearMax = 0;
  double N = q.records;
  q.doiCov = doi / N; q.absCov = ab / N; q.kwCov = kw / N; q.idCov = id / N; q.refCov = ref / N; q.affCov = aff / N; q.yearCov = yr / N;
  q.sources = int(so.size());
  q.authors = int(au.size());
  q.citesPerDoc = q.totalCites / N;
  return q;
}

// ------------------------------------------------------------ network helpers
int Network::weightIndex(const string& name) const {
  for (size_t i = 0; i < weightNames.size(); i++) if (iequals(weightNames[i], name)) return int(i);
  return -1;
}
int Network::scoreIndex(const string& name) const {
  for (size_t i = 0; i < scoreNames.size(); i++) if (iequals(scoreNames[i], name)) return int(i);
  return -1;
}
void Network::recomputeLinkWeights() {
  int iL = weightIndex("Links"), iT = weightIndex("Total link strength");
  if (iL < 0) { weightNames.insert(weightNames.begin(), "Links"); for (auto& nd : nodes) nd.w.insert(nd.w.begin(), 0.0); iL = 0; iT = weightIndex("Total link strength"); }
  if (iT < 0) { weightNames.insert(weightNames.begin() + 1, "Total link strength"); for (auto& nd : nodes) nd.w.insert(nd.w.begin() + 1, 0.0); iT = 1; }
  for (auto& nd : nodes) { nd.w.resize(weightNames.size(), 0.0); nd.w[iL] = 0; nd.w[iT] = 0; }
  for (auto& l : links) {
    nodes[l.a].w[iL] += 1; nodes[l.b].w[iL] += 1;
    nodes[l.a].w[iT] += l.w; nodes[l.b].w[iT] += l.w;
  }
}
void Network::countClusters() {
  int k = 0;
  for (auto& nd : nodes) k = std::max(k, nd.cluster + 1);
  nClusters = k;
  if (int(clusterNames.size()) < k) clusterNames.resize(k);
}
string Network::clusterName(int c) const {
  if (c >= 0 && c < int(clusterNames.size()) && !clusterNames[c].empty()) return clusterNames[c];
  return "Cluster " + std::to_string(c + 1);
}

// ------------------------------------------------------------ VOSviewer files
bool parseVosviewer(const string& mapTextIn, const string& netTextIn, Network& net, string* err) {
  // VOSviewer writes UTF-8 with a byte order mark; left in place it hides the first column header (id)
  auto noBom = [](const string& t) { return t.compare(0, 3, "\xEF\xBB\xBF") == 0 ? t.substr(3) : t; };
  const string mapText = noBom(mapTextIn), netText = noBom(netTextIn);
  net = Network();
  std::unordered_map<string, int> idx;
  if (!trim(mapText).empty()) {
    auto rows = parseCsv(mapText, '\t');
    if (rows.empty()) { if (err) *err = "empty map file"; return false; }
    auto& h = rows[0];
    int iId = -1, iLabel = -1, iX = -1, iY = -1, iC = -1;
    vector<std::pair<int, string>> wcols, scols;
    int iDesc = -1;
    for (size_t i = 0; i < h.size(); i++) {
      string c = lower(trim(h[i]));
      if (c == "id") iId = int(i);
      else if (c == "label") iLabel = int(i);
      else if (c == "x") iX = int(i);
      else if (c == "y") iY = int(i);
      else if (c == "cluster") iC = int(i);
      else if (c == "description") iDesc = int(i);
      else if (startsWith(c, "weight<")) wcols.emplace_back(int(i), trim(h[i]).substr(7, trim(h[i]).size() - 8));
      else if (startsWith(c, "score<")) scols.emplace_back(int(i), trim(h[i]).substr(6, trim(h[i]).size() - 7));
      else if (c == "weight") wcols.emplace_back(int(i), "Weight");
      else if (c == "score") scols.emplace_back(int(i), "Score");
    }
    if (iLabel < 0 && iId < 0) { if (err) *err = "map file has no id/label column"; return false; }
    net.weightNames.clear();
    for (auto& w : wcols) net.weightNames.push_back(w.second);
    for (auto& s : scols) net.scoreNames.push_back(s.second);
    bool hasXY = iX >= 0 && iY >= 0;
    for (size_t r = 1; r < rows.size(); r++) {
      auto& row = rows[r];
      auto g = [&](int i) { return i >= 0 && i < int(row.size()) ? trim(row[i]) : string(); };
      Node nd;
      nd.id = iId >= 0 ? g(iId) : std::to_string(r);
      nd.label = iLabel >= 0 ? g(iLabel) : nd.id;
      if (nd.label.empty() && nd.id.empty()) continue;
      if (hasXY) { nd.x = toDouble(g(iX)); nd.y = -toDouble(g(iY)); }
      nd.cluster = iC >= 0 ? std::max(0, toInt(g(iC), 1) - 1) : 0;
      for (auto& w : wcols) nd.w.push_back(toDouble(g(w.first)));
      for (auto& s : scols) nd.sc.push_back(toDouble(g(s.first), NAN));
      if (iDesc >= 0) nd.title = g(iDesc);
      idx[nd.id] = int(net.nodes.size());
      net.nodes.push_back(nd);
    }
  }
  if (!trim(netText).empty()) {
    std::istringstream in(netText);
    string line;
    std::unordered_map<uint64_t, int> seen;
    while (std::getline(in, line)) {
      auto parts = splitAny(line, "\t ,");
      if (parts.size() < 2) continue;
      auto getNode = [&](const string& id) {
        auto it = idx.find(id);
        if (it != idx.end()) return it->second;
        Node nd;
        nd.id = id;
        nd.label = id;
        idx[id] = int(net.nodes.size());
        net.nodes.push_back(nd);
        return int(net.nodes.size()) - 1;
      };
      if (!isDigits(parts[0]) && idx.find(parts[0]) == idx.end() && net.nodes.empty()) continue;  // header
      int a = getNode(parts[0]), b = getNode(parts[1]);
      if (a == b) continue;
      double w = parts.size() > 2 ? toDouble(parts[2], 1) : 1;
      uint64_t k = a < b ? (uint64_t(a) << 32 | uint32_t(b)) : (uint64_t(b) << 32 | uint32_t(a));
      auto it = seen.find(k);
      if (it != seen.end()) { net.links[it->second].w = std::max(net.links[it->second].w, w); continue; }
      seen[k] = int(net.links.size());
      Link l;
      l.a = std::min(a, b);
      l.b = std::max(a, b);
      l.w = w;
      l.s = w;
      net.links.push_back(l);
    }
  }
  if (net.nodes.empty()) { if (err) *err = "no items found"; return false; }
  for (auto& nd : net.nodes) nd.w.resize(net.weightNames.size(), 0), nd.sc.resize(net.scoreNames.size(), NAN);
  // Links / Total link strength always available
  if (net.weightIndex("Links") < 0 || net.weightIndex("Total link strength") < 0) net.recomputeLinkWeights();
  net.weightIdx = 0;
  for (size_t i = 0; i < net.weightNames.size(); i++) if (!iequals(net.weightNames[i], "Links") && !iequals(net.weightNames[i], "Total link strength")) { net.weightIdx = int(i); break; }
  if (!net.scoreNames.empty()) net.scoreIdx = 0;
  net.countClusters();
  return true;
}
string writeVosMap(const Network& net) {
  string o = "id\tlabel\tx\ty\tcluster";
  for (auto& w : net.weightNames) o += "\tweight<" + w + ">";
  for (auto& s : net.scoreNames) o += "\tscore<" + s + ">";
  o += "\n";
  for (int i = 0; i < net.n(); i++) {
    auto& nd = net.nodes[i];
    o += std::to_string(i + 1) + "\t" + replaceAll(nd.label, "\t", " ") + "\t" + fmtFixed(nd.x, 4) + "\t" + fmtFixed(-nd.y, 4) + "\t" + std::to_string(nd.cluster + 1);
    for (size_t k = 0; k < net.weightNames.size(); k++) o += "\t" + fmtNum(k < nd.w.size() ? nd.w[k] : 0, 4);
    for (size_t k = 0; k < net.scoreNames.size(); k++) o += "\t" + (k < nd.sc.size() && std::isfinite(nd.sc[k]) ? fmtFixed(nd.sc[k], 4) : string(""));
    o += "\n";
  }
  return o;
}
string writeVosNetwork(const Network& net) {
  string o;
  for (auto& l : net.links) o += std::to_string(l.a + 1) + "\t" + std::to_string(l.b + 1) + "\t" + fmtNum(l.w, 4) + "\n";
  return o;
}

// ------------------------------------------------------------ samples
namespace {
struct Theme { const char* name; vector<string> kw, id, jn; };
const vector<Theme>& wosThemes() {
  static const vector<Theme> T = {
      {"ML foundations",
       {"machine learning", "deep learning", "neural network", "representation learning", "transfer learning", "classification accuracy", "feature extraction",
        "self-supervised learning", "graph neural network", "model interpretability", "contrastive learning", "ensemble learning"},
       {"machine learning", "artificial neural network", "learning systems", "pattern recognition", "supervised learning"},
       {"Journal of Machine Learning Research", "Neural Computation", "Pattern Recognition", "Machine Learning", "IEEE Transactions on Neural Networks"}},
      {"Learning analytics",
       {"learning analytics", "student engagement", "academic performance", "dropout prediction", "learning dashboards", "clickstream data",
        "self-regulated learning", "predictive modelling", "early warning system", "learning outcomes", "process mining", "trace data"},
       {"learning analytics", "educational data mining", "student performance", "data mining in education", "predictive analytics", "learning management system"},
       {"Computers & Education", "British Journal of Educational Technology", "Journal of Learning Analytics", "Internet and Higher Education", "Computers in Human Behavior"}},
      {"Generative AI",
       {"generative artificial intelligence", "large language model", "chatgpt", "prompt engineering", "fine-tuning", "hallucination",
        "retrieval augmented generation", "chatbot tutor", "ai-generated feedback", "ai literacy", "conversational agent", "intelligent tutoring system"},
       {"generative ai", "large language models", "chatgpt", "natural language processing", "automated feedback", "tutoring system", "education technology"},
       {"Computers and Education: Artificial Intelligence", "Interactive Learning Environments", "International Journal of Educational Technology in Higher Education",
        "IEEE Transactions on Learning Technologies", "Education and Information Technologies"}},
      {"Ethics & policy",
       {"algorithmic bias", "academic integrity", "data privacy", "digital divide", "equity in education", "surveillance technology", "ai governance",
        "fairness metrics", "explainable ai", "responsible ai", "proctoring software", "student data protection"},
       {"artificial intelligence", "ethics", "privacy", "bias in algorithms", "academic integrity", "governance", "surveillance"},
       {"Ethics and Information Technology", "Journal of Academic Ethics", "AI and Society", "Policy Futures in Education", "Big Data & Society"}}};
  return T;
}
const char* LAST[] = {"Smith", "Johnson", "Kumar", "Wang", "Garcia", "Muller", "Nakamura", "Rossi", "Dubois", "Okafor", "Silva", "Andersson", "Novak", "Haddad", "Tanaka", "Mbatha", "Fernandez", "Petrov", "Yilmaz", "Ahmed"};
const char* FIRST[] = {"A", "J", "M", "L", "K", "R", "S", "T", "P", "N", "E", "D", "H", "B", "C"};
const char* CTRY[][2] = {{"China", "Tsinghua Univ"}, {"USA", "Stanford Univ"}, {"England", "Univ Coll London"}, {"Germany", "Tech Univ Munich"},
                         {"Australia", "Monash Univ"}, {"Brazil", "Univ Sao Paulo"}, {"India", "Indian Inst Technol"}, {"Netherlands", "Univ Amsterdam"},
                         {"Spain", "Univ Barcelona"}, {"South Africa", "Univ Cape Town"}, {"Canada", "Univ Toronto"}, {"Japan", "Kyoto Univ"}};
string abbrSource(const string& s) {
  string t = s;
  for (auto& c : t) if (c == ':' || c == '&' || c == ',') c = ' ';
  vector<string> out;
  for (auto& w : split(t, ' ')) {
    string lw = lower(w);
    if (lw == "of" || lw == "and" || lw == "the" || lw == "in" || lw == "for" || lw == "on") continue;
    string u = upper(w);
    out.push_back(u.size() > 6 ? u.substr(0, 5) : u);
  }
  return join(out, " ");
}
string alnumLower(const string& s) {
  string r;
  for (char c : lower(s)) if (c >= 'a' && c <= 'z') r += c;
  return r;
}
struct Done { string au; int year; string so; int vol, page; string doi; int t; int cites; };
}  // namespace

string sampleWoSExport() {
  Rng R(4242), RR(777);
  auto& T = wosThemes();
  auto pick = [&](const vector<string>& v) -> const string& { return v[R.below(int(v.size()))]; };
  vector<vector<string>> cast(T.size());
  for (size_t i = 0; i < T.size(); i++)
    for (int k = 0; k < 7; k++) cast[i].push_back(string(LAST[(i * 5 + R.below(20)) % 20]) + ", " + FIRST[R.below(15)]);
  // cited-reference pools: theme classics (Zipf-weighted), shared methods pool
  vector<vector<string>> pools(T.size());
  for (size_t t = 0; t < T.size(); t++)
    for (int k = 0; k < 18; k++) {
      string au = string(LAST[(t * 7 + k * 3) % 20]) + " " + FIRST[(t + k) % 15];
      int y = 2003 + int((t * 5 + k * 3) % 14);
      string r = au + ", " + std::to_string(y) + ", " + abbrSource(T[t].jn[k % T[t].jn.size()]) + ", V" + std::to_string(5 + (t * 11 + k * 7) % 60) + ", P" +
                 std::to_string(1 + (t * 97 + k * 131) % 800);
      if (k % 2) r += ", DOI 10." + std::to_string(1000 + t * 7) + "/" + alnumLower(T[t].name) + "." + std::to_string(y) + "." + std::to_string(k);
      pools[t].push_back(r);
    }
  vector<string> methods = {"van Eck NJ, 2010, SCIENTOMETRICS, V84, P523, DOI 10.1007/s11192-009-0146-3", "Waltman L, 2010, J INFORMETR, V4, P629, DOI 10.1016/j.joi.2010.07.002",
                            "Traag VA, 2019, SCI REP-UK, V9, P5233, DOI 10.1038/s41598-019-41695-z", "Aria M, 2017, J INFORMETR, V11, P959, DOI 10.1016/j.joi.2017.08.007",
                            "Donthu N, 2021, J BUS RES, V133, P285, DOI 10.1016/j.jbusres.2021.04.070", "Zupic I, 2015, ORGAN RES METHODS, V18, P429, DOI 10.1177/1094428114562629"};
  vector<Done> done;
  auto zipf = [&](int n) {
    double tot = 0;
    for (int k = 1; k <= n; k++) tot += 1.0 / k;
    double x = RR() * tot;
    for (int k = 1; k <= n; k++) { x -= 1.0 / k; if (x <= 0) return k - 1; }
    return n - 1;
  };
  string out = "FN Clarivate Analytics Web of Science\nVR 1.0\n";
  const int N = 140;
  const char* ctxs[] = {"higher education", "the classroom", "online learning", "teacher training", "university teaching", "assessment practice"};
  const char* kinds[] = {"systematic review", "empirical study", "mixed-methods study", "case study", "large-scale analysis"};
  const char* designs[] = {"a survey of 1,240 students", "interviews with 38 instructors", "a two-year field experiment", "an analysis of 12,000 course records"};
  const char* outcomes[] = {"student outcomes", "engagement", "retention", "assessment quality", "teacher workload"};
  const char* impl[] = {"institutional policy", "course design", "tool development", "faculty development"};
  const char* lim[] = {"sample", "design", "measures"};
  for (int i = 0; i < N; i++) {
    int ti = i % int(T.size());
    const Theme& th = T[ti];
    const Theme* cross = R() < 0.34 ? &T[(i + 1) % T.size()] : nullptr;
    int year = 2016 + R.below(9);
    vector<string> kws;
    auto addK = [&](const string& k) { if (std::find(kws.begin(), kws.end(), k) == kws.end()) kws.push_back(k); };
    int nk = 3 + R.below(4);
    for (int k = 0; k < nk; k++) addK(pick(th.kw));
    if (cross && R() < 0.6) addK(pick(cross->kw));
    if (R() < 0.18) addK(pick(T[(i + 2) % T.size()].kw));
    vector<string> ids;
    auto addI = [&](const string& k) { if (std::find(ids.begin(), ids.end(), k) == ids.end()) ids.push_back(k); };
    int ni = 2 + R.below(3);
    for (int k = 0; k < ni; k++) addI(pick(th.id));
    if (cross) addI(pick(cross->id));
    int na = 1 + R.below(3);
    vector<string> authors;
    for (int a = 0; a < na; a++) {
      if (a == 0 || R() < 0.65) authors.push_back(cast[ti][R.below(7)]);
      else authors.push_back(string(LAST[R.below(20)]) + ", " + FIRST[R.below(15)]);
    }
    int nAff = 1 + R.below(3);
    vector<string> affs;
    for (int a = 0; a < nAff; a++) {
      auto& c = CTRY[R.below(12)];
      affs.push_back("[" + authors[0] + "] " + c[1] + ", Dept Educ, " + c[0] + ", " + c[0] + ".");
    }
    string ctx = ctxs[R.below(6)], kind = kinds[R.below(5)];
    string title = titleCase(pick(th.kw)) + " and " + (cross ? pick(cross->kw) : pick(th.kw)) + " in " + ctx + ": a " + kind;
    string k3;
    for (size_t k = 0; k < kws.size() && k < 3; k++) k3 += (k ? ", " : "") + kws[k];
    string abs = "This study investigates " + k3 + " using " + designs[R.below(4)] + ". The results show that " + pick(th.kw) +
                 " is associated with measurable changes in " + outcomes[R.below(5)] + ". We discuss implications for " + impl[R.below(4)] +
                 ", and the limitations of the " + lim[R.below(3)] + ".";
    int cites = int(std::pow(R(), 2.3) * 220);
    string so = pick(th.jn);
    string doi = R() < 0.7 ? "10.1016/j." + alnumLower(th.name) + "." + std::to_string(year) + "." + std::to_string(100000 + i) : "";
    int vol = 10 + (year - 2016) * 3 + (i % 3), page = 100 + ((i * 37) % 900);
    // cited references
    std::set<string> crefs;
    int nTheme = 4 + int(RR() * 5);
    for (int k = 0; k < nTheme; k++) crefs.insert(pools[ti][zipf(18)]);
    if (RR() < 0.5) crefs.insert(pools[(ti + 1) % T.size()][zipf(18)]);
    if (RR() < 0.35) crefs.insert(methods[int(RR() * methods.size()) % methods.size()]);
    // internal citations to earlier records (same theme preferred, citation-weighted)
    int nInt = done.empty() ? 0 : int(RR() * 4);
    for (int k = 0; k < nInt; k++) {
      vector<const Done*> cand, same;
      for (auto& d : done) if (d.year < year || (d.year == year && RR() < 0.3)) { cand.push_back(&d); if (d.t == ti) same.push_back(&d); }
      if (cand.empty()) break;
      auto& lst = !same.empty() && RR() < 0.75 ? same : cand;
      double tot = 0;
      for (auto d : lst) tot += 1 + d->cites;
      double x = RR() * tot;
      const Done* d = lst[0];
      for (auto e : lst) { x -= 1 + e->cites; if (x <= 0) { d = e; break; } }
      auto comma = d->au.find(',');
      string sur = d->au.substr(0, comma), ini = comma == string::npos ? "" : trim(d->au.substr(comma + 1));
      crefs.insert(sur + " " + ini + ", " + std::to_string(d->year) + ", " + abbrSource(d->so) + ", V" + std::to_string(d->vol) + ", P" + std::to_string(d->page) +
                   (d->doi.empty() ? "" : ", DOI " + d->doi));
    }
    done.push_back({authors[0], year, so, vol, page, doi, ti, cites});
    out += "PT J\n";
    for (size_t a = 0; a < authors.size(); a++) out += (a ? "   " : "AU ") + authors[a] + "\n";
    out += "TI " + title + "\nSO " + so + "\nAB " + abs + "\n";
    for (size_t a = 0; a < affs.size(); a++) out += (a ? "   " : "C1 ") + affs[a] + "\n";
    out += "PY " + std::to_string(year) + "\n";
    if (!doi.empty()) out += "DI " + doi + "\n";
    out += "VL " + std::to_string(vol) + "\nBP " + std::to_string(page) + "\n";
    out += "DE " + join(kws, "; ") + "\nID " + join(ids, "; ") + "\n";
    bool firstCr = true;
    for (auto& c : crefs) { out += (firstCr ? "CR " : "   ") + c + "\n"; firstCr = false; }
    out += "TC " + std::to_string(cites) + "\nDT Article\nLA English\nER\n\n";
  }
  out += "EF\n";
  return out;
}

string sampleScopusExport() {
  Rng R(9090);
  struct ST { const char* name; vector<string> kw; vector<string> jn; };
  vector<ST> T = {
      {"Science mapping", {"bibliometrics", "science mapping", "co-authorship network", "citation analysis", "research fronts", "co-word analysis", "vosviewer", "research impact", "keyword co-occurrence", "scientific collaboration"},
       {"Scientometrics", "Journal of Informetrics", "Quantitative Science Studies", "Research Evaluation"}},
      {"Text mining", {"natural language processing", "topic modelling", "latent dirichlet allocation", "named entity recognition", "text mining", "network science", "publication trends", "word embeddings", "semantic similarity", "document clustering"},
       {"Information Processing & Management", "Journal of the Association for Information Science and Technology", "Expert Systems with Applications", "Knowledge-Based Systems"}},
      {"Research policy", {"knowledge mapping", "country collaboration", "interdisciplinarity", "thematic evolution", "research productivity", "systematic literature review", "open science", "research funding", "gender gap", "altmetrics"},
       {"Research Policy", "Science and Public Policy", "Technological Forecasting and Social Change", "Journal of Business Research"}}};
  vector<vector<string>> pools(T.size());
  for (size_t t = 0; t < T.size(); t++)
    for (int k = 0; k < 14; k++) {
      string sur = LAST[(t * 6 + k * 5) % 20];
      int y = 2005 + int((t * 3 + k * 5) % 15);
      pools[t].push_back(sur + ", " + FIRST[(t * 2 + k) % 15] + "., " + titleCase(T[t].kw[k % T[t].kw.size()]) + " revisited, (" + std::to_string(y) + ") " +
                         T[t].jn[k % T[t].jn.size()] + ", " + std::to_string(3 + (t * 7 + k * 3) % 40) + ", pp. " + std::to_string(10 + (k * 53) % 700) + "-" +
                         std::to_string(30 + (k * 53) % 700));
    }
  const char* CT[][3] = {{"Leiden University", "Leiden", "Netherlands"}, {"University of Granada", "Granada", "Spain"}, {"Indiana University", "Bloomington", "United States"},
                         {"Wuhan University", "Wuhan", "China"}, {"University of Sao Paulo", "Sao Paulo", "Brazil"}, {"University of Tehran", "Tehran", "Iran"},
                         {"University of Dhaka", "Dhaka", "Bangladesh"}, {"KU Leuven", "Leuven", "Belgium"}, {"University of Toronto", "Toronto", "Canada"},
                         {"Universiti Malaya", "Kuala Lumpur", "Malaysia"}};
  auto q = [](const string& s) { return "\"" + replaceAll(s, "\"", "\"\"") + "\""; };
  string out = "Authors,Author full names,Title,Year,Source title,Volume,Issue,Page start,Page end,Cited by,DOI,Link,Affiliations,Abstract,Author Keywords,Index Keywords,References,Document Type,Publisher,Language of Original Document,Source,EID\n";
  for (int i = 0; i < 90; i++) {
    int ti = i % 3;
    auto& th = T[ti];
    int year = 2016 + R.below(9);
    vector<string> kws;
    int nk = 3 + R.below(3);
    for (int k = 0; k < nk; k++) { auto& w = th.kw[R.below(int(th.kw.size()))]; if (std::find(kws.begin(), kws.end(), w) == kws.end()) kws.push_back(w); }
    if (R() < 0.3) kws.push_back(T[(ti + 1) % 3].kw[R.below(10)]);
    int na = 1 + R.below(4);
    vector<string> au, auFull, aff;
    for (int a = 0; a < na; a++) {
      string s = LAST[(ti * 4 + R.below(9)) % 20];
      string f = FIRST[R.below(15)];
      au.push_back(s + " " + f + ".");
      auFull.push_back(s + ", " + f + ". (" + std::to_string(57000000000LL + i * 10 + a) + ")");
      auto& c = CT[(ti * 3 + R.below(5)) % 10];
      aff.push_back(string("Department of Information Science, ") + c[0] + ", " + c[1] + ", " + c[2]);
    }
    string so = th.jn[R.below(int(th.jn.size()))];
    int vol = 20 + (year - 2016) * 4, ps = 1 + (i * 41) % 900;
    int cites = int(std::pow(R(), 2.0) * 160);
    vector<string> refs;
    int nr = 5 + R.below(6);
    for (int k = 0; k < nr; k++) { auto& rf = pools[ti][R.below(14)]; if (std::find(refs.begin(), refs.end(), rf) == refs.end()) refs.push_back(rf); }
    if (R() < 0.4) refs.push_back(pools[(ti + 2) % 3][R.below(14)]);
    string title = titleCase(kws[0]) + " for " + kws[std::min<size_t>(1, kws.size() - 1)] + ": evidence from " + std::to_string(200 + R.below(4000)) + " publications";
    string abs = "We examine " + join(kws, ", ") + " using a corpus of Scopus records. Results indicate growing attention to " + kws[0] + ".";
    string doi = "10.1007/s" + std::to_string(11192 + ti) + "-" + std::to_string(year) + "-" + std::to_string(1000 + i);
    out += q(join(au, "; ")) + "," + q(join(auFull, "; ")) + "," + q(title) + "," + std::to_string(year) + "," + q(so) + "," + std::to_string(vol) + ",," +
           std::to_string(ps) + "," + std::to_string(ps + 14) + "," + std::to_string(cites) + "," + doi + ",," + q(join(aff, "; ")) + "," + q(abs) + "," +
           q(join(kws, "; ")) + ",," + q(join(refs, "; ")) + ",Article,Springer,English,Scopus,2-s2.0-" + std::to_string(85000000000LL + i) + "\n";
  }
  return out;
}

// ------------------------------------------------------------ export
string wosAuthor(const string& name) {
  string n = trim(name);
  if (n.empty() || n.find(',') != string::npos) return n;
  auto parts = splitAny(n, " ");
  if (parts.size() < 2) return n;
  const string& last = parts.back();
  bool initials = last.size() <= 3 && std::all_of(last.begin(), last.end(), [](char c) { return (c >= 'A' && c <= 'Z') || c == '.'; });
  if (initials) {  // "Petrov B" / "van der Berg JA"
    string sur;
    for (size_t i = 0; i + 1 < parts.size(); i++) sur += (i ? " " : "") + parts[i];
    return sur + ", " + replaceAll(last, ".", "");
  }
  string ini;
  for (size_t i = 0; i + 1 < parts.size(); i++) {
    auto sub = splitAny(parts[i], "-");
    for (auto& q : sub) if (!q.empty() && q[0] != '(') ini += char(std::toupper((unsigned char)q[0]));
  }
  return last + ", " + ini;
}

// affiliation/country pairs such that re-parsing yields the same organisation and country sets
static vector<string> affLines(const Record& r) {
  vector<string> out;
  size_t nA = r.affiliations.size(), nC = r.countries.size(), n = std::max(nA, nC);
  for (size_t i = 0; i < n; i++) {
    string a = nA ? replaceAll(r.affiliations[std::min(i, nA - 1)], ";", ",") : string("Unknown");
    if (nC) a += ", " + r.countries[std::min(i, nC - 1)];
    out.push_back(a);
  }
  return out;
}

static string csvQ(const string& v) {
  bool q = v.find_first_of(",\"\n\r") != string::npos;
  if (!q) return v;
  return "\"" + replaceAll(v, "\"", "\"\"") + "\"";
}

string writeRecords(const vector<Record>& recs, RecordExport fmt) {
  string o;
  auto one = [](const string& v) { string t = replaceAll(replaceAll(v, "\r", " "), "\n", " "); return trim(t); };
  if (fmt == RecordExport::WoS) {
    o = "FN Clarivate Analytics Web of Science\nVR 1.0\n";
    for (auto& r : recs) {
      auto list = [&](const char* tag, const vector<string>& v) {
        for (size_t i = 0; i < v.size(); i++) o += (i ? string("   ") : string(tag) + " ") + one(v[i]) + "\n";
      };
      auto field = [&](const char* tag, const string& v) { if (!trim(v).empty()) o += string(tag) + " " + one(v) + "\n"; };
      o += "PT J\n";
      vector<string> au;
      for (auto& a : r.authors) au.push_back(wosAuthor(a));
      list("AU", au);
      list("AF", r.authors);
      field("TI", r.title);
      field("SO", upper(r.source));
      field("LA", r.language);
      field("DT", r.docType);
      field("DE", join(r.keywords, "; "));
      field("ID", join(r.indexTerms, "; "));
      field("AB", r.abstract_);
      list("C1", affLines(r));
      list("CR", r.refs);
      o += "NR " + std::to_string(r.refs.size()) + "\n";
      o += "TC " + std::to_string(r.cites) + "\nZ9 " + std::to_string(r.cites) + "\n";
      field("PU", r.publisher);
      if (r.year) o += "PY " + std::to_string(r.year) + "\n";
      field("VL", r.volume);
      field("BP", r.pages);
      field("DI", r.doi);
      field("UT", r.key);
      o += "ER\n\n";
    }
    o += "EF\n";
  } else if (fmt == RecordExport::RIS) {
    for (auto& r : recs) {
      auto f = [&](const char* tag, const string& v) { if (!trim(v).empty()) o += string(tag) + "  - " + one(v) + "\n"; };
      string ty = lower(r.docType);
      o += string("TY  - ") + (contains(ty, "book") ? "BOOK" : contains(ty, "proceed") || contains(ty, "conference") ? "CONF" : "JOUR") + "\n";
      for (auto& a : r.authors) f("AU", wosAuthor(a));
      f("TI", r.title);
      f("T2", r.source);
      if (r.year) f("PY", std::to_string(r.year));
      f("VL", r.volume);
      f("SP", r.pages);
      f("DO", r.doi);
      f("AB", r.abstract_);
      for (auto& k : r.keywords) f("KW", k);
      for (auto& a : affLines(r)) f("AD", a);
      f("PB", r.publisher);
      f("LA", r.language);
      f("N1", "Times cited: " + std::to_string(r.cites));
      for (auto& c : r.refs) f("CR", c);
      f("ID", r.key);
      o += "ER  - \n\n";
    }
  } else {
    o = "Authors,Author full names,Title,Year,Source title,Volume,Page start,Cited by,DOI,Affiliations,Abstract,Author Keywords,Index Keywords,References,Document Type,Language of Original Document,Publisher,EID\n";
    for (auto& r : recs) {
      vector<string> au;
      for (auto& a : r.authors) au.push_back(wosAuthor(a));
      vector<string> cells = {join(au, "; "), join(r.authors, "; "), one(r.title), r.year ? std::to_string(r.year) : "", one(r.source), r.volume, r.pages, std::to_string(r.cites), r.doi,
                              join(affLines(r), "; "), one(r.abstract_), join(r.keywords, "; "), join(r.indexTerms, "; "), join(r.refs, "; "), r.docType, r.language, r.publisher, r.key};
      for (size_t i = 0; i < cells.size(); i++) o += (i ? "," : "") + csvQ(cells[i]);
      o += "\n";
    }
  }
  return o;
}

}  // namespace vs
