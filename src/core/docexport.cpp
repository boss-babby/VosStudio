// VOSStudio Native: document export — self-contained HTML, and a page layout engine that turns the document into
// Scenes (points) for the PDF writer in figure.cpp. Figures stay vector graphics in both formats.
#include "doc.h"
#include "equation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vs {

namespace {

// ---------------------------------------------------------------- shared helpers
bool captionHasLabel(const string& text) {
  string t = lower(trim(text));
  for (const char* pfx : {"figure ", "fig. ", "fig ", "table ", "tbl. "}) {
    size_t n = strlen(pfx);
    if (t.size() > n && startsWith(t, pfx) && isdigit(uint8_t(t[n]))) return true;
  }
  return false;
}

// a Caption paragraph that introduces the next table (or trails a table)
int tableForCaption(const Document& d, int k) {
  if (k + 1 < int(d.blocks.size()) && d.blocks[size_t(k) + 1].kind == Block::TableBlock) return k + 1;
  if (k > 0 && d.blocks[size_t(k) - 1].kind == Block::TableBlock && !(k + 1 < int(d.blocks.size()) && d.blocks[size_t(k) + 1].kind == Block::TableBlock)) return k - 1;
  return -1;
}

string headingPrefix(const Document& d, const vector<string>& hn, size_t k) {
  return d.setup.numberedHeadings && !hn[k].empty() ? hn[k] + "  " : string();
}

// ---------------------------------------------------------------- HTML
string htmlEsc(const string& s) {
  string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

string cssColor(uint32_t c) {
  char b[16];
  snprintf(b, sizeof b, "#%02x%02x%02x", (c >> 16) & 255, (c >> 8) & 255, c & 255);
  return b;
}

string spansHtml(const vector<Span>& spans) {
  string o;
  for (auto& s : spans) {
    string t = htmlEsc(s.text);
    t = replaceAll(t, "\n", "<br>");
    if (s.isCite()) { o += "<span class=\"cite\">" + (s.fmt & F_SUP ? "<sup>" + t + "</sup>" : t) + "</span>"; continue; }
    if (s.fmt & F_MATH) {
      string m = mathToMathML(s.text);
      o += m.empty() ? "<code class=\"math-source\">" + t + "</code>" : m;
      continue;
    }
    string style;
    if (!s.font.empty()) style += "font-family:'" + htmlEsc(s.font) + "'," + (PageSetup::monoFamily(s.font) ? "monospace" : PageSetup::serifFamily(s.font) ? "serif" : "sans-serif") + ";";
    if (s.size > 0) style += "font-size:" + fmtNum(s.size, 1) + "pt;";
    if (s.color) style += "color:" + cssColor(s.color) + ";";
    if (!style.empty()) t = "<span style=\"" + style + "\">" + t + "</span>";
    if (s.fmt & F_CODE) t = "<code>" + t + "</code>";
    if (s.fmt & F_BOLD) t = "<strong>" + t + "</strong>";
    if (s.fmt & F_ITALIC) t = "<em>" + t + "</em>";
    if (s.fmt & F_UNDER) t = "<u>" + t + "</u>";
    if (s.fmt & F_STRIKE) t = "<s>" + t + "</s>";
    if (s.fmt & F_SUB) t = "<sub>" + t + "</sub>";
    if (s.fmt & F_SUP) t = "<sup>" + t + "</sup>";
    if (s.fmt & F_MARK) t = "<mark>" + t + "</mark>";
    if (!s.link.empty()) t = "<a href=\"" + htmlEsc(s.link) + "\" target=\"_blank\" rel=\"noopener\">" + t + "</a>";
    o += t;
  }
  return o;
}

const char* alignAttr(PAlign a) {
  switch (a) {
    case PAlign::Center: return " style=\"text-align:center\"";
    case PAlign::Right: return " style=\"text-align:right\"";
    case PAlign::Justify: return " style=\"text-align:justify\"";
    default: return "";
  }
}

}  // namespace

string docToHTML(const Document& d, const ScenePng& png) {
  const PageSetup& ps = d.setup;
  string title = d.title();
  if (title.empty()) title = "Document";
  string fam = "'" + htmlEsc(ps.font) + "', " + string(ps.serif() ? "Georgia, 'Times New Roman', Times, serif" : "Calibri, 'Segoe UI', Arial, Helvetica, sans-serif");
  string hfam = ps.headingFont.empty() ? string() : "'" + htmlEsc(ps.headingFont) + "', " + string(PageSetup::serifFamily(ps.headingFont) ? "Georgia, serif" : "Arial, sans-serif");
  bool numericRefs = citeStyleInfo(d.citeStyle).numeric;
  string o = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  o += "<title>" + htmlEsc(title) + "</title>\n";
  if (!d.author.empty()) o += "<meta name=\"author\" content=\"" + htmlEsc(d.author) + "\">\n";
  if (!d.keywords.empty()) o += "<meta name=\"keywords\" content=\"" + htmlEsc(d.keywords) + "\">\n";
  o += "<meta name=\"generator\" content=\"VOSStudio Native " + appVersion() + "\">\n";
  o += "<style>\n";
  o += "@page{size:" + string(ps.paper == "Letter" ? "Letter" : "A4") + ";margin:" + fmtNum(ps.margin() / 72.0, 2) + "in}\n";
  o += "html{background:#e9e9ec}body{margin:0;font-family:" + fam + ";font-size:" + fmtNum(ps.baseSize, 1) + "pt;line-height:" + fmtNum(1.25 + (ps.lineSpacing - 1.0), 2) + ";color:#1a1a1e}\n";
  if (!hfam.empty()) o += "h1,h2,h3,h4,h1.title,p.subtitle{font-family:" + hfam + "}\n";
  if (!ps.header.empty()) o += "header.run{max-width:" + fmtNum(ps.textW() / 72.0, 2) + "in;margin:0 auto;padding:.4em " + fmtNum(ps.margin() / 72.0, 2) + "in 0;font-size:.8em;color:#666;text-align:right}\n";
  o += "span.cite{white-space:nowrap}figure.bordered svg,figure.bordered img{border:1px solid #c5c8ce;padding:4px}figure.left{text-align:left}figure.right{text-align:right}\n";
  o += "article{max-width:" + fmtNum(ps.textW() / 72.0, 2) + "in;margin:0 auto;padding:" + fmtNum(ps.margin() / 72.0, 2) + "in;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.12)}\n";
  o += "@media print{html{background:#fff}article{max-width:none;margin:0;padding:0;box-shadow:none}.pb{break-before:page;page-break-before:always}nav.toc a{color:inherit}}\n";
  o += "h1.title{font-size:2.2em;line-height:1.15;margin:0 0 .3em;font-weight:700}p.subtitle{font-size:1.3em;color:#444;margin:0 0 .4em}p.meta{color:#555;font-size:.92em;margin:0 0 .25em}\n";
  o += "h1{font-size:1.45em;margin:1.4em 0 .4em;line-height:1.2}h2{font-size:1.2em;margin:1.2em 0 .35em}h3{font-size:1.05em;margin:1em 0 .3em}h4{font-size:1em;font-style:italic;margin:1em 0 .3em}\n";
  o += "p{margin:0 0 .7em}ul,ol{margin:0 0 .7em;padding-left:1.6em}li{margin:.15em 0}li.l1{margin-left:1.4em}li.l2{margin-left:2.8em}\n";
  o += "blockquote{margin:.6em 0 .9em;padding:.2em 0 .2em 1em;border-left:3px solid #c9c9cf;color:#333;font-style:italic}\n";
  o += "pre{background:#f3f3f6;border:1px solid #e1e1e6;border-radius:4px;padding:.6em .8em;font-size:.88em;line-height:1.4;overflow:auto;white-space:pre-wrap}code{font-family:Consolas,'Cascadia Mono','Courier New',monospace;font-size:.92em;background:#f3f3f6;padding:0 .2em;border-radius:3px}pre code{background:none;padding:0}\n";
  o += "figure{margin:1em 0 1.2em;text-align:center}figure svg,figure img{max-width:100%;height:auto}figcaption,p.caption{font-size:.9em;color:#333;margin:.4em 0 .8em;text-align:center}p.caption{text-align:left}\n";
  o += "table{border-collapse:collapse;margin:0 0 1em;width:100%;font-size:.92em}th,td{padding:.35em .55em;border-bottom:1px solid #d9d9df;vertical-align:top;text-align:left}th{border-top:2px solid #333;border-bottom:1px solid #333;font-weight:700;background:#f7f7f9}tr:last-child td{border-bottom:2px solid #333}\n";
  o += "hr{border:0;border-top:1px solid #bbb;margin:1.2em 0}mark{background:#fff3a0}a{color:#0b57a4}p.ref{padding-left:2.2em;text-indent:-2.2em;font-size:.94em;margin-bottom:.4em}\n";
  o += "nav.toc{margin:1em 0 1.5em}nav.toc p{margin:.15em 0}nav.toc .t1{font-weight:600}nav.toc .t2{margin-left:1.4em}nav.toc .t3{margin-left:2.8em;font-size:.95em}\n";
  o += ".pb{height:0;border-top:1px dashed #cfcfd6;margin:1.5em 0}\n";
  o += "</style>\n</head>\n<body>\n";
  if (!ps.header.empty()) o += "<header class=\"run\">" + htmlEsc(ps.header) + "</header>\n";
  o += "<article>\n";

  vector<int> nums = docListNumbers(d);
  vector<string> hn = docHeadingNumbers(d);
  auto tocHtml = [&]() {
    string t = "<nav class=\"toc\"><h1>Contents</h1>\n";
    for (size_t k = 0; k < d.blocks.size(); k++) {
      const Block& b = d.blocks[k];
      if (b.kind != Block::Paragraph || !isHeading(b.p.style) || headingLevel(b.p.style) > 3) continue;
      t += "<p class=\"t" + std::to_string(headingLevel(b.p.style)) + "\"><a href=\"#h" + std::to_string(k) + "\">" + htmlEsc(headingPrefix(d, hn, k)) + spansHtml(b.p.spans) + "</a></p>\n";
    }
    return t + "</nav>\n";
  };
  bool tocDone = false, frontDone = false;
  string listOpen;  // "ul" / "ol" while inside a list
  int listLevel = 0;
  auto closeList = [&]() { if (!listOpen.empty()) { o += "</" + listOpen + ">\n"; listOpen.clear(); listLevel = 0; } };
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& b = d.blocks[k];
    bool front = b.kind == Block::Paragraph && (b.p.style == PStyle::Title || b.p.style == PStyle::Subtitle || b.p.style == PStyle::Meta);
    if (!front && !frontDone) {
      frontDone = true;
      if (ps.toc && !tocDone && std::none_of(d.blocks.begin(), d.blocks.end(), [](const Block& x) { return x.kind == Block::Toc; })) { o += tocHtml(); tocDone = true; }
    }
    if (b.kind == Block::Paragraph) {
      const Para& p = b.p;
      if (isList(p.style)) {
        string want = p.style == PStyle::Bullet ? "ul" : "ol";
        if (listOpen != want) {
          closeList();
          listOpen = want;
          o += "<" + want + (want == "ol" && nums[k] > 1 ? " start=\"" + std::to_string(nums[k]) + "\"" : string()) + ">\n";
        } else if (want == "ol" && nums[k] > 0 && p.numStart > 0 && k > 0) {
          closeList();
          listOpen = want;
          o += "<ol start=\"" + std::to_string(nums[k]) + "\">\n";
        }
        o += "<li" + string(p.level ? " class=\"l" + std::to_string(p.level) + "\"" : string()) + (want == "ol" && p.level ? " value=\"" + std::to_string(nums[k]) + "\"" : string()) + ">" + spansHtml(p.spans) + "</li>\n";
        continue;
      }
      closeList();
      string body = spansHtml(p.spans);
      switch (p.style) {
        case PStyle::Title: o += "<h1 class=\"title\"" + string(alignAttr(p.align)) + ">" + body + "</h1>\n"; break;
        case PStyle::Subtitle: o += "<p class=\"subtitle\"" + string(alignAttr(p.align)) + ">" + body + "</p>\n"; break;
        case PStyle::Meta: o += "<p class=\"meta\"" + string(alignAttr(p.align)) + ">" + body + "</p>\n"; break;
        case PStyle::H1: case PStyle::H2: case PStyle::H3: case PStyle::H4: {
          string tag = "h" + std::to_string(headingLevel(p.style));
          o += "<" + tag + " id=\"h" + std::to_string(k) + "\"" + alignAttr(p.align) + ">" + htmlEsc(headingPrefix(d, hn, k)) + body + "</" + tag + ">\n";
          break;
        }
        case PStyle::Quote: o += "<blockquote" + string(alignAttr(p.align)) + "><p>" + body + "</p></blockquote>\n"; break;
        case PStyle::Code: o += "<pre><code>" + htmlEsc(p.text()) + "</code></pre>\n"; break;
        case PStyle::Caption: {
          int t = tableForCaption(d, int(k));
          string label = t >= 0 && !captionHasLabel(p.text()) ? "<strong>Table " + std::to_string(d.tableNumber(t)) + ".</strong> " : string();
          o += "<p class=\"caption\"" + string(alignAttr(p.align)) + ">" + label + body + "</p>\n";
          break;
        }
        case PStyle::Reference: {
          bool selfNumbered = !p.refKey.empty();  // generated entries carry their own number when the style has one
          o += "<p class=\"ref\">" + (numericRefs && !selfNumbered ? "[" + std::to_string(d.refNumber(int(k))) + "] " : string()) + body + "</p>\n";
          break;
        }
        default: o += "<p" + string(alignAttr(p.align)) + ">" + (body.empty() ? "&nbsp;" : body) + "</p>\n"; break;
      }
    } else if (b.kind == Block::FigureBlock) {
      closeList();
      string cls;
      if (b.fig.border) cls += " bordered";
      if (b.fig.align == PAlign::Left) cls += " left";
      else if (b.fig.align == PAlign::Right) cls += " right";
      o += cls.empty() ? "<figure>\n" : "<figure class=\"" + trim(cls) + "\">\n";
      string label = captionHasLabel(b.fig.caption.text()) || b.fig.label == "-" ? string() : "<strong>" + htmlEsc(b.fig.labelText()) + " " + std::to_string(d.figureNumber(int(k))) + ".</strong> ";
      string caption = "<figcaption>" + label + spansHtml(b.fig.caption.spans) + "</figcaption>\n";
      if (b.fig.captionAbove) o += caption;
      if (b.fig.asset >= 0 && b.fig.asset < int(d.assets.size())) {
        const Scene& sc = d.assets[size_t(b.fig.asset)].scene;
        string altText = htmlEsc(b.fig.alt.empty() ? b.fig.caption.text() : b.fig.alt);
        string bytes;
        if (b.fig.formatOr(ps.figureFormat) == "png" && png) bytes = png(sc, 200);
        if (!bytes.empty()) {
          o += "<img src=\"data:image/png;base64," + base64(bytes) + "\" style=\"width:" + std::to_string(b.fig.widthPct) + "%\" alt=\"" + altText + "\">\n";
        } else {
          string svg = toSVG(sc, ps.serif());
          size_t start = svg.find("<svg");
          if (start != string::npos) svg = svg.substr(start);
          svg = replaceAll(svg, "<svg ", "<svg style=\"width:" + std::to_string(b.fig.widthPct) + "%\" role=\"img\" aria-label=\"" + altText + "\" ");
          o += svg + "\n";
        }
      } else o += "<div style=\"padding:2em;border:1px dashed #bbb;color:#888\">[missing figure]</div>\n";
      if (!b.fig.captionAbove) o += caption;
      o += "</figure>\n";
    } else if (b.kind == Block::TableBlock) {
      closeList();
      const Table& t = b.tbl;
      o += "<table>\n";
      if (!t.widths.empty()) { o += "<colgroup>"; for (float w : t.widths) o += "<col style=\"width:" + fmtNum(w * 100, 1) + "%\">"; o += "</colgroup>\n"; }
      for (int r = 0; r < t.rows(); r++) {
        bool th = t.header && r == 0;
        o += th ? "<thead><tr>" : (r == (t.header ? 1 : 0) ? "<tbody><tr>" : "<tr>");
        for (int c = 0; c < t.cols(); c++) {
          const Para& cell = t.cells[size_t(r)][size_t(c)];
          char a = c < int(t.align.size()) ? t.align[size_t(c)] : 'a';
          string st = cell.align != PAlign::Left ? alignAttr(cell.align) : a == 'r' ? " style=\"text-align:right\"" : a == 'c' ? " style=\"text-align:center\"" : "";
          o += string(th ? "<th" : "<td") + st + ">" + spansHtml(cell.spans) + (th ? "</th>" : "</td>");
        }
        o += th ? "</tr></thead>\n" : "</tr>\n";
      }
      if (t.rows() > (t.header ? 1 : 0)) o += "</tbody>";
      o += "</table>\n";
    } else if (b.kind == Block::Rule) { closeList(); o += "<hr>\n"; }
    else if (b.kind == Block::PageBreak) { closeList(); o += "<div class=\"pb\"></div>\n"; }
    else if (b.kind == Block::Toc) { closeList(); o += tocHtml(); tocDone = true; }
  }
  closeList();
  o += "</article>\n</body>\n</html>\n";
  return o;
}

// ---------------------------------------------------------------- page layout
namespace {

struct TStyle {
  float size = 11;
  bool bold = false, italic = false, mono = false;
  float before = 0, after = 8, indent = 0, hanging = 0;
  Color color{0.1f, 0.1f, 0.12f};
  int8_t face = 0;  // headings in a heading font of another class than the body font
};

TStyle styleOf(PStyle st, const PageSetup& ps) {
  float b = ps.baseSize;
  TStyle t;
  t.size = b;
  Color grey(0.36f, 0.36f, 0.40f), dark(0.2f, 0.2f, 0.23f);
  switch (st) {
    case PStyle::Title: t.size = b * 2.2f; t.bold = true; t.after = 6; break;
    case PStyle::Subtitle: t.size = b * 1.3f; t.color = dark; t.after = 5; break;
    case PStyle::Meta: t.size = b * 0.9f; t.color = grey; t.after = 2; break;
    case PStyle::H1: t.size = b * 1.45f; t.bold = true; t.before = 18; t.after = 6; break;
    case PStyle::H2: t.size = b * 1.2f; t.bold = true; t.before = 14; t.after = 4; break;
    case PStyle::H3: t.size = b * 1.05f; t.bold = true; t.before = 12; t.after = 3; break;
    case PStyle::H4: t.size = b; t.bold = true; t.italic = true; t.before = 10; t.after = 2; break;
    case PStyle::Body: t.after = 8; break;
    case PStyle::Bullet: case PStyle::Number: t.after = 3; break;
    case PStyle::Quote: t.italic = true; t.indent = 18; t.color = dark; t.before = 2; t.after = 8; break;
    case PStyle::Code: t.size = b * 0.85f; t.mono = true; t.after = 8; break;
    case PStyle::Caption: t.size = b * 0.88f; t.color = dark; t.after = 10; break;
    case PStyle::Reference: t.size = b * 0.92f; t.hanging = 22; t.after = 4; break;
  }
  if ((isHeading(st) || st == PStyle::Title || st == PStyle::Subtitle) && !ps.headingFont.empty() && PageSetup::serifFamily(ps.headingFont) != ps.serif())
    t.face = PageSetup::serifFamily(ps.headingFont) ? 2 : 1;
  return t;
}

struct Seg {
  string text, link;
  uint16_t fmt = 0;
  float x = 0, w = 0, size = 11;
  bool bold = false, italic = false, mono = false;
  uint32_t color = 0;  // span colour override (0 = the style's)
  int8_t face = 0;     // 0 document family, 1 sans, 2 serif (span font override)
};
float g_lineSpacing = 1.15f;  // set by the Pager (docToPages is single-threaded)
float lineHeightFor(float size) { return size * (1.15f + (g_lineSpacing - 1.0f)) ; }
struct Line {
  vector<Seg> segs;
  float w = 0, h = 0, indent = 0;
  bool last = false;
};

float segWidth(const string& text, float size, bool bold, bool mono) {
  if (mono) return size * 0.6f * float(utf8Len(text));
  return float(textWidth(text, size, bold));
}

vector<Line> layoutLines(const Para& p, const TStyle& ts, float width, const string& prefix = "") {
  struct Tok { string text, link; uint16_t fmt; bool space, brk; float size; uint32_t color; int8_t face; };
  vector<Tok> toks;
  if (!prefix.empty()) toks.push_back({prefix, "", F_BOLD, true, false, 0, 0, 0});
  for (auto& s : p.spans) {
    string cur;
    const bool math = (s.fmt & F_MATH) != 0;
    const string display = math ? mathPreviewText(s.text) : s.text;
    const uint16_t fmt = uint16_t(s.fmt & ~F_MATH);
    int8_t face = math ? 2 : s.font.empty() ? 0 : (PageSetup::serifFamily(s.font) ? 2 : 1);
    auto push = [&](bool space, bool brk) { if (!cur.empty() || space || brk) toks.push_back({cur, s.link, fmt, space, brk, s.size, s.color, face}); cur.clear(); };
    for (char c : display) {
      if (c == ' ') push(true, false);
      else if (c == '\n') push(false, true);
      else cur += c;
    }
    push(false, false);
  }
  vector<Line> lines;
  Line cur;
  cur.indent = ts.hanging > 0 ? 0 : ts.indent;
  float avail = width - cur.indent;
  float x = 0;
  float lineH = lineHeightFor(ts.size);
  for (auto& t : toks) if (t.size > 0) lineH = std::max(lineH, lineHeightFor(t.size));
  auto finish = [&](bool last) {
    cur.w = x;
    cur.h = lineH;
    cur.last = last;
    lines.push_back(cur);
    cur = Line();
    cur.indent = ts.hanging > 0 ? ts.hanging : ts.indent;
    avail = width - cur.indent;
    x = 0;
  };
  auto add = [&](const string& text, uint16_t fmt, const string& link, bool space, float spanSize, uint32_t color, int8_t face) {
    float base = spanSize > 0 ? spanSize : ts.size;
    float size = (fmt & (F_SUB | F_SUP)) ? base * 0.72f : base;
    bool bold = ts.bold || (fmt & F_BOLD), italic = ts.italic || (fmt & F_ITALIC), mono = ts.mono || (fmt & F_CODE);
    float w = segWidth(text, size, bold, mono);
    if (!cur.segs.empty() && x + w > avail && !space) {
      if (!cur.segs.empty()) { while (!cur.segs.empty() && cur.segs.back().text == " ") { x -= cur.segs.back().w; cur.segs.pop_back(); } }
      finish(false);
    }
    if (w > avail && text.size() > 1) {  // a very long word: break it by characters
      string piece;
      for (size_t i = 0; i < text.size();) {
        size_t e = cpNext(text, i);
        string ch = text.substr(i, e - i);
        i = e;
        if (x + segWidth(piece + ch, size, bold, mono) > avail && !piece.empty()) {
          Seg s{piece, link, fmt, x, segWidth(piece, size, bold, mono), size, bold, italic, mono, color, face};
          cur.segs.push_back(s);
          x += s.w;
          finish(false);
          piece.clear();
        }
        piece += ch;
      }
      if (!piece.empty()) { Seg s{piece, link, fmt, x, segWidth(piece, size, bold, mono), size, bold, italic, mono, color, face}; cur.segs.push_back(s); x += s.w; }
      return;
    }
    Seg s{text, link, fmt, x, w, size, bold, italic, mono, color, face};
    cur.segs.push_back(s);
    x += w;
  };
  for (auto& t : toks) {
    if (!t.text.empty()) add(t.text, t.fmt, t.link, false, t.size, t.color, t.face);
    if (t.space) add(" ", t.fmt, t.link, true, t.size, t.color, t.face);
    if (t.brk) finish(false);
  }
  while (!cur.segs.empty() && cur.segs.back().text == " ") { x -= cur.segs.back().w; cur.segs.pop_back(); }
  finish(true);
  return lines;
}

float linesHeight(const vector<Line>& ls) { float h = 0; for (auto& l : ls) h += l.h; return h; }

struct Pager {
  const Document& d;
  const PageSetup& ps;
  vector<Scene> pages;
  Scene* pg = nullptr;
  float W, H, M, textW, top, bottom, y = 0;
  vector<int> headingPage;
  const vector<int>* known;  // heading pages from a previous pass (for the TOC)
  Color link{0.04f, 0.34f, 0.64f}, grey{0.5f, 0.5f, 0.54f};

  Pager(const Document& doc, const vector<int>* prev) : d(doc), ps(doc.setup), known(prev) {
    W = float(ps.pageW());
    H = float(ps.pageH());
    M = float(ps.margin());
    textW = W - 2 * M;
    top = M;
    bottom = H - M;
    headingPage.assign(d.blocks.size(), 0);
    g_lineSpacing = clampv(ps.lineSpacing, 1.0f, 2.5f);
  }
  void newPage() {
    Scene sc;
    sc.W = W;
    sc.H = H;
    sc.serif = ps.serif();
    pages.push_back(sc);
    pg = &pages.back();
    y = top;
    if (!ps.header.empty()) {
      Prim t;
      t.type = Prim::Text;
      t.text = ps.header;
      t.size = 9;
      t.fill = true;
      t.fillC = grey;
      t.anchor = 2;
      t.x = W - M;
      t.y = M * 0.55f;
      t.group = "text";
      pg->items.push_back(t);
      hline(M, W - M, M * 0.62f + 3, 0.5f, Color(0.82f, 0.83f, 0.86f));
    }
    if (ps.pageNumbers) {
      Prim t;
      t.type = Prim::Text;
      t.text = std::to_string(pages.size());
      t.size = 9;
      t.fill = true;
      t.fillC = grey;
      t.anchor = 1;
      t.x = W / 2;
      t.y = H - M * 0.5f;
      t.group = "text";
      pg->items.push_back(t);
    }
  }
  void ensure(float h) { if (!pg || (y + h > bottom && y > top + 1)) newPage(); }
  void rect(float x, float yy, float w, float h, Color c) {
    Prim r;
    r.type = Prim::Rect;
    r.x = x; r.y = yy; r.w = w; r.h = h;
    r.fill = true;
    r.fillC = c;
    r.group = "background";
    pg->items.push_back(r);
  }
  void hline(float x1, float x2, float yy, float sw, Color c) {
    Prim p;
    p.type = Prim::Path;
    p.d.push_back({'M', x1, yy, 0, 0});
    p.d.push_back({'L', x2, yy, 0, 0});
    p.stroke = true;
    p.strokeC = c;
    p.sw = sw;
    p.group = "background";
    pg->items.push_back(p);
  }
  void text(const string& s, float x, float baseline, float size, Color c, bool bold, bool italic, bool mono, int anchor = 0, bool underline = false, const string& href = "", int8_t face = 0) {
    if (s.empty()) return;
    Prim t;
    t.type = Prim::Text;
    t.text = s; t.x = x; t.y = baseline; t.size = size;
    t.fill = true; t.fillC = c;
    t.bold = bold; t.italic = italic; t.mono = mono; t.underline = underline; t.href = href;
    t.anchor = anchor;
    t.face = face;
    t.group = "text";
    pg->items.push_back(t);
  }
  // draw one laid-out line at (x0, y) inside a box of `width`; returns nothing, advances nothing
  void drawLine(const Line& l, float x0, float width, PAlign align, const TStyle& ts, float yy) {
    float off = 0, extra = 0;
    if (align == PAlign::Center) off = (width - l.indent - l.w) / 2;
    else if (align == PAlign::Right) off = width - l.indent - l.w;
    else if (align == PAlign::Justify && !l.last && l.segs.size() > 1) {
      int gaps = 0;
      for (auto& s : l.segs) if (s.text == " ") gaps++;
      if (gaps) extra = std::min((width - l.indent - l.w) / float(gaps), ts.size * 0.6f);
    }
    float base = yy + l.h * 0.76f;
    float shift = 0;
    for (auto& s : l.segs) {
      float x = x0 + l.indent + off + s.x + shift;
      if (s.text == " ") { shift += extra; continue; }
      float b = base;
      if (s.fmt & F_SUP) b -= ts.size * 0.33f;
      if (s.fmt & F_SUB) b += ts.size * 0.12f;
      if (s.fmt & F_MARK) rect(x, yy + l.h * 0.08f, s.w, l.h * 0.86f, Color(1.0f, 0.95f, 0.55f));
      if (s.fmt & F_CODE && !ts.mono) rect(x - 1, yy + l.h * 0.1f, s.w + 2, l.h * 0.82f, Color(0.94f, 0.94f, 0.96f));
      bool isLink = !s.link.empty();
      Color c = isLink ? link : ts.color;
      if (s.color) c = Color(float((s.color >> 16) & 255) / 255.f, float((s.color >> 8) & 255) / 255.f, float(s.color & 255) / 255.f);
      text(s.text, x, b, s.size, c, s.bold, s.italic, s.mono, 0, isLink || (s.fmt & F_UNDER), s.link, s.face ? s.face : ts.face);
      if (s.fmt & F_STRIKE) hline(x, x + s.w, b - s.size * 0.3f, std::max(0.5f, s.size * 0.06f), c);
    }
  }
  // paragraph with automatic page breaks; `prefix` (list marker / caption label) is drawn in the hanging area
  void paragraph(const Para& p, const TStyle& ts, float x0, float width, const string& marker = "", float markerW = 0, bool keepNext = false, const string& labelPrefix = "") {
    vector<Line> lines = layoutLines(p, ts, width - markerW, labelPrefix);
    float lineH = lineHeightFor(ts.size);
    if (!pg) newPage();
    if (y > top + 1) y += ts.before;
    float need = std::min(linesHeight(lines), lineH * 2) + (keepNext ? lineH * 2.5f : 0);
    if (y + need > bottom) newPage();
    for (size_t i = 0; i < lines.size(); i++) {
      if (y + lines[i].h > bottom) {
        // widows: never leave the last line alone at the top of a page when we can move two
        newPage();
      }
      if (i == 0 && !marker.empty()) {
        bool num = p.style == PStyle::Number;
        text(marker, num ? x0 + markerW - 5 : x0 + markerW * 0.35f, y + lines[i].h * 0.76f, ts.size, ts.color, false, false, false, num ? 2 : 0);
      }
      drawLine(lines[i], x0 + markerW, width - markerW, p.align, ts, y);
      y += lines[i].h;
    }
    y += ts.after;
  }
  void figure(const Block& b, size_t k) {
    if (b.fig.asset < 0 || b.fig.asset >= int(d.assets.size())) return;
    const Scene& sc = d.assets[size_t(b.fig.asset)].scene;
    if (sc.W <= 0 || sc.H <= 0) return;
    float fw = textW * float(b.fig.widthPct) / 100.f;
    float s = fw / float(sc.W);
    float fh = float(sc.H) * s;
    TStyle cs = styleOf(PStyle::Caption, ps);
    Para cap = b.fig.caption;
    string label = captionHasLabel(cap.text()) || b.fig.label == "-" ? string() : b.fig.labelText() + " " + std::to_string(d.figureNumber(int(k))) + ".";
    vector<Line> capLines = layoutLines(cap, cs, textW, label);
    float capH = linesHeight(capLines) + 4;
    float maxH = (bottom - top) - capH - 8;
    if (fh > maxH) { s *= maxH / fh; fh = maxH; fw = float(sc.W) * s; }
    if (!pg) newPage();
    y += 6;
    if (y + fh + capH > bottom && y > top + 1) newPage();
    if (cap.align == PAlign::Left) cap.align = PAlign::Center;
    auto drawCaption = [&]() { for (auto& l : capLines) { if (y + l.h > bottom) newPage(); drawLine(l, M, textW, cap.align, cs, y); y += l.h; } };
    if (b.fig.captionAbove) { drawCaption(); y += 3; }
    float ox = b.fig.align == PAlign::Left ? M : b.fig.align == PAlign::Right ? M + textW - fw : M + (textW - fw) / 2;
    if (b.fig.border) {
      Prim r;
      r.type = Prim::Rect;
      r.x = ox - 3; r.y = y - 3; r.w = fw + 6; r.h = fh + 6;
      r.stroke = true; r.strokeC = Color(0.72f, 0.74f, 0.78f); r.sw = 0.6f;
      r.group = "background";
      pg->items.push_back(r);
    }
    for (const Prim& q : sc.items) {
      if (!q.fill && !q.stroke && q.type != Prim::Text && q.type != Prim::Image) continue;
      Prim p = q;
      p.x = ox + q.x * s; p.y = y + q.y * s; p.w = q.w * s; p.h = q.h * s; p.r = q.r * s;
      p.size = q.size * s; p.sw = q.sw * s; p.haloW = q.haloW * s;
      for (auto& v : p.dash) v *= s;
      for (auto& c : p.d) { c.x1 = ox + c.x1 * s; c.y1 = y + c.y1 * s; if (c.op == 'Q' || c.op == 'C') { c.x2 = ox + c.x2 * s; c.y2 = y + c.y2 * s; } }
      p.tip.clear();
      pg->items.push_back(std::move(p));
    }
    y += fh + 4;
    if (!b.fig.captionAbove) drawCaption();
    y += cs.after;
  }
  void table(const Table& t) {
    int R = t.rows(), C = t.cols();
    if (!R || !C) return;
    TStyle ts = styleOf(PStyle::Body, ps);
    ts.size = ps.baseSize * 0.92f;
    ts.after = 0;
    vector<float> cw(size_t(C), textW / float(C));
    if (int(t.widths.size()) == C) { float sum = 0; for (float w : t.widths) sum += std::max(0.02f, w); for (int c = 0; c < C; c++) cw[size_t(c)] = textW * std::max(0.02f, t.widths[size_t(c)]) / sum; }
    const float pad = 4.5f;
    // cell line layouts
    vector<vector<vector<Line>>> cellLines;
    cellLines.resize(size_t(R));
    for (auto& row : cellLines) row.resize(size_t(C));
    vector<float> rowH(size_t(R), 0);
    for (int r = 0; r < R; r++)
      for (int c = 0; c < C; c++) {
        TStyle cs = ts;
        if (t.header && r == 0) cs.bold = true;
        cellLines[size_t(r)][size_t(c)] = layoutLines(t.cells[size_t(r)][size_t(c)], cs, cw[size_t(c)] - 2 * pad);
        rowH[size_t(r)] = std::max(rowH[size_t(r)], linesHeight(cellLines[size_t(r)][size_t(c)]) + 2 * pad);
      }
    if (!pg) newPage();
    y += 2;
    auto drawRow = [&](int r) {
      float x = M;
      bool head = t.header && r == 0;
      if (head) rect(M, y, textW, rowH[size_t(r)], Color(0.955f, 0.955f, 0.965f));
      for (int c = 0; c < C; c++) {
        const Para& cell = t.cells[size_t(r)][size_t(c)];
        char a = c < int(t.align.size()) ? t.align[size_t(c)] : 'a';
        PAlign al = cell.align != PAlign::Left ? cell.align : a == 'r' ? PAlign::Right : a == 'c' ? PAlign::Center : PAlign::Left;
        TStyle cs = ts;
        if (head) cs.bold = true;
        float yy = y + pad;
        for (auto& l : cellLines[size_t(r)][size_t(c)]) { drawLine(l, x + pad, cw[size_t(c)] - 2 * pad, al, cs, yy); yy += l.h; }
        x += cw[size_t(c)];
      }
      if (r == 0) hline(M, M + textW, y, 1.2f, Color(0.2f, 0.2f, 0.23f));
      float yb = y + rowH[size_t(r)];
      hline(M, M + textW, yb, head || r == R - 1 ? (r == R - 1 ? 1.2f : 0.8f) : 0.3f, r == R - 1 || head ? Color(0.2f, 0.2f, 0.23f) : Color(0.78f, 0.78f, 0.82f));
      y = yb;
    };
    for (int r = 0; r < R; r++) {
      float need = rowH[size_t(r)] + (r == 0 && R > 1 ? rowH[1] : 0);
      if (y + need > bottom && y > top + 1) {
        newPage();
        if (t.header && r > 0) drawRow(0);
      }
      drawRow(r);
    }
    y += 10;
  }
  void toc() {
    TStyle hs = styleOf(PStyle::H1, ps);
    Para h = Para();
    h.style = PStyle::H1;
    h.spans.push_back({"Contents", 0, ""});
    paragraph(h, hs, M, textW, "", 0, true);
    TStyle ts = styleOf(PStyle::Body, ps);
    ts.after = 1.5f;
    vector<string> hn = docHeadingNumbers(d);
    for (size_t k = 0; k < d.blocks.size(); k++) {
      const Block& b = d.blocks[k];
      if (b.kind != Block::Paragraph || !isHeading(b.p.style) || headingLevel(b.p.style) > 3) continue;
      int L = headingLevel(b.p.style);
      TStyle es = ts;
      es.bold = L == 1;
      if (L == 3) es.size = ps.baseSize * 0.95f;
      float ind = float(L - 1) * 16;
      string num = known && k < known->size() && (*known)[k] > 0 ? std::to_string((*known)[k]) : string();
      float numW = float(textWidth("000", es.size, false)) + 6;
      Para e;
      e.spans = b.p.spans;
      string pre = headingPrefix(d, hn, k);
      if (!pre.empty()) e.spans.insert(e.spans.begin(), Span{pre, 0, ""});
      vector<Line> lines = layoutLines(e, es, textW - ind - numW);
      if (!pg) newPage();
      if (y + lines[0].h > bottom) newPage();
      for (size_t i = 0; i < lines.size(); i++) {
        if (y + lines[i].h > bottom) newPage();
        drawLine(lines[i], M + ind, textW - ind - numW, PAlign::Left, es, y);
        if (i + 1 == lines.size()) {
          float x1 = M + ind + lines[i].indent + lines[i].w + 4, x2 = M + textW - numW + 2;
          if (x2 > x1 + 8) {
            Prim p;
            p.type = Prim::Path;
            p.d.push_back({'M', x1, y + lines[i].h * 0.76f, 0, 0});
            p.d.push_back({'L', x2, y + lines[i].h * 0.76f, 0, 0});
            p.stroke = true;
            p.strokeC = Color(0.6f, 0.6f, 0.64f);
            p.sw = 0.7f;
            p.dash = {0.7f, 3.f};
            p.roundCap = true;
            p.group = "background";
            pg->items.push_back(p);
          }
          text(num, M + textW, y + lines[i].h * 0.76f, es.size, ts.color, false, false, false, 2);
        }
        y += lines[i].h;
      }
      y += es.after;
    }
    y += 10;
  }
  void run() {
    vector<int> nums = docListNumbers(d);
    vector<string> hn = docHeadingNumbers(d);
    bool frontDone = false, tocDone = false;
    bool hasTocBlock = std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& x) { return x.kind == Block::Toc; });
    newPage();
    for (size_t k = 0; k < d.blocks.size(); k++) {
      const Block& b = d.blocks[k];
      bool front = b.kind == Block::Paragraph && (b.p.style == PStyle::Title || b.p.style == PStyle::Subtitle || b.p.style == PStyle::Meta);
      if (!front && !frontDone) {
        frontDone = true;
        if (ps.toc && !hasTocBlock && !tocDone) { toc(); tocDone = true; }
      }
      switch (b.kind) {
        case Block::Paragraph: {
          const Para& p = b.p;
          TStyle ts = styleOf(p.style, ps);
          if (isHeading(p.style)) {
            // record the page after a potential break: lay out with keep-with-next
            paragraph(p, ts, M, textW, "", 0, true, headingPrefix(d, hn, k));
            headingPage[k] = int(pages.size());
          } else if (isList(p.style)) {
            float ind = 18.f * float(p.level);
            string marker = p.style == PStyle::Bullet ? (p.level == 1 ? "\xE2\x80\x93" : "\xE2\x80\xA2") : std::to_string(nums[k]) + ".";
            paragraph(p, ts, M + ind, textW - ind, marker, 18);
          } else if (p.style == PStyle::Quote) {
            float y0 = y + ts.before;
            int pageBefore = int(pages.size());
            paragraph(p, ts, M, textW);
            if (int(pages.size()) == pageBefore) rect(M + 4, y0, 2.2f, y - ts.after - y0, Color(0.78f, 0.78f, 0.82f));
          } else if (p.style == PStyle::Code) {
            vector<Line> lines = layoutLines(p, ts, textW - 16);
            float h = linesHeight(lines) + 12;
            if (y + std::min(h, bottom - top - 20) > bottom && y > top + 1) newPage();
            float y0 = y;
            rect(M, y0, textW, std::min(h, bottom - y0), Color(0.95f, 0.95f, 0.965f));
            y += 6;
            for (auto& l : lines) {
              if (y + l.h > bottom) { newPage(); rect(M, y, textW, std::min(h, bottom - y), Color(0.95f, 0.95f, 0.965f)); y += 6; }
              drawLine(l, M + 8, textW - 16, PAlign::Left, ts, y);
              y += l.h;
            }
            y += 6 + ts.after;
          } else if (p.style == PStyle::Caption) {
            int t = tableForCaption(d, int(k));
            string label = t >= 0 && !captionHasLabel(p.text()) ? "Table " + std::to_string(d.tableNumber(t)) + "." : string();
            paragraph(p, ts, M, textW, "", 0, t == int(k) + 1, label);
          } else if (p.style == PStyle::Reference) {
            paragraph(p, ts, M, textW, "[" + std::to_string(d.refNumber(int(k))) + "]", 24);
          } else if (p.style == PStyle::Title || p.style == PStyle::Subtitle || p.style == PStyle::Meta) {
            paragraph(p, ts, M, textW, "", 0, p.style == PStyle::Title);
          } else {
            paragraph(p, ts, M, textW);
          }
          break;
        }
        case Block::FigureBlock: figure(b, k); break;
        case Block::TableBlock: table(b.tbl); break;
        case Block::Rule: {
          ensure(14);
          y += 6;
          hline(M, M + textW, y, 0.8f, Color(0.7f, 0.7f, 0.74f));
          y += 8;
          break;
        }
        case Block::PageBreak: newPage(); break;
        case Block::Toc: toc(); tocDone = true; break;
      }
    }
  }
};

}  // namespace

vector<Scene> docToPages(const Document& d) {
  Pager first(d, nullptr);
  first.run();
  bool needsToc = d.setup.toc || std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& x) { return x.kind == Block::Toc; });
  if (!needsToc) return std::move(first.pages);
  Pager second(d, &first.headingPage);
  second.run();
  if (second.headingPage == first.headingPage) return std::move(second.pages);
  Pager third(d, &second.headingPage);
  third.run();
  return std::move(third.pages);
}

string docToPDF(const Document& d) {
  string title = d.title();
  return toPDF(docToPages(d), title.empty() ? string("Document") : title);
}

}  // namespace vs
