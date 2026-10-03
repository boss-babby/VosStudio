#include "oa.h"

#include <algorithm>
#include <cstring>

namespace vs {
namespace oa {

namespace {
string enc(const string& s) {
  string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
    else { char b[4]; snprintf(b, 4, "%%%02X", c); o += b; }
  }
  return o;
}
int versionRank(const string& v) {
  if (v == "publishedVersion") return 0;
  if (v == "acceptedVersion") return 1;
  if (v == "submittedVersion") return 2;
  return 3;
}
int kindRank(const string& k) {
  if (k == "cache") return 0;
  if (k == "publisher") return 1;
  if (k == "repository") return 2;
  if (k == "arxiv") return 3;
  return 4;  // pmc
}
bool isPmcHost(const string& h) { return contains(h, "ncbi.nlm.nih.gov") || contains(h, "europepmc.org"); }
string kindFor(const string& url, const string& sourceType, const string& hostType) {
  string h = hostOf(url);
  if (contains(h, "arxiv.org")) return "arxiv";
  if (isPmcHost(h)) return "pmc";
  if (sourceType == "repository" || hostType == "repository") return "repository";
  return "publisher";
}
}  // namespace

string normDoi(string s) {
  s = trim(s);
  string l = lower(s);
  const char* pre[] = {"https://doi.org/", "http://doi.org/", "https://dx.doi.org/", "http://dx.doi.org/", "doi.org/", "doi:", "doi "};
  for (const char* p : pre) if (startsWith(l, p)) { s = s.substr(strlen(p)); l = lower(s); break; }
  s = trim(s);
  // a DOI is case-insensitive; every service answers the lower-case form
  return lower(s);
}

bool arxivIdFromDoi(const string& doi, string* id) {
  string d = normDoi(doi);
  if (!startsWith(d, "10.48550/arxiv.")) return false;
  string x = d.substr(15);
  if (x.empty()) return false;
  if (id) *id = x;
  return true;
}

string hostOf(const string& url) {
  size_t p = url.find("://");
  if (p == string::npos) return "";
  size_t a = p + 3, b = url.find_first_of("/?#", a);
  string h = lower(url.substr(a, b == string::npos ? string::npos : b - a));
  size_t at = h.find('@');
  if (at != string::npos) h = h.substr(at + 1);
  size_t col = h.find(':');
  if (col != string::npos) h = h.substr(0, col);
  if (startsWith(h, "www.")) h = h.substr(4);
  return h;
}

void Resolved::add(const Candidate& c) {
  if (c.url.empty() || !startsWith(lower(c.url), "http")) return;
  for (auto& x : candidates) if (x.url == c.url) { if (x.version.empty()) x.version = c.version; if (x.license.empty()) x.license = c.license; return; }
  candidates.push_back(c);
}

// ------------------------------------------------------------------ OpenAlex
void parseOpenAlexWork(const Json& w, Resolved& r) {
  r.known = true;
  string id = w["id"].str();
  if (!id.empty()) r.workId = id.substr(id.rfind('/') + 1);
  if (r.doi.empty()) r.doi = normDoi(w["doi"].str());
  if (r.title.empty()) r.title = w["display_name"].str(w["title"].str());
  const Json& oa = w["open_access"];
  if (oa.has("is_oa")) r.isOa = oa["is_oa"].boolean(false);
  if (!oa["oa_status"].str().empty()) r.oaStatus = oa["oa_status"].str();
  r.cached = w["has_content"]["pdf"].boolean(false);
  r.cacheUrl = w["content_urls"]["pdf"].str();
  if (r.cached && r.cacheUrl.empty() && !r.workId.empty()) r.cacheUrl = contentUrl(r.workId);
  const Json& ids = w["ids"];
  if (!ids["pmcid"].str().empty()) { string p = ids["pmcid"].str(); r.pmcid = p.substr(p.rfind('/') + 1); }
  auto loc = [&](const Json& l, bool best) {
    if (l.isNull()) return;
    if (!l["is_oa"].boolean(false)) return;
    string pdf = l["pdf_url"].str();
    string land = l["landing_page_url"].str();
    const Json& src = l["source"];
    string name = src["display_name"].str();
    if (best && !land.empty() && r.landing.empty()) r.landing = land;
    if (pdf.empty()) return;
    Candidate c;
    c.url = pdf;
    c.kind = kindFor(pdf, src["type"].str(), "");
    c.host = !name.empty() ? name : hostOf(pdf);
    c.version = l["version"].str();
    c.license = l["license"].str();
    if (c.kind == "arxiv") {
      string h = lower(pdf);
      size_t p = h.find("arxiv.org/pdf/");
      if (p != string::npos && r.arxivId.empty()) { string x = pdf.substr(p + 14); while (!x.empty() && (x.back() == '/' )) x.pop_back(); if (endsWith(lower(x), ".pdf")) x = x.substr(0, x.size() - 4); r.arxivId = x; }
      c.host = "arXiv";
    }
    r.add(c);
  };
  loc(w["best_oa_location"], true);
  const Json& locs = w["locations"];
  for (size_t i = 0; i < locs.size(); i++) loc(locs[i], false);
  if (r.landing.empty()) r.landing = w["primary_location"]["landing_page_url"].str();
  if (r.landing.empty() && !r.doi.empty()) r.landing = "https://doi.org/" + r.doi;
}

void parseOpenAlexWorks(const Json& js, std::map<string, Resolved>& out) {
  const Json& res = js.has("results") ? js["results"] : js;
  if (res.t == Json::Arr) {
    for (size_t i = 0; i < res.size(); i++) {
      string d = normDoi(res[i]["doi"].str());
      if (d.empty()) continue;
      Resolved& r = out[d];
      r.doi = d;
      parseOpenAlexWork(res[i], r);
    }
  } else if (res.t == Json::Obj && res.has("id")) {
    string d = normDoi(res["doi"].str());
    if (d.empty()) return;
    Resolved& r = out[d];
    r.doi = d;
    parseOpenAlexWork(res, r);
  }
}

// ------------------------------------------------------------------ Unpaywall
void parseUnpaywall(const Json& js, Resolved& r) {
  if (js.t != Json::Obj || !js.has("is_oa")) return;
  r.known = true;
  if (r.oaStatus.empty()) r.oaStatus = js["oa_status"].str();
  if (js["is_oa"].boolean(false)) r.isOa = true;
  if (r.title.empty()) r.title = js["title"].str();
  auto loc = [&](const Json& l, bool best) {
    if (l.isNull()) return;
    string pdf = l["url_for_pdf"].str();
    if (best && r.landing.empty()) r.landing = l["url_for_landing_page"].str();
    if (pdf.empty()) return;
    Candidate c;
    c.url = pdf;
    c.kind = kindFor(pdf, "", l["host_type"].str());
    c.host = c.kind == "arxiv" ? "arXiv" : hostOf(pdf);
    c.version = l["version"].str();
    c.license = l["license"].str();
    r.add(c);
  };
  loc(js["best_oa_location"], true);
  const Json& locs = js["oa_locations"];
  for (size_t i = 0; i < locs.size(); i++) loc(locs[i], false);
  if (r.landing.empty() && !r.doi.empty()) r.landing = "https://doi.org/" + r.doi;
}

// ------------------------------------------------------------------ Semantic Scholar
void parseSemanticScholar(const Json& js, Resolved& r) {
  if (js.t != Json::Obj || (!js.has("externalIds") && !js.has("openAccessPdf"))) return;
  r.known = true;
  if (js["isOpenAccess"].boolean(false)) r.isOa = true;
  if (r.title.empty()) r.title = js["title"].str();
  const Json& ext = js["externalIds"];
  if (r.arxivId.empty()) r.arxivId = ext["ArXiv"].str();
  if (r.pmcid.empty() && !ext["PubMedCentral"].str().empty()) { string p = ext["PubMedCentral"].str(); r.pmcid = startsWith(p, "PMC") ? p : "PMC" + p; }
  string pdf = js["openAccessPdf"]["url"].str();
  if (!pdf.empty()) {
    Candidate c;
    c.url = pdf;
    c.kind = kindFor(pdf, "", "");
    c.host = c.kind == "arxiv" ? "arXiv" : hostOf(pdf);
    c.license = js["openAccessPdf"]["license"].str();
    r.add(c);
  }
}

// ------------------------------------------------------------------ the order of attack
void order(Resolved& r, bool haveKey, bool preferPublished) {
  // the copies only the ids point at
  if (!r.arxivId.empty()) { Candidate c; c.url = "https://arxiv.org/pdf/" + r.arxivId; c.kind = "arxiv"; c.host = "arXiv"; c.version = "submittedVersion"; r.add(c); }
  string ax;
  if (r.arxivId.empty() && arxivIdFromDoi(r.doi, &ax)) { r.arxivId = ax; Candidate c; c.url = "https://arxiv.org/pdf/" + ax; c.kind = "arxiv"; c.host = "arXiv"; c.version = "submittedVersion"; r.add(c); r.known = true; r.isOa = true; if (r.oaStatus.empty()) r.oaStatus = "green"; if (r.landing.empty()) r.landing = "https://arxiv.org/abs/" + ax; }
  // the cached copy leads, or leaves, depending on the key
  r.candidates.erase(std::remove_if(r.candidates.begin(), r.candidates.end(), [](const Candidate& c) { return c.kind == "cache"; }), r.candidates.end());
  if (haveKey && (r.cached || (r.isOa && !r.workId.empty()))) {
    Candidate c;
    c.url = !r.cacheUrl.empty() ? r.cacheUrl : contentUrl(r.workId);
    c.kind = "cache";
    c.host = "OpenAlex (cached copy)";
    r.candidates.insert(r.candidates.begin(), c);
  }
  std::stable_sort(r.candidates.begin(), r.candidates.end(), [&](const Candidate& a, const Candidate& b) {
    int ka = kindRank(a.kind), kb = kindRank(b.kind);
    if ((ka == 0) != (kb == 0)) return ka == 0;      // the cache first
    if ((ka >= 3) != (kb >= 3)) return ka < 3;       // arXiv and PMC after the publisher and repositories
    if (ka >= 3 && kb >= 3) return ka < kb;          // arXiv before PMC
    int va = versionRank(a.version), vb = versionRank(b.version);
    if (preferPublished && va != vb) return va < vb;  // published > accepted > submitted
    if (ka != kb) return ka < kb;                     // the publisher before repositories within a version
    return false;
  });
}

string openAlexBatchUrl(const vector<string>& dois, const string& mail) {
  string f;
  for (auto& d : dois) { if (d.empty()) continue; f += (f.empty() ? "" : "|") + normDoi(d); }
  string u = "https://api.openalex.org/works?filter=doi:" + enc(f) + "&per-page=" + std::to_string(std::max<size_t>(1, dois.size())) +
             "&select=id,doi,display_name,open_access,best_oa_location,locations,primary_location,ids,has_content,content_urls";
  if (!mail.empty()) u += "&mailto=" + enc(mail);
  return u;
}
string unpaywallUrl(const string& doi, const string& mail) { return "https://api.unpaywall.org/v2/" + normDoi(doi) + "?email=" + enc(mail.empty() ? string("vosstudio@example.org") : mail); }
string semanticScholarUrl(const string& doi) { return "https://api.semanticscholar.org/graph/v1/paper/DOI:" + normDoi(doi) + "?fields=title,isOpenAccess,openAccessPdf,externalIds"; }
string contentUrl(const string& workId) { return "https://content.openalex.org/works/" + workId + ".pdf"; }

// ------------------------------------------------------------------ bytes
bool pdfStart(const char* p, size_t n) {
  size_t lim = std::min<size_t>(n, 1024);
  for (size_t i = 0; i + 5 <= lim; i++) if (memcmp(p + i, "%PDF-", 5) == 0) return true;
  return false;
}
bool looksLikeHtml(const char* p, size_t n) {
  size_t i = 0;
  while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n' || (unsigned char)p[i] == 0xEF || (unsigned char)p[i] == 0xBB || (unsigned char)p[i] == 0xBF)) i++;
  if (i < n && p[i] == '<') return true;
  string head = lower(string(p + i, std::min<size_t>(n - i, 256)));
  return contains(head, "<!doctype") || contains(head, "<html");
}
bool pdfEnd(const char* tail, size_t n) {
  if (n < 5) return false;
  for (size_t i = 0; i + 5 <= n; i++) if (memcmp(tail + i, "%%EOF", 5) == 0) return true;
  return false;
}

RateLimit parseRateLimit(const std::function<string(const string&)>& header) {
  RateLimit rl;
  auto num = [&](const char* name, double def) { string v = trim(header(name)); return v.empty() ? def : toDouble(v, def); };
  rl.remainingUsd = num("X-RateLimit-Remaining-USD", -1);
  rl.costUsd = num("X-RateLimit-Cost-USD", -1);
  rl.remaining = long(num("X-RateLimit-Remaining", -1));
  rl.resetSeconds = long(num("X-RateLimit-Reset", -1));
  rl.retryAfter = long(num("Retry-After", -1));
  return rl;
}

string describe(const vector<Attempt>& tried) {
  string s;
  vector<string> seen;
  for (auto& a : tried) {
    string line = a.host + ": " + a.result;
    if (std::find(seen.begin(), seen.end(), line) != seen.end()) continue;
    seen.push_back(line);
    s += (s.empty() ? "" : "; ") + line;
  }
  return s;
}

// ------------------------------------------------------------------ the outcome
const char* fetchStateName(FetchState s) {
  switch (s) {
    case FetchState::Attached: return "attached";
    case FetchState::Closed: return "closed";
    case FetchState::Refused: return "refused";
    case FetchState::Failed: return "failed";
    case FetchState::NoDoi: return "nodoi";
    case FetchState::Budget: return "budget";
    default: return "";
  }
}
FetchState fetchStateFrom(const string& s) {
  for (int i = 1; i < int(FetchState::Count); i++) if (s == fetchStateName(FetchState(i))) return FetchState(i);
  return FetchState::None;
}
Json FetchStatus::toJson() const {
  Json j = Json::object();
  j.set("state", fetchStateName(state));
  if (!when.empty()) j.set("when", when);
  if (!detail.empty()) j.set("detail", detail);
  if (!landing.empty()) j.set("landing", landing);
  if (!oaLink.empty()) j.set("oaLink", oaLink);
  if (!pmcid.empty()) j.set("pmcid", pmcid);
  if (!oaStatus.empty()) j.set("oaStatus", oaStatus);
  if (cachedAvailable) j.set("cached", true);
  if (!source.empty()) j.set("source", source);
  return j;
}
FetchStatus FetchStatus::fromJson(const Json& j) {
  FetchStatus f;
  if (j.t != Json::Obj) return f;
  f.state = fetchStateFrom(j["state"].str());
  f.when = j["when"].str();
  f.detail = j["detail"].str();
  f.landing = j["landing"].str();
  f.oaLink = j["oaLink"].str();
  f.pmcid = j["pmcid"].str();
  f.oaStatus = j["oaStatus"].str();
  f.cachedAvailable = j["cached"].boolean(false);
  f.source = j["source"].str();
  return f;
}
string FetchStatus::message(bool haveKey) const {
  switch (state) {
    case FetchState::Attached: return source.empty() ? "PDF attached" : "PDF attached from " + source;
    case FetchState::Closed: return "Not openly available" + string(oaStatus.empty() || oaStatus == "closed" ? "" : " (" + oaStatus + ", but no PDF is listed)") + " \xE2\x80\x94 attach the file by hand or open the DOI page (your library's access applies there).";
    case FetchState::Refused:
      if (cachedAvailable && !haveKey) return "Open access, but the site refused the automatic download. OpenAlex has a copy: add a free OpenAlex API key in Preferences and try again \xE2\x80\x94 or open the link in the browser and drop the file here.";
      return "Open access, but the site refused the automatic download \xE2\x80\x94 open the link in the browser and drop the file here." + string(detail.empty() ? "" : " (" + detail + ")");
    case FetchState::Failed: return "The lookup failed" + string(detail.empty() ? "" : ": " + detail) + " \xE2\x80\x94 try again later.";
    case FetchState::NoDoi: return "No DOI on the record: nothing to look up. Attach the file by hand.";
    case FetchState::Budget: return "OpenAlex's daily budget is used up; the cached copy can be fetched after midnight UTC." + string(detail.empty() ? "" : " " + detail);
    default: return "";
  }
}

// ------------------------------------------------------------------ the file name
string attachmentName(const string& author, int year, const string& title, const string& doi) {
  auto clean = [](const string& s, size_t maxLen) {
    string f = asciiFold(s), o;
    for (unsigned char c : f) {
      if (isalnum(c)) o += char(c);
      else if ((c == ' ' || c == '-' || c == '_') && !o.empty() && o.back() != '-') o += '-';
      if (o.size() >= maxLen) break;
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o;
  };
  string sur = author;
  size_t comma = sur.find(',');
  if (comma != string::npos) sur = sur.substr(0, comma);
  else { size_t sp = sur.find(' '); if (sp != string::npos && sp > 1) sur = sur.substr(0, sp); }
  sur = clean(sur, 24);
  string words;
  {
    vector<string> ws = split(lower(asciiFold(title)), ' ');  // fold first: lower() knows only ASCII
    int n = 0;
    for (auto& w : ws) {
      string c = clean(w, 20);
      if (c.empty() || c == "the" || c == "a" || c == "an" || c == "of" || c == "and" || c == "on" || c == "in" || c == "for" || c == "to") continue;
      words += (words.empty() ? "" : "-") + c;
      if (++n >= 4) break;
    }
  }
  uint32_t h = 2166136261u;
  for (unsigned char c : normDoi(doi)) { h ^= c; h *= 16777619u; }
  char hx[8];
  snprintf(hx, 8, "%04x", unsigned((h >> 16) ^ (h & 0xFFFF)));
  string name;
  if (!sur.empty()) name += sur;
  if (year > 0) name += (name.empty() ? "" : "_") + std::to_string(year);
  if (!words.empty()) name += (name.empty() ? "" : "_") + words;
  if (name.empty()) name = "paper";
  return name + "_" + hx + ".pdf";
}

}  // namespace oa
}  // namespace vs
