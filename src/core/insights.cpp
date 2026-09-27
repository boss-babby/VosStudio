#include "insights.h"

#include <unordered_set>

namespace vs {

namespace {
double quantile(vector<int> v, double q) {  // type-7 quantile
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double h = (v.size() - 1) * q;
  size_t lo = size_t(std::floor(h)), hi = std::min(v.size() - 1, lo + 1);
  return v[lo] + (h - lo) * (v[hi] - v[lo]);
}
string alnumLower(const string& s, size_t maxLen) {
  string o;
  for (unsigned char ch : s) {
    if (std::isalnum(ch)) o += char(std::tolower(ch));
    if (o.size() >= maxLen) break;
  }
  return o;
}
string lowerAscii(string s) {
  for (auto& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}
string firstPage(const string& pages) {
  string o;
  for (char ch : pages) { if (ch == '-' || ch == ' ') break; o += ch; }
  return o;
}
// DOI inside a reference string ("DOI 10.x/y", "https://doi.org/10.x/y" or a bare "10.x/y")
string refDoi(const string& ref) {
  size_t p = ref.find("10.");
  while (p != string::npos) {
    size_t slash = ref.find('/', p);
    if (slash != string::npos && slash - p < 12) {
      size_t e = p;
      while (e < ref.size() && ref[e] != ' ' && ref[e] != ',' && ref[e] != ';' && ref[e] != ']') e++;
      string d = lowerAscii(ref.substr(p, e - p));
      while (!d.empty() && (d.back() == '.' || d.back() == ')')) d.pop_back();
      return d;
    }
    p = ref.find("10.", p + 3);
  }
  return "";
}
}  // namespace

// ------------------------------------------------------------ trend topics
vector<TrendTopic> trendTopics(const Corpus& c, Unit unit, int minFreq, int perYear, const Thesaurus* th) {
  std::unordered_map<string, std::pair<string, vector<int>>> T;  // key -> (label, years)
  for (auto& r : c.recs) {
    if (!r.year) continue;
    std::unordered_set<string> seen;
    for (auto& x : unitList(r, unit)) {
      string lab = bibClean(x), k = unitKey(lab, unit);
      if (th && !th->empty()) {
        auto it = th->replace.find(bibKey(lab));
        if (it != th->replace.end()) {
          if (it->second.empty()) continue;
          lab = it->second;
          k = unitKey(lab, unit);
        }
      }
      if (k.empty() || !seen.insert(k).second) continue;
      auto& e = T[k];
      if (e.first.empty()) e.first = lab;
      e.second.push_back(r.year);
    }
  }
  vector<TrendTopic> all;
  for (auto& kv : T) {
    if (int(kv.second.second.size()) < minFreq) continue;
    TrendTopic t;
    t.label = kv.second.first;
    t.freq = int(kv.second.second.size());
    t.q1 = quantile(kv.second.second, 0.25);
    t.med = quantile(kv.second.second, 0.5);
    t.q3 = quantile(kv.second.second, 0.75);
    all.push_back(t);
  }
  // keep the most frequent terms of each median year
  std::map<int, vector<TrendTopic>> byYear;
  for (auto& t : all) byYear[int(std::lround(t.med))].push_back(t);
  vector<TrendTopic> out;
  for (auto& kv : byYear) {
    auto& v = kv.second;
    std::sort(v.begin(), v.end(), [](const TrendTopic& a, const TrendTopic& b) { return a.freq != b.freq ? a.freq > b.freq : a.label < b.label; });
    for (size_t i = 0; i < v.size() && int(i) < perYear; i++) out.push_back(v[i]);
  }
  std::sort(out.begin(), out.end(), [](const TrendTopic& a, const TrendTopic& b) { return a.med != b.med ? a.med < b.med : a.freq > b.freq; });
  return out;
}

// ------------------------------------------------------------ RPYS
int refYear(const string& ref) {
  // WoS: "Author X, 2010, SOURCE, V1, P2"
  auto parts = split(ref, ',');
  if (parts.size() >= 2) {
    string y = trim(parts[1]);
    if (y.size() == 4 && isDigits(y)) { int v = toInt(y); if (v >= 1500 && v <= 2100) return v; }
  }
  // Scopus: "(2005)"
  for (size_t p = ref.find('('); p != string::npos; p = ref.find('(', p + 1)) {
    if (p + 5 < ref.size() && ref[p + 5] == ')' && isDigits(ref.substr(p + 1, 4))) { int v = toInt(ref.substr(p + 1, 4)); if (v >= 1500 && v <= 2100) return v; }
  }
  // any stand-alone 4-digit year
  for (size_t i = 0; i + 4 <= ref.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(ref[i]))) continue;
    if (i > 0 && std::isalnum(static_cast<unsigned char>(ref[i - 1]))) continue;
    if (!isDigits(ref.substr(i, 4))) continue;
    if (i + 4 < ref.size() && std::isalnum(static_cast<unsigned char>(ref[i + 4]))) continue;
    int v = toInt(ref.substr(i, 4));
    if (v >= 1500 && v <= 2100) return v;
  }
  return 0;
}

Rpys rpys(const Corpus& c, int from, int to) {
  Rpys R;
  vector<int> years;
  std::unordered_map<int, std::unordered_map<string, std::pair<string, int>>> refsByYear;
  int maxPub = 0;
  for (auto& r : c.recs) maxPub = std::max(maxPub, r.year);
  for (auto& r : c.recs)
    for (auto& ref : r.refs) {
      R.refs++;
      int y = refYear(ref);
      if (!y || (maxPub && y > maxPub)) continue;
      R.dated++;
      years.push_back(y);
      string t = bibClean(ref);
      auto& e = refsByYear[y][alnumLower(t, 80)];
      if (e.first.empty()) e.first = t;
      e.second++;
    }
  if (years.empty()) return R;
  std::sort(years.begin(), years.end());
  int y0 = from > 0 ? from : years[size_t(double(years.size() - 1) * 0.01)];  // ignore the oldest 1 % (usually classics far out)
  int y1 = to > 0 ? to : years.back();
  if (y1 < y0) std::swap(y0, y1);
  R.y0 = y0;
  R.y1 = y1;
  R.counts.assign(size_t(y1 - y0 + 1), 0);
  for (int y : years) if (y >= y0 && y <= y1) R.counts[size_t(y - y0)]++;
  R.dev.assign(R.counts.size(), 0);
  for (size_t i = 0; i < R.counts.size(); i++) {
    vector<int> w;
    for (int k = -2; k <= 2; k++) {
      long j = long(i) + k;
      if (j >= 0 && j < long(R.counts.size())) w.push_back(R.counts[size_t(j)]);
    }
    R.dev[i] = R.counts[i] - quantile(w, 0.5);
  }
  // peaks: positive local maxima of the deviation curve, strongest first
  vector<RpysPeak> pk;
  for (size_t i = 0; i < R.dev.size(); i++) {
    if (R.dev[i] <= 0) continue;
    if (i > 0 && R.dev[i - 1] > R.dev[i]) continue;
    if (i + 1 < R.dev.size() && R.dev[i + 1] >= R.dev[i]) continue;
    RpysPeak p;
    p.year = R.y0 + int(i);
    p.count = R.counts[i];
    p.dev = R.dev[i];
    for (auto& kv : refsByYear[p.year]) if (kv.second.second > p.topRefCount) { p.topRefCount = kv.second.second; p.topRef = kv.second.first; }
    pk.push_back(p);
  }
  std::sort(pk.begin(), pk.end(), [](const RpysPeak& a, const RpysPeak& b) { return a.dev > b.dev; });
  if (pk.size() > 8) pk.resize(8);
  R.peaks = pk;
  return R;
}

// ------------------------------------------------------------ production over time
Production productionOverTime(const Corpus& c, Unit unit, int top) {
  Production P;
  auto actors = topActors(c, unit, top);
  if (actors.empty()) return P;
  int y0 = 9999, y1 = 0, ref = 0;
  for (auto& r : c.recs) if (r.year) { y0 = std::min(y0, r.year); y1 = std::max(y1, r.year); }
  if (y1 < y0) return P;
  ref = y1;
  P.y0 = y0;
  P.y1 = y1;
  std::unordered_map<string, int> idx;
  for (auto& a : actors) { idx[unitKey(a.label, unit)] = int(P.labels.size()); P.labels.push_back(a.label); }
  P.docs.assign(P.labels.size(), vector<int>(size_t(y1 - y0 + 1), 0));
  P.citesPerYear.assign(P.labels.size(), vector<double>(size_t(y1 - y0 + 1), 0));
  for (auto& r : c.recs) {
    if (!r.year) continue;
    std::unordered_set<int> seen;
    for (auto& x : unitList(r, unit)) {
      auto it = idx.find(unitKey(bibClean(x), unit));
      if (it == idx.end() || !seen.insert(it->second).second) continue;
      P.docs[size_t(it->second)][size_t(r.year - y0)]++;
      P.citesPerYear[size_t(it->second)][size_t(r.year - y0)] += double(r.cites) / double(ref - r.year + 1);
    }
  }
  return P;
}

// ------------------------------------------------------------ SCP / MCP
vector<CountryCollab> countryCollaboration(const Corpus& c, int top) {
  std::unordered_map<string, CountryCollab> M;
  for (auto& r : c.recs) {
    std::map<string, string> cs;
    for (auto& x : r.countries) { string lab = bibClean(x), k = bibKey(lab); if (!k.empty()) cs.emplace(k, lab); }
    if (cs.empty()) continue;
    for (auto& kv : cs) {
      auto& e = M[kv.first];
      if (e.country.empty()) e.country = kv.second;
      if (cs.size() == 1) e.scp++; else e.mcp++;
    }
  }
  vector<CountryCollab> out;
  for (auto& kv : M) out.push_back(kv.second);
  std::sort(out.begin(), out.end(), [](const CountryCollab& a, const CountryCollab& b) { return a.scp + a.mcp != b.scp + b.mcp ? a.scp + a.mcp > b.scp + b.mcp : a.country < b.country; });
  if (int(out.size()) > top) out.resize(size_t(top));
  return out;
}

// ------------------------------------------------------------ local citations
size_t corpusSignature(const Corpus& c) {
  size_t h = c.recs.size() * 1000003u;
  for (size_t i = 0; i < c.recs.size(); i += std::max<size_t>(1, c.recs.size() / 64)) h = h * 31 + std::hash<string>()(c.recs[i].title) + c.recs[i].refs.size();
  return h;
}

string firstAuthorSurname(const Record& r) {
  if (r.authors.empty()) return "";
  string a = trim(r.authors[0]);
  size_t comma = a.find(',');
  if (comma != string::npos) return trim(a.substr(0, comma));
  auto parts = split(a, ' ');
  // "Petrov BA" (WoS short form) keeps the first word, "Boris A. Petrov" the last
  if (parts.size() >= 2) {
    const string& last = parts.back();
    bool initials = !last.empty() && last.size() <= 3 && std::all_of(last.begin(), last.end(), [](char ch) { return std::isupper(static_cast<unsigned char>(ch)) || ch == '.'; });
    return initials ? parts[0] : last;
  }
  return a;
}

CitationIndex localCitations(const Corpus& c) {
  CitationIndex X;
  size_t n = c.recs.size();
  X.cites.assign(n, {});
  X.citedBy.assign(n, {});
  X.sig = corpusSignature(c);
  std::unordered_map<string, int> byDoi, byWos, byTitle;
  for (size_t i = 0; i < n; i++) {
    const Record& r = c.recs[i];
    if (!r.doi.empty()) byDoi.emplace(lowerAscii(trim(r.doi)), int(i));
    string sur = alnumLower(firstAuthorSurname(r), 40);
    if (!sur.empty() && r.year && !r.volume.empty() && !r.pages.empty()) byWos.emplace(sur + "|" + std::to_string(r.year) + "|" + alnumLower(r.volume, 10) + "|" + alnumLower(firstPage(r.pages), 10), int(i));
    string tk = alnumLower(r.title, 48);
    if (tk.size() >= 20) byTitle.emplace(tk, int(i));
  }
  for (size_t i = 0; i < n; i++) {
    std::unordered_set<int> seen;
    for (auto& ref : c.recs[i].refs) {
      int hit = -1;
      string d = refDoi(ref);
      if (!d.empty()) { auto it = byDoi.find(d); if (it != byDoi.end()) hit = it->second; }
      if (hit < 0) {
        auto parts = split(ref, ',');
        if (parts.size() >= 4 && isDigits(trim(parts[1]))) {  // WoS form
          string sur = alnumLower(split(trim(parts[0]), ' ').empty() ? string() : split(trim(parts[0]), ' ')[0], 40);
          string vol, pg;
          for (size_t k = 2; k < parts.size(); k++) {
            string t = trim(parts[k]);
            if (t.size() > 1 && t[0] == 'V' && std::isdigit(static_cast<unsigned char>(t[1]))) vol = alnumLower(t.substr(1), 10);
            if (t.size() > 1 && t[0] == 'P' && std::isdigit(static_cast<unsigned char>(t[1]))) pg = alnumLower(t.substr(1), 10);
          }
          if (!sur.empty() && !vol.empty() && !pg.empty()) { auto it = byWos.find(sur + "|" + trim(parts[1]) + "|" + vol + "|" + pg); if (it != byWos.end()) hit = it->second; }
        }
      }
      if (hit < 0) {  // Scopus form: the segment before "(yyyy)" is the title
        size_t p = ref.find(", (");
        if (p != string::npos) {
          size_t q = ref.rfind(", ", p - 1);
          string title = q == string::npos ? ref.substr(0, p) : ref.substr(q + 2, p - q - 2);
          string tk = alnumLower(title, 48);
          if (tk.size() >= 20) { auto it = byTitle.find(tk); if (it != byTitle.end()) hit = it->second; }
        }
      }
      if (hit >= 0 && hit != int(i) && seen.insert(hit).second) {
        X.cites[i].push_back(hit);
        X.citedBy[size_t(hit)].push_back(int(i));
        X.links++;
      }
    }
  }
  return X;
}

CitationSummary citationSummary(const Corpus& c) {
  CitationSummary S;
  vector<int> v;
  for (auto& r : c.recs) { v.push_back(r.cites); S.total += r.cites; if (r.cites == 0) S.uncited++; }
  S.docs = int(v.size());
  if (v.empty()) return S;
  std::sort(v.begin(), v.end(), std::greater<int>());
  S.mean = double(S.total) / v.size();
  S.median = quantile(v, 0.5);
  long long cum = 0;
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] >= int(i + 1)) S.h = int(i + 1);
    cum += v[i];
    if (cum >= (long long)(i + 1) * (long long)(i + 1)) S.g = int(i + 1);
  }
  const int edges[] = {0, 1, 5, 10, 25, 50, 100, 250, 1000000000};
  const char* names[] = {"0", "1\xE2\x80\x93" "4", "5\xE2\x80\x93" "9", "10\xE2\x80\x93" "24", "25\xE2\x80\x93" "49", "50\xE2\x80\x93" "99", "100\xE2\x80\x93" "249", "250+"};
  int last = 0;
  vector<int> cnt(8, 0);
  for (int x : v)
    for (int b = 0; b < 8; b++) if (x >= edges[b] && x < edges[b + 1]) { cnt[size_t(b)]++; last = std::max(last, b); break; }
  for (int b = 0; b <= last; b++) S.bins.push_back({names[b], cnt[size_t(b)]});
  return S;
}

// ------------------------------------------------------------ clusters over time
ClusterYears clusterYears(const Network& net, const Corpus& c) {
  ClusterYears Y;
  int y0 = 9999, y1 = 0;
  for (auto& r : c.recs) if (r.year) { y0 = std::min(y0, r.year); y1 = std::max(y1, r.year); }
  if (y1 < y0 || net.nClusters <= 0) return Y;
  Y.y0 = y0;
  Y.y1 = y1;
  Y.docs.assign(size_t(net.nClusters), vector<int>(size_t(y1 - y0 + 1), 0));
  vector<std::unordered_set<int>> docs(size_t(net.nClusters));
  for (auto& nd : net.nodes)
    if (nd.cluster >= 0 && nd.cluster < net.nClusters)
      for (int d : nd.recs) docs[size_t(nd.cluster)].insert(d);
  for (size_t k = 0; k < docs.size(); k++)
    for (int d : docs[k])
      if (d >= 0 && size_t(d) < c.recs.size() && c.recs[size_t(d)].year) Y.docs[k][size_t(c.recs[size_t(d)].year - y0)]++;
  return Y;
}

// ------------------------------------------------------------ citation strings
string shortCite(const Record& r) {
  string sur = firstAuthorSurname(r);
  if (sur.empty()) sur = truncate(r.title, 24);
  if (r.authors.size() == 2) sur += " & " + [&] { Record t; t.authors = {r.authors[1]}; return firstAuthorSurname(t); }();
  else if (r.authors.size() > 2) sur += " et al.";
  return sur + (r.year ? " (" + std::to_string(r.year) + ")" : "");
}

string sourceCase(const string& s) {
  bool lowerSeen = false;
  for (char ch : s) if (ch >= 'a' && ch <= 'z') { lowerSeen = true; break; }
  if (lowerSeen) return s;
  string t = titleCase(lowerAscii(s));
  static const char* small[] = {" Of ", " And ", " In ", " The ", " For ", " On ", " To ", " A ", " An ", " At ", " By ", " With "};
  for (const char* w : small) {
    string lw = w;
    for (size_t i = 1; i + 1 < lw.size(); i++) lw[i] = char(std::tolower(static_cast<unsigned char>(lw[i])));
    t = replaceAll(t, w, lw);
  }
  return t;
}

string apaCitation(const Record& r) {
  auto apaName = [](const string& a0) {
    string a = trim(a0);
    size_t comma = a.find(',');
    string sur, given;
    if (comma != string::npos) { sur = trim(a.substr(0, comma)); given = trim(a.substr(comma + 1)); }
    else {
      auto p = split(a, ' ');
      if (p.size() < 2) return a;
      bool initials = p.back().size() <= 3 && std::all_of(p.back().begin(), p.back().end(), [](char ch) { return std::isupper(static_cast<unsigned char>(ch)) || ch == '.'; });
      if (initials) { sur = p[0]; given = p.back(); }
      else { sur = p.back(); for (size_t i = 0; i + 1 < p.size(); i++) given += p[i] + " "; }
    }
    string ini;  // "Boris A." -> "B. A.", "BA" (WoS short form) -> "B. A."
    for (auto& w : splitAny(given, " .-")) {
      if (w.empty()) continue;
      bool allUpper = w.size() <= 3 && std::all_of(w.begin(), w.end(), [](char ch) { return std::isupper(static_cast<unsigned char>(ch)); });
      if (allUpper) for (char ch : w) ini += string(1, ch) + ". ";
      else if (std::isalpha(static_cast<unsigned char>(w[0]))) ini += string(1, char(std::toupper(static_cast<unsigned char>(w[0])))) + ". ";
    }
    return sur + (ini.empty() ? "" : ", " + trim(ini));
  };
  string au;
  size_t n = r.authors.size();
  for (size_t i = 0; i < n && i < 20; i++) {
    if (i) au += (i + 1 == n) ? ", & " : ", ";
    au += apaName(r.authors[i]);
  }
  if (n > 20) au += ", \xE2\x80\xA6";
  string o = au.empty() ? "" : au + " ";
  o += "(" + (r.year ? std::to_string(r.year) : string("n.d.")) + "). ";
  o += r.title;
  if (!r.title.empty() && r.title.back() != '.' && r.title.back() != '?') o += ".";
  if (!r.source.empty()) {
    o += " " + sourceCase(r.source);
    if (!r.volume.empty()) o += ", " + r.volume;
    if (!r.pages.empty()) o += ", " + r.pages;
    o += ".";
  }
  if (!r.doi.empty()) o += " https://doi.org/" + r.doi;
  return o;
}

}  // namespace vs
