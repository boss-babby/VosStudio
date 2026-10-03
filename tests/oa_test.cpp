// oa_test: the pure part of Get PDF — parsing the services' answers (shapes as recorded live on 30 September 2026,
// docs/OA-FETCH-TEST.md), the order of attack, the acceptance of bytes, the budget headers, the outcome model and the
// attachment names. No network.
#include <iostream>

#include "library.h"
#include "oa.h"

using namespace vs;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; std::cerr << "FAIL " << __LINE__ << ": " #c << "\n"; } } while (0)

// an OpenAlex works list answer (filter=doi:a|b): a gold PLOS article with a cached copy, a bronze Nature article
// whose publisher PDF sits behind a cookie wall but which has an arXiv copy, and a closed Elsevier article
static const char* kOpenAlex = R"json({"meta":{"count":3},"results":[
 {"id":"https://openalex.org/W2550066059","doi":"https://doi.org/10.1371/journal.pone.0166578","display_name":"Core set",
  "open_access":{"is_oa":true,"oa_status":"gold","oa_url":"https://journals.plos.org/plosone/article/file?id=10.1371/journal.pone.0166578&type=printable"},
  "best_oa_location":{"is_oa":true,"pdf_url":"https://journals.plos.org/plosone/article/file?id=10.1371/journal.pone.0166578&type=printable","landing_page_url":"https://doi.org/10.1371/journal.pone.0166578","version":"publishedVersion","license":"cc-by","source":{"display_name":"PLoS ONE","type":"journal"}},
  "locations":[{"is_oa":true,"pdf_url":"https://journals.plos.org/plosone/article/file?id=10.1371/journal.pone.0166578&type=printable","landing_page_url":"https://doi.org/10.1371/journal.pone.0166578","version":"publishedVersion","license":"cc-by","source":{"display_name":"PLoS ONE","type":"journal"}},
               {"is_oa":true,"pdf_url":null,"landing_page_url":"https://europepmc.org/articles/pmc5113961","version":"publishedVersion","source":{"display_name":"Europe PMC (PubMed Central)","type":"repository"}},
               {"is_oa":true,"pdf_url":"https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5113961","landing_page_url":"https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5113961","version":"publishedVersion","source":{"display_name":"PubMed Central","type":"repository"}}],
  "primary_location":{"landing_page_url":"https://doi.org/10.1371/journal.pone.0166578"},
  "ids":{"openalex":"https://openalex.org/W2550066059","doi":"https://doi.org/10.1371/journal.pone.0166578","pmid":"https://pubmed.ncbi.nlm.nih.gov/27851822","pmcid":"https://www.ncbi.nlm.nih.gov/pmc/articles/5113961"},
  "has_content":{"pdf":true,"grobid_xml":true},"content_urls":{"pdf":"https://content.openalex.org/works/W2550066059.pdf","grobid_xml":"https://content.openalex.org/works/W2550066059.grobid-xml"}},
 {"id":"https://openalex.org/W2159974629","doi":"https://doi.org/10.1038/nature12373","display_name":"Nanometre-scale thermometry in a living cell",
  "open_access":{"is_oa":true,"oa_status":"bronze"},
  "best_oa_location":{"is_oa":true,"pdf_url":"https://www.nature.com/articles/nature12373.pdf","landing_page_url":"https://doi.org/10.1038/nature12373","version":"publishedVersion","license":null,"source":{"display_name":"Nature","type":"journal"}},
  "locations":[{"is_oa":true,"pdf_url":"https://www.nature.com/articles/nature12373.pdf","landing_page_url":"https://doi.org/10.1038/nature12373","version":"publishedVersion","source":{"display_name":"Nature","type":"journal"}},
               {"is_oa":true,"pdf_url":"https://arxiv.org/pdf/1304.1068","landing_page_url":"https://arxiv.org/abs/1304.1068","version":"submittedVersion","source":{"display_name":"arXiv (Cornell University)","type":"repository"}},
               {"is_oa":true,"pdf_url":"https://dash.harvard.edu/bitstream/1/12967848/1/nature12373.pdf","landing_page_url":"https://dash.harvard.edu/handle/1/12967848","version":"acceptedVersion","source":{"display_name":"DASH (Harvard)","type":"repository"}}],
  "ids":{"openalex":"https://openalex.org/W2159974629"},"has_content":{"pdf":true,"grobid_xml":true},"content_urls":{"pdf":"https://content.openalex.org/works/W2159974629.pdf"}},
 {"id":"https://openalex.org/W2755950973","doi":"https://doi.org/10.1016/j.joi.2017.08.007","display_name":"bibliometrix",
  "open_access":{"is_oa":false,"oa_status":"closed","oa_url":null},"best_oa_location":null,"locations":[{"is_oa":false,"pdf_url":null,"landing_page_url":"https://doi.org/10.1016/j.joi.2017.08.007","source":{"display_name":"Journal of Informetrics","type":"journal"}}],
  "primary_location":{"landing_page_url":"https://doi.org/10.1016/j.joi.2017.08.007"},"ids":{"openalex":"https://openalex.org/W2755950973"},"has_content":{"pdf":false,"grobid_xml":false},"content_urls":null}
]})json";

static const char* kUnpaywall = R"json({"doi":"10.1038/nature12373","is_oa":true,"oa_status":"bronze","title":"Nanometre-scale thermometry in a living cell",
 "best_oa_location":{"url":"https://www.nature.com/articles/nature12373.pdf","url_for_pdf":"https://www.nature.com/articles/nature12373.pdf","url_for_landing_page":"https://doi.org/10.1038/nature12373","host_type":"publisher","version":"publishedVersion","license":null},
 "oa_locations":[{"url_for_pdf":"https://www.nature.com/articles/nature12373.pdf","host_type":"publisher","version":"publishedVersion"},
                 {"url_for_pdf":"https://europepmc.org/articles/pmc4221854?pdf=render","url_for_landing_page":"https://europepmc.org/articles/pmc4221854","host_type":"repository","version":"acceptedVersion"},
                 {"url_for_pdf":"http://arxiv.org/pdf/1304.1068","host_type":"repository","version":"submittedVersion"}]})json";
static const char* kUnpaywallClosed = R"json({"doi":"10.1016/j.joi.2017.08.007","is_oa":false,"oa_status":"closed","best_oa_location":null,"oa_locations":[]})json";
static const char* kS2 = R"json({"paperId":"abc","isOpenAccess":true,"openAccessPdf":{"url":"https://www.nature.com/articles/nature12373.pdf","status":"BRONZE","license":null},"externalIds":{"ArXiv":"1304.1068","PubMedCentral":"4221854","PubMed":"23903748","DOI":"10.1038/nature12373"}})json";

int main() {
  // ---- DOIs
  CHECK(oa::normDoi("https://doi.org/10.1371/Journal.PONE.0166578") == "10.1371/journal.pone.0166578");
  CHECK(oa::normDoi(" doi:10.1000/ABC ") == "10.1000/abc" && oa::normDoi("http://dx.doi.org/10.1/x") == "10.1/x");
  string ax;
  CHECK(oa::arxivIdFromDoi("10.48550/arXiv.2303.08774", &ax) && ax == "2303.08774");
  CHECK(!oa::arxivIdFromDoi("10.1038/nature12373", &ax));
  CHECK(oa::hostOf("https://www.nature.com/articles/nature12373.pdf") == "nature.com" && oa::hostOf("HTTPS://Api.OpenAlex.org:443/x?y") == "api.openalex.org" && oa::hostOf("not a url").empty());

  // ---- OpenAlex batch
  std::map<string, oa::Resolved> m;
  string err;
  Json js = Json::parse(kOpenAlex, &err);
  CHECK(err.empty());
  oa::parseOpenAlexWorks(js, m);
  CHECK(m.size() == 3);
  oa::Resolved& plos = m["10.1371/journal.pone.0166578"];
  CHECK(plos.known && plos.isOa && plos.oaStatus == "gold" && plos.workId == "W2550066059" && plos.cached && plos.cacheUrl == "https://content.openalex.org/works/W2550066059.pdf");
  CHECK(plos.pmcid == "5113961" || plos.pmcid == "PMC5113961");
  CHECK(plos.candidates.size() == 2);  // the PLOS file and the PMC page; the location without pdf_url is not a candidate
  CHECK(plos.candidates[0].kind == "publisher" && plos.candidates[0].host == "PLoS ONE" && plos.candidates[0].version == "publishedVersion" && plos.candidates[0].license == "cc-by");
  CHECK(plos.candidates[1].kind == "pmc");
  CHECK(plos.landing == "https://doi.org/10.1371/journal.pone.0166578");
  oa::Resolved& nat = m["10.1038/nature12373"];
  CHECK(nat.isOa && nat.oaStatus == "bronze" && nat.cached && nat.arxivId == "1304.1068" && nat.candidates.size() == 3);
  oa::Resolved& closed = m["10.1016/j.joi.2017.08.007"];
  CHECK(closed.known && !closed.isOa && closed.oaStatus == "closed" && !closed.cached && closed.candidates.empty() && closed.landing == "https://doi.org/10.1016/j.joi.2017.08.007");

  // ---- Unpaywall adds locations (de-duplicated by URL), Semantic Scholar adds ids
  oa::parseUnpaywall(Json::parse(kUnpaywall), nat);
  CHECK(nat.candidates.size() == 5);  // + Europe PMC render + http://arxiv (a different URL from the https one)
  oa::parseSemanticScholar(Json::parse(kS2), nat);
  CHECK(nat.pmcid == "PMC4221854" && nat.arxivId == "1304.1068" && nat.candidates.size() == 5);  // S2's URL is the Nature one again
  oa::Resolved unp;
  unp.doi = "10.1016/j.joi.2017.08.007";
  oa::parseUnpaywall(Json::parse(kUnpaywallClosed), unp);
  CHECK(unp.known && !unp.isOa && unp.oaStatus == "closed" && unp.candidates.empty() && unp.landing == "https://doi.org/10.1016/j.joi.2017.08.007");

  // ---- the order of attack
  oa::order(nat, /*haveKey*/ true);
  CHECK(nat.candidates.size() == 6 && nat.candidates[0].kind == "cache" && nat.candidates[0].url == "https://content.openalex.org/works/W2159974629.pdf");
  CHECK(nat.candidates[1].host == "Nature" && nat.candidates[1].version == "publishedVersion");      // the publisher's published version
  CHECK(nat.candidates[2].host == "DASH (Harvard)" && nat.candidates[2].version == "acceptedVersion");  // then the accepted manuscript
  {  // arXiv (both URLs) before PMC, PMC last
    bool arxivBeforePmc = true;
    size_t lastArxiv = 0, firstPmc = 99;
    for (size_t i = 0; i < nat.candidates.size(); i++) { if (nat.candidates[i].kind == "arxiv") lastArxiv = i; if (nat.candidates[i].kind == "pmc") firstPmc = std::min(firstPmc, i); }
    arxivBeforePmc = lastArxiv < firstPmc;
    CHECK(arxivBeforePmc && nat.candidates.back().kind == "pmc");
  }
  oa::order(nat, /*haveKey*/ false);
  CHECK(nat.candidates.size() == 5 && nat.candidates[0].host == "Nature");  // no key: no cache entry
  oa::order(nat, true);
  oa::order(nat, true);
  CHECK(nat.candidates.size() == 6);  // idempotent
  // the cache is tried for an open work even when has_content lags (the Heliyon case)
  oa::Resolved lag;
  lag.doi = "10.1016/j.heliyon.2024.e38071"; lag.workId = "W4402851183"; lag.known = true; lag.isOa = true; lag.oaStatus = "gold";
  oa::order(lag, true);
  CHECK(lag.candidates.size() == 1 && lag.candidates[0].kind == "cache" && lag.candidates[0].url == "https://content.openalex.org/works/W4402851183.pdf");
  oa::order(closed, true);
  CHECK(closed.candidates.empty());  // a closed work gets nothing, key or not
  // an arXiv DOI resolves without any service
  oa::Resolved arx;
  arx.doi = "10.48550/arxiv.2303.08774";
  oa::order(arx, false);
  CHECK(arx.known && arx.isOa && arx.candidates.size() == 1 && arx.candidates[0].url == "https://arxiv.org/pdf/2303.08774" && arx.landing == "https://arxiv.org/abs/2303.08774");

  // ---- request URLs
  string bu = oa::openAlexBatchUrl({"10.1371/journal.pone.0166578", "https://doi.org/10.1038/NATURE12373"}, "me@example.org");
  CHECK(startsWith(bu, "https://api.openalex.org/works?filter=doi:10.1371%2Fjournal.pone.0166578%7C10.1038%2Fnature12373&per-page=2&select=") && contains(bu, "has_content,content_urls") && contains(bu, "mailto=me%40example.org"));
  CHECK(oa::unpaywallUrl("10.1038/nature12373", "me@example.org") == "https://api.unpaywall.org/v2/10.1038/nature12373?email=me%40example.org");
  CHECK(oa::semanticScholarUrl("10.1038/nature12373") == "https://api.semanticscholar.org/graph/v1/paper/DOI:10.1038/nature12373?fields=title,isOpenAccess,openAccessPdf,externalIds");

  // ---- bytes
  CHECK(oa::pdfStart("%PDF-1.7\n%\xE2\xE3\xCF\xD3", 13));
  CHECK(oa::pdfStart("\xEF\xBB\xBF\r\n%PDF-1.4", 12));
  CHECK(!oa::pdfStart("<!DOCTYPE html><html>", 21) && !oa::pdfStart("{\"error\":\"API key required\"}", 28) && !oa::pdfStart("%PD", 3));
  CHECK(oa::looksLikeHtml("\n\n<!DOCTYPE html>", 17) && oa::looksLikeHtml("  <html lang=en>", 16) && !oa::looksLikeHtml("%PDF-1.7", 8) && !oa::looksLikeHtml("{\"a\":1}", 7));
  CHECK(oa::pdfEnd("startxref\n116\n%%EOF\n", 20) && !oa::pdfEnd("1321 0 obj\r<< /T 7852702", 24));

  // ---- the budget headers (as OpenAlex sent them)
  std::map<string, string> h = {{"x-ratelimit-remaining-usd", "0.99"}, {"x-ratelimit-cost-usd", "0.01"}, {"x-ratelimit-remaining", "9900"}, {"x-ratelimit-reset", "70873"}};
  oa::RateLimit rl = oa::parseRateLimit([&](const string& n) { auto it = h.find(lower(n)); return it == h.end() ? string() : it->second; });
  CHECK(rl.known() && rl.remainingUsd > 0.98 && rl.remainingUsd < 1.0 && rl.remaining == 9900 && rl.resetSeconds == 70873 && rl.retryAfter == -1 && rl.costUsd > 0.009);
  oa::RateLimit none = oa::parseRateLimit([](const string&) { return string(); });
  CHECK(!none.known());

  // ---- notices
  string d = oa::describe({{"onlinelibrary.wiley.com", "publisher", "HTTP 403"}, {"pmc.ncbi.nlm.nih.gov", "pmc", "an HTML page"}, {"onlinelibrary.wiley.com", "publisher", "HTTP 403"}});
  CHECK(d == "onlinelibrary.wiley.com: HTTP 403; pmc.ncbi.nlm.nih.gov: an HTML page");
  oa::FetchStatus f;
  f.state = oa::FetchState::Refused; f.when = "2026-09-30T10:00:00Z"; f.detail = d; f.landing = "https://doi.org/10.1002/x"; f.oaLink = "https://onlinelibrary.wiley.com/doi/pdf/10.1002/x"; f.cachedAvailable = true; f.oaStatus = "hybrid";
  CHECK(contains(f.message(false), "add a free OpenAlex API key") && !contains(f.message(true), "API key") && contains(f.message(true), "wiley"));
  Json fj = f.toJson();
  oa::FetchStatus back = oa::FetchStatus::fromJson(fj);
  CHECK(back.state == oa::FetchState::Refused && back.detail == d && back.cachedAvailable && back.oaLink == f.oaLink && back.oaStatus == "hybrid" && back.when == f.when && back.needsFile());
  CHECK(oa::fetchStateFrom("attached") == oa::FetchState::Attached && oa::fetchStateFrom("junk") == oa::FetchState::None && string(oa::fetchStateName(oa::FetchState::Budget)) == "budget");
  oa::FetchStatus att; att.state = oa::FetchState::Attached; att.source = "arXiv";
  CHECK(!att.needsFile() && att.message(false) == "PDF attached from arXiv");
  oa::FetchStatus cl; cl.state = oa::FetchState::Closed; cl.oaStatus = "closed";
  CHECK(contains(cl.message(false), "Not openly available") && !contains(cl.message(false), "closed,"));

  // ---- the library keeps the source and the outcomes
  {
    PdfLibrary lib;
    PdfItem& it = lib.add("C:\\proj\\attachments\\Aria_2017_bibliometrix_3f9a.pdf");
    it.source.url = "https://arxiv.org/pdf/1704.0001"; it.source.host = "arXiv"; it.source.kind = "arxiv"; it.source.version = "submittedVersion"; it.source.oaStatus = "green"; it.source.fetched = "2026-09-30T10:12:00Z";
    CHECK(it.source.versionLabel() == "preprint");
    lib.fetch["k1"] = f;
    oa::FetchStatus none2; lib.fetch["k2"] = none2;  // never written
    Json j = lib.toJson();
    PdfLibrary lib2;
    lib2.fromJson(j);
    CHECK(lib2.items.size() == 1 && lib2.items[0].source.host == "arXiv" && lib2.items[0].source.version == "submittedVersion" && lib2.items[0].source.fetched == "2026-09-30T10:12:00Z");
    CHECK(lib2.fetch.size() == 1 && lib2.fetch.count("k1") && lib2.fetch["k1"].state == oa::FetchState::Refused && lib2.fetch["k1"].cachedAvailable);
    PdfLibrary lib3;
    lib3.fromJson(Json::parse("{\"nextId\":1,\"items\":[]}"));  // an older project: no fetch key
    CHECK(lib3.fetch.empty());
  }

  // ---- file names
  CHECK(startsWith(oa::attachmentName("Aria, Massimo", 2017, "bibliometrix: An R-tool for comprehensive science mapping analysis", "10.1016/j.joi.2017.08.007"), "Aria_2017_bibliometrix-r-tool-comprehensive-science_"));
  string n1 = oa::attachmentName("van Eck NJ", 2010, "Software survey: VOSviewer, a computer program for bibliometric mapping", "10.1007/s11192-009-0146-3");
  CHECK(startsWith(n1, "van_2010_software-survey-vosviewer-computer_") && endsWith(n1, ".pdf") && n1.size() < 90);
  string n2 = oa::attachmentName("Müller, Jörg", 0, "Über die Größe", "10.1/x");
  CHECK(startsWith(n2, "Muller_uber-die-grosse_"));
  string n3 = oa::attachmentName("", 0, "", "10.1/y");
  CHECK(startsWith(n3, "paper_") && n3.size() == string("paper_0000.pdf").size());
  CHECK(oa::attachmentName("A", 1, "t", "10.1/y") != oa::attachmentName("A", 1, "t", "10.1/z"));  // the hash tells DOIs apart
  for (char c : n1) CHECK(c != '/' && c != '\\' && c != ':' && c != '*' && c != '?' && c != '"' && c != '<' && c != '>' && c != '|' && c != ' ');

  if (fails) { std::cerr << fails << " check(s) failed\n"; return 1; }
  std::cout << "oa_test: all checks passed\n";
  return 0;
}
