// VOSStudio Native: the writer — the document editor in the main area (the Write page of the left rail, Ctrl+Shift+W,
// the `writer` command, the Write menu, and the assistant's report tools, which write into the same document).
//
// Text is laid out with DirectWrite: one IDWriteTextLayout per paragraph, table cell or caption, cached by a content
// signature, so typing re-lays out only the paragraph being edited. The blocks are then paginated onto real pages
// (paragraphs split between lines with widow/orphan control, tables between rows, headings kept with their text, a
// page break starts a new page) — what you see is what the PDF prints. Zooming scales the pages with a Direct2D
// transform and re-lays out only once the zoom has settled, so Ctrl+wheel is smooth. Every change goes through the core
// Editor (doc.h), and the exporters read the same document. Positions are (block, cell, byte); DirectWrite counts
// UTF-16 units, and WPara maps between the two.
#include "../core/doc.h"
#include "../core/docx.h"
#include "../core/equation.h"
#include "../core/insights.h"
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace vs {
namespace win {

// ------------------------------------------------------------------ layouts
struct WPara {  // one laid-out text unit: a paragraph, a table cell or a figure caption
  Com<IDWriteTextLayout> lay;
  std::wstring w;             // prefix + text in UTF-16
  vector<int> b2u, u2b;       // byte -> UTF-16 index (size bytes + 1); UTF-16 index -> byte (size units + 1)
  int pre = 0;                // UTF-16 units of the read-only prefix ("Figure 2. ", "2.1  ")
  float x = 0, y = 0, wd = 0, h = 0;  // layout pixels; y relative to the block top
  vector<float> lineB;        // bottom of every line, relative to the layout top (pagination splits between lines)
};

struct WPlace { int page; float y0, y1, py; };  // the block's local range [y0, y1) sits at py on `page` (text-area coordinates)

struct WBlock {
  uint64_t sig = 0;           // content signature the layout was built for
  float h = 0;                // layout pixels
  WPara para;                 // Paragraph text or the figure caption
  vector<WPara> cells;        // table cells, row-major
  vector<float> colX, colW, rowY, rowH;
  Rect img;                   // figure image, relative to the block top
  string label;               // gutter label: bullet, number, "[n]"
  float labelX = 0, labelSize = 11;
  vector<std::pair<string, float>> toc;  // table of contents lines (text, indent)
  vector<WPlace> places;      // where the block is on the pages (set every frame)
};

struct WriterLayout {
  vector<WBlock> blocks;
  uint64_t key = 0;
  uint64_t stamp = 0;  // document version + page geometry the blocks were laid out for (unchanged = nothing to do)
  float textW = 0, contentH = 0;
  float k = 1, pageWL = 0, pageHL = 0, ML = 0, textHL = 0, gapL = 0;  // layout scale and page geometry in layout pixels
  int pages = 1;
  vector<vector<std::pair<int, int>>> pagePlaces;  // per page: (block, place index) in order
  string family, headFamily, mono;
  Com<ID2D1SolidColorBrush> inkBr, dimBr, linkBr;
  std::map<uint32_t, Com<ID2D1SolidColorBrush>> colorBr;
  struct Img { int wpx = 0, hpx = 0; int frame = 0; Com<ID2D1BitmapRenderTarget> rt; Com<ID2D1Bitmap> bmp; size_t bytes() const { return size_t(wpx) * size_t(hpx) * 4; } };
  std::map<int, Img> images;  // asset -> the scene rendered once at its pixel width (as the chart panels do); an LRU with a pixel budget
  size_t imageBytes() const { size_t n = 0; for (auto& kv : images) n += kv.second.bytes(); return n; }
  void dropImages(size_t budget, int keepFrame) {  // oldest first until the budget holds (never the ones drawn this frame)
    while (imageBytes() > budget) {
      auto victim = images.end();
      for (auto it = images.begin(); it != images.end(); ++it) if (it->second.frame != keepFrame && (victim == images.end() || it->second.frame < victim->second.frame)) victim = it;
      if (victim == images.end()) break;
      images.erase(victim);
    }
  }
};

namespace {

const Color kPaper(1, 1, 1), kInk(0.13f, 0.13f, 0.15f), kInkDim(0.42f, 0.42f, 0.46f), kLink(0.09f, 0.34f, 0.80f),
    kMark(1.0f, 0.93f, 0.55f), kCodeBg(0.95f, 0.96f, 0.97f), kQuoteBar(0.78f, 0.80f, 0.84f), kGrid(0.80f, 0.82f, 0.85f),
    kHeadBg(0.95f, 0.96f, 0.97f), kSel(0.20f, 0.45f, 0.95f, 0.26f), kCaret(0.1f, 0.1f, 0.1f), kPageEdge(0, 0, 0, 0.18f),
    kCiteBg(0.20f, 0.45f, 0.95f, 0.08f), kFigBorder(0.72f, 0.74f, 0.77f);

const PStyle kStyleOrder[] = {PStyle::Body, PStyle::Title, PStyle::Subtitle, PStyle::Meta, PStyle::H1, PStyle::H2, PStyle::H3, PStyle::H4,
                              PStyle::Bullet, PStyle::Number, PStyle::Quote, PStyle::Code, PStyle::Caption, PStyle::Reference};
const int kStyleCount = int(sizeof(kStyleOrder) / sizeof(kStyleOrder[0]));
int styleIndex(PStyle s) { for (int i = 0; i < kStyleCount; i++) if (kStyleOrder[i] == s) return i; return 0; }

const float kSizes[] = {8, 9, 10, 10.5f, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48};
const int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));

// text colours offered by the toolbar (Word's "standard" row plus a few muted ones); 0 = automatic
const uint32_t kTextColors[] = {0, 0xFF000000, 0xFF404040, 0xFF7F7F7F, 0xFFC00000, 0xFFFF0000, 0xFFED7D31, 0xFFFFC000, 0xFF70AD47, 0xFF00B050,
                                0xFF00B0F0, 0xFF0070C0, 0xFF002060, 0xFF7030A0, 0xFF4472C4, 0xFF5B9BD5, 0xFFA5A5A5, 0xFF833C0B};
const int kTextColorCount = int(sizeof(kTextColors) / sizeof(kTextColors[0]));
Color fromArgb(uint32_t c) { return Color(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, ((c >> 24) & 255) / 255.f); }

struct SSpec { float size = 11, before = 0, after = 8, indent = 0, hanging = 0; int weight = 400; bool italic = false, mono = false, dim = false, heading = false; };
SSpec specOf(PStyle st, float b, int level) {
  SSpec t;
  t.size = b;
  switch (st) {
    case PStyle::Title: t.size = b * 2.2f; t.weight = 700; t.after = 6; t.heading = true; break;
    case PStyle::Subtitle: t.size = b * 1.3f; t.after = 5; t.dim = true; t.heading = true; break;
    case PStyle::Meta: t.size = b * 0.9f; t.after = 2; t.dim = true; break;
    case PStyle::H1: t.size = b * 1.45f; t.weight = 700; t.before = 18; t.after = 6; t.heading = true; break;
    case PStyle::H2: t.size = b * 1.2f; t.weight = 700; t.before = 14; t.after = 4; t.heading = true; break;
    case PStyle::H3: t.size = b * 1.05f; t.weight = 700; t.before = 12; t.after = 3; t.heading = true; break;
    case PStyle::H4: t.weight = 700; t.italic = true; t.before = 10; t.after = 2; t.heading = true; break;
    case PStyle::Body: t.after = 8; break;
    case PStyle::Bullet: case PStyle::Number: t.after = 3; t.indent = 18.f * float(clampv(level, 0, 2)); t.hanging = 18; break;
    case PStyle::Quote: t.italic = true; t.indent = 18; t.before = 2; t.after = 8; t.dim = true; break;
    case PStyle::Code: t.size = b * 0.85f; t.mono = true; t.after = 8; t.indent = 8; break;
    case PStyle::Caption: t.size = b * 0.88f; t.after = 10; break;
    case PStyle::Reference: t.size = b * 0.92f; t.hanging = 24; t.after = 4; break;
  }
  return t;
}

vector<string> familyCandidates(const string& f) {
  if (f == "Cambria") return {"Cambria", "Caladea", "Georgia", "Times New Roman"};
  if (f == "Georgia") return {"Georgia", "Cambria", "Times New Roman"};
  if (f == "Times New Roman") return {"Times New Roman", "Liberation Serif", "Cambria"};
  if (f == "Arial") return {"Arial", "Liberation Sans", "Segoe UI"};
  if (f == "Calibri" || f.empty()) return {"Calibri", "Carlito", "Segoe UI", "Arial"};
  vector<string> c = {f};
  if (PageSetup::monoFamily(f)) { c.push_back("Consolas"); c.push_back("Courier New"); }
  else if (PageSetup::serifFamily(f)) { c.push_back("Cambria"); c.push_back("Georgia"); c.push_back("Times New Roman"); }
  else { c.push_back("Calibri"); c.push_back("Segoe UI"); c.push_back("Arial"); }
  return c;
}

bool captionHasLabel(const string& text) {
  string t = lower(trim(text));
  for (const char* pfx : {"figure ", "fig. ", "fig ", "table ", "tbl. ", "chart ", "map "}) {
    size_t n = strlen(pfx);
    if (t.size() > n && startsWith(t, pfx) && isdigit(uint8_t(t[n]))) return true;
  }
  return false;
}

int tableForCaption(const Document& d, int k) {
  int n = int(d.blocks.size());
  if (k + 1 < n && d.blocks[size_t(k) + 1].kind == Block::TableBlock) return k + 1;
  if (k > 0 && d.blocks[size_t(k) - 1].kind == Block::TableBlock && !(k + 1 < n && d.blocks[size_t(k) + 1].kind == Block::TableBlock)) return k - 1;
  return -1;
}

uint64_t fnv(uint64_t h, const void* d, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(d);
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}
uint64_t fnvS(uint64_t h, const string& s) { h = fnv(h, s.data(), s.size()); return fnv(h, "\x1f", 1); }
uint64_t sigPara(uint64_t h, const Para& p) {
  int hd[4] = {int(p.style), int(p.align), p.level, p.numStart};
  h = fnv(h, hd, sizeof hd);
  for (auto& sp : p.spans) {
    h = fnvS(h, sp.text); h = fnv(h, &sp.fmt, sizeof sp.fmt); h = fnvS(h, sp.link);
    h = fnvS(h, sp.font); h = fnv(h, &sp.size, sizeof sp.size); h = fnv(h, &sp.color, sizeof sp.color); h = fnvS(h, sp.cite);
  }
  return fnv(h, "\x1e", 1);
}

// UTF-8 paragraph text -> DirectWrite text and two offset maps. The editor stores LaTeX
// source bytes, while the canvas shows a readable Unicode math preview; offsets through a
// formula are mapped proportionally so hit testing and selection still address source text.
void mapParaText(const Para& p, WPara& P) {
  P.pre = int(P.w.size());
  P.b2u.assign(p.size() + 1, P.pre);
  P.u2b.assign(size_t(P.pre) + 1, 0);
  size_t raw = 0;
  for (const Span& span : p.spans) {
    const size_t srcLen = span.text.size();
    string display = (span.fmt & F_MATH) ? mathPreviewText(span.text) : span.text;
    std::wstring shown = widen(display);
    const size_t uStart = P.w.size(), uLen = shown.size();
    if (srcLen) {
      for (size_t b = 0; b <= srcLen; b++) {
        size_t u = uStart + (uLen * b + srcLen / 2) / srcLen;
        P.b2u[raw + b] = int(std::min(uStart + uLen, u));
      }
    } else P.b2u[raw] = int(uStart);
    for (size_t u = 1; u <= uLen; u++) {
      size_t source = uLen ? (srcLen * u + uLen / 2) / uLen : srcLen;
      source = std::min(source, srcLen);
      while (source && source < srcLen && (uint8_t(span.text[source]) & 0xC0) == 0x80) source--;
      P.u2b.push_back(int(raw + source));
    }
    P.w += shown;
    raw += srcLen;
  }
  P.b2u[p.size()] = int(P.w.size());
}

int byteAt(const WPara& P, float lx, float ly) {  // layout-local point -> byte offset
  if (!P.lay) return 0;
  BOOL trail = FALSE, inside = FALSE;
  DWRITE_HIT_TEST_METRICS hm{};
  P.lay->HitTestPoint(lx, ly, &trail, &inside, &hm);
  int u = int(hm.textPosition) + (trail ? int(hm.length) : 0);
  u = clampv(u, P.pre, int(P.w.size()));
  return P.u2b[size_t(u)];
}

bool caretAt(const WPara& P, int off, float& x, float& y, float& h) {  // layout-local caret box
  if (!P.lay) return false;
  off = clampv(off, 0, int(P.b2u.size()) - 1);
  DWRITE_HIT_TEST_METRICS hm{};
  FLOAT px = 0, py = 0;
  P.lay->HitTestTextPosition(UINT32(P.b2u[size_t(off)]), FALSE, &px, &py, &hm);
  x = px; y = py; h = hm.height > 0 ? hm.height : P.h;
  return true;
}

vector<D2D1_RECT_F> rangeRects(const WPara& P, int a, int b) {  // layout-local boxes of the byte range [a, b)
  vector<D2D1_RECT_F> out;
  if (!P.lay) return out;
  a = clampv(a, 0, int(P.b2u.size()) - 1);
  b = clampv(b, 0, int(P.b2u.size()) - 1);
  UINT32 u0 = UINT32(P.b2u[size_t(a)]), u1 = UINT32(P.b2u[size_t(b)]);
  if (u1 <= u0) return out;
  UINT32 n = 0;
  P.lay->HitTestTextRange(u0, u1 - u0, 0, 0, nullptr, 0, &n);
  if (n == 0) return out;
  vector<DWRITE_HIT_TEST_METRICS> hm(n);
  if (FAILED(P.lay->HitTestTextRange(u0, u1 - u0, 0, 0, hm.data(), n, &n))) return out;
  for (UINT32 i = 0; i < n; i++) out.push_back(D2D1::RectF(hm[i].left, hm[i].top, hm[i].left + std::max(2.f, hm[i].width), hm[i].top + hm[i].height));
  return out;
}

ID2D1SolidColorBrush* colorBrush(Gfx& g, WriterLayout& L, uint32_t argb) {
  auto it = L.colorBr.find(argb);
  if (it != L.colorBr.end()) return it->second.get();
  Com<ID2D1SolidColorBrush> b;
  g.dc->CreateSolidColorBrush(d2c(fromArgb(argb)), b.put());
  L.colorBr[argb] = b;
  return b.get();
}

void buildPara(Gfx& g, WriterLayout& L, const Para& p, const string& prefix, bool prefixBold, const SSpec& sp, float k, float lineSpacing, WPara& P) {
  P.w = widen(prefix);
  mapParaText(p, P);
  float sizePx = std::max(4.f, sp.size * k);
  Com<IDWriteTextFormat> fmt;
  std::wstring fam = widen(sp.mono ? L.mono : sp.heading ? L.headFamily : L.family), mono = widen(L.mono);
  g.dw->CreateTextFormat(fam.c_str(), nullptr, DWRITE_FONT_WEIGHT(sp.weight), sp.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, sizePx, L"en-us", fmt.put());
  if (!fmt) return;
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  fmt->SetTextAlignment(p.align == PAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : p.align == PAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                        : p.align == PAlign::Justify ? DWRITE_TEXT_ALIGNMENT_JUSTIFIED : DWRITE_TEXT_ALIGNMENT_LEADING);
  // line spacing: the document's factor (1.0 single … 2.0 double) on the body-like styles; spans with a larger size get a taller line
  float maxSize = sizePx;
  for (auto& s : p.spans) if (s.size > 0) maxSize = std::max(maxSize, s.size * k);
  if (!sp.heading && p.style != PStyle::Code) {
    float lh = maxSize * 1.25f * clampv(lineSpacing, 1.f, 2.5f) / 1.15f;
    fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, maxSize * 1.0f);
  }
  g.dw->CreateTextLayout(P.w.c_str(), UINT32(P.w.size()), fmt.get(), std::max(10.f, P.wd), 1e6f, P.lay.put());
  if (!P.lay) return;
  size_t off = 0;
  for (auto& s : p.spans) {
    size_t a = off, b = off + s.text.size();
    off = b;
    if (a >= b || b >= P.b2u.size()) continue;
    DWRITE_TEXT_RANGE r{UINT32(P.b2u[a]), UINT32(P.b2u[b] - P.b2u[a])};
    if (r.length == 0) continue;
    float spanSize = s.size > 0 ? s.size * k : sizePx;
    if (s.size > 0) P.lay->SetFontSize(spanSize, r);
    if (s.fmt & F_MATH) P.lay->SetFontFamilyName(L"Cambria Math", r);
    else if (!s.font.empty()) { std::wstring f = widen(g.resolveFamily(familyCandidates(s.font))); P.lay->SetFontFamilyName(f.c_str(), r); }
    if (s.fmt & F_BOLD) P.lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
    if (s.fmt & F_ITALIC) P.lay->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
    if ((s.fmt & F_UNDER) || !s.link.empty()) P.lay->SetUnderline(TRUE, r);
    if (s.fmt & F_STRIKE) P.lay->SetStrikethrough(TRUE, r);
    if (s.fmt & F_CODE) { P.lay->SetFontFamilyName(mono.c_str(), r); P.lay->SetFontSize(spanSize * 0.9f, r); }
    if (s.fmt & (F_SUB | F_SUP)) P.lay->SetFontSize(spanSize * 0.7f, r);
    if (!s.link.empty()) P.lay->SetDrawingEffect(L.linkBr.get(), r);
    else if (s.color) P.lay->SetDrawingEffect(colorBrush(g, L, s.color), r);
  }
  if (P.pre > 0) {
    DWRITE_TEXT_RANGE r{0, UINT32(P.pre)};
    P.lay->SetDrawingEffect(L.dimBr.get(), r);
    if (prefixBold) P.lay->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
  }
  DWRITE_TEXT_METRICS m{};
  P.lay->GetMetrics(&m);
  P.h = m.height > 0 ? m.height : sizePx * 1.3f;
  // line bottoms for the paginator
  P.lineB.clear();
  UINT32 n = 0;
  P.lay->GetLineMetrics(nullptr, 0, &n);
  if (n > 0 && n < 100000) {
    vector<DWRITE_LINE_METRICS> lm(n);
    if (SUCCEEDED(P.lay->GetLineMetrics(lm.data(), n, &n))) {
      float y = 0;
      for (UINT32 i = 0; i < n; i++) { y += lm[i].height; P.lineB.push_back(y); }
    }
  }
}

// marks (highlighter) and citation fields of a paragraph
void drawMarks(Ui& ui, const Para& p, const WPara& P, float ox, float oy) {
  size_t off = 0;
  for (auto& s : p.spans) {
    size_t a = off, b = off + s.text.size();
    off = b;
    if (a >= b) continue;
    if (s.fmt & F_MARK) for (auto& r : rangeRects(P, int(a), int(b))) ui.fill({ox + r.left, oy + r.top, r.right - r.left, r.bottom - r.top}, kMark, 2);
    else if (s.isCite()) for (auto& r : rangeRects(P, int(a), int(b))) ui.fill({ox + r.left - 1, oy + r.top, r.right - r.left + 2, r.bottom - r.top}, kCiteBg, 2);
  }
}

string romanLower(int n) {
  static const char* ones[] = {"", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix"};
  static const char* tens[] = {"", "x", "xx", "xxx", "xl", "l", "lx", "lxx", "lxxx", "xc"};
  if (n <= 0 || n >= 100) return std::to_string(n);
  return string(tens[n / 10]) + ones[n % 10];
}

string sizeLabel(float pt) {
  char b[16];
  if (std::fabs(pt - std::round(pt)) < 0.01f) snprintf(b, sizeof b, "%d", int(std::lround(pt)));
  else snprintf(b, sizeof b, "%.1f", pt);
  return b;
}

// a paper as one line for the citation picker
string recLine(const Record& r) {
  string au = r.authors.empty() ? string("Anonymous") : r.authors[0] + (r.authors.size() > 1 ? " et al." : "");
  return au + " (" + (r.year ? std::to_string(r.year) : string("n.d.")) + ")";
}

bool recMatches(const Record& r, const vector<string>& words) {
  if (words.empty()) return true;
  string hay = lower(asciiFold(r.title + " " + join(r.authors, " ") + " " + r.source + " " + std::to_string(r.year) + " " + r.doi));
  for (auto& w : words) if (hay.find(w) == string::npos) return false;
  return true;
}

}  // namespace

// ------------------------------------------------------------------ fonts
const vector<string>& App::writerFonts() {
  if (!writerFonts_.empty()) return writerFonts_;
  std::set<string> names;
  Com<IDWriteFontCollection> fc;
  if (g.dw && SUCCEEDED(g.dw->GetSystemFontCollection(fc.put(), FALSE)) && fc) {
    UINT32 n = fc->GetFontFamilyCount();
    for (UINT32 i = 0; i < n && i < 2000; i++) {
      Com<IDWriteFontFamily> fam;
      if (FAILED(fc->GetFontFamily(i, fam.put())) || !fam) continue;
      Com<IDWriteLocalizedStrings> ls;
      if (FAILED(fam->GetFamilyNames(ls.put())) || !ls) continue;
      UINT32 idx = 0;
      BOOL exists = FALSE;
      ls->FindLocaleName(L"en-us", &idx, &exists);
      if (!exists) idx = 0;
      UINT32 len = 0;
      ls->GetStringLength(idx, &len);
      std::wstring w(len + 1, L'\0');
      if (FAILED(ls->GetString(idx, &w[0], len + 1))) continue;
      w.resize(len);
      string nm = narrow(w);
      if (nm.empty() || nm[0] == '@' || startsWith(nm, "Marlett") || startsWith(nm, "Segoe MDL") || startsWith(nm, "Segoe Fluent") || nm.find("Symbol") != string::npos ||
          nm.find("Wingdings") != string::npos || nm.find("Webdings") != string::npos || startsWith(nm, "MS Outlook") || startsWith(nm, "MT Extra") || startsWith(nm, "HoloLens"))
        continue;
      names.insert(nm);
    }
  }
  for (const char* f : {"Calibri", "Cambria", "Arial", "Georgia", "Times New Roman"}) names.insert(f);
  writerFonts_.assign(names.begin(), names.end());
  return writerFonts_;
}

// ------------------------------------------------------------------ layout
void App::writerRelayout(float k) {
  if (!wlay) wlay = std::make_shared<WriterLayout>();
  WriterLayout& L = *wlay;
  const PageSetup& ps = wdoc.setup;
  float textW = float(ps.textW()) * k;
  uint64_t key = fnv(1469598103934665603ULL, &k, sizeof k);
  key = fnvS(key, ps.font);
  key = fnvS(key, ps.headingFont);
  key = fnv(key, &ps.baseSize, sizeof ps.baseSize);
  key = fnv(key, &ps.lineSpacing, sizeof ps.lineSpacing);
  key = fnv(key, &ps.numberedHeadings, sizeof ps.numberedHeadings);
  key = fnv(key, &textW, sizeof textW);
  // nothing changed since the last pass (the usual frame): keep every layout and page as it is
  uint64_t stamp = fnv(1469598103934665603ULL, &wed.version, sizeof wed.version);
  { size_t nb = wdoc.blocks.size(), nr = wdoc.refs.size(), na = wdoc.assets.size(); double ph = ps.pageH(), mg = ps.margin();
    stamp = fnv(stamp, &nb, sizeof nb); stamp = fnv(stamp, &nr, sizeof nr); stamp = fnv(stamp, &na, sizeof na); stamp = fnv(stamp, &ph, sizeof ph); stamp = fnv(stamp, &mg, sizeof mg); stamp = fnvS(stamp, wdoc.citeStyle); }
  if (key == L.key && stamp == L.stamp && L.inkBr && L.blocks.size() == wdoc.blocks.size()) return;
  L.stamp = stamp;
  if (key != L.key || !L.inkBr) {
    L.blocks.clear();
    L.images.clear();
    L.key = key;
    L.k = k;
    L.textW = textW;
    L.family = g.resolveFamily(familyCandidates(ps.font));
    L.headFamily = ps.headingFont.empty() ? L.family : g.resolveFamily(familyCandidates(ps.headingFont));
    L.mono = g.monoFamily;
    if (!L.inkBr) {
      g.dc->CreateSolidColorBrush(d2c(kInk), L.inkBr.put());
      g.dc->CreateSolidColorBrush(d2c(kInkDim), L.dimBr.put());
      g.dc->CreateSolidColorBrush(d2c(kLink), L.linkBr.put());
    }
  }
  L.pageWL = float(ps.pageW()) * k;
  L.pageHL = float(ps.pageH()) * k;
  L.ML = float(ps.margin()) * k;
  L.textHL = std::max(40.f, L.pageHL - 2 * L.ML);
  L.gapL = 18 * k;
  // layouts survive by signature, so inserting a paragraph at the top does not re-lay out everything beneath it
  vector<WBlock> old;
  old.swap(L.blocks);
  std::unordered_map<uint64_t, size_t> pool;
  pool.reserve(old.size() * 2 + 1);
  for (size_t i = 0; i < old.size(); i++) pool.emplace(old[i].sig, i);
  L.blocks.resize(wdoc.blocks.size());
  auto reuse = [&](uint64_t sig, WBlock& W) {  // true when a previous layout with this signature was moved into W
    auto it = pool.find(sig);
    if (it == pool.end() || old[it->second].sig != sig) return false;
    W = std::move(old[it->second]);
    old[it->second].sig = 0;  // consumed (a second block with the same content is laid out afresh)
    return true;
  };
  vector<int> nums = docListNumbers(wdoc);
  vector<string> hn = docHeadingNumbers(wdoc);
  vector<int> outline = wdoc.outline();
  const bool numericCites = citeStyleInfo(wdoc.citeStyle).numeric;
  float y = 0;
  for (size_t i = 0; i < wdoc.blocks.size(); i++) {
    const Block& b = wdoc.blocks[i];
    WBlock& W = L.blocks[i];
    int kind = int(b.kind);
    uint64_t h = fnv(1469598103934665603ULL, &kind, sizeof kind);
    if (b.kind == Block::Paragraph) {
      const Para& p = b.p;
      SSpec sp = specOf(p.style, ps.baseSize, p.level);
      string prefix, label;
      bool preBold = false;
      if (isHeading(p.style) && ps.numberedHeadings && !hn[i].empty()) prefix = hn[i] + "  ";
      else if (p.style == PStyle::Caption) {
        int t = tableForCaption(wdoc, int(i));
        if (t >= 0 && !captionHasLabel(p.text())) { prefix = "Table " + std::to_string(wdoc.tableNumber(t)) + ". "; preBold = true; }
      }
      if (isList(p.style)) {
        if (p.style == PStyle::Bullet) label = p.level == 1 ? "\xE2\x80\x93" : p.level >= 2 ? "\xE2\x96\xAA" : "\xE2\x80\xA2";
        else label = (p.level == 1 ? string(1, char('a' + (nums[i] - 1) % 26)) : p.level >= 2 ? romanLower(nums[i]) : std::to_string(nums[i])) + ".";
      } else if (p.style == PStyle::Reference && numericCites && p.refKey.empty()) label = "[" + std::to_string(wdoc.refNumber(int(i))) + "]";
      h = sigPara(h, p);
      h = fnvS(h, prefix);
      h = fnvS(h, label);
      if (h != W.sig && !reuse(h, W)) {
        W = WBlock();
        W.sig = h;
        W.label = label;
        W.labelSize = sp.size * k;
        float ind = sp.indent * k, hang = (p.style == PStyle::Reference && label.empty() ? 0 : sp.hanging) * k;
        W.labelX = ind;
        W.para.x = ind + hang;
        W.para.wd = std::max(20.f, textW - ind - hang - (p.style == PStyle::Code ? 8 * k : 0));
        W.para.y = sp.before * k + (p.style == PStyle::Code ? 6 * k : 0);
        buildPara(g, L, p, prefix, preBold, sp, k, ps.lineSpacing, W.para);
        W.h = W.para.y + W.para.h + sp.after * k + (p.style == PStyle::Code ? 6 * k : 0);
      }
    } else if (b.kind == Block::TableBlock) {
      const Table& t = b.tbl;
      int R = t.rows(), C = t.cols();
      h = fnv(h, &R, sizeof R); h = fnv(h, &C, sizeof C); h = fnv(h, &t.header, sizeof t.header);
      h = fnv(h, t.align.data(), t.align.size());
      h = fnv(h, t.widths.data(), t.widths.size() * sizeof(float));
      for (auto& row : t.cells) for (auto& c : row) h = sigPara(h, c);
      if (h != W.sig && !reuse(h, W)) {
        W = WBlock();
        W.sig = h;
        float pad = 5 * k;
        W.colW.assign(size_t(C), C ? textW / float(C) : textW);
        if (int(t.widths.size()) == C) {
          float sum = 0;
          for (float w : t.widths) sum += std::max(0.f, w);
          if (sum > 0) for (int c = 0; c < C; c++) W.colW[size_t(c)] = textW * std::max(0.f, t.widths[size_t(c)]) / sum;
        }
        W.colX.assign(size_t(C), 0);
        for (int c = 1; c < C; c++) W.colX[size_t(c)] = W.colX[size_t(c) - 1] + W.colW[size_t(c) - 1];
        W.cells.resize(size_t(R) * size_t(C));
        W.rowY.assign(size_t(R), 0);
        W.rowH.assign(size_t(R), 0);
        float yy = 4 * k;
        for (int r = 0; r < R; r++) {
          float rh = 0;
          for (int c = 0; c < C; c++) {
            WPara& P = W.cells[size_t(r) * size_t(C) + size_t(c)];
            Para cell = t.cells[size_t(r)][size_t(c)];
            char al = size_t(c) < t.align.size() ? t.align[size_t(c)] : 'l';
            if (al == 'c') cell.align = PAlign::Center;
            else if (al == 'r') cell.align = PAlign::Right;
            else if (al == 'a') {  // numbers to the right
              string tx = trim(cell.text());
              bool num = !tx.empty();
              for (char ch : tx) if (!(isdigit(uint8_t(ch)) || ch == '.' || ch == ',' || ch == '%' || ch == '-' || ch == '+' || ch == ' ')) { num = false; break; }
              if (num) cell.align = PAlign::Right;
            }
            SSpec sp = specOf(PStyle::Body, ps.baseSize * 0.92f, 0);
            sp.after = 0;
            if (r == 0 && t.header) sp.weight = 600;
            P.x = W.colX[size_t(c)] + pad;
            P.wd = std::max(10.f, W.colW[size_t(c)] - 2 * pad);
            buildPara(g, L, cell, "", false, sp, k, 1.f, P);
            rh = std::max(rh, P.h);
          }
          W.rowY[size_t(r)] = yy;
          W.rowH[size_t(r)] = rh + 2 * pad;
          for (int c = 0; c < C; c++) W.cells[size_t(r) * size_t(C) + size_t(c)].y = yy + pad;
          yy += rh + 2 * pad;
        }
        W.h = yy + 10 * k;
      }
    } else if (b.kind == Block::FigureBlock) {
      const Figure& f = b.fig;
      int fn = wdoc.figureNumber(int(i));
      h = fnv(h, &f.asset, sizeof f.asset); h = fnv(h, &f.widthPct, sizeof f.widthPct); h = fnv(h, &fn, sizeof fn);
      h = fnvS(h, f.label); h = fnv(h, &f.captionAbove, sizeof f.captionAbove); h = fnv(h, &f.border, sizeof f.border); int al = int(f.align); h = fnv(h, &al, sizeof al);
      h = sigPara(h, f.caption);
      const FigAsset* a = f.asset >= 0 && size_t(f.asset) < wdoc.assets.size() ? &wdoc.assets[size_t(f.asset)] : nullptr;
      if (a) { h = fnv(h, &a->scene.W, sizeof a->scene.W); h = fnv(h, &a->scene.H, sizeof a->scene.H); size_t n = a->scene.items.size(); h = fnv(h, &n, sizeof n); }
      if (h != W.sig && !reuse(h, W)) {
        W = WBlock();
        W.sig = h;
        float iw = textW * clampv(f.widthPct, 10, 100) / 100.f;
        float ih = a && a->scene.W > 0 ? iw * float(a->scene.H / a->scene.W) : 60 * k;
        float ix = f.align == PAlign::Left ? 0 : f.align == PAlign::Right ? textW - iw : (textW - iw) / 2;
        string prefix = captionHasLabel(f.caption.text()) || f.label == "-" ? string() : f.labelText() + " " + std::to_string(fn) + ". ";
        Para cap = f.caption;
        cap.style = PStyle::Caption;
        SSpec sp = specOf(PStyle::Caption, ps.baseSize, 0);
        W.para.x = 0;
        W.para.wd = textW;
        if (f.captionAbove) {
          W.para.y = 6 * k;
          buildPara(g, L, cap, prefix, true, sp, k, 1.f, W.para);
          W.img = {ix, W.para.y + W.para.h + 6 * k, iw, ih};
          W.h = W.img.b() + 12 * k;
        } else {
          W.img = {ix, 6 * k, iw, ih};
          W.para.y = W.img.b() + 6 * k;
          buildPara(g, L, cap, prefix, true, sp, k, 1.f, W.para);
          W.h = W.para.y + W.para.h + 10 * k;
        }
      }
    } else if (b.kind == Block::Toc) {
      for (int o : outline) { h = fnvS(h, wdoc.blocks[size_t(o)].p.text()); h = fnvS(h, hn[size_t(o)]); }
      if (h != W.sig && !reuse(h, W)) {
        W = WBlock();
        W.sig = h;
        for (int o : outline) {
          const Para& hp = wdoc.blocks[size_t(o)].p;
          string t = (ps.numberedHeadings && !hn[size_t(o)].empty() ? hn[size_t(o)] + "  " : string()) + hp.text();
          W.toc.push_back({t, 14.f * k * float(headingLevel(hp.style) - 1)});
        }
        W.h = (22 + 16 * float(W.toc.size()) + 12) * k * (ps.baseSize / 11.f);
      }
    } else {  // Rule, PageBreak
      if (h != W.sig && !reuse(h, W)) { W = WBlock(); W.sig = h; W.h = (b.kind == Block::Rule ? 22 : 24) * k; }
    }
    y += W.h;
  }
  L.contentH = y;

  // ---- pagination: blocks onto pages; paragraphs split between lines (2 lines stay together on each side), tables
  // between rows, headings keep at least a few lines of their text with them, a page break opens a new page
  const float textH = L.textHL;
  const float lineBody = ps.baseSize * 1.25f * k;
  int page = 0;
  float py = 0;
  L.pagePlaces.assign(1, {});
  auto newPage = [&]() { page++; py = 0; L.pagePlaces.emplace_back(); };
  auto place = [&](int blk, WBlock& W, float y0, float y1, float at) {
    W.places.push_back({page, y0, y1, at});
    L.pagePlaces[size_t(page)].push_back({blk, int(W.places.size()) - 1});
  };
  for (size_t i = 0; i < wdoc.blocks.size(); i++) {
    const Block& b = wdoc.blocks[i];
    WBlock& W = L.blocks[i];
    W.places.clear();
    if (b.kind == Block::PageBreak) {
      place(int(i), W, 0, W.h, py);
      newPage();
      continue;
    }
    bool heading = b.kind == Block::Paragraph && (isHeading(b.p.style) || b.p.style == PStyle::Title);
    if (heading && py > 0 && W.h + 3 * lineBody > textH - py) newPage();
    float local = 0;
    int guard = 0;
    while (local < W.h - 0.01f && guard++ < 10000) {
      float remaining = textH - py;
      float need = W.h - local;
      if (need <= remaining + 0.01f) { place(int(i), W, local, W.h, py); py += need; break; }
      float split = -1;
      if (b.kind == Block::Paragraph && W.para.lineB.size() >= 4) {
        int n = int(W.para.lineB.size());
        for (int j = n - 3; j >= 1; j--) {  // lines 0..j stay (>= 2), lines j+1.. move (>= 2)
          float cand = W.para.y + W.para.lineB[size_t(j)];
          if (cand <= local + remaining && cand > local + 1) { split = cand; break; }
        }
      } else if (b.kind == Block::TableBlock && W.rowY.size() >= 2) {
        for (int r = int(W.rowY.size()) - 1; r >= 1; r--) {
          float cand = W.rowY[size_t(r)];
          if (cand <= local + remaining && cand > local + 1) { split = cand; break; }
        }
      } else if (b.kind == Block::Toc && W.toc.size() > 6) {
        float lh = 16 * k * (ps.baseSize / 11.f), top = (22 + 10) * k * (ps.baseSize / 11.f);
        for (int j = int(W.toc.size()) - 2; j >= 2; j--) {
          float cand = top + lh * float(j);
          if (cand <= local + remaining && cand > local + 1) { split = cand; break; }
        }
      }
      if (split > local) { place(int(i), W, local, split, py); local = split; newPage(); continue; }
      if (py > 0) { newPage(); continue; }
      // taller than a page on its own: cut at the page edge
      float end = std::min(W.h, local + textH);
      place(int(i), W, local, end, 0);
      local = end;
      if (local < W.h - 0.01f) newPage();
      else py = textH;
    }
    if (W.h <= 0.01f && W.places.empty()) place(int(i), W, 0, 0, py);
  }
  L.pages = page + 1;
  writerPages_ = L.pages;
}

// ------------------------------------------------------------------ coordinates (layout pixels; the page column starts at (0, 0) = top left of page 1)
float App::writerDocY(int blk, float localY) const {
  if (!wlay || blk < 0 || size_t(blk) >= wlay->blocks.size()) return 0;
  const WriterLayout& L = *wlay;
  const WBlock& W = L.blocks[size_t(blk)];
  if (W.places.empty()) return 0;
  const WPlace* pl = &W.places.back();
  for (auto& p : W.places) if (localY < p.y1) { pl = &p; break; }
  float ly = clampv(localY, pl->y0, std::max(pl->y0, pl->y1));
  return float(pl->page) * (L.pageHL + L.gapL) + L.ML + pl->py + (ly - pl->y0);
}

bool App::writerDocToLocal(float, float ly, int& blk, float& localY) const {
  if (!wlay || wlay->blocks.empty()) return false;
  const WriterLayout& L = *wlay;
  float ph = L.pageHL + L.gapL;
  int page = clampv(int(std::floor(ly / std::max(1.f, ph))), 0, L.pages - 1);
  float yIn = ly - float(page) * ph - L.ML;
  // an empty page (after a trailing page break): the end of the previous page
  int pg = page;
  while (pg > 0 && L.pagePlaces[size_t(pg)].empty()) pg--;
  const auto& pp = L.pagePlaces[size_t(pg)];
  if (pp.empty()) { blk = 0; localY = 0; return true; }
  if (pg != page) yIn = 1e9f;
  for (auto& e : pp) {
    const WPlace& pl = L.blocks[size_t(e.first)].places[size_t(e.second)];
    if (yIn < pl.py + (pl.y1 - pl.y0)) { blk = e.first; localY = pl.y0 + std::max(0.f, yIn - pl.py); return true; }
  }
  const WPlace& last = L.blocks[size_t(pp.back().first)].places[size_t(pp.back().second)];
  blk = pp.back().first;
  localY = std::max(last.y0, last.y1 - 0.01f);
  return true;
}

int App::writerHitBlock(float, float py) const {
  int blk = 0;
  float local = 0;
  if (!writerDocToLocal(0, py, blk, local)) return -1;
  return blk;
}

// px: from the text area's left edge; py: page-column y
DocPos App::writerHit(float px, float py) const {
  DocPos p;
  if (!wlay || wlay->blocks.empty() || wdoc.blocks.empty()) return p;
  int bi = 0;
  float ly = 0;
  if (!writerDocToLocal(px, py, bi, ly)) return p;
  bi = clampv(bi, 0, int(std::min(wdoc.blocks.size(), wlay->blocks.size())) - 1);
  const WBlock& W = wlay->blocks[size_t(bi)];
  const Block& b = wdoc.blocks[size_t(bi)];
  p.blk = bi;
  if (b.kind == Block::Paragraph) {
    p.cell = -1;
    p.off = byteAt(W.para, px - W.para.x, ly - W.para.y);
  } else if (b.kind == Block::TableBlock) {
    int R = b.tbl.rows(), C = b.tbl.cols();
    if (R == 0 || C == 0 || W.cells.size() != size_t(R) * size_t(C)) { p.cell = -1; return p; }
    int r = R - 1, c = C - 1;
    for (int i = 0; i < R; i++) if (ly < W.rowY[size_t(i)] + W.rowH[size_t(i)]) { r = i; break; }
    for (int i = 0; i < C; i++) if (px < W.colX[size_t(i)] + W.colW[size_t(i)]) { c = i; break; }
    if (px < 0) c = 0;
    const WPara& P = W.cells[size_t(r) * size_t(C) + size_t(c)];
    p.cell = r * C + c;
    p.off = byteAt(P, px - P.x, ly - P.y);
  } else if (b.kind == Block::FigureBlock) {
    bool onCaption = b.fig.captionAbove ? ly < W.img.y - 3 * wlay->k : ly >= W.img.b() + 3 * wlay->k;
    if (!onCaption) { p.cell = -1; p.off = 0; }
    else { p.cell = -2; p.off = byteAt(W.para, px - W.para.x, ly - W.para.y); }
  } else { p.cell = -1; p.off = 0; }
  return p;
}

bool App::writerCaretLocal(const DocPos& q, float& x, float& y, float& h) const {
  if (!wlay || q.blk < 0 || size_t(q.blk) >= wlay->blocks.size() || size_t(q.blk) >= wdoc.blocks.size()) return false;
  const WBlock& W = wlay->blocks[size_t(q.blk)];
  const Block& b = wdoc.blocks[size_t(q.blk)];
  const WPara* P = nullptr;
  if (b.kind == Block::Paragraph && q.cell == -1) P = &W.para;
  else if (b.kind == Block::FigureBlock && q.cell == -2) P = &W.para;
  else if (b.kind == Block::TableBlock && q.cell >= 0 && size_t(q.cell) < W.cells.size()) P = &W.cells[size_t(q.cell)];
  if (!P) { x = 0; y = 0; h = W.h; return true; }
  float cx = 0, cy = 0, ch = 0;
  if (!caretAt(*P, q.off, cx, cy, ch)) return false;
  x = P->x + cx;
  y = P->y + cy;
  h = ch;
  return true;
}

bool App::writerCaretRect(const DocPos& q, float& x, float& y, float& h) const {  // page-column coordinates
  float lx = 0, ly = 0, lh = 0;
  if (!writerCaretLocal(q, lx, ly, lh)) return false;
  x = lx;
  y = writerDocY(q.blk, ly);
  h = lh;
  return true;
}

namespace {
// text units in document order: (blk, cell) of paragraphs, table cells and captions; other blocks as (blk, -1)
bool nextUnit(const Document& d, DocPos& p, int dir) {
  int n = int(d.blocks.size());
  if (n == 0) return false;
  int blk = p.blk, cell = p.cell;
  if (d.blocks[size_t(blk)].kind == Block::TableBlock) {
    int cells = d.blocks[size_t(blk)].tbl.rows() * d.blocks[size_t(blk)].tbl.cols();
    int nc = (cell < 0 ? -1 : cell) + dir;
    if (nc >= 0 && nc < cells) { p.cell = nc; p.off = 0; return true; }
  }
  blk += dir;
  if (blk < 0 || blk >= n) return false;
  const Block& b = d.blocks[size_t(blk)];
  p.blk = blk;
  p.off = 0;
  if (b.kind == Block::TableBlock) { int cells = b.tbl.rows() * b.tbl.cols(); p.cell = cells ? (dir > 0 ? 0 : cells - 1) : -1; }
  else if (b.kind == Block::FigureBlock) p.cell = -2;
  else p.cell = -1;
  return true;
}
}  // namespace

void App::writerMoveLine(int dir, bool extend) {
  if (wdoc.blocks.empty()) return;
  writerRelayout(writerK_);  // cheap when nothing changed; the caret must move through current layouts
  float cx = 0, cy = 0, ch = 0;
  DocPos p = wed.caret;
  if (!writerCaretLocal(p, cx, cy, ch)) return;
  if (writerCaretX_ < 0) writerCaretX_ = cx;
  const WBlock* W = &wlay->blocks[size_t(p.blk)];
  const Block* b = &wdoc.blocks[size_t(p.blk)];
  const WPara* P = nullptr;
  if (b->kind == Block::Paragraph && p.cell == -1) P = &W->para;
  else if (b->kind == Block::FigureBlock && p.cell == -2) P = &W->para;
  else if (b->kind == Block::TableBlock && p.cell >= 0 && size_t(p.cell) < W->cells.size()) P = &W->cells[size_t(p.cell)];
  if (P) {  // another line of the same unit?
    float ly = cy - P->y + (dir > 0 ? ch + 1 : -1);
    if (ly >= 0 && ly < P->h) {
      int off = byteAt(*P, writerCaretX_ - P->x, ly);
      DocPos q = p;
      q.off = off;
      if (q != p) { wed.setCaret(q, extend); writerFollow_ = true; return; }
    }
  }
  DocPos q = p;
  // tables: down / up moves to the cell beneath / above, not the next cell in the row
  if (b->kind == Block::TableBlock && p.cell >= 0) {
    int C = b->tbl.cols(), R = b->tbl.rows();
    int r = p.cell / std::max(1, C) + dir, c = p.cell % std::max(1, C);
    if (r >= 0 && r < R) {
      q.cell = r * C + c;
      const WPara& T = W->cells[size_t(q.cell)];
      q.off = byteAt(T, writerCaretX_ - T.x, dir > 0 ? 1 : T.h - 1);
      wed.setCaret(q, extend);
      writerFollow_ = true;
      return;
    }
    q.cell = dir > 0 ? R * C - 1 : 0;
  }
  if (!nextUnit(wdoc, q, dir)) { if (dir > 0) wed.docEnd(extend); else wed.docStart(extend); writerFollow_ = true; return; }
  if (size_t(q.blk) >= wlay->blocks.size()) { wed.setCaret(q, extend); writerFollow_ = true; return; }
  const WBlock& W2 = wlay->blocks[size_t(q.blk)];
  const Block& b2 = wdoc.blocks[size_t(q.blk)];
  const WPara* P2 = nullptr;
  if (b2.kind == Block::Paragraph) P2 = &W2.para;
  else if (b2.kind == Block::FigureBlock) P2 = &W2.para;
  else if (b2.kind == Block::TableBlock && q.cell >= 0 && size_t(q.cell) < W2.cells.size()) P2 = &W2.cells[size_t(q.cell)];
  if (P2) q.off = byteAt(*P2, writerCaretX_ - P2->x, dir > 0 ? 1 : P2->h - 1);
  wed.setCaret(q, extend);
  writerFollow_ = true;
}

void App::writerLineEdge(bool end, bool extend) {
  writerRelayout(writerK_);
  DocPos p = wed.caret;
  const Para* para = docPara(wdoc, p);
  if (!para || size_t(p.blk) >= wlay->blocks.size()) return;
  const WBlock& W = wlay->blocks[size_t(p.blk)];
  const WPara* P = p.cell >= 0 && size_t(p.cell) < W.cells.size() ? &W.cells[size_t(p.cell)] : &W.para;
  float cx = 0, cy = 0, ch = 0;
  if (!caretAt(*P, p.off, cx, cy, ch)) return;
  DocPos q = p;
  q.off = byteAt(*P, end ? 1e6f : -1e6f, cy + ch / 2);
  wed.setCaret(q, extend);
  writerCaretX_ = -1;
  writerFollow_ = true;
}

// ------------------------------------------------------------------ open / project

// The text-colour button with its palette popup (toolbar and mini toolbar share it); key names the popup.
void App::writerColorButton(const Rect& b, const string& key, bool inText) {
  const float s = ui.s;
  uint64_t idv = ui.id(key + "btn");
  bool hov = false;
  bool open = ui.isPopupOpen(key);
  if (ui.behave(idv, b, &hov) && inText) { if (open) ui.closePopup(); else ui.openPopup(key); }
  if (hov || open) ui.fill(b, ui.c.hover, 6 * s);
  uint32_t here = wed.colorHere();
  ui.icon("textcolor", b.x + b.w / 2, b.y + b.h / 2 - 1 * s, 15 * s, inText ? ui.c.text : ui.c.textFaint, 1.7f);
  ui.fill({b.x + 6 * s, b.b() - 6 * s, b.w - 12 * s, 3.5f * s}, here ? fromArgb(here) : (ui.dark ? Color(0.9f, 0.9f, 0.9f) : Color(0.15f, 0.15f, 0.15f)), 1);
  ui.tipFor(idv, "Text colour");
  if (open) {
    Rect anchor = b;
    ui.overlay([this, anchor, s]() {
      const float cs = 22 * s, gp = 5 * s;
      const int per = 9;
      int rows = (kTextColorCount + per - 1) / per;
      Rect pr{anchor.x, anchor.b() + 4 * s, per * (cs + gp) + gp, 30 * s + float(rows) * (cs + gp) + gp};
      if (pr.r() > float(g.W) - 8 * s) pr.x = float(g.W) - 8 * s - pr.w;
      if (pr.b() > float(g.H) - 8 * s) pr.y = anchor.y - 4 * s - pr.h;
      ui.shadow(pr, 8 * s);
      ui.fill(pr, ui.c.panel2, 8 * s);
      ui.stroke(pr, ui.c.border, 8 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 8 * s, pr.y + 4 * s, pr.w - 16 * s, 22 * s}, "Text colour", 12 * s, ui.c.textDim, AL_LEFT, 600);
      uint32_t here = wed.colorHere();
      for (int i = 0; i < kTextColorCount; i++) {
        Rect cr{pr.x + gp + float(i % per) * (cs + gp), pr.y + 30 * s + float(i / per) * (cs + gp), cs, cs};
        bool h2 = false;
        uint64_t cid = ui.id("wcol:" + std::to_string(i));
        if (ui.behave(cid, cr, &h2)) { wed.setColor(kTextColors[i]); ui.closePopup(); }
        if (kTextColors[i] == 0) {
          ui.fill(cr, ui.c.input, 4 * s);
          ui.text(cr, "A", 13 * s, ui.c.text, AL_CENTER, 600);
        } else ui.fill(cr, fromArgb(kTextColors[i]), 4 * s);
        ui.stroke(cr, here == kTextColors[i] ? ui.c.accent : (h2 ? ui.c.text : ui.c.borderStrong), 4 * s, here == kTextColors[i] ? 2 : 1);
        ui.tipFor(cid, kTextColors[i] == 0 ? "Automatic (the style's colour)" : "");
      }
    });
  }
}


// Word's mini toolbar: a compact format bar that appears above the pointer when a mouse gesture ends with a text
// selection, and on top of the context menu. It fades away when the pointer leaves it, when a key is pressed, when
// the selection goes or the page scrolls. Its buttons run before the page's hit area (drawWriter skips the page's
// behave under it), so they win the click although they are drawn last.
void App::drawWriterMini(const Rect& pageArea) {
  if (!writerMiniShow_) { writerMiniR_ = {0, 0, 0, 0}; return; }
  const float s = ui.s;
  const bool ctxOpen = ui.isPopupOpen("wctx");
  const bool inText = docPara(wdoc, wed.caret) != nullptr;
  const bool hold = ui.anyPopup();  // a list opened from the bar (or the context menu) keeps it
  if (!inText || (!hold && !wed.hasSelection() && !ctxOpen)) { writerMiniShow_ = false; writerMiniR_ = {0, 0, 0, 0}; return; }
  if (!hold && writerMiniR_.w > 0) {
    float dx = std::max({writerMiniR_.x - ui.in.mx, 0.f, ui.in.mx - writerMiniR_.r()}), dy = std::max({writerMiniR_.y - ui.in.my, 0.f, ui.in.my - writerMiniR_.b()});
    if (std::hypot(dx, dy) > 150 * s) { writerMiniShow_ = false; writerMiniR_ = {0, 0, 0, 0}; return; }
  }
  // two rows, like Word's: paragraph style, font and size above; character formatting, lists, citation and link
  // below — a compact block that stays within reach of the pointer instead of one long strip
  const float bw = 26 * s, gap = 3 * s, rowH = 26 * s, pad = 4 * s;
  const bool showFont = pageArea.w > 420 * s;
  const float styleW = 104 * s, fontW = showFont ? 124 * s : 0, sizeW = 58 * s;
  const float row1 = pad + styleW + gap + (showFont ? fontW + gap : 0) + sizeW + gap + 2 * (bw + gap) - gap + pad;
  const float row2 = pad + 5 * (bw + gap) + 30 * s + gap + 8 * s + 2 * (bw + gap) + 8 * s + 2 * (bw + gap) - gap + pad;
  const float w = std::max(row1, row2), h = pad + rowH + pad + rowH + pad;
  float x = writerMiniX_ - 26 * s, y;
  if (writerMiniCtx_) y = (writerCtxR_.w > 0 ? writerCtxR_.y : writerMiniY_) - h - 6 * s;
  else y = writerMiniY_ - h - 14 * s;
  x = clampv(x, pageArea.x + 8 * s, std::max(pageArea.x + 8 * s, pageArea.r() - w - 16 * s));
  if (y < pageArea.y + 6 * s) y = writerMiniCtx_ && writerCtxR_.w > 0 ? writerCtxR_.b() + 6 * s : writerMiniY_ + 22 * s;  // below the pointer / menu when there is no room above
  y = clampv(y, pageArea.y + 6 * s, std::max(pageArea.y + 6 * s, pageArea.b() - h - 6 * s));
  Rect r{x, y, w, h};
  writerMiniR_ = r;
  ui.shadow(r, 8 * s, 14 * s);
  ui.fill(r, ui.c.panel2, 8 * s);
  ui.stroke(r, ui.c.border, 8 * s);
  const PageSetup& ps = wdoc.setup;
  const PStyle styleHere = wed.styleHere();
  const uint16_t fmtHere = wed.fmtHere();
  float bx = r.x + pad, by = r.y + pad;
  const float bh = rowH;
  auto sepLine = [&]() { bx += 4 * s; ui.line(bx, by + 4 * s, bx, by + bh - 4 * s, ui.c.border, 1); bx += 4 * s; };
  auto ib = [&](const char* icon, const char* tip, bool on = false, bool en = true) { Rect b{bx, by, bw, bh}; bx += bw + gap; return ui.iconButton(b, icon, tip, on, en); };
  // ---- row 1
  {
    vector<string> names;
    for (int i = 0; i < kStyleCount; i++) names.push_back(pstyleName(kStyleOrder[i]));
    int sel = styleIndex(styleHere);
    if (ui.combo({bx, by, styleW, bh}, "wmstyle", names, sel)) { wed.setStyle(kStyleOrder[sel]); writerFollow_ = true; }
    bx += styleW + gap;
  }
  if (showFont) {
    const vector<string>& fonts = writerFonts();
    vector<string> opts;
    opts.push_back("Document font");
    for (auto& f : fonts) opts.push_back(f);
    string here = wed.fontHere();
    int sel = 0;
    for (size_t i = 0; i < fonts.size(); i++) if (fonts[i] == here) sel = int(i) + 1;
    if (ui.combo({bx, by, fontW, bh}, "wmfont", opts, sel)) wed.setFont(sel == 0 ? string() : fonts[size_t(sel - 1)]);
    bx += fontW + gap;
  }
  {
    float cur = wed.sizeHere();
    float shown = cur > 0 ? cur : (styleHere == PStyle::Body ? ps.baseSize : specOf(styleHere, ps.baseSize, 0).size);
    vector<string> opts;
    int sel = 0;
    float best = 1e9;
    for (int i = 0; i < kSizeCount; i++) { opts.push_back(sizeLabel(kSizes[i])); if (std::fabs(kSizes[i] - shown) < best) { best = std::fabs(kSizes[i] - shown); sel = i; } }
    if (best > 0.05f) { opts.insert(opts.begin(), sizeLabel(shown)); sel = 0; }
    int before = sel;
    if (ui.combo({bx, by, sizeW, bh}, "wmsize", opts, sel) && sel != before) wed.setSize(float(atof(opts[size_t(sel)].c_str())));
    bx += sizeW + gap;
    if (ib("minus", "Smaller  (Ctrl+Shift+<)")) { int idx = 0; for (int i = 0; i < kSizeCount; i++) if (kSizes[i] < shown - 0.01f) idx = i; wed.setSize(kSizes[idx]); }
    if (ib("plus", "Larger  (Ctrl+Shift+>)")) { int idx = kSizeCount - 1; for (int i = kSizeCount - 1; i >= 0; i--) if (kSizes[i] > shown + 0.01f) idx = i; wed.setSize(kSizes[idx]); }
  }
  // ---- row 2
  bx = r.x + pad;
  by += rowH + pad;
  if (ib("bold", "Bold  (Ctrl+B)", fmtHere & F_BOLD)) wed.toggleFmt(F_BOLD);
  if (ib("italic", "Italic  (Ctrl+I)", fmtHere & F_ITALIC)) wed.toggleFmt(F_ITALIC);
  if (ib("underline", "Underline  (Ctrl+U)", fmtHere & F_UNDER)) wed.toggleFmt(F_UNDER);
  if (ib("strike", "Strikethrough  (Ctrl+Shift+D)", fmtHere & F_STRIKE)) wed.toggleFmt(F_STRIKE);
  if (ib("mark", "Highlight  (Ctrl+M)", fmtHere & F_MARK)) wed.toggleFmt(F_MARK);
  writerColorButton({bx, by, 30 * s, bh}, "wmcolor", true);
  bx += 30 * s + gap;
  sepLine();
  if (ib("listbullet", "Bulleted list  (Ctrl+Shift+8)", styleHere == PStyle::Bullet)) { wed.setStyle(styleHere == PStyle::Bullet ? PStyle::Body : PStyle::Bullet); writerFollow_ = true; }
  if (ib("listnum", "Numbered list  (Ctrl+Shift+7)", styleHere == PStyle::Number)) { wed.setStyle(styleHere == PStyle::Number ? PStyle::Body : PStyle::Number); writerFollow_ = true; }
  sepLine();
  if (ib("quote", "Insert citation\xE2\x80\xA6  (Ctrl+Q)", false, hasCorpus())) { writerCiteSel.assign(hasCorpus() ? P->corpus.recs.size() : 0, 0); writerCiteFocus_ = true; ui.openPopup("wcite"); }
  if (ib("link", "Link\xE2\x80\xA6  (Ctrl+K)", !wed.linkHere().empty())) { writerLinkBuf = wed.linkHere(); writerLinkFocus_ = true; ui.openPopup("wlink"); }
}

// the zoom at which the page just fits the width of the page area (0 = not laid out yet)
float App::writerFitZoom() const {
  if (writerPageAreaW_ <= 0) return 0;
  float pagePx100 = float(wdoc.setup.pageW()) * ui.s * 96.f / 72.f;  // page width at 100 %
  return pagePx100 > 0 ? clampv((writerPageAreaW_ - 32 * ui.s) / pagePx100, 0.4f, 3.f) : 0;
}

void App::openWriter() {
  if (workspace != WS_BIBLIOGRAPHY) setWorkspace(WS_BIBLIOGRAPHY);
  if (!writerOpen && lastBibliographyRoute != BR_WRITE) {
    bibliographyWriterReturnRoute = readerOpen ? BR_REVIEW : lastBibliographyRoute;
    bibliographyWriterReturnToPapers = readerOpen ? bibliographyReaderReturnToPapers : papersOpen;
  }
  if (!writerOpen) { writerFitOnce_ = true; writerMiniShow_ = false; }  // a page wider than the window opens fitted, then the zoom is the user's
  if (readerOpen) closeReader();  // the writer takes the main area back from the reader
  writerOpen = true;
  ui.focus = 0;  // typing goes to the page, not to a text field that had the caret
  showStart = false;
  papersOpen = false;
  mainChartOpen = false;
  figZoomOpen = false;
  page = PG_WRITER;
  lastBibliographyRoute = BR_WRITE;
  settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  if (wdoc.blocks.empty()) { wdoc.blocks.push_back(Block::para("", PStyle::Body)); wed.attach(&wdoc); }
  writerFollow_ = true;
}

void App::closeWriter() {
  if (writerPdfPreviewOpen_) writerSetPdfPreview(false);
  writerOpen = false;
  ui.focus = 0;
  if (workspace == WS_BIBLIOGRAPHY) {
    lastBibliographyRoute = bibliographyWriterReturnRoute;
    if (lastBibliographyRoute == BR_WRITE) lastBibliographyRoute = BR_PAPERS;
    page = lastBibliographyRoute == BR_REVIEW ? PG_READ : PG_NONE;
    papersOpen = lastBibliographyRoute == BR_PAPERS || bibliographyWriterReturnToPapers;
    mainChartOpen = false;
    settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  }
}

void App::writerDropImages() {
  if (wlay && !wlay->images.empty()) wlay->images.clear();
}

void App::writerReset() {
  if (writerPdfPreviewOpen_) writerSetPdfPreview(false);
  writerPreviewClear();
  wdoc = Document();
  wed.attach(&wdoc);
  wlay.reset();
  writerScroll = 0;
  writerSavedVer_ = writerSeenVer_ = wed.version;
  writerRefreshedVer_ = ~0ull;
  writerOpen = false;
}

void App::writerNewDocument() {
  writerPreviewClear();
  wed.begin("New document");
  wdoc.blocks.clear();
  wdoc.assets.clear();
  wdoc.refs.clear();
  wdoc.blocks.push_back(Block::para("", PStyle::Body));
  wed.setCaret({0, -1, 0});
  wed.externalChange();
  writerScroll = 0;
  writerFollow_ = true;
  ui.toast("New document", "The previous document is one Undo (Ctrl+Z) away.", 1, 4);
}

void App::writerFromProject() {
  if (writerPdfPreviewOpen_) writerSetPdfPreview(false);
  writerPreviewClear();
  wdoc = Document();
  int migratedLinks = 0;
  if (P->document.t == Json::Obj) {
    docFromJson(wdoc, P->document);
    migratedLinks = migrateDocRecordLinks(wdoc, P->corpus);
  }
  P->document = Json();  // the App owns it from here; written back on save
  if (migratedLinks) { writerToProject(); P->dirty = true; }
  wed.attach(&wdoc);
  wlay.reset();
  writerScroll = 0;
  writerSavedVer_ = writerSeenVer_ = wed.version;
  writerRefreshedVer_ = ~0ull;
}

void App::writerToProject() {
  P->document = wdoc.empty() ? Json() : docToJson(wdoc);
  writerSavedVer_ = wed.version;
}

string App::writerSummary() const {
  if (wdoc.empty()) return "";
  int secs = 0;
  for (auto& s : docSections(wdoc)) secs += s.heading >= 0;
  string t = wdoc.title();
  string s = (t.empty() ? string("Untitled document") : "\"" + truncate(t, 60) + "\"") + ": " + plural(secs, "section") + ", " + plural(wdoc.figures(), "figure") +
             ", " + plural(wdoc.tables(), "table") + ", " + plural(wdoc.words(), "word");
  int refs = 0;
  for (auto& b : wdoc.blocks) refs += b.kind == Block::Paragraph && b.p.style == PStyle::Reference;
  if (refs) s += ", " + plural(refs, "reference");
  return s;
}

string App::writerSlug() const {
  string t = wdoc.title();
  if (t.empty()) t = agentReportTitle.empty() ? "document" : agentReportTitle;
  string slug;
  for (char ch : lower(t)) {
    if (isalnum(uint8_t(ch))) slug += ch;
    else if (!slug.empty() && slug.back() != '-') slug += '-';
    if (slug.size() >= 48) break;
  }
  while (!slug.empty() && slug.back() == '-') slug.pop_back();
  return slug.empty() ? string("document") : slug;
}

ScenePng App::scenePng() {
  return [this](const Scene& sc, double dpi) {
    vector<uint8_t> px;
    int w = 0, h = 0;
    if (!renderSceneBGRA(g, sc, dpi, px, w, h, false) || w <= 0 || h <= 0) return string();
    vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
      uint8_t a = px[i * 4 + 3];
      auto up = [&](uint8_t v) { return a ? uint8_t(std::min(255, v * 255 / a)) : uint8_t(255); };
      rgba[i * 4] = up(px[i * 4 + 2]); rgba[i * 4 + 1] = up(px[i * 4 + 1]); rgba[i * 4 + 2] = up(px[i * 4]); rgba[i * 4 + 3] = 255;
    }
    return pngEncode(w, h, rgba.data(), false, dpi);
  };
}

string App::writerExport(const string& fmt0, const string& path0) {
  string fmt = lower(trim(fmt0));
  if (fmt == "word") fmt = "docx";
  if (fmt == "tex") fmt = "latex";
  if (fmt != "pdf" && fmt != "docx" && fmt != "html" && fmt != "latex") {
    ui.toast("Export", "Unknown format \"" + fmt0 + "\" (pdf, docx, html or latex).", 2);
    return "";
  }
  if (wdoc.empty()) { ui.toast("Export", "The document is empty.", 2); return ""; }
  string path = path0;
  if (path.empty()) {
    string title = fmt == "pdf" ? "Save as PDF" : fmt == "docx" ? "Save as Word document" : fmt == "html" ? "Save as web page" : "Export LaTeX source bundle";
    vector<FileFilter> ff = fmt == "pdf" ? vector<FileFilter>{{"PDF", "*.pdf"}} :
                            fmt == "docx" ? vector<FileFilter>{{"Word document", "*.docx"}} :
                            fmt == "html" ? vector<FileFilter>{{"Web page", "*.html"}} : vector<FileFilter>{{"LaTeX source", "*.tex"}};
    string ext = fmt == "latex" ? "tex" : fmt;
    path = saveFileDialog(hwnd, title, ff, writerSlug() + "." + ext, ext);
    if (path.empty()) return "";
  }
  wed.refreshCitations();
  if (fmt == "latex") {
    size_t slash = path.find_last_of("\\/");
    string dir = slash == string::npos ? "." : path.substr(0, slash);
    string leaf = slash == string::npos ? path : path.substr(slash + 1);
    size_t dot = leaf.find_last_of('.');
    string stem = dot == string::npos ? leaf : leaf.substr(0, dot);
    if (stem.empty()) stem = "document";
    const string assetDirName = writerSlug() + "-" + sha256Hex(lower(path)).substr(0, 8) + "-figures";
    LatexBundle bundle = docToLatex(wdoc, assetDirName);
    if (bundle.source.empty()) { ui.toast("LaTeX export failed", "The document source could not be generated.", 3); return ""; }
    bool ok = true;
    if (!bundle.assets.empty()) {
      ok = ensureDir(dir + "\\" + assetDirName);
      for (const auto& asset : bundle.assets) {
        string relative = replaceAll(asset.first, "/", "\\");
        if (relative.find("..") != string::npos || startsWith(relative, "\\")) { ok = false; break; }
        if (!writeFileU(dir + "\\" + relative, asset.second)) { ok = false; break; }
      }
    }
    if (ok && !bundle.bibliography.empty()) ok = writeFileU(dir + "\\" + stem + ".bib", bundle.bibliography);
    if (ok) ok = writeFileU(path, bundle.source);
    if (!ok) { ui.toast("LaTeX export failed", "Could not write the .tex file or one of its companion assets.", 3); return ""; }
    string detail = fileName(path) + " and " + plural(int(bundle.assets.size()), "figure asset");
    if (!bundle.bibliography.empty()) detail += "; companion BibTeX written";
    if (!bundle.warnings.empty()) detail += "; " + plural(int(bundle.warnings.size()), "warning") + ": " + bundle.warnings.front();
    ui.toast(bundle.warnings.empty() ? "LaTeX source bundle exported" : "LaTeX source exported with warnings", detail, bundle.warnings.empty() ? 1 : 2, 7);
    return path;
  }
  string bytes;
  if (fmt == "pdf") bytes = docToPDF(wdoc);
  else if (fmt == "docx") bytes = docToDOCX(wdoc, scenePng(), 200);
  else bytes = docToHTML(wdoc, scenePng());
  if (bytes.empty() || !writeFileU(path, bytes)) { ui.toast("Export failed", "Could not write " + fileName(path), 3); return ""; }
  if (fmt == "docx") agentReportDocx = path;
  else agentReportPath = path;
  if (agentReportPath.empty()) agentReportPath = path;
  ui.toast("Saved", fileName(path) + " \xC2\xB7 " + plural(wdoc.words(), "word") + ", " + plural(wdoc.figures(), "figure") + ", " + plural(writerPages_, "page"), 1, 4);
  return path;
}

void App::writerEnsureTitle(const string& title0, const string& subtitle) {
  string title = trim(title0);
  bool has = false;
  for (auto& b : wdoc.blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Title && !b.p.empty()) { has = true; break; }
  if (has) return;
  if (title.empty()) title = "Literature report";
  vector<Block> front;
  front.push_back(Block::para(title, PStyle::Title));
  if (!trim(subtitle).empty()) front.push_back(Block::para(trim(subtitle), PStyle::Subtitle));
  if (hasCorpus()) {
    const Growth& gr = statGrowth();
    vector<string> src;
    for (auto& f : P->corpus.files) src.push_back(f.name);
    string m = "Data: " + plural(long(P->corpus.recs.size()), "record");
    if (gr.y0 && gr.y1) m += ", " + std::to_string(gr.y0) + "\xE2\x80\x93" + std::to_string(gr.y1);
    if (!src.empty()) m += " (" + truncate(join(src, "; "), 160) + ")";
    front.push_back(Block::para(m, PStyle::Meta));
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  static const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  front.push_back(Block::para("Prepared with VOSStudio on " + std::to_string(st.wDay) + " " + months[clampv(int(st.wMonth) - 1, 0, 11)] + " " + std::to_string(st.wYear) +
                              ". Text written with an AI model from the papers listed; check it before use.", PStyle::Meta));
  // drop a leading empty paragraph (a fresh document)
  if (!wdoc.blocks.empty() && wdoc.blocks[0].kind == Block::Paragraph && wdoc.blocks[0].p.empty() && wdoc.blocks.size() == 1) wdoc.blocks.clear();
  wdoc.blocks.insert(wdoc.blocks.begin(), front.begin(), front.end());
  wed.externalChange();
}

// ------------------------------------------------------------------ insertions
void App::writerInsertChart(const string& id) {
  ReportBlock b;
  Json g = Json::object();
  g.set("chart", id);
  string err = agentChart(id, g, b);
  if (!err.empty()) { ui.toast("Chart", err, 2, 5); return; }
  int a = wdoc.addAsset(b.fig, id, b.title, b.figId);
  wed.begin("Insert figure");
  wed.insertFigure(a, b.text.empty() ? b.title : b.text, 100);
  writerFollow_ = true;
  writerCaretX_ = -1;
}

void App::writerInsertMap() {
  if (!hasMap()) { ui.toast("Map figure", "Build a map first.", 2); return; }
  string id = view == ViewKind::Overlay ? "map_overlay" : view == ViewKind::Density ? "map_density" : view == ViewKind::Timeline ? "map_timeline" : view == ViewKind::Geo ? "map_geo" : "map_network";
  writerInsertChart(id);
}

void App::writerCite(int rec) { writerCiteMany({rec}); }

void App::writerCiteMany(const vector<int>& recs) {
  if (!hasCorpus()) { ui.toast("Citation", "Load records first.", 2, 4); return; }
  vector<string> keys;
  string first;
  for (int rec : recs) {
    if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) continue;
    const Record& r = P->corpus.recs[size_t(rec)];
    RefEntry e = refFromRecord(r);
    e.rec = rec;
    if (first.empty()) first = r.title;
    wed.begin("Citation");
    docAddReference(wdoc, e);
    keys.push_back(e.key);
  }
  if (keys.empty()) { ui.toast("Citation", "Select a paper first.", 2, 4); return; }
  if (!wed.insertCitation(keys)) { ui.toast("Citation", "Put the caret in a paragraph first.", 2, 4); return; }
  ui.toast("Cited", keys.size() == 1 ? truncate(first, 70) : plural(long(keys.size()), "paper") + " in one citation", 1, 3);
  writerFollow_ = true;
}

// ------------------------------------------------------------------ clipboard
// The reference keys cited in the copied fragment, as Word sources (tags as in the CITATION fields of the HTML).
vector<WordSource> App::writerClipSources() const {
  vector<WordSource> out;
  std::set<string> cited = docFragmentCitedKeys(writerClip_);
  if (cited.empty() || wdoc.refs.empty()) return out;
  std::map<string, string> tags;
  for (auto& it : docBibSourceItems(wdoc, tags, &cited)) out.push_back({it.first, it.second});
  return out;
}

// Ctrl+C / Ctrl+X. The clipboard gets the plain text, Word's HTML dialect (styles, lists, tables, pictures, CITATION and
// SEQ fields) and, for a figure on its own, the picture itself (vector metafile + PNG). Word treats the pasted citations
// as its own only when their sources are in the document's source list, and the clipboard cannot carry that list — so
// when Word is open, the sources are handed to its active document right away (automation), and a later Ctrl+V there
// gives citations with sources; toWord (Ctrl+Shift+C) pastes at Word's cursor itself.
void App::writerCopy(bool cut, bool toWord) {
  if (!wed.hasSelection() && !wed.blockSelected()) return;
  writerClip_ = cut ? wed.cut() : wed.copy();
  if (writerClip_.empty()) return;
  const bool figureOnly = writerClip_.spans.empty() && writerClip_.blocks.size() == 1 && writerClip_.blocks[0].kind == Block::FigureBlock;
  writerClipText_ = writerClip_.blocks.empty() ? wed.selectionText() : string();
  if (writerClipText_.empty()) {
    Document tmp;
    tmp.blocks = writerClip_.blocks;
    if (tmp.blocks.empty()) { Block b; b.p.spans = writerClip_.spans; tmp.blocks.push_back(b); }
    writerClipText_ = trim(blocksToMarkdown(tmp));
  }
  if (cut) { writerClipText_ = writerClipText_.empty() ? " " : writerClipText_; }
  string cf, emf, png;
  int imgW = 0, imgH = 0;
  vector<uint8_t> bgra;
  writerClipCites_ = 0;
  vector<WordSource> sources;
  if (figureOnly) {
    // a picture, not a document fragment: vector (Enhanced Metafile) unless the figure is set to PNG, plus PNG / DIB
    const Figure& F = writerClip_.blocks[0].fig;
    if (F.asset >= 0 && size_t(F.asset) < wdoc.assets.size()) {
      const Scene& sc = wdoc.assets[size_t(F.asset)].scene;
      double wpt = wdoc.setup.textW() * F.widthPct / 100.0;
      if (F.formatOr(wdoc.setup.figureFormat) == "vector") emf = renderSceneEMF(sc, wpt, sc.W > 0 ? wpt * sc.H / sc.W : 0);
      if (ScenePng pf = scenePng()) png = pf(sc, 200);
      renderSceneBGRA(g, sc, 200, bgra, imgW, imgH);
      string cap = F.caption.text();
      writerClipText_ = cap.empty() ? wdoc.assets[size_t(F.asset)].title : cap;
    }
  } else {
    ClipOptions o;
    o.fileDir = clipboardFilesDir();
    o.fileUrlBase = fileUrl(o.fileDir);
    o.bibliography = false;  // Word builds the reference list from its sources (References > Bibliography)
    o.png = scenePng();
    o.emf = [](const Scene& sc, double w, double h) { return renderSceneEMF(sc, w, h); };
    ClipHtml c = docFragmentToWordHtml(wdoc, writerClip_, o);
    for (auto& f : c.files) writeFile(o.fileDir + "\\" + f.first, f.second);  // the same names every time: the folder never grows
    cf = cfHtmlWrap(c.html);
    writerClipCites_ = c.citations;
    if (c.citations) sources = writerClipSources();
  }
  setClipboardRich(hwnd, writerClipText_, cf, imgW, imgH, bgra.empty() ? nullptr : bgra.data(), emf.empty() ? nullptr : &emf, png.empty() ? nullptr : &png);
  if (toWord) {
    WordResult r = wordPasteAtCursor(sources, true);
    if (!r.reachable) ui.toast("Paste into Word", r.error.empty() ? "Word could not be reached." : r.error, 2, 5);
    else if (!r.error.empty()) ui.toast("Pasted into Word", r.error + (r.added ? " (" + std::to_string(r.added) + " sources added)" : string()), 2, 6);
    else {
      string what = figureOnly ? string("The figure is in ") : "The selection is in ";
      what += r.document.empty() ? string("Word") : "\"" + r.document + "\"";
      if (r.added || r.present) what += "; " + plural(long(r.added + r.present), "source") + " in its source list" + (r.added ? " (" + std::to_string(r.added) + " new)" : string());
      ui.toast("Pasted into Word", what + ".", 1, 4);
    }
  } else if (!sources.empty() && writerWordSources_ && wordRunning()) {
    WordResult r = wordAddSources(sources, false);
    if (r.reachable && r.error.empty())
      ui.toast("Copied", (r.added ? plural(long(r.added), "source") + " added to " : string("The sources are already in ")) + (r.document.empty() ? string("the Word document") : "\"" + r.document + "\"") +
                             " \xC2\xB7 Ctrl+V there pastes native citations.", 1, 4);
    else ui.toast("Copied", "Word's source list could not be updated: " + (r.error.empty() ? string("no document") : r.error) + ". Use Paste into Word (Ctrl+Shift+C).", 2, 5);
  } else if (!sources.empty() && writerWordSources_) {
    ui.toast("Copied", plural(long(sources.size()), "citation") + " \xC2\xB7 open the target document in Word and copy again, or press Ctrl+Shift+C to paste there directly, so Word gets the sources.", 1, 5);
  }
  if (cut) { writerFollow_ = true; writerCaretX_ = -1; }
}

void App::writerPaste(bool plain) {
  string clip = getClipboardText(hwnd);
  wed.begin("Paste");
  if (!plain && !writerClip_.empty() && clip == writerClipText_) { wed.paste(writerClip_); }
  else if (!plain && writerPasteHtml()) {}
  else if (!clip.empty()) {
    string t = replaceAll(clip, "\r\n", "\n");
    bool md = !plain && (t.find("\n\n") != string::npos || t.find("\n#") != string::npos || startsWith(t, "#") || t.find("\n- ") != string::npos ||
                         startsWith(t, "- ") || t.find("**") != string::npos || t.find("\n|") != string::npos || startsWith(t, "|"));
    if (t.find('\n') == string::npos) wed.insertText(t);
    else if (md) wed.pasteText(t, true);
    else {  // plain lines: one paragraph per line
      string lines;
      for (auto& l : split(t, '\n')) lines += l + "\n\n";
      wed.pasteText(lines, false);
    }
  }
  writerFollow_ = true;
  writerCaretX_ = -1;
}

// One click from the document to Word: the .docx (native styles, citations, bibliography, figures) is written to the
// Reports folder and opened with whatever handles .docx — the most faithful way to continue in Word.
void App::writerOpenInWord() {
  if (wdoc.empty()) { ui.toast("Open in Word", "The document is empty.", 2); return; }
  string path = writerExport("docx", reportsDir() + "\\" + writerSlug() + ".docx");
  if (path.empty()) return;
  openUrl(path);
  ui.toast("Opened in Word", "Saved as " + path + " and handed to Word. Citations and the bibliography are Word's own there.", 1, 5);
}

// HTML from Word, a browser or another editor: headings, lists, tables and inline formats come in as such
bool App::writerPasteHtml() {
  string html = getClipboardHtml(hwnd);
  if (html.empty()) return false;
  DocFragment f;
  if (!docFragmentFromHtml(html, f) || f.empty()) return false;
  wed.paste(f);
  return true;
}

// ------------------------------------------------------------------ keyboard
void App::writerTyped() {
  if (ui.in.chars.empty()) return;
  string s;
  for (char32_t cp : ui.in.chars) {
    if (cp < 32 || cp == 127) continue;
    if (ui.in.ctrl && !ui.in.alt) continue;  // shortcuts, not text (AltGr sends ctrl + alt)
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
  }
  if (s.empty()) return;
  wed.insertText(s);
  writerFollow_ = true;
  writerCaretX_ = -1;
}

bool App::writerKey(int k) {
  const Input& in = ui.in;
  const bool ctrl = in.ctrl, shift = in.shift, alt = in.alt;
  if (k != VK_SHIFT && k != VK_CONTROL && k != VK_MENU && k != VK_CAPITAL) writerMiniShow_ = false;  // typing or moving: the mini toolbar goes
  auto moved = [&]() { writerFollow_ = true; writerCaretX_ = -1; };
  auto zoomTo = [&](float z) { writerZoom = clampv(z, 0.4f, 3.f); writerZoomT_ = ui.time; writerZoomAnchorY_ = -1; writerZoomAnchorX_ = -1; };
  auto sizeStep = [&](int dir) {
    float cur = wed.sizeHere();
    if (cur <= 0) cur = wdoc.setup.baseSize;
    int idx = 0;
    for (int i = 0; i < kSizeCount; i++) if (kSizes[i] <= cur + 0.01f) idx = i;
    if (dir > 0 && kSizes[idx] <= cur + 0.01f) idx = std::min(kSizeCount - 1, idx + 1);
    if (dir < 0) idx = std::max(0, kSizes[idx] >= cur - 0.01f ? idx - 1 : idx);
    wed.setSize(kSizes[idx]);
  };
  if (ctrl && !alt) {
    switch (k) {
      case 'A': wed.selectAll(); return true;
      case 'C': writerCopy(false, shift); return true;  // Ctrl+Shift+C: paste into Word at its cursor
      case 'X': writerCopy(true); return true;
      case 'V': writerPaste(shift); return true;
      case 'Z': if (shift) wed.redo(); else wed.undo(); moved(); return true;
      case 'Y': wed.redo(); moved(); return true;
      case 'B': wed.toggleFmt(F_BOLD); return true;
      case 'I': wed.toggleFmt(F_ITALIC); return true;
      case 'U': wed.toggleFmt(F_UNDER); return true;
      case 'D': if (shift) { wed.toggleFmt(F_STRIKE); return true; } return false;
      case 'M': wed.toggleFmt(F_MARK); return true;
      case 'K': writerLinkBuf = wed.linkHere(); writerLinkFocus_ = true; ui.openPopup("wlink"); return true;
      case 'E': if (!shift) { wed.setAlign(PAlign::Center); return true; } return false;
      case 'L': if (!shift) { wed.setAlign(PAlign::Left); return true; } return false;
      case 'R': wed.setAlign(PAlign::Right); return true;
      case 'J': if (shift) { wed.setAlign(PAlign::Justify); return true; } return false;
      case 'F': writerPane = 3; ui.focusText(ui.id("ti:wfind"), writerFind); return true;
      case 'H': writerPane = 3; ui.focusText(ui.id("ti:wrepl"), writerReplace); return true;
      case 'G': writerPane = 3; if (!writerFind.empty()) { wed.find(writerFind, !shift, writerMatchCase); moved(); } return true;
      case 'W': if (shift) { closeWriter(); return true; } return false;
      case VK_SPACE: wed.clearFormatting(); return true;
      case VK_RETURN: wed.insertPageBreak(); moved(); return true;
      case VK_HOME: wed.docStart(shift); moved(); return true;
      case VK_END: wed.docEnd(shift); moved(); return true;
      case VK_LEFT: wed.moveWord(-1, shift); moved(); return true;
      case VK_RIGHT: wed.moveWord(1, shift); moved(); return true;
      case VK_UP: wed.moveBlock(-1, shift); moved(); return true;
      case VK_DOWN: wed.moveBlock(1, shift); moved(); return true;
      case VK_BACK: if (!wed.hasSelection()) wed.moveWord(-1, true); wed.deleteSelection(); moved(); return true;
      case VK_DELETE: if (!wed.hasSelection()) wed.moveWord(1, true); wed.deleteSelection(); moved(); return true;
      case VK_OEM_PLUS: if (shift) wed.toggleFmt(F_SUP); else zoomTo(writerZoom * 1.1f); return true;  // Ctrl+= zoom, Ctrl+Shift+= superscript
      case VK_ADD: zoomTo(writerZoom * 1.1f); return true;
      case VK_OEM_MINUS: if (shift) wed.toggleFmt(F_SUB); else zoomTo(writerZoom / 1.1f); return true;  // Ctrl+Shift+- subscript
      case VK_SUBTRACT: zoomTo(writerZoom / 1.1f); return true;
      case '0': case VK_NUMPAD0: zoomTo(1); return true;
      case '7': if (shift) { wed.toggleList(true); return true; } return false;
      case '8': if (shift) { wed.toggleList(false); return true; } return false;
      case VK_OEM_PERIOD: if (shift) { sizeStep(1); return true; } return false;   // Ctrl+Shift+>  larger
      case VK_OEM_COMMA: if (shift) { sizeStep(-1); return true; } return false;   // Ctrl+Shift+<  smaller
      case VK_OEM_3: wed.toggleFmt(F_CODE); return true;  // Ctrl+`
      case VK_OEM_4: wed.indent(-1); return true;          // Ctrl+[
      case VK_OEM_6: wed.indent(1); return true;           // Ctrl+]
      case 'T': ui.openPopup("wtbl"); return true;
      case 'P': if (shift) { writerExport("pdf"); return true; } return false;
      case 'Q': writerCiteSel.assign(hasCorpus() ? P->corpus.recs.size() : 0, 0); writerCiteFocus_ = true; ui.openPopup("wcite"); return true;
      case 'N': if (shift) { writerNewConfirm_ = true; ui.openPopup("wnew"); return true; } return false;
    }
    return false;
  }
  if (ctrl && alt) {  // heading levels: Ctrl+Alt+0..4; Ctrl+Alt+M edits/inserts a math span
    if (k == 'M') { writerEquationBuf_ = wed.mathAtCaret(); writerEquationError_.clear(); writerEquationFocus_ = true; ui.openPopup("wequation"); return true; }
    if (k == '0' || k == VK_NUMPAD0) { wed.setStyle(PStyle::Body); return true; }
    if (k >= '1' && k <= '4') { wed.setStyle(headingStyle(k - '0')); return true; }
    return false;
  }
  if (alt && shift && (k == VK_UP || k == VK_DOWN)) { wed.moveBlockUpDown(k == VK_UP ? -1 : 1); moved(); return true; }
  if (alt) return false;
  switch (k) {
    case VK_LEFT: wed.moveChar(-1, shift); moved(); return true;
    case VK_RIGHT: wed.moveChar(1, shift); moved(); return true;
    case VK_UP: writerMoveLine(-1, shift); return true;
    case VK_DOWN: writerMoveLine(1, shift); return true;
    case VK_HOME: writerLineEdge(false, shift); return true;
    case VK_END: writerLineEdge(true, shift); return true;
    case VK_PRIOR: case VK_NEXT: {
      writerRelayout(writerK_);
      float cx = 0, cy = 0, ch = 0;
      float step = std::max(60.f, writerViewH_ * 0.85f / std::max(0.05f, writerF_));
      if (writerCaretRect(wed.caret, cx, cy, ch)) {
        float ty = cy + (k == VK_NEXT ? step : -step);
        DocPos q = writerHit(writerCaretX_ >= 0 ? writerCaretX_ : cx, ty);
        wed.setCaret(q, shift);
        writerScroll += (k == VK_NEXT ? step : -step) * writerF_;
      }
      writerFollow_ = true;
      return true;
    }
    case VK_RETURN:
      if (shift) wed.insertLineBreak();
      else wed.insertParagraphBreak();
      moved();
      return true;
    case VK_BACK: wed.backspace(); moved(); return true;
    case VK_DELETE: if (shift) writerCopy(true); else wed.del(); moved(); return true;
    case VK_INSERT: if (shift) writerPaste(false); else if (ctrl) writerCopy(false); return true;
    case VK_TAB: {
      int row = 0, col = 0;
      if (wed.inTable(&row, &col)) wed.tableNextCell(shift ? -1 : 1);
      else if (isList(wed.styleHere())) wed.indent(shift ? -1 : 1);
      else if (!shift) wed.insertText("    ");
      moved();
      return true;
    }
    case VK_F3: if (!writerFind.empty()) { wed.find(writerFind, !shift, writerMatchCase); moved(); } else writerPane = 3; return true;
    case VK_F2: {  // rename: select the heading of the current section
      vector<DocSection> secs = docSections(wdoc);
      for (auto& s : secs) if (s.heading >= 0 && wed.caret.blk >= s.first && wed.caret.blk <= s.last) { wed.setCaret({s.heading, -1, 0}); wed.selectParagraph(); moved(); break; }
      return true;
    }
    case VK_APPS: {
      float cx = 0, cy = 0, ch = 0;
      if (writerCaretRect(wed.caret, cx, cy, ch)) {
        writerCtxX_ = writerPageR_.x + (wlay ? wlay->ML : 0) * writerF_ + cx * writerF_;
        writerCtxY_ = writerPageR_.y + (cy + ch) * writerF_;
        writerCtxSub_.clear();
        ui.openPopup("wctx");
      }
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------ the Write page (left panel)
void App::pageWriter(Lay& L) {
  const float s = ui.s;
  string title = wdoc.title();
  {
    Rect r = L.row(40 * s);
    ui.text({r.x, r.y, r.w, 22 * s}, title.empty() ? "Untitled document" : truncate(title, 48), 14 * s, ui.c.text, AL_LEFT, 600);
    string sub = wdoc.empty() ? "Nothing written yet" : plural(wdoc.words(), "word") + " \xC2\xB7 " + plural(writerPages_, "page") + " \xC2\xB7 " + plural(wdoc.figures(), "figure") + " \xC2\xB7 " +
                                                        plural(long(wdoc.refs.size()), "reference") + (writerDirty() ? " \xC2\xB7 edited" : "");
    ui.text({r.x, r.y + 22 * s, r.w, 16 * s}, sub, 11.5f * s, ui.c.textDim);
  }
  {
    Rect r = L.row(32 * s);
    float bw = (r.w - 6 * s) / 2;
    if (!writerOpen) { if (ui.button({r.x, r.y, bw, r.h}, "Open the writer", BTN_PRIMARY, "writer")) openWriter(); }
    else if (ui.button({r.x, r.y, bw, r.h}, workspace == WS_BIBLIOGRAPHY ? "Back to Bibliography" : hasMap() ? "Back to the map" : "Close the writer", BTN_NORMAL, workspace == WS_BIBLIOGRAPHY ? "book" : "map")) closeWriter();
    if (ui.button({r.x + bw + 6 * s, r.y, bw, r.h}, "New document", BTN_NORMAL, "newdoc")) { writerNewConfirm_ = true; ui.openPopup("wnew"); }
  }
  sectionTitle(L, "Insert", "Everything is inserted at the caret. Charts and maps are vector figures of the loaded data; citations become real Word citations in the .docx export.", "");
  {
    Rect r = L.row(30 * s);
    auto c3 = cols(r, 3, 6 * s);
    if (ui.button(c3[0], "Chart\xE2\x80\xA6", BTN_NORMAL, "image", hasCorpus())) { openWriter(); ui.openPopup("wchart"); }
    if (ui.button(c3[1], "Map", BTN_NORMAL, "map", hasMap())) { openWriter(); writerInsertMap(); }
    if (ui.button(c3[2], "Cite\xE2\x80\xA6", BTN_NORMAL, "quote", hasCorpus())) { openWriter(); writerCiteSel.assign(P->corpus.recs.size(), 0); writerCiteFocus_ = true; ui.openPopup("wcite"); }
    r = L.row(30 * s);
    c3 = cols(r, 3, 6 * s);
    if (ui.button(c3[0], "Table\xE2\x80\xA6", BTN_NORMAL, "table")) { openWriter(); ui.openPopup("wtbl"); }
    if (ui.button(c3[1], "Page break", BTN_NORMAL, "pagebreak")) { openWriter(); wed.insertPageBreak(); writerFollow_ = true; }
    if (ui.button(c3[2], "Contents", BTN_NORMAL, "toc")) { openWriter(); wed.insertToc(); writerFollow_ = true; }
  }
  sectionTitle(L, "Export", "PDF: paginated exactly like the pages here. Word: styles, a live table of contents, native citations and a bibliography you can restyle inside Word. HTML: one self-contained file.", "");
  {
    Rect r = L.row(30 * s);
    auto c3 = cols(r, 3, 6 * s);
    bool en = !wdoc.empty();
    if (ui.button(c3[0], "PDF", BTN_NORMAL, "download", en)) writerExport("pdf");
    if (ui.button(c3[1], "Word", BTN_NORMAL, "download", en)) writerExport("docx");
    if (ui.button(c3[2], "HTML", BTN_NORMAL, "download", en)) writerExport("html");
    r = L.row(30 * s);
    if (ui.button(r, "All three into Documents\\VOSStudio\\Reports", BTN_GHOST, "folder", en)) {
      string base = reportsDir() + "\\" + writerSlug();
      string p1 = writerExport("pdf", base + ".pdf"), p2 = writerExport("docx", base + ".docx"), p3 = writerExport("html", base + ".html");
      if (!p1.empty() || !p2.empty() || !p3.empty()) revealInExplorer(!p1.empty() ? p1 : !p2.empty() ? p2 : p3);
    }
  }
  sectionTitle(L, "Citation style", "The in-text citations and the reference list are generated from the bibliography and follow this style everywhere (page, PDF, HTML). In Word they are native citations, so References > Style there changes them too.", "");
  {
    vector<string> names;
    int sel = 0;
    const auto& styles = citeStyles();
    for (size_t i = 0; i < styles.size(); i++) { names.push_back(styles[i].name); if (wdoc.citeStyle == styles[i].id) sel = int(i); }
    Rect r = L.row(30 * s);
    if (ui.combo(r, "wcitestyle_left", names, sel)) { wed.setCiteStyle(styles[size_t(sel)].id); writerFollow_ = true; }
  }
  sectionTitle(L, "Outline", "Click a heading, figure or table to jump to it. Use the style box (Heading 1, 2, 3) to structure the document.", "");
  {
    int fig = 0, tbl = 0, rows = 0;
    const float rh = 26 * s;
    for (size_t i = 0; i < wdoc.blocks.size(); i++) {
      const Block& b = wdoc.blocks[i];
      string label, icon;
      float ind = 0;
      if (b.kind == Block::Paragraph && (isHeading(b.p.style) || b.p.style == PStyle::Title)) {
        label = b.p.text();
        if (label.empty()) label = "(empty heading)";
        ind = b.p.style == PStyle::Title ? 0 : float(headingLevel(b.p.style) - 1) * 12 * s;
        icon = b.p.style == PStyle::Title ? "type" : "";
      } else if (b.kind == Block::FigureBlock) { label = b.fig.labelText() + " " + std::to_string(++fig) + ": " + (b.fig.caption.empty() ? (b.fig.asset >= 0 && size_t(b.fig.asset) < wdoc.assets.size() ? wdoc.assets[size_t(b.fig.asset)].title : string("figure")) : b.fig.caption.text()); icon = "image"; ind = 12 * s; }
      else if (b.kind == Block::TableBlock) { label = "Table " + std::to_string(++tbl) + " (" + std::to_string(b.tbl.rows()) + "\xC3\x97" + std::to_string(b.tbl.cols()) + ")"; icon = "table"; ind = 12 * s; }
      else continue;
      Rect rr = L.row(rh);
      L.y -= L.gap;
      bool current = wed.caret.blk == int(i);
      if (ui.listRow(rr, "wlout:" + std::to_string(i), current)) {
        openWriter();
        DocPos q{int(i), b.kind == Block::FigureBlock ? -2 : b.kind == Block::TableBlock ? 0 : -1, 0};
        wed.setCaret(q);
        writerFollow_ = true;
        writerCaretX_ = -1;
      }
      float tx = rr.x + 6 * s + ind;
      if (!icon.empty()) { ui.icon(icon, tx + 7 * s, rr.y + rh / 2, 12 * s, ui.c.textDim, 1.5f); tx += 18 * s; }
      bool bold = b.kind == Block::Paragraph && (b.p.style == PStyle::H1 || b.p.style == PStyle::Title);
      ui.text({tx, rr.y, rr.r() - tx - 4 * s, rh}, truncate(label, 60), 12.f * s, current ? ui.c.text : ui.c.textDim, AL_LEFT, bold ? 650 : 400);
      rows++;
    }
    if (rows == 0) ui.textWrap(L.row(40 * s), "No headings yet.", 12 * s, ui.c.textDim);
    L.space(6 * s);
  }
  sectionTitle(L, "Assistant", "The AI assistant writes into this same document: ask it to draft a report, add a section or a chart, and edit the result here.", "");
  {
    Rect r = L.row(30 * s);
    if (ui.button(r, "Ask the assistant to write the report\xE2\x80\xA6", BTN_NORMAL, "sparkle")) { page = PG_AI; }
  }
}

// ------------------------------------------------------------------ drawing
void App::drawWriter(const Rect& r) {
  const float s = ui.s;
  ui.fill(r, ui.c.bg);
  if (wdoc.blocks.empty()) { wdoc.blocks.push_back(Block::para("", PStyle::Body)); wed.attach(&wdoc); }
  if (wed.version != writerRefreshedVer_) { wed.refreshCitations(); writerRefreshedVer_ = wed.version; }  // citations and the reference list follow every edit
  if (wed.version != writerSeenVer_) { writerSeenVer_ = wed.version; P->dirty = true; }
  const PageSetup& ps = wdoc.setup;
  const bool editable = !ui.editing() && !ui.anyModal();
  uint16_t fmtHere = wed.fmtHere();
  PStyle styleHere = wed.styleHere();
  PAlign alignHere = wed.alignHere();
  const bool inText = docPara(wdoc, wed.caret) != nullptr;
  int trow = 0, tcol = 0;
  const bool inTbl = wed.inTable(&trow, &tcol);
  const bool onFig = wed.blockSelected() && wed.curBlock().kind == Block::FigureBlock;
  auto zoomTo = [&](float z) { writerZoom = clampv(z, 0.4f, 3.f); writerZoomT_ = ui.time; writerZoomAnchorY_ = -1; writerZoomAnchorX_ = -1; };
  auto openCite = [this]() { writerCiteSel.assign(hasCorpus() ? P->corpus.recs.size() : 0, 0); writerCiteFocus_ = true; ui.openPopup("wcite"); };

  // ---- toolbar: groups flowed into rows (the right-hand block — panes, zoom, export, close — sits on the last row)
  const float rowH = 38 * s, bh = 28 * s, by0 = 5 * s;
  struct Group { float w; std::function<void(Rect)> draw; };
  vector<Group> groups;
  auto ibtn = [&](float& x, float y, const string& icon, const string& tip, bool on = false, bool enabled = true) {
    Rect b{x, y + by0, 30 * s, bh};
    x += 31 * s;
    return ui.iconButton(b, icon, tip, on, enabled);
  };
  auto sepAt = [&](float x, float y) { ui.line(x + 4 * s, y + 10 * s, x + 4 * s, y + rowH - 10 * s, ui.c.border, 1); };
  // undo / redo
  groups.push_back({62 * s, [&](Rect gr) { float x = gr.x; if (ibtn(x, gr.y, "undo", "Undo  (Ctrl+Z)", false, wed.canUndo())) { wed.undo(); writerFollow_ = true; } if (ibtn(x, gr.y, "redo", "Redo  (Ctrl+Y)", false, wed.canRedo())) { wed.redo(); writerFollow_ = true; } }});
  // paragraph style
  groups.push_back({140 * s, [&](Rect gr) {
    vector<string> names;
    for (int i = 0; i < kStyleCount; i++) names.push_back(pstyleName(kStyleOrder[i]));
    int sel = styleIndex(styleHere);
    if (ui.combo({gr.x, gr.y + by0, 134 * s, bh}, "wstyle", names, sel)) { wed.setStyle(kStyleOrder[sel]); writerFollow_ = true; }
  }});
  // font family
  groups.push_back({150 * s, [&](Rect gr) {
    const vector<string>& fonts = writerFonts();
    vector<string> opts;
    opts.push_back("Document font (" + ps.font + ")");
    for (auto& f : fonts) opts.push_back(f);
    string here = wed.fontHere();
    int sel = 0;
    for (size_t i = 0; i < fonts.size(); i++) if (fonts[i] == here) sel = int(i) + 1;
    if (ui.combo({gr.x, gr.y + by0, 144 * s, bh}, "wfontfam", opts, sel)) wed.setFont(sel == 0 ? string() : fonts[size_t(sel - 1)]);
  }});
  // font size
  groups.push_back({112 * s, [&](Rect gr) {
    float x = gr.x;
    float cur = wed.sizeHere();
    float shown = cur > 0 ? cur : (styleHere == PStyle::Body ? ps.baseSize : specOf(styleHere, ps.baseSize, 0).size);
    if (ui.iconButton({x, gr.y + by0, 24 * s, bh}, "minus", "Smaller  (Ctrl+Shift+<)", false, inText)) {
      int idx = 0;
      for (int i = 0; i < kSizeCount; i++) if (kSizes[i] < shown - 0.01f) idx = i;
      wed.setSize(kSizes[idx]);
    }
    x += 25 * s;
    vector<string> opts;
    opts.push_back("Style size");
    int sel = 0;
    for (int i = 0; i < kSizeCount; i++) { opts.push_back(sizeLabel(kSizes[i])); if (cur > 0 && std::fabs(kSizes[i] - cur) < 0.05f) sel = i + 1; }
    if (sel == 0 && cur > 0) { opts.push_back(sizeLabel(cur)); sel = int(opts.size()) - 1; }
    string label = sizeLabel(shown);
    Rect cb{x, gr.y + by0, 58 * s, bh};
    // the combo shows the effective size; "Style size" removes the override
    vector<string> shownOpts = opts;
    shownOpts[0] = "Style size";
    int before = sel;
    if (sel == 0) shownOpts[0] = label;
    if (ui.combo(cb, "wfontsize", shownOpts, sel) && sel != before) wed.setSize(sel == 0 ? 0.f : float(atof(opts[size_t(sel)].c_str())));
    x += 59 * s;
    if (ui.iconButton({x, gr.y + by0, 24 * s, bh}, "plus", "Larger  (Ctrl+Shift+>)", false, inText)) {
      int idx = kSizeCount - 1;
      for (int i = kSizeCount - 1; i >= 0; i--) if (kSizes[i] > shown + 0.01f) idx = i;
      wed.setSize(kSizes[idx]);
    }
  }});
  // text colour
  groups.push_back({36 * s, [&](Rect gr) { writerColorButton({gr.x, gr.y + by0, 30 * s, bh}, "wcolor", inText); }});
  // inline formats
  groups.push_back({10 * 31 * s + 8 * s, [&](Rect gr) {
    float x = gr.x;
    if (ibtn(x, gr.y, "bold", "Bold  (Ctrl+B)", fmtHere & F_BOLD, inText)) wed.toggleFmt(F_BOLD);
    if (ibtn(x, gr.y, "italic", "Italic  (Ctrl+I)", fmtHere & F_ITALIC, inText)) wed.toggleFmt(F_ITALIC);
    if (ibtn(x, gr.y, "underline", "Underline  (Ctrl+U)", fmtHere & F_UNDER, inText)) wed.toggleFmt(F_UNDER);
    if (ibtn(x, gr.y, "strike", "Strikethrough  (Ctrl+Shift+D)", fmtHere & F_STRIKE, inText)) wed.toggleFmt(F_STRIKE);
    if (ibtn(x, gr.y, "sub", "Subscript  (Ctrl+Shift+-)", fmtHere & F_SUB, inText)) wed.toggleFmt(F_SUB);
    if (ibtn(x, gr.y, "sup", "Superscript  (Ctrl+Shift+=)", fmtHere & F_SUP, inText)) wed.toggleFmt(F_SUP);
    if (ibtn(x, gr.y, "code", "Code (monospace)  (Ctrl+`)", fmtHere & F_CODE, inText)) wed.toggleFmt(F_CODE);
    if (ibtn(x, gr.y, "mark", "Highlight  (Ctrl+M)", fmtHere & F_MARK, inText)) wed.toggleFmt(F_MARK);
    if (ibtn(x, gr.y, "link", "Link\xE2\x80\xA6  (Ctrl+K)", !wed.linkHere().empty(), inText)) { writerLinkBuf = wed.linkHere(); writerLinkFocus_ = true; ui.openPopup("wlink"); }
    if (ibtn(x, gr.y, "clearfmt", "Clear formatting  (Ctrl+Space)", false, inText)) wed.clearFormatting();
  }});
  // alignment
  groups.push_back({4 * 31 * s + 8 * s, [&](Rect gr) {
    float x = gr.x;
    if (ibtn(x, gr.y, "alignleft", "Align left  (Ctrl+L)", alignHere == PAlign::Left, inText)) wed.setAlign(PAlign::Left);
    if (ibtn(x, gr.y, "aligncenter", "Centre  (Ctrl+E)", alignHere == PAlign::Center, inText)) wed.setAlign(PAlign::Center);
    if (ibtn(x, gr.y, "alignright", "Align right  (Ctrl+R)", alignHere == PAlign::Right, inText)) wed.setAlign(PAlign::Right);
    if (ibtn(x, gr.y, "justify", "Justify  (Ctrl+Shift+J)", alignHere == PAlign::Justify, inText)) wed.setAlign(PAlign::Justify);
  }});
  // lists
  groups.push_back({4 * 31 * s + 8 * s, [&](Rect gr) {
    float x = gr.x;
    if (ibtn(x, gr.y, "listbullet", "Bulleted list  (Ctrl+Shift+8)", styleHere == PStyle::Bullet, inText)) wed.toggleList(false);
    if (ibtn(x, gr.y, "listnum", "Numbered list  (Ctrl+Shift+7)", styleHere == PStyle::Number, inText)) wed.toggleList(true);
    if (ibtn(x, gr.y, "outdent", "Decrease indent  (Shift+Tab, Ctrl+[)", false, isList(styleHere) || inTbl)) wed.indent(-1);
    if (ibtn(x, gr.y, "indent", "Increase indent  (Tab, Ctrl+])", false, isList(styleHere) || inTbl)) wed.indent(1);
  }});
  // insert
  groups.push_back({92 * s + 6 * 31 * s + 8 * s, [&](Rect gr) {
    float x = gr.x;
    Rect b{x, gr.y + by0, 86 * s, bh};
    if (ui.button(b, "Insert", BTN_NORMAL, "plus")) ui.openPopup("wins");
    if (ui.isPopupOpen("wins")) {
      Rect anchor = b;
      ui.overlay([this, anchor, s, openCite]() {
        struct It { const char* icon; const char* label; const char* key; bool enabled; };
        vector<It> items = {{"math", "Equation\xE2\x80\xA6  (Ctrl+Alt+M)", "equation", true}, {"table", "Table\xE2\x80\xA6", "table", true}, {"image", "Chart of the loaded data\xE2\x80\xA6", "chart", hasCorpus()},
                            {"map", "The current map view", "map", hasMap()}, {"quote", "Citation\xE2\x80\xA6", "cite", hasCorpus()},
                            {"minus", "Horizontal rule", "rule", true}, {"pagebreak", "Page break", "pb", true}, {"toc", "Table of contents", "toc", true},
                            {"book", "References heading", "refs", true}, {"caption", "Caption paragraph", "cap", true}};
        Rect pr{anchor.x, anchor.b() + 4 * s, 260 * s, float(items.size()) * 30 * s + 8 * s};
        ui.shadow(pr, 8 * s);
        ui.fill(pr, ui.c.panel2, 8 * s);
        ui.stroke(pr, ui.c.border, 8 * s);
        ui.popupRect(pr);
        for (size_t i = 0; i < items.size(); i++) {
          Rect rr{pr.x + 4 * s, pr.y + 4 * s + float(i) * 30 * s, pr.w - 8 * s, 28 * s};
          bool click = items[i].enabled && ui.listRow(rr, string("wins:") + items[i].key, false);
          Color col = items[i].enabled ? ui.c.text : ui.c.textFaint;
          ui.icon(items[i].icon, rr.x + 16 * s, rr.y + rr.h / 2, 14 * s, col, 1.6f);
          ui.text({rr.x + 32 * s, rr.y, rr.w - 36 * s, rr.h}, items[i].label, 12.5f * s, col);
          if (!click) continue;
          ui.closePopup();
          string key = items[i].key;
          if (key == "equation") { writerEquationBuf_ = wed.mathAtCaret(); writerEquationError_.clear(); writerEquationFocus_ = true; ui.openPopup("wequation"); }
          else if (key == "table") ui.openPopup("wtbl");
          else if (key == "chart") ui.openPopup("wchart");
          else if (key == "map") writerInsertMap();
          else if (key == "cite") openCite();
          else if (key == "rule") { wed.insertRule(); writerFollow_ = true; }
          else if (key == "pb") { wed.insertPageBreak(); writerFollow_ = true; }
          else if (key == "toc") { wed.insertToc(); writerFollow_ = true; }
          else if (key == "cap") { wed.insertParagraphBreak(); wed.setStyle(PStyle::Caption); writerFollow_ = true; }
          else if (key == "refs") { if (docBodyEnd(wdoc) == int(wdoc.blocks.size())) { wed.begin("References"); wdoc.blocks.push_back(Block::para("References", PStyle::H1)); wed.externalChange(); } wed.setCaret({docBodyEnd(wdoc), -1, 0}); writerFollow_ = true; }
        }
      });
    }
    x += 92 * s;
    if (ibtn(x, gr.y, "table", "Insert table\xE2\x80\xA6  (Ctrl+T)")) ui.openPopup("wtbl");
    if (ibtn(x, gr.y, "image", "Insert a chart of the loaded data\xE2\x80\xA6", false, hasCorpus())) ui.openPopup("wchart");
    if (ibtn(x, gr.y, "map", "Insert the current map view as a figure", false, hasMap())) writerInsertMap();
    if (ibtn(x, gr.y, "quote", "Insert a citation\xE2\x80\xA6  (Ctrl+Q)", false, hasCorpus())) openCite();
    if (ibtn(x, gr.y, "minus", "Horizontal rule")) { wed.insertRule(); writerFollow_ = true; }
    if (ibtn(x, gr.y, "pagebreak", "Page break  (Ctrl+Enter)")) { wed.insertPageBreak(); writerFollow_ = true; }
  }});
  // citation style
  groups.push_back({190 * s, [&](Rect gr) {
    vector<string> names;
    int sel = 0;
    const auto& styles = citeStyles();
    for (size_t i = 0; i < styles.size(); i++) { names.push_back(styles[i].name); if (wdoc.citeStyle == styles[i].id) sel = int(i); }
    ui.icon("book", gr.x + 10 * s, gr.y + rowH / 2, 14 * s, ui.c.textDim, 1.6f);
    Rect cb{gr.x + 22 * s, gr.y + by0, 162 * s, bh};
    if (ui.combo(cb, "wcitestyle", names, sel)) { wed.setCiteStyle(styles[size_t(sel)].id); writerFollow_ = true; }
  }});
  // Compile the visual document's generated LaTeX on demand; the editor remains the source of truth.
  groups.push_back({102 * s, [&](Rect gr) {
    const bool stalePdf = writerPdfPath_.empty() || writerPdfCompiledVersion_ != wed.version;
    const string label = writerPdfCompileBusy_ ? "Building\xE2\x80\xa6" : stalePdf ? (writerPdfPath_.empty() ? "Compile" : "Recompile") : "Compile";
    Rect b{gr.x, gr.y + by0, 96 * s, bh};
    if (ui.button(b, label, stalePdf ? BTN_PRIMARY : BTN_NORMAL, "play", !wdoc.empty() && !writerPdfCompileBusy_ && !busy())) writerCompileLatex();
    ui.tip("Compile the Writer document's LaTeX and refresh its PDF preview.");
  }});
  // context tools: table / figure
  if (inTbl) groups.push_back({150 * s, [&](Rect gr) {
    ui.text({gr.x, gr.y, 40 * s, rowH}, "Table", 12 * s, ui.c.textDim, AL_LEFT, 600);
    Rect b{gr.x + 42 * s, gr.y + by0, 104 * s, bh};
    if (ui.button(b, "Row / column", BTN_NORMAL, "chev-down")) ui.openPopup("wtblops");
    if (ui.isPopupOpen("wtblops")) {
      Rect anchor = b;
      ui.overlay([this, anchor, s]() {
        struct It { const char* label; int op; };
        const It items[] = {{"Insert row above", 1}, {"Insert row below", 2}, {"Insert column left", 3}, {"Insert column right", 4}, {"Delete row", 5}, {"Delete column", 6},
                            {"Header row on / off", 7}, {"Column: align left", 8}, {"Column: centre", 9}, {"Column: align right", 10}, {"Column: numbers right", 11}, {"Delete table", 12}};
        const int n = int(sizeof(items) / sizeof(items[0]));
        Rect pr{anchor.x, anchor.b() + 4 * s, 220 * s, float(n) * 28 * s + 8 * s};
        ui.shadow(pr, 8 * s);
        ui.fill(pr, ui.c.panel2, 8 * s);
        ui.stroke(pr, ui.c.border, 8 * s);
        ui.popupRect(pr);
        for (int i = 0; i < n; i++) {
          Rect rr{pr.x + 4 * s, pr.y + 4 * s + float(i) * 28 * s, pr.w - 8 * s, 26 * s};
          if (ui.listRow(rr, "wtblop:" + std::to_string(items[i].op), false)) {
            ui.closePopup();
            switch (items[i].op) {
              case 1: wed.tableInsertRow(false); break; case 2: wed.tableInsertRow(true); break; case 3: wed.tableInsertCol(false); break;
              case 4: wed.tableInsertCol(true); break; case 5: wed.tableDeleteRow(); break; case 6: wed.tableDeleteCol(); break;
              case 7: wed.tableToggleHeader(); break; case 8: wed.tableSetColAlign('l'); break; case 9: wed.tableSetColAlign('c'); break;
              case 10: wed.tableSetColAlign('r'); break; case 11: wed.tableSetColAlign('a'); break; case 12: wed.tableDeleteTable(); break;
            }
            writerFollow_ = true;
          }
          ui.text({rr.x + 10 * s, rr.y, rr.w - 14 * s, rr.h}, items[i].label, 12.5f * s, items[i].op == 12 ? ui.c.danger : ui.c.text);
        }
      });
    }
  }});
  if (onFig) groups.push_back({262 * s, [&](Rect gr) {
    ui.text({gr.x, gr.y, 44 * s, rowH}, "Figure", 12 * s, ui.c.textDim, AL_LEFT, 600);
    int pct = wed.curBlock().fig.widthPct;
    int sel = pct <= 50 ? 0 : pct <= 75 ? 1 : 2;
    Rect sr{gr.x + 46 * s, gr.y + by0 + 1 * s, 150 * s, bh - 2 * s};
    if (ui.segmented(sr, {"50%", "75%", "100%"}, sel, "wfigw")) wed.setFigureWidth(sel == 0 ? 50 : sel == 1 ? 75 : 100);
    float x = gr.x + 200 * s;
    if (ibtn(x, gr.y, "sliders", "Figure properties: format, caption, border, alignment", writerPane == 2)) writerPane = 2;
    if (ibtn(x, gr.y, "trash", "Delete the figure")) { wed.deleteBlock(wed.caret.blk); writerFollow_ = true; }
  }});
  // flow the groups into rows
  const bool toolbarPreviewOpen = writerPdfPreviewOpen_;
  const bool compactToolbar = r.w < 560 * s;
  const bool toolbarShowExternal = r.w >= 240 * s;
  const bool toolbarShowPanes = !toolbarPreviewOpen && r.w >= 420 * s;
  const float exportControlW = compactToolbar ? 30 * s : 92 * s;
  const float rightW = (30 + 8 + (compactToolbar ? 30 : 92) + 10 + (toolbarShowExternal ? 31 : 0) + 31 + 8 + (toolbarShowPanes ? 8 + 6 + 3 * 31 : 0) + 10) * s;
  vector<vector<int>> rows(1);
  vector<float> rowUsed(1, 8 * s);
  for (size_t gi = 0; gi < groups.size(); gi++) {
    float need = groups[gi].w + 10 * s;
    if (rowUsed.back() + need > r.w - 8 * s && !rows.back().empty()) { rows.emplace_back(); rowUsed.push_back(8 * s); }
    rows.back().push_back(int(gi));
    rowUsed.back() += need;
  }
  if (rowUsed.back() + rightW > r.w - 8 * s && !rows.back().empty()) { rows.emplace_back(); rowUsed.push_back(8 * s); }  // keep actions on their own row rather than overlapping editor controls
  const float tbH = float(rows.size()) * rowH;
  ui.fill({r.x, r.y, r.w, tbH}, ui.c.panel);
  ui.line(r.x, r.y + tbH, r.r(), r.y + tbH, ui.c.border, 1);
  for (size_t ri = 0; ri < rows.size(); ri++) {
    float x = r.x + 8 * s, y = r.y + float(ri) * rowH;
    for (size_t j = 0; j < rows[ri].size(); j++) {
      const Group& G = groups[size_t(rows[ri][j])];
      G.draw({x, y, G.w, rowH});
      x += G.w;
      if (j + 1 < rows[ri].size()) sepAt(x, y);
      x += 10 * s;
    }
  }
  // right block on the last row: panes, zoom, export, close
  {
    float y = r.y + float(rows.size() - 1) * rowH;
    float rx = r.r() - 10 * s;
    rx -= 30 * s;
    if (ui.iconButton({rx, y + by0, 30 * s, bh}, "x", "Close the writer  (Esc, Ctrl+Shift+W)")) { closeWriter(); return; }
    rx -= 8 * s;
    {
      float w = exportControlW;
      rx -= w;
      Rect b{rx, y + by0, w, bh};
      if (ui.button(b, compactToolbar ? "" : "Export", BTN_PRIMARY, "download", !wdoc.empty())) ui.openPopup("wexp");
      if (compactToolbar) ui.tip("Export the document");
      if (ui.isPopupOpen("wexp")) {
        Rect anchor = b;
        ui.overlay([this, anchor, s]() {
          struct It { const char* label; const char* fmt; };
          const It items[] = {{"PDF (print-ready)\xE2\x80\xA6", "pdf"}, {"Word document (.docx)\xE2\x80\xA6", "docx"}, {"Web page (.html)\xE2\x80\xA6", "html"}, {"Open in Word (save + launch)", "word"}, {"All three into Documents\\VOSStudio\\Reports", "all"}};
          Rect pr{anchor.r() - 300 * s, anchor.b() + 4 * s, 300 * s, 5 * 30 * s + 8 * s};
          ui.shadow(pr, 8 * s);
          ui.fill(pr, ui.c.panel2, 8 * s);
          ui.stroke(pr, ui.c.border, 8 * s);
          ui.popupRect(pr);
          for (int i = 0; i < 5; i++) {
            Rect rr{pr.x + 4 * s, pr.y + 4 * s + float(i) * 30 * s, pr.w - 8 * s, 28 * s};
            if (ui.listRow(rr, string("wexp:") + items[i].fmt, false)) {
              ui.closePopup();
              string f = items[i].fmt;
              if (f == "word") writerOpenInWord();
              else if (f != "all") writerExport(f);
              else {
                string base = reportsDir() + "\\" + writerSlug();
                string p1 = writerExport("pdf", base + ".pdf"), p2 = writerExport("docx", base + ".docx"), p3 = writerExport("html", base + ".html");
                if (!p1.empty() || !p2.empty() || !p3.empty()) revealInExplorer(!p1.empty() ? p1 : !p2.empty() ? p2 : p3);
              }
            }
            ui.text({rr.x + 10 * s, rr.y, rr.w - 14 * s, rr.h}, items[i].label, 12.5f * s, ui.c.text);
          }
        });
      }
      rx -= 10 * s;
    }
    if (toolbarShowExternal) {
      rx -= 31 * s;
      if (ui.iconButton({rx, y + by0, 30 * s, bh}, "external", "Open the latest compiled PDF in the system viewer", false, !writerPdfPath_.empty())) writerOpenPdfSeparately();
    }
    rx -= 31 * s;
    if (ui.iconButton({rx, y + by0, 30 * s, bh}, "eye", "Side-by-side PDF preview", writerPdfPreviewOpen_)) writerSetPdfPreview(!writerPdfPreviewOpen_);
    rx -= 8 * s;
    if (toolbarShowPanes) {
      rx -= 8 * s;
      ui.line(rx + 4 * s, y + 10 * s, rx + 4 * s, y + rowH - 10 * s, ui.c.border, 1);
      rx -= 6 * s;
      rx -= 31 * s;
      if (ui.iconButton({rx, y + by0, 30 * s, bh}, "search", "Find and replace  (Ctrl+F)", writerPane == 3)) writerPane = writerPane == 3 ? 0 : 3;
      rx -= 31 * s;
      if (ui.iconButton({rx, y + by0, 30 * s, bh}, "sliders", "Properties: page, paragraph, text, figure, table, citations", writerPane == 2)) writerPane = writerPane == 2 ? 0 : 2;
      rx -= 31 * s;
      if (ui.iconButton({rx, y + by0, 30 * s, bh}, "list", "Outline: headings, figures and tables", writerPane == 1)) writerPane = writerPane == 1 ? 0 : 1;
    }
  }

  // ---- body: page area + side pane + status
  const float statusH = 24 * s;
  Rect body{r.x, r.y + tbH + 1, r.w, r.h - tbH - 1 - statusH};
  Rect status{r.x, body.b(), r.w, statusH};
  Rect pageArea, pane, previewArea;
  if (writerPdfPreviewOpen_) {
    const bool stacked = body.w < 760 * s && body.h > 420 * s;
    if (stacked) {
      float editorH = clampv(body.h * 0.56f, 180 * s, body.h - 180 * s);
      pageArea = {body.x, body.y, body.w, editorH};
      previewArea = {body.x, pageArea.b() + 1 * s, body.w, std::max(0.f, body.b() - pageArea.b() - 1 * s)};
    } else {
      const float splitW = body.w - 1 * s;
      float editorW = splitW * 0.52f;
      if (splitW >= 640 * s) editorW = clampv(editorW, 320 * s, splitW - 300 * s);
      pageArea = {body.x, body.y, editorW, body.h};
      previewArea = {pageArea.r() + 1 * s, body.y, std::max(0.f, body.r() - pageArea.r() - 1 * s), body.h};
    }
    pane = {body.r(), body.y, 0, body.h};
  } else {
    const float paneW = writerPane ? std::min(320 * s, r.w * 0.36f) : 0;
    pageArea = {body.x, body.y, body.w - paneW, body.h};
    pane = {pageArea.r(), body.y, paneW, body.h};
  }
  writerViewH_ = pageArea.h;

  writerPageAreaW_ = pageArea.w;
  if (writerFitOnce_) {
    writerFitOnce_ = false;
    float fz = writerFitZoom();
    if (fz > 0 && writerZoom > fz) { writerZoom = fz; writerZoomShown_ = fz; }
  }
  // zoom: the displayed scale eases towards the target on a clock (the same feel at 60 or 144 Hz). The layout scale is
  // fixed at 100 % — zooming is a pure transform, as in Word: line breaks and pages never move, nothing is re-laid out,
  // and the text stays crisp because Direct2D rasterises the glyphs at the final scale.
  {
    float dt = writerZoomPrevT_ > 0 ? clampv(float(ui.time - writerZoomPrevT_), 0.f, 0.05f) : 1 / 60.f;
    writerZoomPrevT_ = ui.time;
    if (std::fabs(writerZoomShown_ - writerZoom) > 0.0015f) { writerZoomShown_ += (writerZoom - writerZoomShown_) * (1 - std::exp(-dt / 0.045f)); ui.animating = true; writerZoomT_ = ui.time; }
    else writerZoomShown_ = writerZoom;
  }
  float kD = std::max(0.2f, writerZoomShown_ * s * 96.f / 72.f);  // the page may be wider than the area: it scrolls sideways then
  const bool settled = ui.time - writerZoomT_ > 0.15;  // figures are re-rendered at their new pixel size only once the zoom rests
  writerK_ = s * 96.f / 72.f;
  writerRelayout(writerK_);
  WriterLayout& L = *wlay;
  const float f = kD / std::max(0.01f, writerK_);  // display scale over the layout scale
  const float gapS = 20 * s;  // above the first page (screen px)
  const float colHL = float(L.pages) * L.pageHL + float(L.pages - 1) * L.gapL;  // page column height (layout px)
  const float maxScroll = std::max(0.f, colHL * f + 2 * gapS - pageArea.h);
  // scroll compensation: a change of the displayed scale keeps the anchor point (mouse or middle) still.
  // Screen y = column top + document y * kD, so only kD matters — a relayout (kL := kD, f := 1) moves nothing.
  static float kDprev = 0;
  const float pageWpxNow = L.pageWL * (kD / std::max(0.01f, writerK_));
  const float maxScrollX = std::max(0.f, pageWpxNow + 32 * s - pageArea.w);
  if (kDprev > 0 && std::fabs(kD - kDprev) > 1e-4f) {
    float anchor = writerZoomAnchorY_ >= 0 ? writerZoomAnchorY_ : pageArea.y + pageArea.h / 2;
    float colTop = pageArea.y + gapS - writerScroll;
    writerScroll += (anchor - colTop) * (kD / kDprev - 1);
    // sideways too, once the page is wider than the area: the point under the mouse stays put
    float prevW = L.pageWL * (kDprev / std::max(0.01f, writerK_));
    float prevLeft = prevW + 32 * s > pageArea.w ? pageArea.x + 16 * s - writerScrollX_ : pageArea.x + (pageArea.w - prevW) / 2;
    float anchorX = writerZoomAnchorX_ >= 0 ? writerZoomAnchorX_ : pageArea.x + pageArea.w / 2;
    float newLeft = anchorX - (anchorX - prevLeft) * (kD / kDprev);
    writerScrollX_ = maxScrollX > 0 ? pageArea.x + 16 * s - newLeft : 0;
  }
  kDprev = kD;
  writerF_ = f;

  // page hit area + mouse (before the scroll is applied for this frame, so the caret follows the same coordinates)
  uint64_t pid = ui.id("wpage");
  bool pageHov = false, pageHeld = false;
  const bool overMini = writerMiniShow_ && writerMiniR_.has(ui.in.mx, ui.in.my);  // the floating mini toolbar takes the mouse over it
  if (!overMini) ui.behave(pid, pageArea, &pageHov, &pageHeld);
  const float writerZoomGesture = ui.in.wheel + ui.in.pinch;
  if (pageHov && writerZoomGesture != 0 && !ui.anyPopup()) {
    writerMiniShow_ = false;
    if (ui.in.pinch != 0 || ui.in.ctrl) { writerZoom = clampv(writerZoom * std::pow(1.1f, clampv(writerZoomGesture, -4.f, 4.f)), 0.4f, 3.f); writerZoomT_ = ui.time; writerZoomAnchorY_ = ui.in.my; writerZoomAnchorX_ = ui.in.mx; }
    else if (ui.in.shift) writerScrollX_ -= ui.in.wheel * 64 * s;  // Shift+wheel: sideways (a wide page at a high zoom)
    else writerScroll -= ui.in.wheel * 64 * s;
  }
  // A touch drag pans the page; a stationary tap still places the caret as usual.
  if (ui.in.touchCancel) {
    writerTouchPending_ = writerTouchPanning_ = writerTouchSelecting_ = false;
    writerDrag_ = writerSelGesture_ = false;
  }
  if (ui.in.touch && ui.in.pressed[0] && pageHov && !overMini && !ui.anyPopup()) {
    writerTouchPending_ = true;
    writerTouchPanning_ = writerTouchSelecting_ = false;
    writerTouchStartX_ = writerTouchLastX_ = ui.in.mx;
    writerTouchStartY_ = writerTouchLastY_ = ui.in.my;
    writerTouchStartT_ = ui.time;
  }
  if (pageHov && ui.in.hwheel != 0 && !ui.anyPopup()) writerScrollX_ += ui.in.hwheel * 64 * s;  // a touchpad's sideways swipe
  // keep the caret in view
  if (writerFollow_) {
    writerFollow_ = false;
    float cx = 0, cy = 0, ch = 0;
    if (writerCaretRect(wed.caret, cx, cy, ch)) {
      float top = gapS + cy * f - writerScroll, bot = top + ch * f;  // relative to pageArea.y
      if (top < 24 * s) writerScroll -= 24 * s - top;
      else if (bot > pageArea.h - 24 * s) writerScroll += bot - (pageArea.h - 24 * s);
    }
  }
  writerScroll = clampv(writerScroll, 0.f, maxScroll);
  writerScrollX_ = clampv(writerScrollX_, 0.f, maxScrollX);
  const float pageWpx = L.pageWL * f;
  const float originX = maxScrollX > 0 ? pageArea.x + 16 * s - writerScrollX_ : pageArea.x + (pageArea.w - pageWpx) / 2;  // screen position of the page column's (0, 0)
  const float originY = pageArea.y + gapS - writerScroll;
  writerPageR_ = {originX, originY, pageWpx, colHL * f};
  const float ML = L.ML;
  auto toCol = [&](float mx, float my) { return std::make_pair((mx - originX) / f - ML, (my - originY) / f); };  // screen -> (text-area x, column y) in layout px

  // mouse: caret, selection, double / triple click, context menu, links
  if (pageHov && !ui.anyPopup()) ui.cursor = "ibeam";
  if (pageHov && ui.in.pressed[0] && editable) {
    auto pp = toCol(ui.in.mx, ui.in.my);
    DocPos q = writerHit(pp.first, pp.second);
    // Ctrl+click on a link opens it
    if (ui.in.ctrl) {
      const Para* pr = docPara(wdoc, q);
      if (pr) { string url = pr->linkAt(std::min(pr->size(), size_t(q.off) + 1)); if (!url.empty()) { openUrl(url); ui.in.pressed[0] = false; } }
    }
    if (ui.in.pressed[0]) {
      double now = ui.time;
      writerClicks_ = (now - writerClickT_ < 0.45 && !ui.in.shift) ? writerClicks_ + 1 : 1;
      writerClickT_ = now;
      if (writerClicks_ >= 3) { wed.setCaret(q); wed.selectParagraph(); writerClicks_ = 0; }
      else if (writerClicks_ == 2 || ui.in.dbl) { wed.setCaret(q); wed.selectWord(); }
      else wed.setCaret(q, ui.in.shift);
      writerDrag_ = writerClicks_ == 1;
      writerCaretX_ = -1;
      writerSelGesture_ = true;
      writerMiniShow_ = false;  // a new gesture: the bar comes back when this one ends with a selection
    }
  }
  if (writerTouchPending_ && ui.in.down[0]) {
    if (writerDrag_ && ui.time - writerTouchStartT_ >= 0.5) {
      // Long-press retains the existing caret-drag selection; a quicker movement pans the page.
      writerTouchPending_ = false;
      writerTouchSelecting_ = true;
    } else if (!writerTouchSelecting_ && ui.in.touch && std::hypot(ui.in.mx - writerTouchStartX_, ui.in.my - writerTouchStartY_) >= 8 * s) {
      writerTouchPanning_ = true;
      writerTouchPending_ = false;
      writerDrag_ = false;
      writerSelGesture_ = false;
      writerMiniShow_ = false;
    } else if (writerDrag_) {
      const double wake = writerTouchStartT_ + 0.5;
      if (ui.wakeAt <= 0 || ui.wakeAt > wake) ui.wakeAt = wake;
    }
  }
  if (writerTouchPanning_ && ui.in.touch && ui.in.down[0]) {
    writerScroll -= ui.in.my - writerTouchLastY_;
    writerScrollX_ -= ui.in.mx - writerTouchLastX_;
    writerTouchLastX_ = ui.in.mx;
    writerTouchLastY_ = ui.in.my;
  }
  if (ui.in.touch && ui.in.released[0] && !ui.in.down[0]) writerTouchPending_ = writerTouchPanning_ = writerTouchSelecting_ = false;
  if (writerDrag_ && pageHeld) {
    auto pp = toCol(ui.in.mx, ui.in.my);
    DocPos q = writerHit(pp.first, pp.second);
    if (q != wed.caret) wed.setCaret(q, true);
    // auto-scroll while dragging beyond the edges
    if (ui.in.my < pageArea.y + 10 * s) { writerScroll -= 12 * s; ui.animating = true; }
    else if (ui.in.my > pageArea.b() - 10 * s) { writerScroll += 12 * s; ui.animating = true; }
  }
  if (!ui.in.down[0]) writerDrag_ = false;
  // the mini toolbar (Word's): a mouse gesture that ends with a text selection shows it above the pointer
  if (ui.in.released[0] && writerSelGesture_) {
    writerSelGesture_ = false;
    if (wed.hasSelection() && docPara(wdoc, wed.caret)) {
      writerMiniShow_ = true;
      writerMiniCtx_ = false;
      writerMiniX_ = ui.in.mx;
      writerMiniY_ = ui.in.my;
    }
  }
  // where and when the selection last changed (a right-click near there keeps it)
  if (wed.caret != writerPrevCaret_ || wed.anchor != writerPrevAnchor_) {
    writerPrevCaret_ = wed.caret;
    writerPrevAnchor_ = wed.anchor;
    if (wed.hasSelection()) {
      writerSelT_ = ui.time;
      if (ui.in.down[0] || ui.in.released[0]) { writerSelRefX_ = ui.in.mx; writerSelRefY_ = ui.in.my; }
      else {  // keyboard: the caret's place on the screen
        float cx = 0, cy = 0, ch = 0;
        if (writerCaretRect(wed.caret, cx, cy, ch)) { writerSelRefX_ = originX + (ML + cx) * f; writerSelRefY_ = originY + (cy + ch / 2) * f; }
      }
    }
  }
  if (pageHov && ui.in.pressed[1] && editable) {
    auto pp = toCol(ui.in.mx, ui.in.my);
    DocPos q = writerHit(pp.first, pp.second);
    // the selection stays when the click is inside it, or shortly after it was made and near where it ended (the
    // pointer usually rests just outside a fresh selection)
    bool inSel = wed.hasSelection() && !docBefore(q, wed.selStart()) && !docBefore(wed.selEnd(), q);
    bool fresh = wed.hasSelection() && ui.time - writerSelT_ < 3.0 && std::hypot(ui.in.mx - writerSelRefX_, ui.in.my - writerSelRefY_) < 180 * s;
    if (!inSel && !fresh) wed.setCaret(q);
    writerCtxX_ = ui.in.mx;
    writerCtxY_ = ui.in.my;
    writerCtxSub_.clear();
    writerCtxR_ = {0, 0, 0, 0};
    ui.openPopup("wctx");
    if (docPara(wdoc, wed.caret)) { writerMiniShow_ = true; writerMiniCtx_ = true; writerMiniX_ = ui.in.mx; writerMiniY_ = ui.in.my; }  // rides on top of the menu
    else writerMiniShow_ = false;
  }

  // ---- the pages (drawn in layout pixels under a scale transform; clipped to the page area)
  ui.pushClip(pageArea);
  ID2D1DeviceContext* dc = ui.dc();
  D2D1_MATRIX_3X2_F oldT;
  dc->GetTransform(&oldT);
  dc->SetTransform(D2D1::Matrix3x2F::Scale(f, f) * D2D1::Matrix3x2F::Translation(originX, originY) * oldT);
  const float k = L.k;
  const float textW = L.textW;
  DocPos selA = wed.selStart(), selB = wed.selEnd();
  const bool hasSel = wed.hasSelection();
  auto selRange = [&](int blk, int cell, size_t len, int& a, int& b) {  // selected byte range of a unit; false when none
    if (!hasSel) return false;
    DocPos u0{blk, cell, 0}, u1{blk, cell, int(len)};
    if (docBefore(u1, selA) || docBefore(selB, u0)) return false;
    a = (selA.blk == blk && selA.cell == cell) ? selA.off : 0;
    b = (selB.blk == blk && selB.cell == cell) ? selB.off : int(len);
    return true;
  };
  auto drawUnit = [&](const Para& p, const WPara& P, float bx, float by, int blk, int cell) {
    float px = bx + P.x, py = by + P.y;
    if (!P.lay) return;
    drawMarks(ui, p, P, px, py);
    int a = 0, b = 0;
    if (selRange(blk, cell, p.size(), a, b)) {
      if (a == b) ui.fill({px - 1, py, 4 * k, P.h}, kSel);  // an empty paragraph inside the selection
      for (auto& rr : rangeRects(P, a, b)) ui.fill({px + rr.left, py + rr.top, rr.right - rr.left, rr.bottom - rr.top}, kSel);
    }
    dc->DrawTextLayout(D2D1::Point2F(px, py), P.lay.get(), L.inkBr.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
  };
  auto drawBlock = [&](size_t i, float bx, float by) {
    const WBlock& W = L.blocks[i];
    const Block& b = wdoc.blocks[i];
    bool blockSel = hasSel && !docBefore(DocPos{int(i), -1, 0}, selA) && !docBefore(selB, DocPos{int(i), -1, 0}) && b.kind != Block::Paragraph;
    bool caretHere = wed.blockSelected() && wed.caret.blk == int(i);
    if (b.kind == Block::Paragraph) {
      const Para& p = b.p;
      if (p.style == PStyle::Code) ui.fill({bx, by + W.para.y - 6 * k, textW, W.para.h + 12 * k}, kCodeBg, 3 * k);
      if (p.style == PStyle::Quote) ui.fill({bx + 6 * k, by + W.para.y, 2.2f * k, W.para.h}, kQuoteBar);
      if (!W.label.empty()) {
        Rect lr{bx + W.labelX, by + W.para.y, std::max(4.f, W.para.x - W.labelX - 4 * k), W.para.h};
        IDWriteTextFormat* lf = g.format(W.labelSize, 400, false, false, false);
        if (lf) {
          std::wstring lw = widen(W.label);
          dc->DrawText(lw.c_str(), UINT32(lw.size()), lf, D2D1::RectF(lr.x, lr.y, lr.r() + 200, lr.b()), L.inkBr.get());
        }
      }
      drawUnit(p, W.para, bx, by, int(i), -1);
    } else if (b.kind == Block::TableBlock) {
      int R = b.tbl.rows(), C = b.tbl.cols();
      if (R && C && W.cells.size() == size_t(R) * size_t(C)) {
        float tblH = W.rowY.back() + W.rowH.back() - W.rowY.front();
        if (b.tbl.header) ui.fill({bx, by + W.rowY[0], textW, W.rowH[0]}, kHeadBg);
        for (int rr = 0; rr <= R; rr++) { float yy = rr < R ? W.rowY[size_t(rr)] : W.rowY.back() + W.rowH.back(); ui.line(bx, by + yy, bx + textW, by + yy, kGrid, rr == 0 || rr == R ? 1.4f : 1.f); }
        for (int cc = 0; cc <= C; cc++) { float xx = cc < C ? W.colX[size_t(cc)] : textW; ui.line(bx + xx, by + W.rowY[0], bx + xx, by + W.rowY[0] + tblH, kGrid, 1.f); }
        for (int rr = 0; rr < R; rr++) for (int cc = 0; cc < C; cc++) drawUnit(b.tbl.cells[size_t(rr)][size_t(cc)], W.cells[size_t(rr) * size_t(C) + size_t(cc)], bx, by, int(i), rr * C + cc);
        if (blockSel || caretHere) ui.stroke({bx - 2, by + W.rowY[0] - 2, textW + 4, tblH + 4}, ui.c.accent, 2, 1.5f * k);
      }
    } else if (b.kind == Block::FigureBlock) {
      Rect ir{bx + W.img.x, by + W.img.y, W.img.w, W.img.h};
      const FigAsset* a = b.fig.asset >= 0 && size_t(b.fig.asset) < wdoc.assets.size() ? &wdoc.assets[size_t(b.fig.asset)] : nullptr;
      if (a && a->scene.W > 0) {
        int wpx = std::max(1, int(std::lround(ir.w * f)));
        float scale = float(wpx) / float(a->scene.W);
        int hpx = std::max(1, int(std::ceil(a->scene.H * scale)));
        if (double(wpx) * hpx > 12e6) {  // enormous zoom: draw the vectors directly
          dc->PushAxisAlignedClip(D2D1::RectF(ir.x, ir.y, ir.r(), ir.b()), D2D1_ANTIALIAS_MODE_ALIASED);
          drawScene(g.dc.get(), g.d2f.get(), g.dw.get(), g, a->scene, ir.x, ir.y, ir.w / float(a->scene.W), a->scene.serif);
          dc->PopAxisAlignedClip();
        } else {
          auto it = L.images.find(b.fig.asset);
          // while the zoom animates the existing bitmap is stretched (a re-render every frame is what made zooming
          // stutter); once it rests the figure is rendered once at its exact pixel size
          bool stale = it == L.images.end() || !it->second.bmp || (it->second.wpx != wpx && (settled || it->second.wpx < wpx / 2 || it->second.wpx > wpx * 2));
          if (stale) {
            if (it != L.images.end()) L.images.erase(it);
            L.dropImages(size_t(96) << 20, frameNo);  // 96 MB of figure bitmaps at most; the least recently drawn go first
            WriterLayout::Img& im = L.images[b.fig.asset];
            im = WriterLayout::Img();
            im.wpx = wpx;
            im.hpx = hpx;
            D2D1_SIZE_F sz = D2D1::SizeF(float(wpx), float(hpx));
            D2D1_SIZE_U pxs = D2D1::SizeU(UINT32(wpx), UINT32(hpx));
            D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            if (SUCCEEDED(g.dc->CreateCompatibleRenderTarget(&sz, &pxs, &pf, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, im.rt.put()))) {
              im.rt->SetDpi(96, 96);
              im.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
              im.rt->BeginDraw();
              im.rt->SetTransform(D2D1::Matrix3x2F::Identity());
              im.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
              drawScene(im.rt.get(), g.d2f.get(), g.dw.get(), g, a->scene, 0, 0, scale, a->scene.serif);
              if (FAILED(im.rt->EndDraw()) || FAILED(im.rt->GetBitmap(im.bmp.put()))) { im.rt.reset(); im.bmp.reset(); }
            }
            it = L.images.find(b.fig.asset);
          }
          if (it != L.images.end() && it->second.bmp) { it->second.frame = frameNo; dc->DrawBitmap(it->second.bmp.get(), D2D1::RectF(ir.x, ir.y, ir.r(), ir.b()), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); }
          else ui.fill(ir, kCodeBg, 2 * k);
          if (!settled) ui.animating = true;  // come back once the zoom rests to render the figure sharp
        }
      } else {
        ui.fill(ir, kCodeBg, 2 * k);
        ui.text(ir, "Figure missing", 12 * k, kInkDim, AL_CENTER);
      }
      if (b.fig.border) ui.stroke({ir.x - 0.5f, ir.y - 0.5f, ir.w + 1, ir.h + 1}, kFigBorder, 0, 1.f);
      if (blockSel || caretHere) ui.stroke({ir.x - 3, ir.y - 3, ir.w + 6, ir.h + 6}, ui.c.accent, 3, 1.5f * k);
      drawUnit(b.fig.caption, W.para, bx, by, int(i), -2);
    } else if (b.kind == Block::Rule) {
      ui.line(bx, by + W.h / 2, bx + textW, by + W.h / 2, kGrid, 1.2f);
      if (blockSel || caretHere) ui.stroke({bx - 2, by + 2, textW + 4, W.h - 4}, ui.c.accent, 2, 1.5f * k);
    } else if (b.kind == Block::PageBreak) {
      Color dash(0.55f, 0.57f, 0.62f);
      for (float xx = 0; xx < textW; xx += 8 * k) ui.line(bx + xx, by + W.h / 2, bx + std::min(textW, xx + 4 * k), by + W.h / 2, dash, 1);
      string lab = "Page break";
      float lw = ui.textW(lab, 10 * k, 600) + 12 * k;
      ui.fill({bx + textW / 2 - lw / 2, by + W.h / 2 - 8 * k, lw, 16 * k}, kPaper);
      ui.text({bx + textW / 2 - lw / 2, by + W.h / 2 - 8 * k, lw, 16 * k}, lab, 10 * k, dash, AL_CENTER, 600);
      if (blockSel || caretHere) ui.stroke({bx - 2, by + 2, textW + 4, W.h - 4}, ui.c.accent, 2, 1.5f * k);
    } else if (b.kind == Block::Toc) {
      float lh = 16 * k * (ps.baseSize / 11.f);
      ui.text({bx, by + 4 * k, textW, lh + 4 * k}, "Contents", 13 * k * (ps.baseSize / 11.f), kInk, AL_LEFT, 700);
      float yy = by + 4 * k + lh + 10 * k;
      for (auto& e : W.toc) { ui.text({bx + e.second, yy, textW - e.second, lh}, e.first, 10.5f * k * (ps.baseSize / 11.f), kInkDim, AL_LEFT, 400); yy += lh; }
      if (W.toc.empty()) ui.text({bx, by + 4 * k + lh + 6 * k, textW, lh}, "(headings appear here; page numbers in the exports)", 10.5f * k, kInkDim);
      if (blockSel || caretHere) ui.stroke({bx - 2, by + 2, textW + 4, W.h - 4}, ui.c.accent, 2, 1.5f * k);
    }
  };
  const float phL = L.pageHL + L.gapL;
  const float visTop = (pageArea.y - originY) / f, visBot = (pageArea.b() - originY) / f;  // visible band in column coordinates
  int caretPage = 0;
  {
    float cx = 0, cy = 0, ch = 0;
    if (writerCaretLocal(wed.caret, cx, cy, ch) && size_t(wed.caret.blk) < L.blocks.size()) {
      const WBlock& W = L.blocks[size_t(wed.caret.blk)];
      if (!W.places.empty()) { caretPage = W.places.back().page; for (auto& p : W.places) if (cy < p.y1) { caretPage = p.page; break; } }
    }
  }
  writerCaretPage_ = caretPage;
  for (int pg = 0; pg < L.pages; pg++) {
    float top = float(pg) * phL;
    if (top + L.pageHL < visTop - 40) continue;
    if (top > visBot + 40) break;
    Rect pageR{0, top, L.pageWL, L.pageHL};
    ui.shadow(pageR, 2 * k, 14 * k);
    ui.fill(pageR, kPaper, 2 * k);
    ui.stroke(pageR, kPageEdge, 2 * k, 1);
    {  // margin guides (faint corner marks)
      Color guide(0, 0, 0, 0.10f);
      float ml = 10 * k;
      for (int cx2 = 0; cx2 < 2; cx2++) for (int cy2 = 0; cy2 < 2; cy2++) {
        float gx = cx2 ? pageR.r() - ML : pageR.x + ML, gy = cy2 ? pageR.b() - ML : pageR.y + ML;
        ui.line(gx - (cx2 ? 0 : ml), gy, gx + (cx2 ? ml : 0), gy, guide, 1);
        ui.line(gx, gy - (cy2 ? 0 : ml), gx, gy + (cy2 ? ml : 0), guide, 1);
      }
    }
    // running head and page number, as the PDF prints them
    if (!ps.header.empty()) {
      float hy = pageR.y + ML * 0.55f;
      ui.text({pageR.x + ML, hy - 8 * k, textW, 16 * k}, ps.header, 9 * k, kInkDim, AL_RIGHT);
      ui.line(pageR.x + ML, hy + 10 * k, pageR.r() - ML, hy + 10 * k, Color(0, 0, 0, 0.12f), 0.8f);
    }
    if (ps.pageNumbers) ui.text({pageR.x + ML, pageR.b() - ML * 0.6f - 7 * k, textW, 14 * k}, std::to_string(pg + 1), 9 * k, kInkDim, AL_CENTER);
    // the blocks placed on this page
    for (auto& e : L.pagePlaces[size_t(pg)]) {
      const WBlock& W = L.blocks[size_t(e.first)];
      const WPlace& pl = W.places[size_t(e.second)];
      float sliceTop = top + ML + pl.py, sliceH = pl.y1 - pl.y0;
      if (sliceTop + sliceH < visTop - 10 || sliceTop > visBot + 10) continue;
      float by = sliceTop - pl.y0;
      D2D1_RECT_F clip = D2D1::RectF(pageR.x, sliceTop - (pl.y0 <= 0.01f ? 3 * k : 0), pageR.r(), sliceTop + std::max(sliceH, 1.f) + (pl.y1 >= W.h - 0.01f ? 3 * k : 0));
      dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
      drawBlock(size_t(e.first), ML, by);
      dc->PopAxisAlignedClip();
    }
  }
  // caret
  if (editable && !hasSel && !wed.blockSelected()) {
    float cx = 0, cy = 0, ch = 0;
    if (writerCaretRect(wed.caret, cx, cy, ch)) {
      ui.wantsCaret = true;
      static double lastMove = 0;
      static uint64_t lastVer = 0;
      static DocPos lastPos;
      if (lastVer != wed.version || !(lastPos == wed.caret)) { lastMove = ui.time; lastVer = wed.version; lastPos = wed.caret; }
      if (std::fmod(ui.time - lastMove, 1.0) < 0.55) ui.line(ML + cx, cy, ML + cx, cy + ch, kCaret, std::max(1.f, 1.2f * k));
    }
  }
  dc->SetTransform(oldT);
  ui.popClip();
  // scrollbar
  if (maxScroll > 0) {
    float total = colHL * f + 2 * gapS;
    float th = std::max(24 * s, pageArea.h * pageArea.h / total);
    float ty = pageArea.y + (pageArea.h - th) * (writerScroll / maxScroll);
    Rect bar{pageArea.r() - 6 * s, ty, 4 * s, th};
    uint64_t bid = ui.id("wscroll");
    bool hov = false, held = false;
    ui.behave(bid, {pageArea.r() - 12 * s, pageArea.y, 12 * s, pageArea.h}, &hov, &held);
    if (held) { float t = clampv((ui.in.my - pageArea.y - th / 2) / std::max(1.f, pageArea.h - th), 0.f, 1.f); writerScroll = t * maxScroll; }
    ui.fill(bar, (hov || held) ? ui.c.textFaint : ui.c.textFaint.withA(0.45f), 2 * s);
  }
  if (maxScrollX > 0) {  // horizontal: the page is wider than the area (Shift+wheel, or drag the bar)
    float total = pageWpx + 32 * s;
    float tw = std::max(24 * s, pageArea.w * pageArea.w / total);
    float tx = pageArea.x + (pageArea.w - tw) * (writerScrollX_ / maxScrollX);
    Rect bar{tx, pageArea.b() - 6 * s, tw, 4 * s};
    uint64_t bid = ui.id("wscrollx");
    bool hov = false, held = false;
    ui.behave(bid, {pageArea.x, pageArea.b() - 12 * s, pageArea.w, 12 * s}, &hov, &held);
    if (held) { float t = clampv((ui.in.mx - pageArea.x - tw / 2) / std::max(1.f, pageArea.w - tw), 0.f, 1.f); writerScrollX_ = t * maxScrollX; }
    ui.fill(bar, (hov || held) ? ui.c.textFaint : ui.c.textFaint.withA(0.45f), 2 * s);
  }

  // ---- the floating mini toolbar (after the pages, before the pane: it lives in the page area)
  drawWriterMini(pageArea);

  // ---- side pane (scrollable)
  if (writerPane) {
    ui.fill(pane, ui.c.panel);
    ui.line(pane.x, pane.y, pane.x, pane.b(), ui.c.border, 1);
    Rect hdr{pane.x + 12 * s, pane.y, pane.w - 24 * s, 36 * s};
    int tab = writerPane - 1;
    if (ui.segmented({hdr.x, hdr.y + 6 * s, hdr.w, 26 * s}, {"Outline", "Properties", "Find"}, tab, "wpane")) writerPane = tab + 1;
    Rect content{pane.x + 12 * s, pane.y + 40 * s, pane.w - 24 * s, pane.h - 40 * s};
    if (writerPane == 1) {
      ui.beginScroll("woutline", {pane.x, content.y, pane.w, content.h});
      float sy = ui.scrollY();
      float yy = content.y - sy;
      int fig = 0, tbl = 0;
      const float rh = 26 * s;
      int rows = 0;
      for (size_t i = 0; i < wdoc.blocks.size(); i++) {
        const Block& b = wdoc.blocks[i];
        string label, icon;
        float ind = 0;
        if (b.kind == Block::Paragraph && (isHeading(b.p.style) || b.p.style == PStyle::Title)) {
          label = b.p.text();
          if (label.empty()) label = "(empty heading)";
          ind = b.p.style == PStyle::Title ? 0 : float(headingLevel(b.p.style) - 1) * 12 * s;
          icon = b.p.style == PStyle::Title ? "type" : "";
        } else if (b.kind == Block::FigureBlock) { label = b.fig.labelText() + " " + std::to_string(++fig) + ": " + (b.fig.caption.empty() ? (b.fig.asset >= 0 && size_t(b.fig.asset) < wdoc.assets.size() ? wdoc.assets[size_t(b.fig.asset)].title : string("figure")) : b.fig.caption.text()); icon = "image"; ind = 12 * s; }
        else if (b.kind == Block::TableBlock) { label = "Table " + std::to_string(++tbl) + " (" + std::to_string(b.tbl.rows()) + "\xC3\x97" + std::to_string(b.tbl.cols()) + ")"; icon = "table"; ind = 12 * s; }
        else continue;
        Rect rr{content.x, yy, content.w, rh};
        bool current = wed.caret.blk == int(i);
        if (ui.listRow(rr, "wout:" + std::to_string(i), current)) {
          DocPos q{int(i), b.kind == Block::FigureBlock ? -2 : b.kind == Block::TableBlock ? 0 : -1, 0};
          wed.setCaret(q);
          writerFollow_ = true;
          writerCaretX_ = -1;
        }
        float tx = rr.x + 8 * s + ind;
        if (!icon.empty()) { ui.icon(icon, tx + 7 * s, rr.y + rh / 2, 12 * s, ui.c.textDim, 1.5f); tx += 18 * s; }
        bool bold = b.kind == Block::Paragraph && (b.p.style == PStyle::H1 || b.p.style == PStyle::Title);
        ui.text({tx, rr.y, rr.r() - tx - 4 * s, rh}, truncate(label, 60), 12.f * s, current ? ui.c.text : ui.c.textDim, AL_LEFT, bold ? 650 : 400);
        // page number of the heading
        if (size_t(i) < L.blocks.size() && !L.blocks[i].places.empty()) ui.text({rr.r() - 34 * s, rr.y, 30 * s, rh}, std::to_string(L.blocks[i].places[0].page + 1), 10.5f * s, ui.c.textFaint, AL_RIGHT);
        yy += rh;
        rows++;
      }
      if (rows == 0) ui.textWrap({content.x, content.y + 8 * s, content.w, 80 * s}, "Headings, figures and tables appear here as you add them. Use the style box (Heading 1, 2, 3) to structure the document.", 12 * s, ui.c.textDim);
      ui.endScroll(float(rows) * rh + 12 * s);
    } else if (writerPane == 2) {
      ui.beginScroll("wprops", {pane.x, content.y, pane.w, content.h});
      float yy = content.y + 4 * s - ui.scrollY();
      const float y0 = yy;
      auto row = [&](float h) { Rect rr{content.x, yy, content.w, h}; yy += h + 6 * s; return rr; };
      auto lbl = [&](const string& t) { ui.text(row(18 * s), t, 11.5f * s, ui.c.textDim, AL_LEFT, 600); };
      auto head = [&](const string& t, const string& icon) {
        yy += 4 * s;
        Rect rr = row(22 * s);
        ui.icon(icon, rr.x + 8 * s, rr.y + rr.h / 2, 14 * s, ui.c.accent, 1.7f);
        ui.text({rr.x + 22 * s, rr.y, rr.w - 22 * s, rr.h}, t, 12.5f * s, ui.c.text, AL_LEFT, 650);
      };
      auto rule = [&]() { yy += 4 * s; ui.line(content.x, yy, content.r(), yy, ui.c.border, 1); yy += 8 * s; };
      PageSetup& S = wdoc.setup;
      bool changed = false;
      // ---- the caret's object first
      if (onFig) {
        Block& fb = wed.curBlock();
        Figure fg = fb.fig;
        head("Figure " + std::to_string(wdoc.figureNumber(wed.caret.blk)), "image");
        lbl("Width (of the text width)");
        { int sel = fg.widthPct <= 40 ? 0 : fg.widthPct <= 50 ? 1 : fg.widthPct <= 60 ? 2 : fg.widthPct <= 75 ? 3 : 4; static const int W[] = {40, 50, 60, 75, 100};
          if (ui.segmented(row(26 * s), {"40", "50", "60", "75", "100%"}, sel, "wfigw2")) { fg.widthPct = W[sel]; wed.setFigure(fg); } }
        lbl("Alignment");
        { int sel = fg.align == PAlign::Left ? 0 : fg.align == PAlign::Right ? 2 : 1; if (ui.segmented(row(26 * s), {"Left", "Centre", "Right"}, sel, "wfigal")) { fg.align = sel == 0 ? PAlign::Left : sel == 2 ? PAlign::Right : PAlign::Center; wed.setFigure(fg); } }
        lbl("Export format (Word, HTML)");
        { int sel = fg.format == "vector" ? 1 : fg.format == "png" ? 2 : 0;
          if (ui.segmented(row(26 * s), {"Document default", "Vector", "PNG"}, sel, "wfigfmt")) { fg.format = sel == 1 ? "vector" : sel == 2 ? "png" : ""; wed.setFigure(fg); } }
        ui.textWrap(row(30 * s), string("Now: ") + (fg.formatOr(S.figureFormat) == "png" ? "PNG picture at 200 dpi" : "vector (SVG in Word and HTML; native drawing in the PDF)"), 11 * s, ui.c.textDim);
        lbl("Caption");
        { int sel = fg.captionAbove ? 0 : 1; if (ui.segmented(row(26 * s), {"Above", "Below"}, sel, "wfigcap")) { fg.captionAbove = sel == 0; wed.setFigure(fg); } }
        { bool v = fg.border; if (ui.toggle(row(24 * s), "Thin border around the image", v)) { fg.border = v; wed.setFigure(fg); } }
        lbl("Caption label");
        {
          vector<string> labels = {"Figure", "Fig.", "Chart", "Map", "Diagram", "Plate", "No label / number"};
          int sel = 0;
          string cur = fg.label;
          for (size_t i = 0; i < labels.size(); i++) if (labels[i] == cur || (cur.empty() && i == 0) || (cur == "-" && i == labels.size() - 1)) sel = int(i);
          if (ui.combo(row(28 * s), "wfiglabel", labels, sel)) { fg.label = sel == 0 ? "" : sel == int(labels.size()) - 1 ? "-" : labels[size_t(sel)]; wed.setFigure(fg); }
        }
        lbl("Alternative text (Word, HTML)");
        { string a = fg.alt; if (ui.textInput(row(28 * s), "wfigalt", a, "Describes the figure for screen readers")) { fg.alt = a; wed.setFigure(fg); } }
        rule();
      }
      if (inTbl) {
        head("Table " + std::to_string(wdoc.tableNumber(wed.caret.blk)) + " \xC2\xB7 row " + std::to_string(trow + 1) + ", column " + std::to_string(tcol + 1), "table");
        { bool v = wed.curBlock().tbl.header; if (ui.toggle(row(24 * s), "Header row", v)) wed.tableToggleHeader(); }
        lbl("Column alignment");
        {
          const Table& t = wed.curBlock().tbl;
          char al = size_t(tcol) < t.align.size() ? t.align[size_t(tcol)] : 'l';
          int sel = al == 'c' ? 1 : al == 'r' ? 2 : al == 'a' ? 3 : 0;
          if (ui.segmented(row(26 * s), {"Left", "Centre", "Right", "Numbers"}, sel, "wtblal")) wed.tableSetColAlign(sel == 1 ? 'c' : sel == 2 ? 'r' : sel == 3 ? 'a' : 'l');
        }
        {
          Rect rr = row(28 * s);
          float bw = (rr.w - 6 * s) / 2;
          if (ui.button({rr.x, rr.y, bw, rr.h}, "Row below", BTN_NORMAL, "plus")) wed.tableInsertRow(true);
          if (ui.button({rr.x + bw + 6 * s, rr.y, bw, rr.h}, "Column right", BTN_NORMAL, "plus")) wed.tableInsertCol(true);
          rr = row(28 * s);
          if (ui.button({rr.x, rr.y, bw, rr.h}, "Delete row", BTN_NORMAL, "trash")) wed.tableDeleteRow();
          if (ui.button({rr.x + bw + 6 * s, rr.y, bw, rr.h}, "Delete column", BTN_NORMAL, "trash")) wed.tableDeleteCol();
        }
        rule();
      }
      if (inText) {
        head("Paragraph", "text");
        lbl("Style");
        {
          vector<string> names;
          for (int i = 0; i < kStyleCount; i++) names.push_back(pstyleName(kStyleOrder[i]));
          int sel = styleIndex(styleHere);
          if (ui.combo(row(28 * s), "wstyle2", names, sel)) wed.setStyle(kStyleOrder[sel]);
        }
        lbl("Alignment");
        { int sel = int(alignHere); if (ui.segmented(row(26 * s), {"Left", "Centre", "Right", "Justify"}, sel, "walign2")) wed.setAlign(PAlign(sel)); }
        if (isList(styleHere)) {
          lbl("List level");
          { int lv = wed.curPara() ? wed.curPara()->level : 0; if (ui.segmented(row(26 * s), {"1", "2", "3"}, lv, "wlevel")) { Para* p = wed.curPara(); if (p) { int cur = p->level; wed.indent(lv - cur); } } }
        }
        head("Text", "font");
        lbl("Font");
        {
          const vector<string>& fonts = writerFonts();
          vector<string> opts;
          opts.push_back("Document font (" + ps.font + ")");
          for (auto& f : fonts) opts.push_back(f);
          string here = wed.fontHere();
          int sel = 0;
          for (size_t i = 0; i < fonts.size(); i++) if (fonts[i] == here) sel = int(i) + 1;
          if (ui.combo(row(28 * s), "wfontfam2", opts, sel)) wed.setFont(sel == 0 ? string() : fonts[size_t(sel - 1)]);
        }
        lbl("Size (pt)");
        {
          float cur = wed.sizeHere();
          int v = int(std::lround(cur > 0 ? cur : ps.baseSize));
          Rect rr = row(28 * s);
          if (ui.numberInput({rr.x, rr.y, rr.w * 0.5f, rr.h}, "wsize2", v, 5, 96, 1)) wed.setSize(float(v));
          if (ui.button({rr.x + rr.w * 0.55f, rr.y, rr.w * 0.45f, rr.h}, "Style size", BTN_NORMAL, "", cur > 0)) wed.setSize(0);
        }
        lbl("Colour");
        {
          uint32_t here = wed.colorHere();
          const float cs = 20 * s, gp = 4 * s;
          int per = std::max(1, int((content.w + gp) / (cs + gp)));
          int rowsN = (kTextColorCount + per - 1) / per;
          Rect rr = row(float(rowsN) * (cs + gp));
          for (int i = 0; i < kTextColorCount; i++) {
            Rect cr{rr.x + float(i % per) * (cs + gp), rr.y + float(i / per) * (cs + gp), cs, cs};
            bool h2 = false;
            if (ui.behave(ui.id("wcol2:" + std::to_string(i)), cr, &h2)) wed.setColor(kTextColors[i]);
            if (kTextColors[i] == 0) { ui.fill(cr, ui.c.input, 4 * s); ui.text(cr, "A", 12 * s, ui.c.text, AL_CENTER, 600); }
            else ui.fill(cr, fromArgb(kTextColors[i]), 4 * s);
            ui.stroke(cr, here == kTextColors[i] ? ui.c.accent : (h2 ? ui.c.text : ui.c.borderStrong), 4 * s, here == kTextColors[i] ? 2 : 1);
          }
        }
        // citation at the caret
        vector<string> here = wed.citeHere();
        if (!here.empty()) {
          head("Citation", "quote");
          for (auto& key : here) {
            const RefEntry* e = wdoc.findRef(key);
            if (!e) continue;
            string t = e->structured() ? (e->authors.empty() ? string() : splitAuthor(e->authors[0]).last + (e->authors.size() > 1 ? " et al." : "") + " (" + e->year + "). ") + e->title : e->raw;
            ui.textWrap(row(34 * s), truncate(t, 120), 11.5f * s, ui.c.textDim);
          }
          Rect rr = row(28 * s);
          float bw = (rr.w - 6 * s) / 2;
          if (ui.button({rr.x, rr.y, bw, rr.h}, "Add a paper\xE2\x80\xA6", BTN_NORMAL, "plus", hasCorpus())) openCite();
          if (ui.button({rr.x + bw + 6 * s, rr.y, bw, rr.h}, "Remove citation", BTN_NORMAL, "trash")) { wed.removeCitationHere(); writerFollow_ = true; }
        }
        rule();
      }
      // ---- document
      head("Page", "pages");
      lbl("Paper");
      { int sel = S.paper == "Letter" ? 1 : 0; if (ui.segmented(row(26 * s), {"A4", "US Letter"}, sel, "wpaper")) { S.paper = sel ? "Letter" : "A4"; changed = true; } }
      lbl("Margins");
      { int sel = S.margins == "narrow" ? 0 : S.margins == "wide" ? 2 : 1; if (ui.segmented(row(26 * s), {"Narrow", "Normal", "Wide"}, sel, "wmarg")) { S.margins = sel == 0 ? "narrow" : sel == 2 ? "wide" : "normal"; changed = true; } }
      lbl("Body font");
      {
        const vector<string>& fonts = writerFonts();
        int sel = 0;
        for (size_t i = 0; i < fonts.size(); i++) if (fonts[i] == S.font) sel = int(i);
        if (ui.combo(row(28 * s), "wfont", fonts, sel)) { S.font = fonts[size_t(sel)]; changed = true; }
      }
      lbl("Heading font");
      {
        const vector<string>& fonts = writerFonts();
        vector<string> opts;
        opts.push_back("Same as the body");
        for (auto& f : fonts) opts.push_back(f);
        int sel = 0;
        for (size_t i = 0; i < fonts.size(); i++) if (fonts[i] == S.headingFont) sel = int(i) + 1;
        if (ui.combo(row(28 * s), "wheadfont", opts, sel)) { S.headingFont = sel == 0 ? string() : fonts[size_t(sel - 1)]; changed = true; }
      }
      lbl("Base size");
      { int sel = S.baseSize <= 10 ? 0 : S.baseSize >= 12 ? 2 : 1; if (ui.segmented(row(26 * s), {"10 pt", "11 pt", "12 pt"}, sel, "wsize")) { S.baseSize = sel == 0 ? 10.f : sel == 2 ? 12.f : 11.f; changed = true; } }
      lbl("Line spacing");
      { int sel = S.lineSpacing <= 1.05f ? 0 : S.lineSpacing <= 1.3f ? 1 : S.lineSpacing <= 1.7f ? 2 : 3;
        if (ui.segmented(row(26 * s), {"1.0", "1.15", "1.5", "2.0"}, sel, "wls")) { S.lineSpacing = sel == 0 ? 1.f : sel == 1 ? 1.15f : sel == 2 ? 1.5f : 2.f; changed = true; } }
      lbl("Running head (top of every page)");
      { string h = S.header; if (ui.textInput(row(28 * s), "wheader", h, "e.g. a short title")) { S.header = h; changed = true; } }
      yy += 2 * s;
      { bool v = S.numberedHeadings; if (ui.toggle(row(24 * s), "Numbered headings (1, 1.1, 1.2)", v)) { S.numberedHeadings = v; changed = true; } }
      { bool v = S.toc; if (ui.toggle(row(24 * s), "Table of contents in the exports", v)) { S.toc = v; changed = true; } }
      { bool v = S.pageNumbers; if (ui.toggle(row(24 * s), "Page numbers", v)) { S.pageNumbers = v; changed = true; } }
      lbl("Figures in Word and HTML");
      { int sel = S.figureFormat == "png" ? 1 : 0; if (ui.segmented(row(26 * s), {"Vector (SVG)", "PNG pictures"}, sel, "wfigdef")) { S.figureFormat = sel ? "png" : "vector"; changed = true; } }
      ui.textWrap(row(44 * s), "Vector figures stay sharp at any size and can be edited in Word (Word 2016 and newer; a PNG copy is included for older versions). Each figure can override this.", 11 * s, ui.c.textDim);
      head("Citations", "book");
      lbl("Citation style");
      {
        vector<string> names;
        int sel = 0;
        const auto& styles = citeStyles();
        for (size_t i = 0; i < styles.size(); i++) { names.push_back(styles[i].name); if (wdoc.citeStyle == styles[i].id) sel = int(i); }
        if (ui.combo(row(28 * s), "wcitestyle2", names, sel)) { wed.setCiteStyle(styles[size_t(sel)].id); writerFollow_ = true; }
      }
      ui.textWrap(row(58 * s), plural(long(wdoc.refs.size()), "entry") + " in the bibliography. In-text citations and the reference list are generated in this style; hand edits of a generated entry are kept. In the Word export they are native citations (References > Style there restyles them).", 11 * s, ui.c.textDim);
      head("Document", "file");
      lbl("Author");
      { string a = wdoc.author; if (ui.textInput(row(28 * s), "wauthor", a, "Name(s) for the file properties")) { wdoc.author = a; changed = true; } }
      lbl("Keywords");
      { string kw = wdoc.keywords; if (ui.textInput(row(28 * s), "wkeyw", kw, "Comma-separated")) { wdoc.keywords = kw; changed = true; } }
      if (changed) { wed.externalChange(); wlay->key = 0; }
      rule();
      lbl("Statistics");
      int refs = 0;
      for (auto& b : wdoc.blocks) refs += b.kind == Block::Paragraph && b.p.style == PStyle::Reference;
      string st = plural(wdoc.words(), "word") + " \xC2\xB7 " + plural(writerPages_, "page") + "\n" + plural(long(wdoc.blocks.size()), "block") + ", " + plural(int(wdoc.outline().size()), "heading") + "\n" +
                  plural(wdoc.figures(), "figure") + ", " + plural(wdoc.tables(), "table") + ", " + plural(refs, "reference");
      ui.textWrap(row(60 * s), st, 12 * s, ui.c.textDim);
      ui.endScroll(yy - y0 + 20 * s);
    } else if (writerPane == 3) {
      float yy = content.y + 4 * s;
      auto row = [&](float h) { Rect rr{content.x, yy, content.w, h}; yy += h + 6 * s; return rr; };
      bool sub = false;
      ui.text(row(18 * s), "Find", 11.5f * s, ui.c.textDim, AL_LEFT, 600);
      if (ui.textInput(row(28 * s), "wfind", writerFind, "Text to find", &sub, "search")) {}
      if (sub && !writerFind.empty()) { wed.find(writerFind, !ui.in.shift, writerMatchCase); writerFollow_ = true; }
      ui.text(row(18 * s), "Replace with", 11.5f * s, ui.c.textDim, AL_LEFT, 600);
      bool sub2 = false;
      ui.textInput(row(28 * s), "wrepl", writerReplace, "Replacement", &sub2);
      if (sub2 && !writerFind.empty()) { wed.replaceCurrent(writerFind, writerReplace, writerMatchCase); writerFollow_ = true; }
      { bool v = writerMatchCase; if (ui.toggle(row(24 * s), "Match case", v)) writerMatchCase = v; }
      {
        Rect rr = row(28 * s);
        float bw = (rr.w - 6 * s) / 2;
        if (ui.button({rr.x, rr.y, bw, rr.h}, "Previous", BTN_NORMAL, "chev-up", !writerFind.empty())) { wed.find(writerFind, false, writerMatchCase); writerFollow_ = true; }
        if (ui.button({rr.x + bw + 6 * s, rr.y, bw, rr.h}, "Next  (F3)", BTN_NORMAL, "chev-down", !writerFind.empty())) { wed.find(writerFind, true, writerMatchCase); writerFollow_ = true; }
      }
      {
        Rect rr = row(28 * s);
        float bw = (rr.w - 6 * s) / 2;
        if (ui.button({rr.x, rr.y, bw, rr.h}, "Replace", BTN_NORMAL, "", !writerFind.empty())) { wed.replaceCurrent(writerFind, writerReplace, writerMatchCase); writerFollow_ = true; }
        if (ui.button({rr.x + bw + 6 * s, rr.y, bw, rr.h}, "Replace all", BTN_NORMAL, "", !writerFind.empty())) {
          int n = wed.replaceAll(writerFind, writerReplace, writerMatchCase);
          ui.toast("Replace", plural(n, "occurrence") + " replaced.", n ? 1 : 2, 3);
          writerFollow_ = true;
        }
      }
      yy += 6 * s;
      ui.textWrap({content.x, yy, content.w, 120 * s}, "Enter in the Find box jumps to the next match (Shift+Enter: previous). Enter in the Replace box replaces the current match and moves on. The search covers headings, paragraphs, table cells and captions.", 11.5f * s, ui.c.textDim);
    }
  }

  if (writerPdfPreviewOpen_) writerDrawPdfPreview(previewArea);

  // ---- status bar
  ui.fill(status, ui.c.panel);
  ui.line(status.x, status.y, status.r(), status.y, ui.c.border, 1);
  {
    string where = pstyleName(styleHere);
    if (inTbl) where = "Table, row " + std::to_string(trow + 1) + ", column " + std::to_string(tcol + 1);
    else if (wed.blockSelected()) { Block::Kind kd = wed.curBlock().kind; where = kd == Block::FigureBlock ? "Figure" : kd == Block::Rule ? "Rule" : kd == Block::PageBreak ? "Page break" : kd == Block::Toc ? "Table of contents" : "Block"; }
    if (hasSel) {
      static DocPos selA0, selB0; static uint64_t selVer0 = ~0ull; static int wds = 0;
      if (!(wed.caret == selA0) || !(wed.anchor == selB0) || selVer0 != wed.version) {  // counted again only when the selection moved
        selA0 = wed.caret; selB0 = wed.anchor; selVer0 = wed.version;
        string st = wed.selectionText();
        wds = 0;
        for (auto& w : splitAny(st, " \n\t")) wds += !w.empty();
      }
      where += " \xC2\xB7 " + plural(wds, "word") + " selected";
    }
    string left = "Page " + std::to_string(caretPage + 1) + " of " + std::to_string(L.pages) + " \xC2\xB7 " + where;
    const float leftX = status.x + 14 * s;
    const float summaryX = status.x + status.w * 0.5f;
    ui.text({leftX, status.y, std::max(0.f, summaryX - leftX - 8 * s), status.h}, left, 11.5f * s, ui.c.textDim);
    // zoom control at the right, as in Word: [-] slider [+] 100 %  (the percentage resets to 100 %)
    const float zw = 110 * s;
    float zx = status.r() - 14 * s - 44 * s;
    {
      char zb[16];
      snprintf(zb, sizeof zb, "%d%%", int(std::lround(writerZoom * 100)));
      Rect zr{zx, status.y, 44 * s, status.h};
      uint64_t zid = ui.id("wzoompct");
      bool zh = false;
      if (ui.behave(zid, zr, &zh)) zoomTo(1);
      ui.text(zr, zb, 11.5f * s, zh ? ui.c.text : ui.c.textDim, AL_CENTER, 500);
      ui.tipFor(zid, "Zoom \xE2\x80\x94 click for 100 %. Ctrl+wheel zooms around the mouse, Ctrl+plus / Ctrl+minus step.");
    }
    zx -= 22 * s;
    if (ui.iconButton({zx, status.y + 1 * s, 22 * s, status.h - 2 * s}, "plus", "Zoom in  (Ctrl +)")) zoomTo(writerZoom * 1.1f);
    zx -= zw + 4 * s;
    {  // the slider maps 40 %..300 % on a log scale, with a notch at 100 %
      Rect sr{zx, status.y, zw, status.h};
      uint64_t sid = ui.id("wzoomsl");
      bool sh = false, sheld = false;
      ui.behave(sid, sr, &sh, &sheld);
      const float lo = std::log(0.4f), hi = std::log(3.f);
      float t = (std::log(clampv(writerZoom, 0.4f, 3.f)) - lo) / (hi - lo);
      float tx = sr.x + 6 * s, tw = sr.w - 12 * s;
      if (sheld) { t = clampv((ui.in.mx - tx) / tw, 0.f, 1.f); float z = std::exp(lo + t * (hi - lo)); if (std::fabs(z - 1) < 0.03f) z = 1; zoomTo(z); }
      float cy = sr.y + sr.h / 2;
      ui.line(tx, cy, tx + tw, cy, ui.c.border, 2 * s);
      float nx = tx + tw * (std::log(1.f) - lo) / (hi - lo);
      ui.line(nx, cy - 4 * s, nx, cy + 4 * s, ui.c.textDim, 1 * s);
      float kx = tx + tw * t;
      ui.fill({kx - 4 * s, cy - 6 * s, 8 * s, 12 * s}, sh || sheld ? ui.c.accent : ui.c.textDim, 2 * s);
      ui.tipFor(sid, "Zoom slider (40 % to 300 %)");
    }
    zx -= 22 * s;
    if (ui.iconButton({zx, status.y + 1 * s, 22 * s, status.h - 2 * s}, "minus", "Zoom out  (Ctrl -)")) zoomTo(writerZoom / 1.1f);
    zx -= 24 * s;
    if (ui.iconButton({zx, status.y + 1 * s, 22 * s, status.h - 2 * s}, "fit", "Fit the page width")) { float fz = writerFitZoom(); if (fz > 0) zoomTo(fz); }
    // document summary: computed again only when the document or its saved state changed
    if (writerStatusVer_ != wed.version || writerStatusDirty_ != writerDirty() || writerStatusRight_.empty()) {
      writerStatusVer_ = wed.version;
      writerStatusDirty_ = writerDirty();
      string title = wdoc.title();
      writerStatusRight_ = (title.empty() ? string("Untitled document") : truncate(title, 40)) + " \xC2\xB7 " + plural(wdoc.words(), "word") + " \xC2\xB7 " + plural(wdoc.figures(), "figure") + " \xC2\xB7 " + (writerStatusDirty_ ? "edited" : "saved with the project");
    }
    float rx0 = summaryX;
    float summaryW = std::max(0.f, zx - 16 * s - rx0);
    ui.text({rx0, status.y, summaryW, status.h}, writerStatusRight_, 11.5f * s, ui.c.textDim, AL_RIGHT);
  }

  writerPopupPrev_ = ui.anyPopup();
  // ---- popups: link, table size, chart, citation picker, new document, context menu
  auto dialogRect = [this, pageArea, s](float w, float h) {  // by value: the overlays run after this function returns
    Rect pr{pageArea.x + std::max(20 * s, (pageArea.w - w) / 2), pageArea.y + 40 * s, w, h};
    pr.y = std::min(pr.y, float(g.H) - pr.h - 20 * s);
    return pr;
  };
  if (ui.isPopupOpen("wequation")) {
    ui.overlay([this, s, dialogRect]() {
      Rect pr = dialogRect(540 * s, 426 * s);
      ui.shadow(pr, 10 * s);
      ui.fill(pr, ui.c.panel2, 10 * s);
      ui.stroke(pr, ui.c.border, 10 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 16 * s, pr.y + 10 * s, pr.w - 32 * s, 24 * s}, "Equation", 14 * s, ui.c.text, AL_LEFT, 600);
      ui.text({pr.x + 16 * s, pr.y + 36 * s, pr.w - 32 * s, 30 * s}, "Write LaTeX math; apply to insert or replace the equation at the caret.", 11.5f * s, ui.c.textDim);
      Rect edit{pr.x + 16 * s, pr.y + 70 * s, pr.w - 32 * s, 156 * s};
      ui.textArea(edit, "wequation", writerEquationBuf_, "\\frac{a}{b}  or  x^2 + y^2 = z^2", nullptr, nullptr, 13 * s);
      if (writerEquationFocus_) { ui.focusText(ui.id("ta:wequation"), writerEquationBuf_); writerEquationFocus_ = false; }
      string parseError;
      mathToOmml(writerEquationBuf_, &parseError);
      string preview = writerEquationBuf_.empty() ? "Type an equation to see a readable preview." : mathPreviewText(writerEquationBuf_);
      Rect pv{pr.x + 16 * s, pr.y + 238 * s, pr.w - 32 * s, 78 * s};
      ui.text({pv.x, pv.y - 18 * s, pv.w, 16 * s}, "Preview", 10.5f * s, ui.c.textDim);
      ui.fill(pv, ui.c.input, 8 * s);
      ui.stroke(pv, ui.c.border, 8 * s);
      ui.textWrap({pv.x + 12 * s, pv.y + 8 * s, pv.w - 24 * s, pv.h - 16 * s}, preview, 15 * s, writerEquationBuf_.empty() ? ui.c.textFaint : ui.c.text);
      if (!parseError.empty()) {
        string note = "Word's native-math converter will keep this source as text: " + parseError;
        ui.textWrap({pr.x + 16 * s, pr.y + 320 * s, pr.w - 32 * s, 36 * s}, note, 10.5f * s, ui.c.warn);
      } else {
        ui.text({pr.x + 16 * s, pr.y + 322 * s, pr.w - 32 * s, 22 * s}, "DOCX export uses native Word math. Compile PDF for a full LaTeX rendering.", 10.5f * s, ui.c.textDim);
      }
      Rect br{pr.x + 16 * s, pr.y + 378 * s, pr.w - 32 * s, 32 * s};
      float bw = (br.w - 12 * s) / 3;
      if (ui.button({br.x, br.y, bw, br.h}, "Apply", BTN_PRIMARY, "check", !trim(writerEquationBuf_).empty())) {
        wed.insertMath(writerEquationBuf_);
        ui.closePopup(); ui.focus = 0; writerFollow_ = true; writerCaretX_ = -1;
      }
      if (ui.button({br.x + bw + 6 * s, br.y, bw, br.h}, "PDF preview", BTN_NORMAL, "eye")) {
        writerSetPdfPreview(true);
        if (writerPdfPath_.empty() || writerPdfCompiledVersion_ != wed.version) writerCompileLatex();
      }
      if (ui.button({br.x + 2 * (bw + 6 * s), br.y, bw, br.h}, "Cancel", BTN_NORMAL)) { ui.closePopup(); ui.focus = 0; }
    });
  }
  if (ui.isPopupOpen("wlink")) {
    ui.overlay([this, s, dialogRect]() {
      Rect pr = dialogRect(380 * s, 118 * s);
      ui.shadow(pr, 10 * s);
      ui.fill(pr, ui.c.panel2, 10 * s);
      ui.stroke(pr, ui.c.border, 10 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 14 * s, pr.y + 8 * s, pr.w - 28 * s, 22 * s}, wed.hasSelection() ? "Link the selected text to" : "Link (the word at the caret, or the address itself)", 12.5f * s, ui.c.text, AL_LEFT, 600);
      bool sub = false;
      Rect ir{pr.x + 14 * s, pr.y + 36 * s, pr.w - 28 * s, 30 * s};
      ui.textInput(ir, "wlinkurl", writerLinkBuf, "https://doi.org/10.1000/xyz", &sub, "link");
      if (writerLinkFocus_) { ui.focusText(ui.id("ti:wlinkurl"), writerLinkBuf); writerLinkFocus_ = false; }
      Rect br{pr.x + 14 * s, pr.y + 76 * s, pr.w - 28 * s, 30 * s};
      float bw = (br.w - 12 * s) / 3;
      bool apply = ui.button({br.x, br.y, bw, br.h}, "Apply", BTN_PRIMARY) || sub;
      bool remove = ui.button({br.x + bw + 6 * s, br.y, bw, br.h}, "Remove link", BTN_NORMAL, "", !wed.linkHere().empty());
      bool cancel = ui.button({br.x + 2 * (bw + 6 * s), br.y, bw, br.h}, "Cancel", BTN_NORMAL);
      if (apply) { string u = trim(writerLinkBuf); if (!u.empty() && u.find("://") == string::npos && !startsWith(u, "mailto:")) u = startsWith(lower(u), "10.") ? "https://doi.org/" + u : "https://" + u; wed.setLink(u); ui.closePopup(); ui.focus = 0; }
      if (remove) { wed.setLink(""); ui.closePopup(); ui.focus = 0; }
      if (cancel) { ui.closePopup(); ui.focus = 0; }
    });
  }
  if (ui.isPopupOpen("wtbl")) {
    ui.overlay([this, s, dialogRect]() {
      Rect pr = dialogRect(300 * s, 120 * s);
      ui.shadow(pr, 10 * s);
      ui.fill(pr, ui.c.panel2, 10 * s);
      ui.stroke(pr, ui.c.border, 10 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 14 * s, pr.y + 8 * s, pr.w - 28 * s, 22 * s}, "Insert a table", 12.5f * s, ui.c.text, AL_LEFT, 600);
      ui.text({pr.x + 14 * s, pr.y + 38 * s, 50 * s, 28 * s}, "Rows", 12 * s, ui.c.textDim);
      ui.numberInput({pr.x + 60 * s, pr.y + 38 * s, 80 * s, 28 * s}, "wtblr", writerTblRows, 1, 50, 1);
      ui.text({pr.x + 154 * s, pr.y + 38 * s, 60 * s, 28 * s}, "Columns", 12 * s, ui.c.textDim);
      ui.numberInput({pr.x + 214 * s, pr.y + 38 * s, 72 * s, 28 * s}, "wtblc", writerTblCols, 1, 12, 1);
      Rect br{pr.x + 14 * s, pr.y + 78 * s, pr.w - 28 * s, 30 * s};
      float bw = (br.w - 6 * s) / 2;
      if (ui.button({br.x, br.y, bw, br.h}, "Insert", BTN_PRIMARY)) { wed.insertTable(writerTblRows, writerTblCols); ui.closePopup(); writerFollow_ = true; }
      if (ui.button({br.x + bw + 6 * s, br.y, bw, br.h}, "Cancel", BTN_NORMAL)) ui.closePopup();
    });
  }
  if (ui.isPopupOpen("wnew")) {
    ui.overlay([this, s, dialogRect]() {
      Rect pr = dialogRect(360 * s, 122 * s);
      ui.shadow(pr, 10 * s);
      ui.fill(pr, ui.c.panel2, 10 * s);
      ui.stroke(pr, ui.c.border, 10 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 14 * s, pr.y + 8 * s, pr.w - 28 * s, 22 * s}, "Start a new document?", 12.5f * s, ui.c.text, AL_LEFT, 600);
      ui.textWrap({pr.x + 14 * s, pr.y + 32 * s, pr.w - 28 * s, 40 * s}, "The current text, figures and bibliography are cleared (Undo brings them back). Export first if you want to keep a file.", 11.5f * s, ui.c.textDim);
      Rect br{pr.x + 14 * s, pr.y + 80 * s, pr.w - 28 * s, 30 * s};
      float bw = (br.w - 6 * s) / 2;
      if (ui.button({br.x, br.y, bw, br.h}, "New document", BTN_PRIMARY, "newdoc")) { ui.closePopup(); writerNewDocument(); }
      if (ui.button({br.x + bw + 6 * s, br.y, bw, br.h}, "Cancel", BTN_NORMAL)) ui.closePopup();
    });
  }
  if (ui.isPopupOpen("wchart")) {
    ui.overlay([this, s, dialogRect]() {
      const auto& kCharts = chartChoices();
      const int n = int(kCharts.size());
      Rect pr = dialogRect(300 * s, float(n) * 26 * s + 44 * s);
      ui.shadow(pr, 10 * s);
      ui.fill(pr, ui.c.panel2, 10 * s);
      ui.stroke(pr, ui.c.border, 10 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 14 * s, pr.y + 8 * s, pr.w - 28 * s, 22 * s}, "Insert a chart of the loaded data", 12.5f * s, ui.c.text, AL_LEFT, 600);
      for (int i = 0; i < n; i++) {
        Rect rr{pr.x + 6 * s, pr.y + 36 * s + float(i) * 26 * s, pr.w - 12 * s, 25 * s};
        bool en = !kCharts[i].needsMap || hasMap();
        if (en && ui.listRow(rr, string("wch:") + kCharts[i].id, false)) { ui.closePopup(); writerInsertChart(kCharts[i].id); }
        ui.text({rr.x + 10 * s, rr.y, rr.w - 14 * s, rr.h}, kCharts[i].label, 12.5f * s, en ? ui.c.text : ui.c.textFaint);
      }
    });
  }
  if (ui.isPopupOpen("wcite")) {
    ui.overlay([this, s, dialogRect]() {
      // every paper of the corpus: searchable, scrollable, multi-select; the ones behind the current selection / preview come first
      const size_t N = hasCorpus() ? P->corpus.recs.size() : 0;
      if (writerCiteSel.size() != N) writerCiteSel.assign(N, 0);
      // the filtered, ordered list is rebuilt only when the query or the selection context changes (not per frame:
      // matching folds every record's text, and a corpus can have thousands)
      size_t nPs = 0;
      for (size_t i = 0; i < papersSel.size() && i < N; i++) nPs += papersSel[i] != 0;
      string okey = writerCiteQuery + "|" + std::to_string(N) + "|" + std::to_string(docPreview) + "|" + std::to_string(papersCursor) + "|" + std::to_string(nPs) + "|" + std::to_string(reinterpret_cast<uintptr_t>(P.get()));
      if (okey != writerCiteOrderKey_) {
        writerCiteOrderKey_ = okey;
        vector<string> words;
        for (auto& w : splitAny(lower(asciiFold(writerCiteQuery)), " ,;")) if (!w.empty()) words.push_back(w);
        vector<int>& order = writerCiteOrder_;
        order.clear();
        order.reserve(N);
        vector<char> seen(N, 0);
        auto push = [&](int i) { if (i >= 0 && size_t(i) < N && !seen[size_t(i)] && recMatches(P->corpus.recs[size_t(i)], words)) { seen[size_t(i)] = 1; order.push_back(i); } };
        push(docPreview);
        push(papersCursor);
        for (size_t i = 0; i < papersSel.size() && i < N; i++) if (papersSel[i]) push(int(i));
        vector<int> rest;
        for (size_t i = 0; i < N; i++) if (!seen[i]) rest.push_back(int(i));
        std::stable_sort(rest.begin(), rest.end(), [&](int a, int b) { return P->corpus.recs[size_t(a)].cites > P->corpus.recs[size_t(b)].cites; });
        for (int i : rest) push(i);
      }
      const vector<int>& order = writerCiteOrder_;
      int nSel = 0;
      for (char c : writerCiteSel) nSel += c != 0;
      Rect pr = dialogRect(std::min(560 * s, float(g.W) - 40 * s), std::min(float(g.H) - 80 * s, 520 * s));
      ui.shadow(pr, 10 * s);
      ui.fill(pr, ui.c.panel2, 10 * s);
      ui.stroke(pr, ui.c.border, 10 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 14 * s, pr.y + 8 * s, pr.w - 28 * s, 22 * s}, "Cite papers", 13 * s, ui.c.text, AL_LEFT, 600);
      ui.text({pr.x + 14 * s, pr.y + 28 * s, pr.w - 28 * s, 16 * s}, N ? plural(long(N), "paper") + " in the loaded data. Click a paper to cite it, or tick several and insert one citation (Smith, 2020; Doe, 2021)." : "Load records first.", 11 * s, ui.c.textDim);
      bool sub = false;
      Rect sr{pr.x + 14 * s, pr.y + 50 * s, pr.w - 28 * s, 30 * s};
      ui.textInput(sr, "wciteq", writerCiteQuery, "Search title, authors, source, year, DOI", &sub, "search");
      if (writerCiteFocus_) { ui.focusText(ui.id("ti:wciteq"), writerCiteQuery); writerCiteFocus_ = false; }
      Rect list{pr.x + 8 * s, pr.y + 88 * s, pr.w - 16 * s, pr.h - 88 * s - 46 * s};
      const float rh = 42 * s;
      ui.beginScroll("wcitelist", list);
      float yy = list.y - ui.scrollY();
      int shownRows = 0;
      for (int idx : order) {
        if (yy + rh >= list.y && yy <= list.b()) {
          const Record& rec = P->corpus.recs[size_t(idx)];
          Rect rr{list.x, yy, list.w, rh - 2 * s};
          bool selRow = writerCiteSel[size_t(idx)] != 0;
          Rect cb{rr.x + 8 * s, rr.y + rr.h / 2 - 8 * s, 16 * s, 16 * s};
          bool cbHov = false;
          bool cbClick = ui.behave(ui.id("wcitecb:" + std::to_string(idx)), cb, &cbHov);
          if (cbClick) writerCiteSel[size_t(idx)] = selRow ? 0 : 1;
          else if (ui.listRow(rr, "wcite:" + std::to_string(idx), selRow)) { ui.closePopup(); ui.focus = 0; writerCiteMany({idx}); }
          ui.fill(cb, selRow ? ui.c.accent : ui.c.input, 3 * s);
          ui.stroke(cb, selRow ? ui.c.accent : (cbHov ? ui.c.text : ui.c.borderStrong), 3 * s);
          if (selRow) ui.icon("check", cb.x + cb.w / 2, cb.y + cb.h / 2, 11 * s, ui.c.accentText, 2.2f);
          ui.text({rr.x + 32 * s, rr.y + 3 * s, rr.w - 40 * s, 18 * s}, truncate(rec.title, 90), 12 * s, ui.c.text, AL_LEFT, 500);
          ui.text({rr.x + 32 * s, rr.y + 21 * s, rr.w - 40 * s, 16 * s}, recLine(rec) + " \xC2\xB7 " + truncate(rec.source, 40) + " \xC2\xB7 " + plural(long(rec.cites), "citation") + (wdoc.findRef(refKeyFor(rec.doi, rec.title, rec.year ? std::to_string(rec.year) : string())) ? " \xC2\xB7 in the bibliography" : ""), 11 * s, ui.c.textDim);
        }
        yy += rh;
        shownRows++;
      }
      if (order.empty()) ui.text({list.x + 8 * s, list.y + 8 * s, list.w, 20 * s}, N ? "No paper matches the search." : "", 12 * s, ui.c.textDim);
      ui.endScroll(float(shownRows) * rh + 4 * s);
      Rect br{pr.x + 14 * s, pr.b() - 40 * s, pr.w - 28 * s, 30 * s};
      float bw = 150 * s;
      bool insert = ui.button({br.x, br.y, bw + 40 * s, br.h}, nSel ? "Insert citation (" + std::to_string(nSel) + ")" : "Insert the first match", BTN_PRIMARY, "quote", nSel > 0 || !order.empty()) || sub;
      if (nSel && ui.button({br.x + bw + 46 * s, br.y, 100 * s, br.h}, "Clear ticks", BTN_NORMAL)) writerCiteSel.assign(N, 0);
      if (ui.button({br.r() - 90 * s, br.y, 90 * s, br.h}, "Cancel", BTN_NORMAL)) { ui.closePopup(); ui.focus = 0; }
      if (insert) {
        vector<int> recs;
        for (size_t i = 0; i < N; i++) if (writerCiteSel[i]) recs.push_back(int(i));
        if (recs.empty() && !order.empty()) recs.push_back(order[0]);
        ui.closePopup();
        ui.focus = 0;
        if (!recs.empty()) writerCiteMany(recs);
      }
    });
  }
  if (ui.isPopupOpen("wctx")) {
    ui.overlay([this, s, openCite]() {
      struct It { string label; string key; bool enabled; bool danger; bool sub; };
      vector<It> items;
      bool sel = wed.hasSelection() || wed.blockSelected();
      int trow2 = 0, tcol2 = 0;
      bool inTbl2 = wed.inTable(&trow2, &tcol2);
      bool fig = wed.blockSelected() && wed.curBlock().kind == Block::FigureBlock;
      bool text = docPara(wdoc, wed.caret) != nullptr;
      const string& sub = writerCtxSub_;
      auto add = [&](const string& label, const string& key, bool en = true, bool danger = false, bool hasSub = false) { items.push_back({label, key, en, danger, hasSub}); };
      auto sepI = [&]() { items.push_back({"-", "", false, false, false}); };
      if (sub.empty()) {
        add("Cut", "cut", sel); add("Copy", "copy", sel); add("Paste into Word at the cursor", "toword", sel); add("Paste", "paste"); add("Paste as plain text", "pasteplain");
        sepI();
        if (text) {
          add("Paragraph style", "sub:style", true, false, true);
          add("Font, size and colour", "sub:font", true, false, true);
          add("Link\xE2\x80\xA6", "link");
          if (!wed.linkHere().empty()) { add("Open link", "openlink"); add("Remove link", "unlink"); }
          if (!wed.citeHere().empty()) { add("Add a paper to this citation\xE2\x80\xA6", "cite", hasCorpus()); add("Remove citation", "uncite"); }
          else add("Insert citation\xE2\x80\xA6", "cite", hasCorpus());
          sepI();
        }
        add("Insert", "sub:insert", true, false, true);
        if (inTbl2) add("Table", "sub:table", true, false, true);
        if (fig) add("Figure", "sub:figure", true, false, true);
        sepI();
        add("Select word", "selword", text); add("Select paragraph", "selpara", text); add("Select all", "selall");
        sepI();
        add("Move block up", "up", wed.caret.blk > 0); add("Move block down", "down", wed.caret.blk + 1 < int(wdoc.blocks.size()));
        add(fig ? "Delete figure" : inTbl2 ? "Delete table" : "Delete block", "delblk", true, true);
      } else {
        add("\xE2\x80\xB9 Back", "back");
        sepI();
        if (sub == "style") { for (int i = 0; i < kStyleCount; i++) add(string(wed.styleHere() == kStyleOrder[i] ? "\xE2\x9C\x93 " : "   ") + pstyleName(kStyleOrder[i]), "style:" + std::to_string(i)); }
        else if (sub == "font") {
          add(string(wed.fmtHere() & F_BOLD ? "\xE2\x9C\x93 " : "   ") + "Bold", "fmt:b"); add(string(wed.fmtHere() & F_ITALIC ? "\xE2\x9C\x93 " : "   ") + "Italic", "fmt:i");
          add(string(wed.fmtHere() & F_UNDER ? "\xE2\x9C\x93 " : "   ") + "Underline", "fmt:u"); add(string(wed.fmtHere() & F_STRIKE ? "\xE2\x9C\x93 " : "   ") + "Strikethrough", "fmt:s");
          add(string(wed.fmtHere() & F_SUP ? "\xE2\x9C\x93 " : "   ") + "Superscript", "fmt:sup"); add(string(wed.fmtHere() & F_SUB ? "\xE2\x9C\x93 " : "   ") + "Subscript", "fmt:sub");
          add(string(wed.fmtHere() & F_CODE ? "\xE2\x9C\x93 " : "   ") + "Code", "fmt:code"); add(string(wed.fmtHere() & F_MARK ? "\xE2\x9C\x93 " : "   ") + "Highlight", "fmt:mark");
          sepI();
          add("   Larger  (Ctrl+Shift+>)", "size:+"); add("   Smaller  (Ctrl+Shift+<)", "size:-"); add("   Style size", "size:0", wed.sizeHere() > 0);
          sepI();
          add("   Clear formatting", "clear");
          add("   More in the Properties pane\xE2\x80\xA6", "props");
        } else if (sub == "insert") {
          add("Equation\xE2\x80\xA6", "ins:equation"); add("Table\xE2\x80\xA6", "ins:table"); add("Chart of the loaded data\xE2\x80\xA6", "ins:chart", hasCorpus()); add("The current map view", "ins:map", hasMap());
          add("Citation\xE2\x80\xA6", "cite", hasCorpus()); add("Horizontal rule", "ins:rule"); add("Page break", "ins:pb"); add("Table of contents", "ins:toc");
        } else if (sub == "table") {
          add("Insert row above", "rowa"); add("Insert row below", "rowb"); add("Insert column left", "coll"); add("Insert column right", "colr");
          sepI(); add("Delete row", "delrow"); add("Delete column", "delcol"); add("Header row on / off", "hdr");
          sepI(); add("Column: align left", "cal:l"); add("Column: centre", "cal:c"); add("Column: align right", "cal:r"); add("Column: numbers right", "cal:a");
          sepI(); add("Delete table", "deltbl", true, true);
        } else if (sub == "figure") {
          const Figure& fg = wed.curBlock().fig;
          add(string(fg.widthPct == 50 ? "\xE2\x9C\x93 " : "   ") + "Width 50%", "w50"); add(string(fg.widthPct == 75 ? "\xE2\x9C\x93 " : "   ") + "Width 75%", "w75"); add(string(fg.widthPct == 100 ? "\xE2\x9C\x93 " : "   ") + "Width 100%", "w100");
          sepI();
          add(string(fg.align == PAlign::Left ? "\xE2\x9C\x93 " : "   ") + "Align left", "fal:l"); add(string(fg.align == PAlign::Center ? "\xE2\x9C\x93 " : "   ") + "Centre", "fal:c"); add(string(fg.align == PAlign::Right ? "\xE2\x9C\x93 " : "   ") + "Align right", "fal:r");
          sepI();
          add(string(fg.captionAbove ? "\xE2\x9C\x93 " : "   ") + "Caption above", "fcap:a"); add(string(!fg.captionAbove ? "\xE2\x9C\x93 " : "   ") + "Caption below", "fcap:b");
          add(string(fg.border ? "\xE2\x9C\x93 " : "   ") + "Border", "fborder");
          sepI();
          add(string(fg.format.empty() ? "\xE2\x9C\x93 " : "   ") + "Export: document default", "ffmt:"); add(string(fg.format == "vector" ? "\xE2\x9C\x93 " : "   ") + "Export: vector (SVG)", "ffmt:vector"); add(string(fg.format == "png" ? "\xE2\x9C\x93 " : "   ") + "Export: PNG picture", "ffmt:png");
          sepI();
          add("   All figure properties\xE2\x80\xA6", "props");
        }
      }
      const bool mini = false;  // the floating mini toolbar sits above the menu instead (drawWriterMini)
      float hgt = 6 * s;
      for (auto& it : items) hgt += it.label[0] == '-' ? 7 * s : 27 * s;
      float wdt = 236 * s;
      Rect pr{writerCtxX_, writerCtxY_, wdt, hgt};
      if (pr.r() > float(g.W) - 8 * s) pr.x = float(g.W) - 8 * s - pr.w;
      if (pr.b() > float(g.H) - 8 * s) pr.y = std::max(8 * s, float(g.H) - 8 * s - pr.h);
      ui.shadow(pr, 8 * s);
      ui.fill(pr, ui.c.panel2, 8 * s);
      ui.stroke(pr, ui.c.border, 8 * s);
      ui.popupRect(pr);
      writerCtxR_ = pr;
      float yy = pr.y + 3 * s;
      if (mini) {
        float x = pr.x + 6 * s;
        uint16_t fm = wed.fmtHere();
        auto mb = [&](const char* icon, uint16_t f, const char* tip) { Rect b{x, yy + 3 * s, 28 * s, 28 * s}; x += 29 * s; if (ui.iconButton(b, icon, tip, fm & f)) wed.toggleFmt(f); };
        mb("bold", F_BOLD, "Bold"); mb("italic", F_ITALIC, "Italic"); mb("underline", F_UNDER, "Underline"); mb("mark", F_MARK, "Highlight");
        x += 4 * s;
        if (ui.iconButton({x, yy + 3 * s, 28 * s, 28 * s}, "minus", "Smaller")) { float c = wed.sizeHere() > 0 ? wed.sizeHere() : wdoc.setup.baseSize; int idx = 0; for (int i = 0; i < kSizeCount; i++) if (kSizes[i] < c - 0.01f) idx = i; wed.setSize(kSizes[idx]); }
        x += 29 * s;
        if (ui.iconButton({x, yy + 3 * s, 28 * s, 28 * s}, "plus", "Larger")) { float c = wed.sizeHere() > 0 ? wed.sizeHere() : wdoc.setup.baseSize; int idx = kSizeCount - 1; for (int i = kSizeCount - 1; i >= 0; i--) if (kSizes[i] > c + 0.01f) idx = i; wed.setSize(kSizes[idx]); }
        x += 29 * s;
        if (ui.iconButton({x, yy + 3 * s, 28 * s, 28 * s}, "clearfmt", "Clear formatting")) wed.clearFormatting();
        yy += 36 * s;
        ui.line(pr.x + 8 * s, yy, pr.r() - 8 * s, yy, ui.c.border, 1);
        yy += 2 * s;
      }
      for (auto& it : items) {
        if (it.label[0] == '-') { ui.line(pr.x + 8 * s, yy + 3 * s, pr.r() - 8 * s, yy + 3 * s, ui.c.border, 1); yy += 7 * s; continue; }
        Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, 26 * s};
        bool click = it.enabled && ui.listRow(rr, string("wctx:") + it.key, false);
        ui.text({rr.x + 10 * s, rr.y, rr.w - 14 * s, rr.h}, it.label, 12.5f * s, !it.enabled ? ui.c.textFaint : it.danger ? ui.c.danger : ui.c.text);
        if (it.sub) ui.icon("chev-right", rr.r() - 12 * s, rr.y + rr.h / 2, 11 * s, ui.c.textDim, 2.f);
        yy += 27 * s;
        if (!click) continue;
        string key = it.key;
        if (startsWith(key, "sub:")) { writerCtxSub_ = key.substr(4); return; }
        if (key == "back") { writerCtxSub_.clear(); return; }
        ui.closePopup();
        if (key == "cut") writerCopy(true);
        else if (key == "copy") writerCopy(false);
        else if (key == "toword") writerCopy(false, true);
        else if (key == "paste") writerPaste(false);
        else if (key == "pasteplain") writerPaste(true);
        else if (key == "link") { writerLinkBuf = wed.linkHere(); writerLinkFocus_ = true; ui.openPopup("wlink"); }
        else if (key == "openlink") openUrl(wed.linkHere());
        else if (key == "unlink") wed.setLink("");
        else if (key == "cite") openCite();
        else if (key == "uncite") wed.removeCitationHere();
        else if (key == "selword") wed.selectWord();
        else if (key == "selpara") wed.selectParagraph();
        else if (key == "selall") wed.selectAll();
        else if (startsWith(key, "style:")) wed.setStyle(kStyleOrder[clampv(toInt(key.substr(6), 0), 0, kStyleCount - 1)]);
        else if (key == "fmt:b") wed.toggleFmt(F_BOLD); else if (key == "fmt:i") wed.toggleFmt(F_ITALIC); else if (key == "fmt:u") wed.toggleFmt(F_UNDER);
        else if (key == "fmt:s") wed.toggleFmt(F_STRIKE); else if (key == "fmt:sup") wed.toggleFmt(F_SUP); else if (key == "fmt:sub") wed.toggleFmt(F_SUB);
        else if (key == "fmt:code") wed.toggleFmt(F_CODE); else if (key == "fmt:mark") wed.toggleFmt(F_MARK);
        else if (key == "size:+") { float c = wed.sizeHere() > 0 ? wed.sizeHere() : wdoc.setup.baseSize; int idx = kSizeCount - 1; for (int i = kSizeCount - 1; i >= 0; i--) if (kSizes[i] > c + 0.01f) idx = i; wed.setSize(kSizes[idx]); }
        else if (key == "size:-") { float c = wed.sizeHere() > 0 ? wed.sizeHere() : wdoc.setup.baseSize; int idx = 0; for (int i = 0; i < kSizeCount; i++) if (kSizes[i] < c - 0.01f) idx = i; wed.setSize(kSizes[idx]); }
        else if (key == "size:0") wed.setSize(0);
        else if (key == "clear") wed.clearFormatting();
        else if (key == "props") writerPane = 2;
        else if (key == "ins:equation") { writerEquationBuf_ = wed.mathAtCaret(); writerEquationError_.clear(); writerEquationFocus_ = true; ui.openPopup("wequation"); }
        else if (key == "ins:table") ui.openPopup("wtbl");
        else if (key == "ins:chart") ui.openPopup("wchart");
        else if (key == "ins:map") writerInsertMap();
        else if (key == "ins:rule") wed.insertRule();
        else if (key == "ins:pb") wed.insertPageBreak();
        else if (key == "ins:toc") wed.insertToc();
        else if (key == "rowa") wed.tableInsertRow(false);
        else if (key == "rowb") wed.tableInsertRow(true);
        else if (key == "coll") wed.tableInsertCol(false);
        else if (key == "colr") wed.tableInsertCol(true);
        else if (key == "delrow") wed.tableDeleteRow();
        else if (key == "delcol") wed.tableDeleteCol();
        else if (key == "hdr") wed.tableToggleHeader();
        else if (startsWith(key, "cal:")) wed.tableSetColAlign(key[4]);
        else if (key == "deltbl") wed.tableDeleteTable();
        else if (key == "w50") wed.setFigureWidth(50);
        else if (key == "w75") wed.setFigureWidth(75);
        else if (key == "w100") wed.setFigureWidth(100);
        else if (startsWith(key, "fal:")) { Figure fg = wed.curBlock().fig; fg.align = key[4] == 'l' ? PAlign::Left : key[4] == 'r' ? PAlign::Right : PAlign::Center; wed.setFigure(fg); }
        else if (startsWith(key, "fcap:")) { Figure fg = wed.curBlock().fig; fg.captionAbove = key[5] == 'a'; wed.setFigure(fg); }
        else if (key == "fborder") { Figure fg = wed.curBlock().fig; fg.border = !fg.border; wed.setFigure(fg); }
        else if (startsWith(key, "ffmt:")) { Figure fg = wed.curBlock().fig; fg.format = key.substr(5); wed.setFigure(fg); }
        else if (key == "up") wed.moveBlockUpDown(-1);
        else if (key == "down") wed.moveBlockUpDown(1);
        else if (key == "delblk") { if (wed.inTable()) wed.tableDeleteTable(); else wed.deleteBlock(wed.caret.blk); }
        writerFollow_ = true;
        writerCaretX_ = -1;
      }
    });
  }
}

}  // namespace win
}  // namespace vs
