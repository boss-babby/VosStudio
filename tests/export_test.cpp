// round-trip: sample corpora -> writeRecords (WoS / RIS / CSV) -> parseRecords -> compare key fields
#include <cstdio>
#include <set>
#include "../src/core/records.h"
#include "../src/core/project.h"
#include "../src/core/figure.h"
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
  CHECK(wosAuthor("Boris A. Petrov") == "Petrov, BA", "author1");
  CHECK(wosAuthor("Petrov B") == "Petrov, B", "author2");
  CHECK(wosAuthor("Jean-Luc Picard") == "Picard, JL", "author3");
  printf("fails=%d\n", fails);
  return fails ? 1 : 0;
}
