// Open-access PDF fetching by DOI — the pure part: what the services say about a DOI (OpenAlex, Unpaywall,
// Semantic Scholar), the order in which the copies are tried, the rule that decides whether an answer is a PDF, the
// per-record outcome that is kept in the project, and the attachment's file name. No network here: the Windows side
// (src/win/fetch.cpp) does the requests and hands the JSON and the bytes to these functions, and tests/oa_test.cpp
// runs them on the answers recorded in docs/oa-probe-results.json / docs/OA-FETCH-TEST.md (30 September 2026).
#pragma once
#include <functional>
#include <map>

#include "common.h"
#include "json.h"

namespace vs {
namespace oa {

// "https://doi.org/10.1000/ABC" / "doi:10.1000/abc" / " 10.1000/abc " -> "10.1000/abc"
string normDoi(string s);
// arXiv's DataCite DOIs (10.48550/arXiv.2303.08774) are unknown to every lookup service; the id is in the DOI itself
bool arxivIdFromDoi(const string& doi, string* id);
string hostOf(const string& url);  // "www." stripped, lower-case; "" when not a URL

struct Candidate {
  string url;
  string kind;     // "cache" (OpenAlex's copy), "publisher", "repository", "arxiv", "pmc"
  string host;     // what the user is told: the source's name when known, else the host
  string version;  // publishedVersion | acceptedVersion | submittedVersion | ""
  string license;
};

struct Resolved {
  string doi;         // normalised
  string workId;      // OpenAlex W-id when known
  bool known = false; // at least one service knew the DOI
  bool isOa = false;
  string oaStatus;    // gold | hybrid | bronze | green | closed | ""
  bool cached = false;  // OpenAlex has_content.pdf
  string cacheUrl;      // content_urls.pdf
  string landing;       // the DOI's page (best OA landing page, else https://doi.org/<doi>)
  string arxivId, pmcid, title;
  vector<Candidate> candidates;  // collected by the parsers, put in order by order()
  void add(const Candidate& c);  // de-duplicated by URL
};

// OpenAlex: a works list (works?filter=doi:a|b|...) or a single work; results keyed by normalised DOI
void parseOpenAlexWorks(const Json& js, std::map<string, Resolved>& out);
void parseOpenAlexWork(const Json& w, Resolved& r);
void parseUnpaywall(const Json& js, Resolved& r);        // adds its locations; the status when OpenAlex had none
void parseSemanticScholar(const Json& js, Resolved& r);  // the arXiv id and PMCID above all, its own PDF link too
// The order of attack: OpenAlex's cached copy first when a key is set (no bot walls, $0.01), then the open
// locations — published before accepted before submitted, the publisher before repositories within a version —
// then arXiv, and PubMed Central's endpoints last (they refused every non-browser request in the test).
void order(Resolved& r, bool haveKey, bool preferPublished = true);
// The request URLs (the key goes in a header on the Windows side, never in a URL that could be logged)
string openAlexBatchUrl(const vector<string>& dois, const string& mail);  // <= 50 DOIs
string unpaywallUrl(const string& doi, const string& mail);
string semanticScholarUrl(const string& doi);
string contentUrl(const string& workId);

// Accepting a body: the first bytes decide. "%PDF-" within the first kilobyte (the format allows a little junk
// before the header); an HTML page or JSON is refused before the rest is read.
bool pdfStart(const char* p, size_t n);
bool looksLikeHtml(const char* p, size_t n);
bool pdfEnd(const char* tail, size_t n);  // "%%EOF" in the last bytes — a truncated transfer has none

// OpenAlex's budget, from the response headers (header(name) -> value, "" when absent, case-insensitive names)
struct RateLimit {
  double remainingUsd = -1, costUsd = -1;
  long remaining = -1, resetSeconds = -1, retryAfter = -1;
  bool known() const { return remainingUsd >= 0 || remaining >= 0; }
};
RateLimit parseRateLimit(const std::function<string(const string&)>& header);

// One download attempt, for the notice ("onlinelibrary.wiley.com: HTTP 403; pmc.ncbi.nlm.nih.gov: an HTML page")
struct Attempt { string host, kind, result; };
string describe(const vector<Attempt>& tried);

// The outcome kept per record (PdfLibrary::fetch, keyed by the record key)
enum class FetchState { None = 0, Attached, Closed, Refused, Failed, NoDoi, Budget, Count };
const char* fetchStateName(FetchState s);  // "attached", "closed", "refused", "failed", "nodoi", "budget"
FetchState fetchStateFrom(const string& s);
struct FetchStatus {
  FetchState state = FetchState::None;
  string when;      // ISO 8601
  string detail;    // what happened, for the row note
  string landing;   // the DOI's page
  string oaLink;    // the best open link, for the browser
  string pmcid;
  string oaStatus;  // gold | hybrid | ...
  bool cachedAvailable = false;  // OpenAlex has a copy: a free key would get it
  string source;    // when attached: where the file came from
  Json toJson() const;
  static FetchStatus fromJson(const Json& j);
  // the one-line message for the reading list / papers table (no key: says what a key would do)
  string message(bool haveKey) const;
  bool needsFile() const { return state != FetchState::None && state != FetchState::Attached; }
};

// <Surname>_<Year>_<four-words-of-the-title>_<hash>.pdf — ASCII, Windows-safe, at most ~90 characters
string attachmentName(const string& author, int year, const string& title, const string& doi);

}  // namespace oa
}  // namespace vs
