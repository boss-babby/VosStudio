// pdf_test: the PDF engine wrapper against the real PDFium library (the Linux build of the same release the
// Windows executable embeds). Documents are assembled here by hand — text at known positions, a rotated page, an
// outline, a URI link and an existing highlight — so every coordinate the wrapper reports can be checked against
// what was written, and the rendering against the pixels. Skips (successfully) when libpdfium.so is not present:
//   make third_party/pdfium/linux-x64/lib/libpdfium.so   (tools/fetch-pdfium.sh linux-x64)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <chrono>

#include "common.h"
#include "pdfdoc.h"
#include "pdfium.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace vs;
using namespace vs::pdf;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; std::cerr << "FAIL " << __LINE__ << ": " #c << "\n"; } } while (0)

namespace {
void* resolveSym(void* ctx, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(ctx), name));
#else
  return dlsym(ctx, name);
#endif
}
void* loadLib(const string& path) {
#ifdef _WIN32
  return LoadLibraryA(path.c_str());
#else
  return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

// a tiny PDF writer: objects by number, xref computed
struct Pdf {
  vector<string> objs;
  int add(const string& body) { objs.push_back(body); return int(objs.size()); }
  string build(int root) {
    string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    vector<size_t> offs;
    for (size_t i = 0; i < objs.size(); i++) {
      offs.push_back(out.size());
      out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    size_t xref = out.size();
    char buf[64];
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offs) { snprintf(buf, sizeof buf, "%010zu 00000 n \n", o); out += buf; }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root " + std::to_string(root) + " 0 R /Info " + std::to_string(root + 1) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
  }
};
string stream(const string& dict, const string& data) { return "<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream"; }

// Three pages, 612 x 792 pt as displayed. Page 1: "Hello World" at (72, 700) in 24 pt Helvetica, "Second line here" at (72, 660)
// in 12 pt, a URI link over the first word, one highlight annotation over "World". Page 2: the same content with
// /Rotate 90. Outline: "Start" -> page 1, "Turned" -> page 2. Info: Title / Author.
string samplePdf() {
  Pdf p;
  string content = "BT /F1 24 Tf 72 700 Td (Hello World) Tj ET\nBT /F1 12 Tf 72 660 Td (Second line here) Tj ET\n";
  int font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");          // 1
  int cs = p.add(stream("", content));                                                 // 2
  int pagesN = 3;                                                                      // 3 (pages, filled below)
  p.add("");                                                                           // placeholder 3
  int link = p.add("<< /Type /Annot /Subtype /Link /Rect [72 694 130 722] /Border [0 0 0] /A << /S /URI /URI (https://example.org/hello) >> >>");  // 4
  // "World" in 24 pt Helvetica starts about 72 + width("Hello ") = 72 + 24 * (0.722+0.556+0.222+0.222+0.556+0.278) = 72 + 61.3
  int hl = p.add("<< /Type /Annot /Subtype /Highlight /Rect [133 694 205 722] /C [1 1 0] /QuadPoints [133 722 205 722 133 694 205 694] /Contents (existing note) /T (Tester) >>");  // 5
  int p1 = p.add("<< /Type /Page /Parent 3 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " + std::to_string(font) + " 0 R >> >> /Contents " + std::to_string(cs) + " 0 R /Annots [" + std::to_string(link) + " 0 R " + std::to_string(hl) + " 0 R] >>");  // 6
  int p2 = p.add("<< /Type /Page /Parent 3 0 R /MediaBox [0 0 612 792] /Rotate 90 /Resources << /Font << /F1 " + std::to_string(font) + " 0 R >> >> /Contents " + std::to_string(cs) + " 0 R >>");  // 7
  // page 3: a larger MediaBox cropped to 612 x 792 with an offset origin (50, 60) — coordinates must come out relative to the crop
  int p3 = p.add("<< /Type /Page /Parent 3 0 R /MediaBox [0 0 700 900] /CropBox [50 60 662 852] /Resources << /Font << /F1 " + std::to_string(font) + " 0 R >> >> /Contents " + std::to_string(cs) + " 0 R >>");  // 8
  p.objs[size_t(pagesN - 1)] = "<< /Type /Pages /Kids [" + std::to_string(p1) + " 0 R " + std::to_string(p2) + " 0 R " + std::to_string(p3) + " 0 R] /Count 3 >>";
  int o2 = 0, o1 = 0, outlines = int(p.objs.size()) + 3;
  o1 = p.add("<< /Title (Start) /Parent " + std::to_string(outlines) + " 0 R /Next " + std::to_string(outlines - 1) + " 0 R /Dest [" + std::to_string(p1) + " 0 R /XYZ 0 700 0] >>");  // 8
  o2 = p.add("<< /Title (Turned) /Parent " + std::to_string(outlines) + " 0 R /Prev " + std::to_string(o1) + " 0 R /Dest [" + std::to_string(p2) + " 0 R /Fit] >>");  // 9
  p.add("<< /Type /Outlines /First " + std::to_string(o1) + " 0 R /Last " + std::to_string(o2) + " 0 R /Count 2 >>");  // 10
  int root = p.add("<< /Type /Catalog /Pages 3 0 R /Outlines " + std::to_string(outlines) + " 0 R /PageMode /UseOutlines >>");  // 11
  p.add("<< /Title (Sample paper) /Author (A. Tester) /Producer (pdf_test) >>");  // 12
  return p.build(root);
}

// One page with four images: a colourful "photograph" (noise, RGB), a "plot" (white with two coloured lines, RGB),
// a "scan" (grey, dark strokes on light), and a small RGB checker drawn inside a form XObject placed by a matrix.
string imagesPdf() {
  Pdf p;
  auto raw = [](int w, int h, int comps, const std::function<void(int, int, uint8_t*)>& f) {
    string d(size_t(w) * size_t(h) * size_t(comps), '\0');
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) f(x, y, reinterpret_cast<uint8_t*>(&d[(size_t(y) * size_t(w) + size_t(x)) * size_t(comps)]));
    return d;
  };
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return int(seed >> 24); };
  string photo = raw(96, 64, 3, [&](int x, int y, uint8_t* o) { o[0] = uint8_t((x * 2 + rnd() / 4) & 255); o[1] = uint8_t((y * 3 + rnd() / 4) & 255); o[2] = uint8_t((120 + rnd() / 2) & 255); });
  string plot = raw(120, 80, 3, [&](int x, int y, uint8_t* o) { bool l1 = std::abs(y - (70 - x / 2)) < 2, l2 = std::abs(y - (10 + x / 3)) < 2; o[0] = l1 ? 30 : l2 ? 220 : 255; o[1] = l1 ? 90 : l2 ? 40 : 255; o[2] = l1 ? 220 : l2 ? 40 : 255; });
  string scan = raw(120, 80, 1, [&](int x, int y, uint8_t* o) { o[0] = (y % 12 < 5 && (x / 9) % 3 != 2) ? uint8_t(20 + rnd() / 8) : 245; });
  string nest = raw(20, 20, 3, [&](int x, int y, uint8_t* o) { bool g = ((x / 5) + (y / 5)) % 2 == 0; o[0] = g ? 20 : 255; o[1] = g ? 160 : 255; o[2] = g ? 60 : 255; });
  int font = p.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  int iPhoto = p.add(stream("/Type /XObject /Subtype /Image /Width 96 /Height 64 /ColorSpace /DeviceRGB /BitsPerComponent 8", photo));
  int iPlot = p.add(stream("/Type /XObject /Subtype /Image /Width 120 /Height 80 /ColorSpace /DeviceRGB /BitsPerComponent 8", plot));
  int iScan = p.add(stream("/Type /XObject /Subtype /Image /Width 120 /Height 80 /ColorSpace /DeviceGray /BitsPerComponent 8", scan));
  int iNest = p.add(stream("/Type /XObject /Subtype /Image /Width 20 /Height 20 /ColorSpace /DeviceRGB /BitsPerComponent 8", nest));
  int form = p.add(stream("/Type /XObject /Subtype /Form /BBox [0 0 60 60] /Matrix [1 0 0 1 20 30] /Resources << /XObject << /ImN " + std::to_string(iNest) + " 0 R >> >>", "q 60 0 0 60 0 0 cm /ImN Do Q"));
  string content = "BT /F1 18 Tf 72 740 Td (Images) Tj ET\n0 0 1 rg BT /F1 12 Tf 72 715 Td (blue text) Tj ET\n"
                   "q 160 0 0 120 72 440 cm /ImPhoto Do Q\nq 200 0 0 140 260 430 cm /ImPlot Do Q\nq 240 0 0 160 72 240 cm /ImScan Do Q\nq 1 0 0 1 340 260 cm /Fx Do Q\n";
  int cs = p.add(stream("", content));
  int pages = int(p.objs.size()) + 2;
  int pg = p.add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 612 792] /Contents " + std::to_string(cs) + " 0 R /Resources << /Font << /F1 " + std::to_string(font) +
                 " 0 R >> /XObject << /ImPhoto " + std::to_string(iPhoto) + " 0 R /ImPlot " + std::to_string(iPlot) + " 0 R /ImScan " + std::to_string(iScan) + " 0 R /Fx " + std::to_string(form) + " 0 R >> >> >>");
  p.add("<< /Type /Pages /Kids [" + std::to_string(pg) + " 0 R] /Count 1 >>");
  int root = p.add("<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
  p.add("<< /Title (Images) >>");
  return p.build(root);
}

bool darkAt(const Bitmap& bm, int x, int y) {
  if (x < 0 || y < 0 || x >= bm.w || y >= bm.h) return false;
  const uint8_t* p = &bm.bgra[(size_t(y) * size_t(bm.w) + size_t(x)) * 4];
  return p[0] < 128 && p[1] < 128 && p[2] < 128;
}
bool anyDark(const Bitmap& bm, int x0, int y0, int x1, int y1) {
  for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (darkAt(bm, x, y)) return true;
  return false;
}
uint32_t pixel(const Bitmap& bm, int x, int y) {
  const uint8_t* p = &bm.bgra[(size_t(y) * size_t(bm.w) + size_t(x)) * 4];
  return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
}  // namespace

int main(int argc, char** argv) {
  string lib = argc > 1 ? argv[1] : (getenv("VS_PDFIUM") ? getenv("VS_PDFIUM") : "third_party/pdfium/linux-x64/lib/libpdfium.so");
  void* h = loadLib(lib);
  if (!h) { std::cout << "pdf_test: skipped (no PDFium library at " << lib << ")\n"; return 0; }
  string err;
  CHECK(bind(&resolveSym, h, &err));
  if (!bound()) { std::cerr << err << "\n"; return 1; }

  // the worker owns the library; everything below runs as tasks on it, results come back through `post`
  std::mutex qm;
  std::deque<std::function<void()>> uiq;
  Worker w;
  w.start([&](std::function<void()> f) { std::lock_guard<std::mutex> lk(qm); uiq.push_back(std::move(f)); });
  auto pump = [&]() {  // run posted closures until the worker is idle and nothing is left
    for (int i = 0; i < 20000; i++) {
      std::deque<std::function<void()>> q;
      { std::lock_guard<std::mutex> lk(qm); q.swap(uiq); }
      for (auto& f : q) f();
      if (q.empty() && w.idle()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); std::lock_guard<std::mutex> lk(qm); if (uiq.empty() && w.idle()) return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  // priorities and cancellation
  {
    vector<int> order;
    std::atomic<int> blocker{0}, started{0};
    w.submit(5, [&](Worker&) { started = 1; while (!blocker) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });  // holds the thread so the rest queues up
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    w.submit(3, [&](Worker& wk) { wk.toUi([&] { order.push_back(3); }); });
    w.submit(1, [&](Worker& wk) { wk.toUi([&] { order.push_back(1); }); });
    Worker::Handle c = w.submit(2, [&](Worker& wk) { wk.toUi([&] { order.push_back(2); }); });
    w.submit(1, [&](Worker& wk) { wk.toUi([&] { order.push_back(11); }); });
    c.cancel();
    blocker = 1;
    pump();
    CHECK(order.size() == 3 && order[0] == 1 && order[1] == 11 && order[2] == 3);
  }

  const string pdfBytes = samplePdf();
  const string dir = argc > 2 ? argv[2] : "build/host";
  const string path = dir + "/pdf_test_sample.pdf";
  CHECK(writeFile(path, pdfBytes));
  int id = w.newDocId();
  DocInfo info;
  bool opened = false;
  string openErr;
  w.submit(0, [&](Worker& wk) { Document& d = wk.doc(id); opened = d.open(path, &openErr); info = d.info(); });
  pump();
  CHECK(opened);
  if (!opened) { std::cerr << openErr << "\n"; return 1; }
  CHECK(info.pages == 3 && std::fabs(info.sizes[0][0] - 612) < 0.5 && std::fabs(info.sizes[0][1] - 792) < 0.5);
  CHECK(std::fabs(info.sizes[2][0] - 612) < 0.5 && std::fabs(info.sizes[2][1] - 792) < 0.5);  // cropped page
  CHECK(std::fabs(info.sizes[1][0] - 792) < 0.5 && std::fabs(info.sizes[1][1] - 612) < 0.5);  // rotated page: swapped
  CHECK(info.title == "Sample paper" && info.author == "A. Tester");
  CHECK(info.outline.size() == 2 && info.outline[0].title == "Start" && info.outline[0].page == 0 && std::fabs(info.outline[0].y - 92) < 0.5 && info.outline[1].title == "Turned" && info.outline[1].page == 1);
  CHECK(info.labels.empty());

  // text of page 1: characters, boxes in display space (origin top-left), lines, words, slices, hit testing
  TextPage tp;
  w.submit(0, [&](Worker& wk) { wk.doc(id).text(0, tp); });
  pump();
  CHECK(!tp.empty() && tp.text.find("Hello World") == 0 && tp.text.find("Second line here") != string::npos);
  CHECK(tp.lines.size() == 2);
  CHECK(tp.chars[0].cp == 'H' && std::fabs(tp.chars[0].b.x0 - 72) < 2.5 && tp.chars[0].b.y0 > 792 - 700 - 26 && tp.chars[0].b.y1 < 792 - 700 + 8);
  CHECK(std::fabs(tp.fontSize - 12) < 0.5 || std::fabs(tp.fontSize - 24) < 0.5);
  {
    float hx = (tp.chars[0].b.x0 + tp.chars[0].b.x1) / 2, hy = (tp.chars[0].b.y0 + tp.chars[0].b.y1) / 2;
    CHECK(tp.hit(hx, hy, false) == 0);
    CHECK(tp.hit(hx, hy + 200, false) == -1 && tp.hit(400, hy, true) == 10);  // far right on the first line: the last char 'd'
    auto wd = tp.word(7);                                                     // inside "World"
    CHECK(wd[0] == 6 && wd[1] == 10 && tp.slice(wd[0], wd[1]) == "World");
    vector<Box> rs = tp.rects(0, int(tp.chars.size()) - 1);
    CHECK(rs.size() == 2 && rs[1].y0 > rs[0].y1 - 2);  // one rectangle per line, the second lower on the page
    CHECK(tp.slice(0, int(tp.chars.size()) - 1) == "Hello World\nSecond line here");
    CHECK(tp.byteAt.size() == tp.chars.size() + 1 && tp.byteAt[6] == 6);
  }
  // rotated page: the same text turned 90 degrees clockwise — the first glyph lands near x = 700, y = 72 of a 792 x 612 page
  TextPage tp2;
  w.submit(0, [&](Worker& wk) { wk.doc(id).text(1, tp2); });
  pump();
  // (the engine orders the runs by their rotated position, so "Second line here" comes first here)
  int hIdx = -1;
  for (size_t i = 0; i < tp2.chars.size(); i++) if (tp2.chars[i].cp == 'H') { hIdx = int(i); break; }
  CHECK(!tp2.empty() && hIdx >= 0 && tp2.text.find("Hello World") != string::npos);
  if (hIdx >= 0) {
    // baseline at user y = 700 becomes display x = 700; ascenders point to the right (x up to ~723), descenders left
    CHECK(tp2.chars[size_t(hIdx)].b.x0 > 690 && tp2.chars[size_t(hIdx)].b.x0 < 700 && tp2.chars[size_t(hIdx)].b.x1 > 700 && tp2.chars[size_t(hIdx)].b.x1 < 726 && std::fabs(tp2.chars[size_t(hIdx)].b.y0 - 72) < 2.5);
    CHECK(tp2.chars[size_t(hIdx) + 10].b.y0 > tp2.chars[size_t(hIdx)].b.y0 + 60);  // "World" continues downwards
  }

  // cropped page: the same text shifted by the crop origin (72 - 50, 852 - 700)
  TextPage tp3;
  Bitmap cropped;
  w.submit(0, [&](Worker& wk) { wk.doc(id).text(2, tp3); wk.doc(id).render(2, 1.0, 0, 0, 612, 792, cropped, false); });
  pump();
  CHECK(!tp3.empty() && tp3.chars[0].cp == 'H' && std::fabs(tp3.chars[0].b.x0 - 22) < 2.5 && std::fabs(tp3.chars[0].b.y0 - (tp.chars[0].b.y0 + 60)) < 0.5);
  CHECK(anyDark(cropped, 22, 134, 42, 152) && !anyDark(cropped, 72, 74, 92, 92));

  // rendering: full page at 1 px/pt, then a tile at 2 px/pt
  Bitmap full, tile, turned;
  w.submit(0, [&](Worker& wk) { wk.doc(id).render(0, 1.0, 0, 0, 612, 792, full, false); wk.doc(id).render(0, 2.0, 140, 160, 120, 80, tile, false); wk.doc(id).render(1, 1.0, 0, 0, 792, 612, turned, false); });
  pump();
  CHECK(full.w == 612 && full.h == 792 && full.bgra.size() == size_t(612 * 792 * 4));
  CHECK(anyDark(full, 72, 74, 92, 92));      // the H
  CHECK(!anyDark(full, 300, 300, 400, 400));  // blank area
  CHECK(pixel(full, 300, 300) == 0xFFFFFF && full.bgra[3] == 255);  // white, opaque
  CHECK(tile.w == 120 && tile.h == 80 && anyDark(tile, 0, 0, 120, 80));  // the tile covers x 70..130, y 80..120 pt: "Hello"
  CHECK(anyDark(turned, 694, 72, 724, 92) && !anyDark(turned, 72, 74, 92, 92));  // rotated: glyphs where the text layer says

  // view rotation: a quarter turn clockwise is the transposition of the unrotated page, the region offset works on the
  // rotated page, and a half turn is the point reflection
  Bitmap r1, r2, r3, r1part;
  w.submit(0, [&](Worker& wk) {
    wk.doc(id).render(0, 1.0, 0, 0, 792, 612, r1, false, false, 1);
    wk.doc(id).render(0, 1.0, 0, 0, 612, 792, r2, false, false, 2);
    wk.doc(id).render(0, 1.0, 0, 0, 792, 612, r3, false, false, 3);
    wk.doc(id).render(0, 1.0, 700, 60, 60, 40, r1part, false, false, 1);
  });
  pump();
  CHECK(r1.w == 792 && r1.h == 612 && r3.w == 792 && r3.h == 612 && r2.w == 612 && r2.h == 792);
  {
    // rot 1: (X, Y) <- (x = Y, y = H - 1 - X); rot 2: (W - 1 - x, H - 1 - y); rot 3: (X, Y) <- (x = W - 1 - Y, y = X)
    long same1 = 0, same2 = 0, same3 = 0, dark0 = 0, dark1 = 0, total = 0;
    for (int Y = 0; Y < 612; Y += 2)
      for (int X = 0; X < 792; X += 2) {
        total++;
        const uint32_t p1 = pixel(r1, X, Y), o1 = pixel(full, Y, 791 - X);
        const uint32_t p3 = pixel(r3, X, Y), o3 = pixel(full, 611 - Y, X);
        if (p1 == o1) same1++;
        if (p3 == o3) same3++;
        if ((o1 & 255) < 128) dark0++;
        if ((p1 & 255) < 128) dark1++;
      }
    for (int y = 0; y < 792; y += 2)
      for (int x = 0; x < 612; x += 2)
        if (pixel(r2, x, y) == pixel(full, 611 - x, 791 - y)) same2++;
    CHECK(same1 > total * 0.97 && same3 > total * 0.97 && same2 > total * 0.97);
    CHECK(dark0 > 50 && std::labs(dark0 - dark1) <= dark0 / 20 + 2);
  }
  // the H (x 72..92, y 74..92 unrotated) lands at X = 792 - y = 700..718, Y = x = 72..92 on the turned page
  CHECK(anyDark(r1, 700, 72, 720, 92) && !anyDark(r1, 0, 72, 30, 92) && anyDark(r1part, 0, 12, 20, 32) && !anyDark(r1, 300, 300, 400, 400));
  {
    Box t = rotateBox(1, 612, 792, {72, 74, 92, 92});
    CHECK(std::fabs(t.x0 - 700) < 0.01f && std::fabs(t.x1 - 718) < 0.01f && std::fabs(t.y0 - 72) < 0.01f && std::fabs(t.y1 - 92) < 0.01f);
    float ux, uy;
    unrotatePoint(1, 612, 792, t.x0, t.y0, ux, uy);
    CHECK(std::fabs(ux - 72) < 0.01f && std::fabs(uy - 92) < 0.01f);
    for (int rot = 0; rot < 4; rot++) {
      float X, Y, bx, by;
      rotatePoint(rot, 612, 792, 100, 200, X, Y);
      unrotatePoint(rot, 612, 792, X, Y, bx, by);
      CHECK(std::fabs(bx - 100) < 0.01f && std::fabs(by - 200) < 0.01f);
    }
  }

  // marks burnt into a rendered page: highlight multiplies, underline draws a dark line under the quad, area frames
  {
    Bitmap bm = full;
    vector<Mark> marks;
    Mark hl; hl.kind = 0; hl.rgb = 0xFFE066; hl.quads = {{300, 300, 400, 320}}; marks.push_back(hl);
    Mark ul; ul.kind = 1; ul.rgb = 0x66CCFF; ul.quads = {{300, 400, 400, 420}}; marks.push_back(ul);
    Mark ar; ar.kind = 4; ar.rgb = 0xFF8800; ar.rect = {300, 500, 400, 560}; marks.push_back(ar);
    Mark ink; ink.kind = 5; ink.rgb = 0x2255AA; ink.strokeWidth = 2; InkStroke path; path.points = {{300, 600}, {360, 600}}; ink.strokes.push_back(path); marks.push_back(ink);
    burnMarks(bm, 1.0, 0, 612, 792, 0, 0, marks);
    CHECK(pixel(bm, 350, 310) == 0xFFE066 && pixel(bm, 350, 290) == 0xFFFFFF);  // white x colour = colour
    CHECK(pixel(bm, 350, 419) != 0xFFFFFF && pixel(bm, 350, 405) == 0xFFFFFF);  // the line sits at the bottom of the quad
    CHECK(pixel(bm, 350, 500) != 0xFFFFFF && pixel(bm, 350, 530) == 0xFFFFFF && pixel(bm, 399, 530) != 0xFFFFFF);
    { uint32_t c = pixel(bm, 330, 600); CHECK((c & 255) > ((c >> 16) & 255) + 20 && (c & 255) > ((c >> 8) & 255) + 20); }
    // rotated: both the highlight and freehand path land on the turned page in the same geometry as PDFium.
    Bitmap rb = r1;
    burnMarks(rb, 1.0, 1, 612, 792, 0, 0, {hl});
    CHECK(pixel(rb, 792 - 310, 350) == 0xFFE066 && pixel(rb, 350, 310) == 0xFFFFFF);
    Bitmap rInk = r1;
    burnMarks(rInk, 1.0, 1, 612, 792, 0, 0, {ink});
    { uint32_t c = pixel(rInk, 192, 330); CHECK((c & 255) > ((c >> 16) & 255) + 20 && (c & 255) > ((c >> 8) & 255) + 20); }
  }

  // search
  vector<Hit> hits;
  w.submit(0, [&](Worker& wk) { wk.doc(id).find(0, u"world", false, false, hits, &tp); });
  pump();
  CHECK(hits.size() == 1 && hits[0].start == 6 && hits[0].count == 5 && hits[0].rects.size() == 1 && hits[0].match == "World" && hits[0].before == "Hello ");
  CHECK(hits[0].rects[0].x0 > 125 && hits[0].rects[0].x0 < 140 && hits[0].rects[0].y0 > 70 && hits[0].rects[0].y1 < 100);
  hits.clear();
  w.submit(0, [&](Worker& wk) { wk.doc(id).find(0, u"world", true, false, hits, &tp); });
  pump();
  CHECK(hits.empty());  // case-sensitive: no match

  // links: the URI annotation over "Hello"
  vector<Link> links;
  w.submit(0, [&](Worker& wk) { wk.doc(id).links(0, links); });
  pump();
  CHECK(!links.empty());
  bool uri = false;
  for (auto& l : links) if (l.url == "https://example.org/hello" && std::fabs(l.b.x0 - 72) < 0.5 && std::fabs(l.b.y0 - 70) < 0.5 && std::fabs(l.b.y1 - 98) < 0.5) uri = true;
  CHECK(uri);

  // annotations already in the file
  vector<FileAnnot> fa;
  w.submit(0, [&](Worker& wk) { wk.doc(id).readAnnots(0, fa); });
  pump();
  CHECK(fa.size() == 1 && fa[0].subtype == FPDF_ANNOT_HIGHLIGHT && fa[0].contents == "existing note" && fa[0].author == "Tester" && (fa[0].color & 0xFFFFFF) == 0xFFFF00);
  CHECK(fa[0].quads.size() == 1 && std::fabs(fa[0].quads[0].x0 - 133) < 0.5 && std::fabs(fa[0].quads[0].y0 - 70) < 0.5 && std::fabs(fa[0].quads[0].y1 - 98) < 0.5);

  // write two annotations (a green highlight over "Second", a sticky note), save a copy, reopen it, read them back
  // and see the highlight in the rendering
  const string copy = dir + "/pdf_test_annotated.pdf";
  bool wrote = false, saved = false;
  w.submit(0, [&](Worker& wk) {
    Document& d = wk.doc(id);
    vector<FileAnnot> add(3);
    add[0].subtype = FPDF_ANNOT_HIGHLIGHT;
    std::array<int, 2> sw = tp.word(13);  // "Second" (11 letters, then the engine's "\r\n")
    add[0].quads = tp.rects(sw[0], sw[1]);
    add[0].color = 0xFF70E070;
    add[0].contents = "method";
    add[0].author = "VOSStudio";
    add[0].name = "vs:42";
    add[1].subtype = FPDF_ANNOT_TEXT;
    add[1].rect = {400, 100, 420, 120};
    add[1].color = 0xFFFFC000;
    add[1].contents = "a note";
    add[2].subtype = FPDF_ANNOT_INK;
    add[2].rect = {95, 145, 165, 185};
    add[2].strokeWidth = 2.5f;
    add[2].color = 0xFF2255AA;
    add[2].author = "VOSStudio";
    add[2].name = "vs:ink";
    InkStroke ink; ink.points = {{100, 150}, {140, 150}, {160, 180}};
    add[2].strokes.push_back(ink);
    wrote = d.writeAnnots(0, add);
    saved = d.saveAs(copy);
  });
  pump();
  CHECK(wrote && saved);
  int id2 = w.newDocId();
  vector<FileAnnot> fa2;
  Bitmap annotated;
  bool opened2 = false;
  int removed = 0;
  w.submit(0, [&](Worker& wk) {
    Document& d = wk.doc(id2);
    opened2 = d.open(copy);
    if (opened2) { d.readAnnots(0, fa2); d.render(0, 1.0, 0, 0, 612, 792, annotated, true); removed = d.removeAnnots(0, [](const FileAnnot& a) { return a.name == "vs:42"; }); }
  });
  pump();
  CHECK(opened2 && fa2.size() == 4);
  bool green = false, note = false, inkSaved = false;
  for (auto& a : fa2) {
    if (a.subtype == FPDF_ANNOT_HIGHLIGHT && a.contents == "method") { green = a.quads.size() == 1 && (a.color & 0xFFFFFF) == 0x70E070 && a.author == "VOSStudio" && a.name == "vs:42" && std::fabs(a.quads[0].x0 - tp.chars[13].b.x0) < 1; }
    if (a.subtype == FPDF_ANNOT_TEXT && a.contents == "a note") note = true;
    if (a.subtype == FPDF_ANNOT_INK && a.name == "vs:ink") inkSaved = (a.color & 0xFFFFFF) == 0x2255AA && std::fabs(a.rect.x0 - 95) < 1 && std::fabs(a.rect.y1 - 185) < 1 &&
        a.strokes.size() == 1 && a.strokes[0].points.size() == 3 && std::fabs(a.strokes[0].points[1].x - 140) < 1 && std::fabs(a.strokes[0].points[1].y - 150) < 1 && std::fabs(a.strokeWidth - 2.5f) < 0.5f;
  }
  CHECK(green && note && inkSaved);
  {
    bool blueStroke = false;
    for (int y = 145; y < 185 && !blueStroke; y++) for (int x = 95; x < 165; x++) {
      uint32_t c = pixel(annotated, x, y);
      int r = int((c >> 16) & 255), g = int((c >> 8) & 255), b = int(c & 255);
      if (b > r + 30 && b > g + 20 && b > 80) { blueStroke = true; break; }
    }
    CHECK(blueStroke);
  }
  {
    // the highlight's appearance stream was generated and saved: the pixels over "Second" are tinted green
    Box b = tp.rects(13, 18)[0];
    int cx = int((b.x0 + b.x1) / 2), cy = int((b.y0 + b.y1) / 2);
    bool tinted = false;
    for (int y = int(b.y0); y < int(b.y1) && !tinted; y++) for (int x = int(b.x0); x < int(b.x1); x++) {
      uint32_t px = pixel(annotated, x, y);
      int r = (px >> 16) & 255, g = (px >> 8) & 255, bl = px & 255;
      if (g > 150 && r < 200 && bl < 200 && g > r + 20) { tinted = true; break; }
    }
    CHECK(tinted);
    (void)cx; (void)cy;
  }
  CHECK(removed == 1);

  // images on a page and the dark page (1.17): boxes in display space (top-left origin), the nested one placed by
  // the form's matrix; the photograph recognised; darkenPage: paper -> dark grey, ink -> light, hue kept, kept
  // rectangles only dimmed
  {
    string imgPath = dir + "/pdf_test_images.pdf";
    writeFile(imgPath, imagesPdf());
    vector<ImageBox> ims;
    Bitmap light, darkBm;
    bool ok = false;
    w.submit(0, [&](Worker& wk) { Document d; ok = d.open(imgPath); if (ok) { d.images(0, ims); d.render(0, 1.0, 0, 0, 612, 792, light, false, false); d.render(0, 1.0, 0, 0, 612, 792, darkBm, false, true); } });
    pump();
    CHECK(ok && ims.size() == 4);
    if (ims.size() == 4) {
      auto near = [](const Box& b, float x0, float y0, float x1, float y1) { return std::fabs(b.x0 - x0) < 1 && std::fabs(b.y0 - y0) < 1 && std::fabs(b.x1 - x1) < 1 && std::fabs(b.y1 - y1) < 1; };
      CHECK(near(ims[0].box, 72, 232, 232, 352) && ims[0].photo);     // the photograph (y from the top: 792 - 560 = 232)
      CHECK(near(ims[1].box, 260, 222, 460, 362) && !ims[1].photo);   // the plot
      CHECK(near(ims[2].box, 72, 392, 312, 552) && !ims[2].photo);    // the scan
      CHECK(near(ims[3].box, 360, 442, 420, 502) && !ims[3].photo);   // nested in the form: 340+20, 792-(260+30+60)
    }
    CHECK(pixel(light, 500, 100) == 0xFFFFFF && pixel(darkBm, 500, 100) == 0x1C1C1C);  // paper
    {  // ink: the darkest pixel of the title becomes the lightest of the dark page
      int lightMin = 255, darkMax = 0;
      for (int y = 40; y < 60; y++) for (int x = 72; x < 140; x++) { lightMin = std::min(lightMin, int(light.bgra[(size_t(y) * 612 + size_t(x)) * 4])); darkMax = std::max(darkMax, int(darkBm.bgra[(size_t(y) * 612 + size_t(x)) * 4])); }
      CHECK(lightMin < 40 && darkMax > 200);
    }
    {  // blue text keeps its hue and becomes light: a pixel that is blue in the light page is a light blue in the dark one
      bool found = false;
      for (int y = 66; y < 82 && !found; y++) for (int x = 72; x < 140 && !found; x++) {
        const uint8_t* lp = &light.bgra[(size_t(y) * 612 + size_t(x)) * 4];
        const uint8_t* dp = &darkBm.bgra[(size_t(y) * 612 + size_t(x)) * 4];
        if (lp[0] > 200 && lp[2] < 60) { found = true; CHECK(dp[0] > 200 && dp[0] >= dp[2] && dp[2] > 150); }
      }
      CHECK(found);
    }
    {  // the photograph is kept (dimmed by 220/255), the plot's white becomes dark, the scan's light grey becomes dark
      const uint8_t* lp = &light.bgra[(size_t(290) * 612 + size_t(150)) * 4];
      const uint8_t* dp = &darkBm.bgra[(size_t(290) * 612 + size_t(150)) * 4];
      CHECK(std::abs(int(dp[0]) - int(lp[0]) * 220 / 255) <= 1 && std::abs(int(dp[1]) - int(lp[1]) * 220 / 255) <= 1);
      CHECK(pixel(light, 300, 235) == 0xFFFFFF && pixel(darkBm, 300, 235) == 0x1C1C1C);
      CHECK((pixel(light, 80, 409) & 255) > 230 && (pixel(darkBm, 80, 409) & 255) < 0x40);  // a gap between the scan's strokes
    }
    // darkenPage alone, with a kept rectangle
    Bitmap bm; bm.w = 4; bm.h = 1; bm.bgra = {255, 255, 255, 255, 0, 0, 0, 255, 255, 0, 0, 255, 100, 100, 100, 255};
    darkenPage(bm, {PxRect{3, 0, 4, 1}});
    CHECK(bm.bgra[0] == 0x1C && bm.bgra[4] == 0xE6 && bm.bgra[8] > 200 && bm.bgra[10] > 150 && bm.bgra[12] == 100 * 220 / 255);
  }

  // memory: pages drop, document closes, a bad file is refused with a message
  bool bad = true;
  string badErr;
  w.submit(0, [&](Worker& wk) { wk.doc(id).dropPages(); wk.closeDoc(id); wk.closeDoc(id2); Document d; bad = d.open(dir + "/pdf_test_missing.pdf", &badErr); });
  pump();
  CHECK(!bad && !badErr.empty());
  string junkPath = dir + "/pdf_test_junk.pdf";
  writeFile(junkPath, "this is not a pdf at all");
  w.submit(0, [&](Worker& wk) { Document d; bad = d.open(junkPath, &badErr); });
  pump();
  CHECK(!bad && badErr.find("PDF") != string::npos);
  CHECK(w.openDocs() == 0);

  // the library can be released while no document is open, and comes back for the next task
  bool released = false, loadedAfter = true, reopened = false;
  w.submit(0, [&](Worker& wk) { released = wk.releaseLibrary(); loadedAfter = wk.libraryLoaded(); });
  pump();
  CHECK(released && !loadedAfter);
  w.submit(0, [&](Worker& wk) { Document d; reopened = d.open(path) && wk.libraryLoaded(); Bitmap t; if (reopened) reopened = d.render(0, 0.5, 0, 0, 306, 396, t, false) && anyDark(t, 36, 37, 46, 46); });
  pump();
  CHECK(reopened);
  w.submit(0, [&](Worker& wk) { int nid = wk.newDocId(); wk.doc(nid).open(path); released = wk.releaseLibrary(); wk.closeDoc(nid); });
  pump();
  CHECK(!released);  // not while a document is open
  w.stop();
  unbind();

  if (fails) { std::cerr << fails << " check(s) failed\n"; return 1; }
  std::cout << "pdf_test: all checks passed\n";
  return 0;
}
