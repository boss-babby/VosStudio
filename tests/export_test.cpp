// round-trip: sample corpora -> writeRecords (WoS / RIS / CSV) -> parseRecords -> compare key fields
#include <cstdio>
#include <set>
#include "../src/core/records.h"
#include "../src/core/project.h"
#include "../src/core/figure.h"
#include "../src/core/inflate.h"
using namespace vs;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { fails++; printf("FAIL %s\n", msg); } } while (0)
static std::set<string> S(const vector<Record>& v, int what) {
  std::set<string> o;
  for (auto& r : v) {
    if (what == 0) for (auto& x : r.countries) o.insert(x);
    if (what == 1) for (auto& x : r.keywords) o.insert(lower(x));
    if (what == 2) o.insert(r.doi);
  }
  return o;
}
int main() {
  for (int src = 0; src < 2; src++) {
    vector<Record> a;
    string t = src ? sampleScopusExport() : sampleWoSExport();
    parseRecords(t, detectFormat(t, src ? "x.csv" : "x.txt"), a);
    size_t refsA = 0; long cA = 0;
    for (auto& r : a) { refsA += r.refs.size(); cA += r.cites; }
    const char* nm[3] = {"wos", "ris", "csv"};
    for (int f = 0; f < 3; f++) {
      string out = writeRecords(a, RecordExport(f));
      vector<Record> b;
      string fn = string("x.") + (f == 0 ? "txt" : f == 1 ? "ris" : "csv");
      BibFormat bf = detectFormat(out, fn);
      parseRecords(out, bf, b);
      size_t refsB = 0; long cB = 0;
      for (auto& r : b) { refsB += r.refs.size(); cB += r.cites; }
      printf("%s -> %s: fmt=%s recs %zu/%zu refs %zu/%zu cites %ld/%ld countries %zu/%zu kw %zu/%zu\n", src ? "scopus" : "wos", nm[f], formatLabel(bf), b.size(), a.size(), refsB, refsA, cB, cA,
             S(b, 0).size(), S(a, 0).size(), S(b, 1).size(), S(a, 1).size());
      char m[64];
      snprintf(m, 64, "%d/%s count", src, nm[f]); CHECK(b.size() == a.size(), m);
      snprintf(m, 64, "%d/%s countries", src, nm[f]); CHECK(S(b, 0) == S(a, 0), m);
      snprintf(m, 64, "%d/%s dois", src, nm[f]); CHECK(S(b, 2) == S(a, 2), m);
      if (f != 1) { snprintf(m, 64, "%d/%s refs", src, nm[f]); CHECK(refsB == refsA, m); snprintf(m, 64, "%d/%s cites", src, nm[f]); CHECK(cB == cA, m); }
    }
  }
  {  // Stable local identity, added metadata, and arbitrary fields round-trip through the three ID-aware formats.
    Record r;
    r.id = "rec:roundtrip-123"; r.key = "source-key-7"; r.title = "Field-preserving export"; r.authors = {"Doe, J", "Smith, A"}; r.authorIds = {"orcid:0000-0001-2345-6789", "scopus:123456"}; r.year = 2024;
    r.source = "Journal of Tests"; r.volume = "12"; r.issue = "3"; r.pages = "22-34"; r.doi = "10.5555/example"; r.url = "https://example.org/paper";
    r.publisher = "Testing Press"; r.language = "English"; r.abstract_ = "Line one. Line two."; r.keywords = {"round trip", "metadata"}; r.indexTerms = {"index term one", "index term two"};
    r.refs = {"Smith J, 2020, Journal A", "Doe A, 2021, Journal B"}; r.cites = 7;
    r.extra["PMID"] = "12345"; r.extra["custom note"] = "first line, with comma\nsecond line"; r.extra["arbitrary_custom%field&"] = "value with {braces}, %, and &";
    for (RecordExport f : {RecordExport::RIS, RecordExport::BibTeX, RecordExport::CSV}) {
      string text = writeRecords({r}, f);
      vector<Record> parsed;
      string ext = f == RecordExport::RIS ? "x.ris" : f == RecordExport::BibTeX ? "x.bib" : "x.csv";
      BibFormat format = detectFormat(text, ext);
      CHECK(parseRecords(text, format, parsed) == 1, "ID-aware format parsed");
      CHECK(parsed.size() == 1 && parsed[0].id == r.id, "stable ID preserved");
      CHECK(parsed[0].key == r.key && parsed[0].issue == r.issue && parsed[0].pages == r.pages, "source key, issue, and page range preserved");
      CHECK(parsed[0].url == r.url && parsed[0].extra == r.extra, "URL and arbitrary fields preserved");
      CHECK(parsed[0].title == r.title && parsed[0].authors == r.authors && parsed[0].authorIds == r.authorIds, "title, authors, and author IDs preserved");
      CHECK(parsed[0].year == r.year && parsed[0].source == r.source && parsed[0].doi == r.doi, "year, source, and DOI preserved");
      CHECK(parsed[0].publisher == r.publisher && parsed[0].language == r.language && parsed[0].abstract_ == r.abstract_, "publisher, language, and abstract preserved");
      CHECK(parsed[0].keywords == r.keywords && parsed[0].indexTerms == r.indexTerms && parsed[0].refs == r.refs && parsed[0].cites == r.cites, "keywords, index terms, references, and citation count preserved");
    }
  }
  {  // Import-format fixture matrix for field mapping and unknown-field retention.
    string wos = "PT J\nAU Doe, J.\nTI WoS fixture\nSO Journal A\nPY 2020\nIS 4\nBP 10-18\nDI 10.5555/wos\nUR https://example.org/wos\nUT WOS-1\nZZ vendor value\nER\n\n";
    vector<Record> parsed;
    CHECK(detectFormat(wos, "wos.txt") == BibFormat::WoS, "WoS plain detected");
    CHECK(parseRecords(wos, BibFormat::WoS, parsed) == 1 && parsed.size() == 1, "WoS plain fixture parsed");
    CHECK(parsed.size() == 1 && parsed[0].issue == "4" && parsed[0].pages == "10-18" && parsed[0].url == "https://example.org/wos", "WoS plain issue/pages/URL mapping");
    CHECK(parsed.size() == 1 && parsed[0].key == "WOS-1" && parsed[0].extra["ZZ"] == "vendor value" && !parsed[0].id.empty(), "WoS plain source key/unknown field/local ID");

    string wosTab = "PT\tAU\tTI\tPY\tDI\tIS\tBP\tURL\tUT\tVOSStudio Record ID\tCustom Column\nJ\tDoe J.\tWoS tab fixture\t2022\t10.5555/tab\t2\t3-8\thttps://example.org/tab\tUT-2\trec:tab-fixture\tcustom value\n";
    parsed.clear();
    CHECK(detectFormat(wosTab, "wos-tab.txt") == BibFormat::WoSTab, "WoS tab detected");
    CHECK(parseRecords(wosTab, BibFormat::WoSTab, parsed) == 1 && parsed.size() == 1, "WoS tab fixture parsed");
    CHECK(parsed.size() == 1 && parsed[0].id == "rec:tab-fixture" && parsed[0].key == "UT-2" && parsed[0].issue == "2" && parsed[0].pages == "3-8" && parsed[0].url == "https://example.org/tab", "WoS tab local ID and standard metadata mapping");
    CHECK(parsed.size() == 1 && parsed[0].extra["Custom Column"] == "custom value", "WoS tab unknown column retained");

    auto csvQuote = [](const string& value) { return "\"" + replaceAll(value, "\"", "\"\"") + "\""; };
    vector<string> scopusCells = {"Doe J.", "Doe, Jane", "Scopus fixture", "2023", "Journal B", "5", "12-19", "10.5555/scopus", "https://example.org/scopus", "EID-3", "rec:scopus-fixture", "", "{\"PMID\":\"777\"}", "provider-specific value"};
    string scopus = "Authors,Author full names,Title,Year,Source title,Issue,Page start,DOI,URL,EID,VOSStudio Record ID,VOSStudio Author IDs,VOSStudio Extra Fields,Provider Field\n";
    for (size_t i = 0; i < scopusCells.size(); i++) scopus += (i ? "," : "") + csvQuote(scopusCells[i]);
    scopus += "\n";
    parsed.clear();
    CHECK(detectFormat(scopus, "scopus.csv") == BibFormat::Scopus, "Scopus CSV detected");
    CHECK(parseRecords(scopus, BibFormat::Scopus, parsed) == 1 && parsed.size() == 1, "Scopus CSV fixture parsed");
    CHECK(parsed.size() == 1 && parsed[0].id == "rec:scopus-fixture" && parsed[0].key == "EID-3" && parsed[0].issue == "5" && parsed[0].pages == "12-19" && parsed[0].url == "https://example.org/scopus", "Scopus custom ID and standard metadata mapping");
    CHECK(parsed.size() == 1 && parsed[0].authors == vector<string>{"Doe, Jane"} && parsed[0].extra["PMID"] == "777" && parsed[0].extra["Provider Field"] == "provider-specific value", "Scopus author parsing and known/unknown extra fields");

    string openAlex = R"({"results":[{"id":"https://openalex.org/W123","title":"OpenAlex fixture","publication_year":2021,"doi":"https://doi.org/10.5555/openalex","cited_by_count":9,"type":"article","language":"en","primary_location":{"landing_page_url":"https://example.org/openalex","source":{"display_name":"Journal C"}},"biblio":{"volume":"7","issue":"1","first_page":"20","last_page":"29"},"authorships":[{"author":{"id":"https://openalex.org/A123","orcid":"https://orcid.org/0000-0001-2345-6789","display_name":"Doe Jane"},"institutions":[]}],"keywords":[{"display_name":"open data"}],"concepts":[],"referenced_works":[],"abstract_inverted_index":{},"vendor_field":{"nested":true}}]})";
    parsed.clear();
    CHECK(detectFormat(openAlex, "openalex.json") == BibFormat::OpenAlex, "OpenAlex JSON detected");
    CHECK(parseRecords(openAlex, BibFormat::OpenAlex, parsed) == 1 && parsed.size() == 1, "OpenAlex JSON fixture parsed");
    CHECK(parsed.size() == 1 && parsed[0].key == "https://openalex.org/W123" && !parsed[0].id.empty() && parsed[0].issue == "1" && parsed[0].pages == "20-29" && parsed[0].url == "https://example.org/openalex", "OpenAlex source/local ID, issue/pages/URL mapping");
    CHECK(parsed.size() == 1 && parsed[0].authorIds == vector<string>{"0000-0001-2345-6789"} && parsed[0].extra["vendor_field"] == "{\"nested\":true}", "OpenAlex author ID and unknown field retained");

    string malformed = "TY  - JOUR\nTI  - Malformed extension\nX2  - VOSStudioAuthorIds:V1:not-json\nX3  - VOSStudioIndexTerms:bad-payload\nER  - \n";
    parsed.clear();
    CHECK(parseRecords(malformed, BibFormat::RIS, parsed) == 1 && parsed.size() == 1, "malformed VOSStudio extension record still imports");
    CHECK(parsed.size() == 1 && parsed[0].extra["X2"] == "VOSStudioAuthorIds:V1:not-json" && parsed[0].extra["X3"] == "VOSStudioIndexTerms:bad-payload", "malformed list extensions remain visible as raw metadata");
  }
  {  // Web of Science export is a compatibility boundary: keep the legacy tag layout and do not add local fields.
    Record r; r.id = "rec:must-not-leak"; r.title = "Title"; r.source = "Journal of Tests"; r.year = 2024; r.volume = "12"; r.issue = "3"; r.pages = "22-34"; r.doi = "10.5555/example";
    r.url = "https://example.org/paper"; r.key = "source-key"; r.extra["vendor field"] = "value";
    string expected = "FN Clarivate Analytics Web of Science\nVR 1.0\nPT J\nTI Title\nSO JOURNAL OF TESTS\nNR 0\nTC 0\nZ9 0\nPY 2024\nVL 12\nBP 22-34\nDI 10.5555/example\nUT source-key\nER\n\nEF\n";
    CHECK(writeRecords({r}, RecordExport::WoS) == expected, "Web of Science export remains byte-compatible and omits VOSStudio-only fields");
  }
  {  // 1.5: one page per panel
    Project F; F.addSample(false); F.spec.type = AnaType::Cooc; F.spec.unit = Unit::Keywords; F.spec.setDefaults(); F.build();
    FigureSpec fs; fs.panelNetwork = fs.panelOverlay = fs.panelDensity = true; fs.transparent = true;
    auto pages = buildFigurePages(F.net, F.style, fs, nullptr);
    string pdf = toPDF(pages, "pages");
    writeFile("/tmp/pages.pdf", pdf);
    CHECK(pages.size() == 3, "pages count");
    CHECK(pdf.find("/Count 3") != string::npos, "pdf /Count 3");
    CHECK(pages.size() == 3 && pages[0].W == pages[2].W, "pages same size");
    bool bg = false; for (auto& p : pages[0].items) if (p.group == "background") bg = true;
    CHECK(!bg, "transparent page has no background");
    CHECK(toPDF(pages[0]).find("/Count 1") != string::npos, "single page");
    printf("multi-page pdf: %zu pages, %zu bytes\n", pages.size(), pdf.size());
  }
  {  // hybrid serialization: embedded artwork layers stay interleaved with native, linked text
    Scene sc; sc.W = 80; sc.H = 40;
    Prim a; a.type = Prim::Image; a.group = "hybrid-artwork"; a.w = 10; a.h = 10; a.imgW = a.imgH = 1; a.rgba = {220, 20, 30, 255};
    Prim t; t.type = Prim::Text; t.x = 12; t.y = 20; t.text = "Native hybrid label"; t.size = 10; t.fillC = Color(0, 0, 0); t.href = "https://example.org/label";
    Prim b = a; b.x = 30; b.y = 15; b.rgba = {0, 120, 220, 128};
    sc.items = {a, t, b};
    string svg = toSVG(sc);
    size_t imageA = svg.find("<image"), text = svg.find("<text"), imageB = svg.find("<image", imageA + 1);
    CHECK(imageA != string::npos && text != string::npos && imageB != string::npos, "hybrid svg contains images and native text");
    CHECK(imageA < text && text < imageB, "hybrid svg preserves paint order");
    CHECK(svg.find("data:image/png;base64,") != string::npos && svg.find("<a href=\"https://example.org/label\">") != string::npos, "hybrid svg embeds art and retains text link");
    string pdf = toPDF(sc, "hybrid");
    CHECK(pdf.find("/Subtype /Image") != string::npos && pdf.find("/Subtype /Link") != string::npos, "hybrid pdf has embedded image and link annotation");
    string content; size_t at = 0;
    while ((at = pdf.find("\nstream\n", at)) != string::npos) {
      size_t begin = at + 8, end = pdf.find("\nendstream", begin);
      if (end == string::npos) break;
      string decoded;
      if (inflateZlib(reinterpret_cast<const uint8_t*>(pdf.data() + begin), end - begin, decoded)) content += decoded;
      at = end + 10;
    }
    size_t pdfImageA = content.find("/Im1 Do"), pdfText = content.find("(Native hybrid label) Tj"), pdfImageB = content.find("/Im2 Do");
    CHECK(pdfImageA != string::npos && pdfText != string::npos && pdfImageB != string::npos, "hybrid pdf keeps text as native content");
    CHECK(pdfImageA < pdfText && pdfText < pdfImageB, "hybrid pdf preserves paint order");
  }
  CHECK(wosAuthor("Boris A. Petrov") == "Petrov, BA", "author1");
  CHECK(wosAuthor("Petrov B") == "Petrov, B", "author2");
  CHECK(wosAuthor("Jean-Luc Picard") == "Picard, JL", "author3");
  printf("fails=%d\n", fails);
  return fails ? 1 : 0;
}
