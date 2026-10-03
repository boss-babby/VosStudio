// VOSStudio Native: clipboard exchange with Word (and every other HTML-aware editor).
//
// Copy: the selection becomes HTML in the dialect Word itself writes — Mso paragraph classes with mso-style-name
// (Normal, Heading 1-4, Caption, Quote, Bibliography ...), real <ul>/<ol> lists, MsoTableGrid tables, pictures as
// files, and Word *fields* in the conditional comments Word uses for its own HTML: CITATION fields for citations,
// SEQ fields for figure numbers and a BIBLIOGRAPHY field around the reference list. The bibliography sources travel
// as a dataStoreItem (the same b:Sources part the .docx writer produces), linked from <head> exactly as in a Web-Page
// file Word saves, so after a paste Word knows the sources: References > Manage Sources lists them, the citations
// are live and Bibliography > Insert Bibliography builds the list.
//
// Paste: Word's clipboard HTML (and a browser's) is read back into blocks: headings, list paragraphs (mso-list),
// tables, bold / italic / underline / strike / sub / sup / links / colours / fonts; Word's field code comments and
// the bullet characters it hides in <![if !supportLists]> are skipped, leaving the field results.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "doc.h"
#include "docx.h"
#include "equation.h"

namespace vs {

// Word's standard picture shape type (VML "t75"), as every Word HTML file defines it before its first picture
static const char* kVmlPictureType =
    "<v:shapetype id=\"_x0000_t75\" coordsize=\"21600,21600\" o:spt=\"75\" o:preferrelative=\"t\" path=\"m@4@5l@4@11@9@11@9@5xe\" filled=\"f\" stroked=\"f\">\n"
    " <v:stroke joinstyle=\"miter\"/>\n <v:formulas>\n  <v:f eqn=\"if lineDrawn pixelLineWidth 0\"/>\n  <v:f eqn=\"sum @0 1 0\"/>\n  <v:f eqn=\"sum 0 0 @1\"/>\n"
    "  <v:f eqn=\"prod @2 1 2\"/>\n  <v:f eqn=\"prod @3 21600 pixelWidth\"/>\n  <v:f eqn=\"prod @3 21600 pixelHeight\"/>\n  <v:f eqn=\"sum @0 0 1\"/>\n"
    "  <v:f eqn=\"prod @6 1 2\"/>\n  <v:f eqn=\"prod @7 21600 pixelWidth\"/>\n  <v:f eqn=\"sum @8 21600 0\"/>\n  <v:f eqn=\"prod @7 21600 pixelHeight\"/>\n"
    "  <v:f eqn=\"sum @10 21600 0\"/>\n </v:formulas>\n <v:path o:extrusionok=\"f\" gradientshapeok=\"t\" o:connecttype=\"rect\"/>\n <o:lock v:ext=\"edit\" aspectratio=\"t\"/>\n</v:shapetype>\n";

namespace {

// ---------------------------------------------------------------- helpers
string hEsc(const string& s) {
  string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

string hexColor(uint32_t c) {
  char b[16];
  snprintf(b, sizeof b, "#%02X%02X%02X", (c >> 16) & 255, (c >> 8) & 255, c & 255);
  return b;
}

const char* kFieldBegin = "<!--[if supportFields]><span style='mso-element:field-begin'></span>";
const char* kFieldSep = "<span style='mso-element:field-separator'></span><![endif]-->";
const char* kFieldEnd = "<!--[if supportFields]><span style='mso-element:field-end'></span><![endif]-->";

string field(const string& code, const string& result) { return string(kFieldBegin) + " " + hEsc(code) + " " + kFieldSep + result + kFieldEnd; }

bool captionLabelled(const string& text) {
  string t = lower(trim(text));
  for (const char* pfx : {"figure ", "fig. ", "fig ", "table ", "tbl. "}) {
    size_t n = strlen(pfx);
    if (t.size() > n && startsWith(t, pfx) && isdigit(uint8_t(t[n]))) return true;
  }
  return false;
}

struct Emit {
  const Document& d;
  const std::map<string, string>& tags;  // ref key -> Word source tag
  int citations = 0;

  string spans(const vector<Span>& sp) {
    string o;
    for (auto& s : sp) {
      string t = hEsc(s.text);
      t = replaceAll(t, "\n", "<br>");
      if (s.isCite()) {
        // one CITATION field per citation; several keys become "CITATION A \m B \m C" as Word writes them
        vector<string> ts;
        for (auto& k : split(s.cite, ';')) { auto it = tags.find(trim(k)); if (it != tags.end()) ts.push_back(it->second); }
        string res = s.fmt & F_SUP ? "<sup>" + t + "</sup>" : t;
        if (ts.empty()) { o += res; continue; }
        string code = "CITATION " + ts[0];
        for (size_t i = 1; i < ts.size(); i++) code += " \\m " + ts[i];
        code += " \\l 1033";
        citations++;
        o += field(code, res);
        continue;
      }
      if (s.fmt & F_MATH) {
        string m = mathToMathML(s.text);
        o += m.empty() ? "<span class=\"MsoMath\">" + t + "</span>" : "<span class=\"MsoMath\">" + m + "</span>";
        continue;
      }
      string style;
      if (!s.font.empty()) style += "font-family:\"" + hEsc(s.font) + "\"," + (PageSetup::monoFamily(s.font) ? "monospace" : PageSetup::serifFamily(s.font) ? "serif" : "sans-serif") + ";";
      if (s.size > 0) style += "font-size:" + fmtNum(s.size, 1) + "pt;";
      if (s.color) style += "color:" + hexColor(s.color) + ";";
      if (s.fmt & F_CODE) style += "font-family:Consolas,\"Courier New\",monospace;";
      if (s.fmt & F_MARK) style += "background:yellow;mso-highlight:yellow;";
      if (!style.empty()) t = "<span style='" + style + "'>" + t + "</span>";
      if (s.fmt & F_BOLD) t = "<b>" + t + "</b>";
      if (s.fmt & F_ITALIC) t = "<i>" + t + "</i>";
      if (s.fmt & F_UNDER) t = "<u>" + t + "</u>";
      if (s.fmt & F_STRIKE) t = "<s>" + t + "</s>";
      if (s.fmt & F_SUB) t = "<sub>" + t + "</sub>";
      if (s.fmt & F_SUP) t = "<sup>" + t + "</sup>";
      if (!s.link.empty()) t = "<a href=\"" + hEsc(s.link) + "\">" + t + "</a>";
      o += t;
    }
    return o;
  }
};

string alignStyle(PAlign a) {
  switch (a) {
    case PAlign::Center: return " align=center style='text-align:center'";
    case PAlign::Right: return " align=right style='text-align:right'";
    case PAlign::Justify: return " style='text-align:justify'";
    default: return "";
  }
}

// ---------------------------------------------------------------- HTML import: a small tolerant tokenizer
struct Tag {
  string name;                // lower case, "" for text
  bool close = false, selfClose = false;
  std::map<string, string> attrs;
  string text;                // for text nodes (entities decoded later)
};

void appendCp(string& o, uint32_t cp) {
  if (cp < 0x80) o += char(cp);
  else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
  else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 63)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
}

string decodeEntities(const string& s) {
  static const struct { const char* n; uint32_t cp; } named[] = {
      {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"ndash", 0x2013}, {"mdash", 0x2014},
      {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"copy", 0xA9}, {"reg", 0xAE},
      {"trade", 0x2122}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"times", 0xD7}, {"divide", 0xF7}, {"middot", 0xB7}, {"bull", 0x2022},
      {"laquo", 0xAB}, {"raquo", 0xBB}, {"eacute", 0xE9}, {"egrave", 0xE8}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"ccedil", 0xE7},
      {"ouml", 0xF6}, {"uuml", 0xFC}, {"auml", 0xE4}, {"szlig", 0xDF}, {"ntilde", 0xF1}, {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA},
      {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"mu", 0x3BC}, {"pi", 0x3C0}, {"sigma", 0x3C3}, {"le", 0x2264},
      {"ge", 0x2265}, {"ne", 0x2260}, {"minus", 0x2212}, {"micro", 0xB5}, {"sect", 0xA7}, {"para", 0xB6}, {"euro", 0x20AC}, {"pound", 0xA3},
      {"yen", 0xA5}, {"cent", 0xA2}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"sup2", 0xB2}, {"sup3", 0xB3}, {"rarr", 0x2192}, {"larr", 0x2190},
      {"harr", 0x2194}, {"uarr", 0x2191}, {"darr", 0x2193}, {"shy", 0}, {"zwj", 0}, {"zwnj", 0}, {"ensp", ' '}, {"emsp", ' '}, {"thinsp", ' '}};
  string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') { o += s[i]; continue; }
    size_t semi = s.find(';', i);
    if (semi == string::npos || semi - i > 12) { o += '&'; continue; }
    string ent = s.substr(i + 1, semi - i - 1);
    uint32_t cp = 0xFFFFFFFF;
    if (!ent.empty() && ent[0] == '#') {
      if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) cp = uint32_t(strtoul(ent.c_str() + 2, nullptr, 16));
      else cp = uint32_t(strtoul(ent.c_str() + 1, nullptr, 10));
      if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
    } else {
      for (auto& n : named) if (ent == n.n) { cp = n.cp; break; }
    }
    if (cp == 0xFFFFFFFF) { o += '&'; continue; }
    if (cp) appendCp(o, cp);
    i = semi;
  }
  return o;
}

vector<Tag> tokenize(const string& h) {
  vector<Tag> out;
  size_t i = 0, n = h.size();
  auto ieq = [&](size_t at, const char* lit) { size_t m = strlen(lit); if (at + m > n) return false; for (size_t k = 0; k < m; k++) if (tolower(uint8_t(h[at + k])) != tolower(uint8_t(lit[k]))) return false; return true; };
  string text;
  auto flush = [&]() { if (!text.empty()) { Tag t; t.text = text; out.push_back(std::move(t)); text.clear(); } };
  while (i < n) {
    if (h[i] != '<') { text += h[i++]; continue; }
    // comments and conditional comments: <!--...--> (Word's field codes live here and are dropped with the comment)
    if (ieq(i, "<!--")) { size_t e = h.find("-->", i + 4); i = e == string::npos ? n : e + 3; continue; }
    // downlevel-revealed conditionals: <![if !supportLists]> ... <![endif]>  — the hidden bullet text is skipped
    if (ieq(i, "<![if !supportLists]>")) { size_t e = h.find("<![endif]>", i); i = e == string::npos ? n : e + 10; continue; }
    if (ieq(i, "<![")) { size_t e = h.find(">", i); i = e == string::npos ? n : e + 1; continue; }
    if (ieq(i, "<!") || ieq(i, "<?")) { size_t e = h.find('>', i); i = e == string::npos ? n : e + 1; continue; }
    size_t e = i + 1;
    // find the end of the tag honouring quotes
    char q = 0;
    while (e < n) { char c = h[e]; if (q) { if (c == q) q = 0; } else if (c == '"' || c == '\'') q = c; else if (c == '>') break; e++; }
    if (e >= n) break;
    string body = h.substr(i + 1, e - i - 1);
    i = e + 1;
    flush();
    Tag t;
    size_t p = 0;
    if (!body.empty() && body[0] == '/') { t.close = true; p = 1; }
    while (p < body.size() && isspace(uint8_t(body[p]))) p++;
    size_t ns = p;
    while (p < body.size() && !isspace(uint8_t(body[p])) && body[p] != '/' && body[p] != '>') p++;
    t.name = lower(body.substr(ns, p - ns));  // namespaced tags (o:p, w:sdt, v:shape) keep their prefix and stay transparent
    if (!body.empty() && body.back() == '/') t.selfClose = true;
    // attributes
    while (p < body.size()) {
      while (p < body.size() && (isspace(uint8_t(body[p])) || body[p] == '/')) p++;
      size_t as = p;
      while (p < body.size() && !isspace(uint8_t(body[p])) && body[p] != '=' && body[p] != '/') p++;
      string an = lower(body.substr(as, p - as));
      if (an.empty()) break;
      string av;
      size_t pp = p;
      while (pp < body.size() && isspace(uint8_t(body[pp]))) pp++;
      if (pp < body.size() && body[pp] == '=') {
        p = pp + 1;
        while (p < body.size() && isspace(uint8_t(body[p]))) p++;
        if (p < body.size() && (body[p] == '"' || body[p] == '\'')) { char qq = body[p++]; size_t vs0 = p; while (p < body.size() && body[p] != qq) p++; av = body.substr(vs0, p - vs0); if (p < body.size()) p++; }
        else { size_t vs0 = p; while (p < body.size() && !isspace(uint8_t(body[p]))) p++; av = body.substr(vs0, p - vs0); }
      }
      t.attrs[an] = decodeEntities(av);
    }
    // script / style / xml / head content is not text
    if (!t.close && (t.name == "style" || t.name == "script" || t.name == "xml" || t.name == "title")) {
      string endTag = "</" + t.name;
      size_t ee = i;
      while (ee < n && !ieq(ee, endTag.c_str())) ee++;
      size_t gt = h.find('>', ee);
      i = gt == string::npos ? n : gt + 1;
      continue;
    }
    out.push_back(std::move(t));
  }
  flush();
  return out;
}

struct CssBits { bool bold = false, italic = false, under = false, strike = false, sup = false, sub = false, mark = false; bool boldOff = false, italicOff = false; string font; float size = 0; uint32_t color = 0; string msoList; string textAlign; };

CssBits parseCss(const string& css) {
  CssBits b;
  for (auto& decl : split(css, ';')) {
    size_t c = decl.find(':');
    if (c == string::npos) continue;
    string k = lower(trim(decl.substr(0, c))), v = lower(trim(decl.substr(c + 1)));
    if (k == "font-weight") { if (v == "bold" || v == "bolder" || (isdigit(uint8_t(v[0])) && atoi(v.c_str()) >= 600)) b.bold = true; else if (v == "normal" || v == "400") b.boldOff = true; }
    else if (k == "font-style") { if (v == "italic" || v == "oblique") b.italic = true; else if (v == "normal") b.italicOff = true; }
    else if (k == "text-decoration" || k == "text-decoration-line") { if (v.find("underline") != string::npos) b.under = true; if (v.find("line-through") != string::npos) b.strike = true; }
    else if (k == "vertical-align") { if (v == "super") b.sup = true; else if (v == "sub") b.sub = true; }
    else if (k == "background" || k == "background-color" || k == "mso-highlight") { if (v != "none" && v != "transparent" && v != "white" && v != "#ffffff" && v != "#fff") b.mark = true; }
    else if (k == "font-family") { string f = trim(decl.substr(c + 1)); f = split(f, ',')[0]; f = trim(f); if (f.size() > 1 && (f[0] == '"' || f[0] == '\'')) f = f.substr(1, f.size() - 2); if (f != "Symbol" && f != "Wingdings") b.font = f; }
    else if (k == "font-size") { if (v.size() > 2 && v.substr(v.size() - 2) == "pt") b.size = float(atof(v.c_str())); else if (v.size() > 2 && v.substr(v.size() - 2) == "px") b.size = float(atof(v.c_str())) * 0.75f; }
    else if (k == "color") { if (v.size() == 7 && v[0] == '#') b.color = 0xFF000000u | uint32_t(strtoul(v.c_str() + 1, nullptr, 16)); else if (v.size() == 4 && v[0] == '#') { uint32_t r = uint32_t(strtoul(v.substr(1, 1).c_str(), nullptr, 16)), g = uint32_t(strtoul(v.substr(2, 1).c_str(), nullptr, 16)), bb = uint32_t(strtoul(v.substr(3, 1).c_str(), nullptr, 16)); b.color = 0xFF000000u | (r * 17 << 16) | (g * 17 << 8) | (bb * 17); } }
    else if (k == "mso-list") b.msoList = v;
    else if (k == "text-align") b.textAlign = v;
  }
  if (b.color == 0xFF000000u) b.color = 0;  // black = automatic
  return b;
}

struct InlineState { uint16_t fmt = 0; string link, font; float size = 0; uint32_t color = 0; };

}  // namespace

// ---------------------------------------------------------------- copy
std::set<string> docFragmentCitedKeys(const DocFragment& f) {
  std::set<string> cited;
  auto scanPara = [&](const Para& p) { for (auto& s : p.spans) if (s.isCite()) for (auto& k : split(s.cite, ';')) if (!trim(k).empty()) cited.insert(trim(k)); };
  Para only;
  only.spans = f.spans;
  scanPara(only);
  for (auto& b : f.blocks) {
    scanPara(b.p);
    scanPara(b.fig.caption);
    for (auto& row : b.tbl.cells) for (auto& c : row) scanPara(c);
    if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference && !b.p.refKey.empty()) cited.insert(b.p.refKey);
  }
  return cited;
}

ClipHtml docFragmentToWordHtml(const Document& d, const DocFragment& f, const ClipOptions& o) {
  ClipHtml out;
  const PageSetup& ps = d.setup;
  // the blocks to emit (a spans-only fragment becomes one paragraph)
  vector<Block> blocks = f.blocks;
  if (blocks.empty()) { Block b; b.p.spans = f.spans; blocks.push_back(b); }
  // which references are cited inside the fragment
  std::set<string> cited = docFragmentCitedKeys(f);
  // tags are computed over the whole document (stable across several copies from the same document); only the cited sources travel
  std::map<string, string> tags;
  string sourcesXml;
  if (!cited.empty() && !d.refs.empty()) sourcesXml = docBibSourcesXml(d, tags, &cited);
  Emit em{d, tags, 0};

  // the reference list: the document's own Reference paragraphs for the cited keys (their numbers as in the document)
  vector<const Para*> bib;
  bool fragmentHasBib = false;
  for (auto& b : blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference && !b.p.refKey.empty()) fragmentHasBib = true;
  if (o.bibliography && !fragmentHasBib && !cited.empty())
    for (auto& b : d.blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference && !b.p.refKey.empty() && cited.count(b.p.refKey)) bib.push_back(&b.p);

  string fam = ps.font, hfam = ps.headingFamily();
  string famCss = "\"" + hEsc(fam) + "\"," + (ps.serif() ? "serif" : "sans-serif");
  string hfamCss = "\"" + hEsc(hfam) + "\"," + (PageSetup::serifFamily(hfam) ? "serif" : "sans-serif");
  string base = fmtNum(ps.baseSize, 1);
  string lh = std::to_string(int(std::lround(ps.lineSpacing * 100))) + "%";
  string& h = out.html;
  h = "<html xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" xmlns:w=\"urn:schemas-microsoft-com:office:word\" xmlns=\"http://www.w3.org/TR/REC-html40\">\n<head>\n";
  h += "<meta http-equiv=Content-Type content=\"text/html; charset=utf-8\">\n<meta name=ProgId content=Word.Document>\n";
  h += "<meta name=Generator content=\"VOSStudio Native " + appVersion() + "\">\n<meta name=Originator content=\"VOSStudio Native " + appVersion() + "\">\n";
  if (!sourcesXml.empty()) {
    if (!o.fileDir.empty()) {
      out.files.push_back({"clip_sources.xml", sourcesXml});
      out.files.push_back({"clip_sourcesprops.xml", docBibSourcesPropsXml()});
      h += "<link rel=dataStoreItem href=\"" + o.fileUrlBase + "/clip_sources.xml\" target=\"" + o.fileUrlBase + "/clip_sourcesprops.xml\">\n";
    }
  }
  h += "<!--[if gte mso 9]><xml>\n <o:OfficeDocumentSettings>\n  <o:AllowPNG/>\n </o:OfficeDocumentSettings>\n</xml><![endif]-->\n";
  h += "<!--[if gte mso 9]><xml>\n <w:WordDocument>\n  <w:View>Normal</w:View>\n  <w:Zoom>0</w:Zoom>\n  <w:TrackMoves/>\n  <w:TrackFormatting/>\n  <w:PunctuationKerning/>\n  <w:ValidateAgainstSchemas/>\n  <w:SaveIfXMLInvalid>false</w:SaveIfXMLInvalid>\n  <w:IgnoreMixedContent>false</w:IgnoreMixedContent>\n  <w:AlwaysShowPlaceholderText>false</w:AlwaysShowPlaceholderText>\n  <w:DoNotPromoteQF/>\n  <w:LidThemeOther>EN-US</w:LidThemeOther>\n  <w:Compatibility>\n   <w:BreakWrappedTables/>\n   <w:SnapToGridInCell/>\n   <w:WrapTextWithPunct/>\n   <w:UseAsianBreakRules/>\n   <w:DontGrowAutofit/>\n   <w:SplitPgBreakAndParaMark/>\n   <w:EnableOpenTypeKerning/>\n   <w:DontFlipMirrorIndents/>\n   <w:OverrideTableStyleHps/>\n  </w:Compatibility>\n  <w:BrowserLevel>MicrosoftInternetExplorer4</w:BrowserLevel>\n </w:WordDocument>\n</xml><![endif]-->\n";
  h += "<style>\n<!--\n";
  h += " p.MsoNormal, li.MsoNormal, div.MsoNormal\n\t{mso-style-unhide:no;\n\tmso-style-qformat:yes;\n\tmso-style-parent:\"\";\n\tmargin-top:0in;\n\tmargin-right:0in;\n\tmargin-bottom:8.0pt;\n\tmargin-left:0in;\n\tline-height:" + lh + ";\n\tmso-pagination:widow-orphan;\n\tfont-size:" + base + "pt;\n\tfont-family:" + famCss + ";}\n";
  auto heading = [&](int lvl, const char* size, const char* color, const char* before, bool italic) {
    h += "h" + std::to_string(lvl) + "\n\t{mso-style-priority:9;\n\tmso-style-unhide:no;\n\tmso-style-qformat:yes;\n\tmso-style-link:\"Heading " + std::to_string(lvl) + " Char\";\n\tmso-style-next:Normal;\n\tmargin-top:" + before + "pt;\n\tmargin-right:0in;\n\tmargin-bottom:4.0pt;\n\tmargin-left:0in;\n\tline-height:" + lh + ";\n\tmso-pagination:widow-orphan lines-together;\n\tpage-break-after:avoid;\n\tmso-outline-level:" + std::to_string(lvl) + ";\n\tfont-size:" + size + "pt;\n\tfont-family:" + hfamCss + ";\n\tcolor:" + color + ";\n\tfont-weight:bold;" + (italic ? "\n\tfont-style:italic;" : "") + "}\n";
  };
  heading(1, "16.0", "#2F5496", "12.0", false);
  heading(2, "13.0", "#2F5496", "10.0", false);
  heading(3, "12.0", "#1F3763", "8.0", false);
  heading(4, "11.0", "#2F5496", "6.0", true);
  h += "p.MsoTitle, li.MsoTitle, div.MsoTitle\n\t{mso-style-priority:10;\n\tmso-style-unhide:no;\n\tmso-style-qformat:yes;\n\tmso-style-link:\"Title Char\";\n\tmso-style-next:Normal;\n\tmargin:0in 0in 6.0pt 0in;\n\tline-height:110%;\n\tfont-size:26.0pt;\n\tfont-family:" + hfamCss + ";\n\tfont-weight:bold;}\n";
  h += "p.MsoSubtitle, li.MsoSubtitle, div.MsoSubtitle\n\t{mso-style-priority:11;\n\tmso-style-unhide:no;\n\tmso-style-qformat:yes;\n\tmso-style-link:\"Subtitle Char\";\n\tmso-style-next:Normal;\n\tmargin:0in 0in 8.0pt 0in;\n\tfont-size:14.0pt;\n\tfont-family:" + famCss + ";\n\tcolor:#595959;\n\tletter-spacing:.75pt;}\n";
  h += "p.MsoCaption, li.MsoCaption, div.MsoCaption\n\t{mso-style-priority:35;\n\tmso-style-unhide:no;\n\tmso-style-qformat:yes;\n\tmso-style-next:Normal;\n\tmargin:0in 0in 10.0pt 0in;\n\tfont-size:9.0pt;\n\tfont-family:" + famCss + ";\n\tcolor:#44546A;\n\tfont-style:italic;}\n";
  h += "p.MsoQuote, li.MsoQuote, div.MsoQuote\n\t{mso-style-priority:29;\n\tmso-style-unhide:no;\n\tmso-style-qformat:yes;\n\tmso-style-link:\"Quote Char\";\n\tmso-style-next:Normal;\n\tmargin:8.0pt 0in 8.0pt 0in;\n\ttext-align:center;\n\tfont-size:" + base + "pt;\n\tfont-family:" + famCss + ";\n\tcolor:#404040;\n\tfont-style:italic;}\n";
  h += "p.MsoListParagraph, li.MsoListParagraph, div.MsoListParagraph\n\t{mso-style-priority:34;\n\tmso-style-unhide:no;\n\tmso-style-qformat:yes;\n\tmargin:0in 0in 8.0pt .5in;\n\tmso-add-space:auto;\n\tline-height:" + lh + ";\n\tfont-size:" + base + "pt;\n\tfont-family:" + famCss + ";}\n";
  h += "p.MsoBibliography, li.MsoBibliography, div.MsoBibliography\n\t{mso-style-priority:37;\n\tmso-style-unhide:no;\n\tmso-style-next:Normal;\n\tmargin:0in 0in 8.0pt .5in;\n\ttext-indent:-.5in;\n\tline-height:" + lh + ";\n\tfont-size:" + base + "pt;\n\tfont-family:" + famCss + ";}\n";
  h += "pre\n\t{mso-style-name:\"HTML Preformatted\";\n\tmso-style-priority:99;\n\tmargin:0in 0in 8.0pt 0in;\n\tfont-size:10.0pt;\n\tfont-family:Consolas,\"Courier New\",monospace;\n\tbackground:#F3F3F6;}\n";
  h += "span.MsoHyperlink\n\t{mso-style-priority:99;\n\tcolor:#0563C1;\n\ttext-decoration:underline;}\n";
  h += "table.MsoTableGrid\n\t{mso-style-name:\"Table Grid\";\n\tmso-tstyle-rowband-size:0;\n\tmso-tstyle-colband-size:0;\n\tmso-style-priority:39;\n\tmso-style-unhide:no;\n\tborder:solid windowtext 1.0pt;\n\tmso-border-alt:solid windowtext .5pt;\n\tmso-padding-alt:0in 5.4pt 0in 5.4pt;\n\tmso-border-insideh:.5pt solid windowtext;\n\tmso-border-insidev:.5pt solid windowtext;\n\tmso-para-margin:0in;\n\tfont-size:" + base + "pt;\n\tfont-family:" + famCss + ";}\n";
  h += "-->\n</style>\n</head>\n\n<body lang=EN-US style='tab-interval:.5in;word-wrap:break-word'>\n<!--StartFragment-->\n";

  vector<int> nums = docListNumbers(d);  // numbers in the source document (the fragment's blocks may be a subset: numbering restarts, which is what Word does too)
  Document sub;
  sub.blocks = blocks;
  sub.setup = ps;
  vector<int> subNums = docListNumbers(sub);
  vector<string> subHn = docHeadingNumbers(sub);
  string listOpen;
  int listLevel = 0;
  auto closeLists = [&]() { while (listLevel > 0) { h += "</" + listOpen + ">\n"; listLevel--; } listOpen.clear(); };
  bool vmlShapeType = false;  // the t75 picture shape type is defined once, before the first picture
  int figN = 0, tblN = 0;
  bool bibOpen = false;  // inside the BIBLIOGRAPHY field
  auto para = [&](const string& cls, const Para& p, const string& body, const string& extraStyle = "") {
    string a = alignStyle(p.align);
    if (!extraStyle.empty()) { if (a.find("style='") != string::npos) a = replaceAll(a, "style='", "style='" + extraStyle); else a += " style='" + extraStyle + "'"; }
    h += "<p class=" + cls + a + ">" + (body.empty() ? "&nbsp;" : body) + "<o:p></o:p></p>\n";
  };
  for (size_t k = 0; k < blocks.size(); k++) {
    const Block& b = blocks[k];
    if (b.kind == Block::Paragraph) {
      const Para& p = b.p;
      if (isList(p.style)) {
        string want = p.style == PStyle::Bullet ? "ul" : "ol";
        int lvl = std::min(2, std::max(0, p.level)) + 1;
        if (listOpen != want && listLevel > 0) closeLists();
        listOpen = want;
        while (listLevel > lvl) { h += "</" + listOpen + ">\n"; listLevel--; }
        while (listLevel < lvl) { h += "<" + want + (want == "ol" && listLevel + 1 == lvl && subNums[k] > 1 && listLevel == 0 ? " start=" + std::to_string(subNums[k]) : string()) + " type=" + (want == "ol" ? (lvl == 2 ? "a" : lvl == 3 ? "i" : "1") : (lvl == 2 ? "circle" : lvl == 3 ? "square" : "disc")) + ">\n"; listLevel++; }
        h += "<li class=MsoNormal" + string(p.align != PAlign::Left ? alignStyle(p.align) : "") + ">" + em.spans(p.spans) + "<o:p></o:p></li>\n";
        continue;
      }
      closeLists();
      string body = em.spans(p.spans);
      switch (p.style) {
        case PStyle::Title: para("MsoTitle", p, body); break;
        case PStyle::Subtitle: para("MsoSubtitle", p, body); break;
        case PStyle::Meta: para("MsoNormal", p, body, "color:#595959;"); break;
        case PStyle::H1: case PStyle::H2: case PStyle::H3: case PStyle::H4: {
          string tag = "h" + std::to_string(headingLevel(p.style));
          string pre = ps.numberedHeadings && !subHn[k].empty() ? hEsc(subHn[k]) + "  " : string();
          h += "<" + tag + alignStyle(p.align) + ">" + pre + body + "<o:p></o:p></" + tag + ">\n";
          break;
        }
        case PStyle::Quote: para("MsoQuote", p, body); break;
        case PStyle::Code: h += "<pre>" + hEsc(p.text()) + "</pre>\n"; break;
        case PStyle::Caption: {
          // a table caption gets a SEQ Table field so Word's cross-references and "Insert Caption" continue the numbering
          bool forTable = (k + 1 < blocks.size() && blocks[k + 1].kind == Block::TableBlock) || (k > 0 && blocks[k - 1].kind == Block::TableBlock);
          string label;
          if (forTable && !captionLabelled(p.text())) label = "Table " + field("SEQ Table \\* ARABIC", std::to_string(++tblN)) + ". ";
          para("MsoCaption", p, label + body);
          break;
        }
        case PStyle::Reference: {
          if (!p.refKey.empty() && !tags.empty()) {
            // Word's own bibliography: the entries are the result of one BIBLIOGRAPHY field
            string open = !bibOpen ? string(kFieldBegin) + " BIBLIOGRAPHY " + kFieldSep : string();
            bibOpen = true;
            bool last = true;
            for (size_t j = k + 1; j < blocks.size(); j++) if (blocks[j].kind == Block::Paragraph && blocks[j].p.style == PStyle::Reference && !blocks[j].p.refKey.empty()) { last = false; break; }
            string close = last ? string(kFieldEnd) : string();
            h += "<p class=MsoBibliography>" + open + body + close + "<o:p></o:p></p>\n";
            if (last) bibOpen = false;
          } else para("MsoBibliography", p, body);
          break;
        }
        default: para("MsoNormal", p, body); break;
      }
    } else if (b.kind == Block::FigureBlock) {
      closeLists();
      out.figures++;
      figN++;
      string label = captionLabelled(b.fig.caption.text()) || b.fig.label == "-" ? string() : hEsc(b.fig.labelText()) + " " + field("SEQ " + b.fig.labelText() + " \\* ARABIC", std::to_string(figN)) + ". ";
      string caption = "<p class=MsoCaption" + alignStyle(b.fig.caption.align == PAlign::Left && b.fig.align == PAlign::Center ? PAlign::Center : b.fig.caption.align) + ">" + label + em.spans(b.fig.caption.spans) + "<o:p></o:p></p>\n";
      if (b.fig.captionAbove) h += caption;
      string img;
      if (b.fig.asset >= 0 && b.fig.asset < int(d.assets.size()) && o.png) {
        const Scene& sc = d.assets[size_t(b.fig.asset)].scene;
        if (sc.W > 0 && sc.H > 0) {
          string bytes = o.png(sc, o.dpi);
          if (!bytes.empty()) {
            double wpx = ps.textW() / 72.0 * 96.0 * b.fig.widthPct / 100.0, hpx = wpx * sc.H / sc.W;
            string alt = hEsc(b.fig.alt.empty() ? b.fig.caption.text() : b.fig.alt);
            string src;
            if (!o.fileDir.empty()) {
              char name[48];
              snprintf(name, sizeof name, "clip_image%03d.png", out.figures);
              out.files.push_back({name, bytes});
              src = o.fileUrlBase + "/" + name;
            } else src = "data:image/png;base64," + base64(bytes);
            string border = b.fig.border ? " style='border:solid #B7BAC0 1.0pt'" : "";
            img = "<img width=" + std::to_string(int(std::lround(wpx))) + " height=" + std::to_string(int(std::lround(hpx))) + " src=\"" + src + "\" alt=\"" + alt + "\"" + border + ">";
            // vector figures: Word reads the picture from a VML shape (an Enhanced Metafile file, as in its own
            // clipboard HTML); everything else sees only the <img>
            if (o.emf && !o.fileDir.empty() && b.fig.formatOr(ps.figureFormat) == "vector") {
              double wpt = ps.textW() * b.fig.widthPct / 100.0, hpt = wpt * sc.H / sc.W;
              string emf = o.emf(sc, wpt, hpt);
              if (!emf.empty()) {
                char ename[48];
                snprintf(ename, sizeof ename, "clip_image%03d.emf", out.figures);
                out.files.push_back({ename, emf});
                string wpts = fmtFixed(wpt, 2), hpts = fmtFixed(hpt, 2);
                string shape = string(vmlShapeType ? "" : kVmlPictureType) +
                               "<v:shape id=\"Picture_x0020_" + std::to_string(out.figures) + "\" o:spid=\"_x0000_i10" + std::to_string(24 + out.figures) + "\" type=\"#_x0000_t75\" alt=\"" + alt +
                               "\" style='width:" + wpts + "pt;height:" + hpts + "pt;visibility:visible;mso-wrap-style:square'>\n" +
                               " <v:imagedata src=\"" + o.fileUrlBase + "/" + ename + "\" o:title=\"\"/>\n</v:shape>";
                vmlShapeType = true;
                img = "<!--[if gte vml 1]>" + shape + "<![endif]--><![if !vml]>" + img + "<![endif]>";
              }
            }
          }
        }
      }
      if (img.empty()) img = "<span style='color:#888888'>[figure]</span>";
      h += "<p class=MsoNormal" + alignStyle(b.fig.align == PAlign::Left ? PAlign::Left : b.fig.align == PAlign::Right ? PAlign::Right : PAlign::Center) + ">" + img + "<o:p></o:p></p>\n";
      if (!b.fig.captionAbove) h += caption;
    } else if (b.kind == Block::TableBlock) {
      closeLists();
      const Table& t = b.tbl;
      int R = t.rows(), C = t.cols();
      if (!R || !C) continue;
      double totalPt = ps.textW();
      h += "<table class=MsoTableGrid border=1 cellspacing=0 cellpadding=0 width=\"100%\" style='width:100.0%;border-collapse:collapse;border:none;mso-border-alt:solid windowtext .5pt;mso-yfti-tbllook:1184;mso-padding-alt:0in 5.4pt 0in 5.4pt'>\n";
      for (int r = 0; r < R; r++) {
        bool hdr = t.header && r == 0;
        h += "<tr" + string(r == 0 ? " style='mso-yfti-irow:0;mso-yfti-firstrow:yes" : " style='mso-yfti-irow:" + std::to_string(r)) + (r == R - 1 ? ";mso-yfti-lastrow:yes'" : "'") + ">\n";
        for (int c = 0; c < C; c++) {
          const Para& cell = t.cells[size_t(r)][size_t(c)];
          double frac = c < int(t.widths.size()) && t.widths[size_t(c)] > 0 ? t.widths[size_t(c)] : 1.0 / C;
          char a = c < int(t.align.size()) ? t.align[size_t(c)] : 'a';
          PAlign pa = cell.align != PAlign::Left ? cell.align : a == 'r' ? PAlign::Right : a == 'c' ? PAlign::Center : PAlign::Left;
          h += "<td width=" + std::to_string(int(std::lround(frac * totalPt / 72.0 * 96.0))) + " valign=top style='width:" + fmtNum(frac * totalPt, 1) + "pt;border:solid windowtext 1.0pt;mso-border-alt:solid windowtext .5pt;padding:0in 5.4pt 0in 5.4pt'>\n";
          string body = em.spans(cell.spans);
          if (hdr) body = "<b>" + body + "</b>";
          h += "<p class=MsoNormal" + alignStyle(pa) + " style='margin-bottom:0in;line-height:normal'>" + (body.empty() ? "&nbsp;" : body) + "<o:p></o:p></p>\n</td>\n";
        }
        h += "</tr>\n";
      }
      h += "</table>\n";
    } else if (b.kind == Block::Rule) { closeLists(); h += "<hr size=1 width=\"100%\" align=center>\n"; }
    else if (b.kind == Block::PageBreak) { closeLists(); h += "<br clear=all style='mso-special-character:line-break;page-break-before:always'>\n"; }
    else if (b.kind == Block::Toc) { closeLists(); }
  }
  closeLists();
  if (!bib.empty()) {
    h += "<h1>References<o:p></o:p></h1>\n";
    for (size_t i = 0; i < bib.size(); i++) {
      string open = i == 0 ? string(kFieldBegin) + " BIBLIOGRAPHY " + kFieldSep : string();
      string close = i + 1 == bib.size() ? string(kFieldEnd) : string();
      h += "<p class=MsoBibliography>" + open + em.spans(bib[i]->spans) + close + "<o:p></o:p></p>\n";
    }
  }
  h += "<!--EndFragment-->\n</body>\n</html>\n";
  out.citations = em.citations;
  return out;
}

// ---------------------------------------------------------------- CF_HTML framing
string cfHtmlWrap(const string& html) {
  // the header's offsets are byte offsets into the final (UTF-8) buffer, header included; fixed-width numbers keep
  // the header length independent of the values
  const char* tmpl = "Version:0.9\r\nStartHTML:%010u\r\nEndHTML:%010u\r\nStartFragment:%010u\r\nEndFragment:%010u\r\nSourceURL:vosstudio:document\r\n";
  char probe[256];
  snprintf(probe, sizeof probe, tmpl, 0u, 0u, 0u, 0u);
  size_t hl = strlen(probe);
  size_t fs = html.find("<!--StartFragment-->"), fe = html.find("<!--EndFragment-->");
  if (fs == string::npos) fs = 0; else fs += strlen("<!--StartFragment-->");
  if (fe == string::npos) fe = html.size();
  char head[256];
  snprintf(head, sizeof head, tmpl, unsigned(hl), unsigned(hl + html.size()), unsigned(hl + fs), unsigned(hl + fe));
  return string(head) + html;
}

string cfHtmlFragment(const string& cf) {
  // prefer the explicit byte offsets; fall back to the markers, then to the whole body
  auto num = [&](const char* key) -> long { size_t p = cf.find(key); if (p == string::npos) return -1; return atol(cf.c_str() + p + strlen(key)); };
  long a = num("StartFragment:"), b = num("EndFragment:");
  if (a >= 0 && b > a && size_t(b) <= cf.size()) return cf.substr(size_t(a), size_t(b - a));
  size_t fs = cf.find("<!--StartFragment-->"), fe = cf.find("<!--EndFragment-->");
  if (fs != string::npos && fe != string::npos && fe > fs) return cf.substr(fs + 20, fe - fs - 20);
  size_t bs = cf.find("<body");
  if (bs != string::npos) { size_t gt = cf.find('>', bs); size_t be = cf.rfind("</body>"); if (gt != string::npos) return cf.substr(gt + 1, be == string::npos || be < gt ? string::npos : be - gt - 1); }
  size_t hs = cf.find('<');
  return hs == string::npos ? string() : cf.substr(hs);
}

// ---------------------------------------------------------------- paste
bool docFragmentFromHtml(const string& html0, DocFragment& out) {
  out = DocFragment();
  string html = html0;
  // a CF_HTML buffer or a whole document: take the body / fragment
  if (startsWith(html, "Version:") || html.find("<!--StartFragment-->") != string::npos) html = cfHtmlFragment(html);
  vector<Tag> toks = tokenize(html);
  if (toks.empty()) return false;

  vector<Block> blocks;
  Para cur;                    // paragraph being filled
  bool curOpen = false;        // a block-level element started it (otherwise loose text opens one)
  bool inPre = false, inTable = false;
  int preDepth = 0;
  vector<InlineState> st{InlineState()};
  vector<string> lists;        // "ul"/"ol" stack
  vector<int> listStartPending;
  Table tbl;
  vector<Para> row;
  vector<bool> rowIsHeader;
  bool cellOpen = false;
  Para cell;
  bool sawContent = false;
  auto cleanSpace = [&](Para& p) {  // collapse runs of blanks, trim the ends (Word pads with &nbsp; and newlines)
    if (inPre) return;
    for (auto& s : p.spans) {
      string o;
      o.reserve(s.text.size());
      for (size_t i = 0; i < s.text.size(); i++) {
        char c = s.text[i];
        if (c == '\r' || c == '\t') c = ' ';
        if (c == '\n') { if (!s.text.empty() && i > 0 && s.text[i - 1] == '\n') continue; c = ' '; }  // HTML newlines are blanks (soft breaks come in as <br>)
        if (c == '\xC2' && i + 1 < s.text.size() && s.text[i + 1] == '\xA0') { c = ' '; i++; }  // &nbsp; (Word's spacerun padding)
        if (c == ' ' && !o.empty() && o.back() == ' ') continue;
        o += c;
      }
      s.text = o;
    }
    // trim leading space of the first span and trailing of the last; join adjacent spaces across spans
    for (size_t i = 1; i < p.spans.size(); i++) if (!p.spans[i].text.empty() && p.spans[i].text[0] == ' ' && !p.spans[i - 1].text.empty() && p.spans[i - 1].text.back() == ' ') p.spans[i].text.erase(0, 1);
    while (!p.spans.empty() && p.spans.front().text.empty()) p.spans.erase(p.spans.begin());
    while (!p.spans.empty() && p.spans.back().text.empty()) p.spans.pop_back();
    if (!p.spans.empty()) { string& a = p.spans.front().text; while (!a.empty() && a[0] == ' ') a.erase(0, 1); string& z = p.spans.back().text; while (!z.empty() && z.back() == ' ') z.pop_back(); }
    while (!p.spans.empty() && p.spans.front().text.empty()) p.spans.erase(p.spans.begin());
    while (!p.spans.empty() && p.spans.back().text.empty()) p.spans.pop_back();
    for (auto& s : p.spans) s.text = replaceAll(replaceAll(replaceAll(s.text, " \xE2\x80\xA8", "\n"), "\xE2\x80\xA8 ", "\n"), "\xE2\x80\xA8", "\n");
    if (!p.spans.empty()) { string& a = p.spans.front().text; while (!a.empty() && a[0] == '\n') a.erase(0, 1); string& z = p.spans.back().text; while (!z.empty() && z.back() == '\n') z.pop_back(); }
    while (!p.spans.empty() && p.spans.front().text.empty()) p.spans.erase(p.spans.begin());
    while (!p.spans.empty() && p.spans.back().text.empty()) p.spans.pop_back();
  };
  auto flushPara = [&](bool force) {
    Para& p = cellOpen ? cell : cur;
    cleanSpace(p);
    if (cellOpen) return;  // cells are collected by the table code
    bool empty = p.spans.empty();
    if (!empty || force) {
      Block b;
      b.kind = Block::Paragraph;
      b.p = p;
      if (empty) b.p.spans.clear();
      // several empty paragraphs in a row collapse to one
      if (!(empty && !blocks.empty() && blocks.back().kind == Block::Paragraph && blocks.back().p.spans.empty())) blocks.push_back(b);
      sawContent = sawContent || !empty;
    }
    cur = Para();
    curOpen = false;
  };
  auto addText = [&](const string& raw) {
    Para& p = cellOpen ? cell : cur;
    string t = decodeEntities(raw);
    if (!inPre) { bool allSpace = true; for (char c : t) if (!isspace(uint8_t(c))) { allSpace = false; break; } if (allSpace && p.spans.empty()) return; }
    const InlineState& s = st.back();
    if (!p.spans.empty() && p.spans.back().fmt == s.fmt && p.spans.back().link == s.link && p.spans.back().font == s.font && p.spans.back().size == s.size && p.spans.back().color == s.color) p.spans.back().text += t;
    else { Span sp(t, s.fmt, s.link); sp.font = s.font; sp.size = s.size; sp.color = s.color; p.spans.push_back(sp); }
  };
  auto pushInline = [&](const Tag& t) {
    InlineState s = st.back();
    const string& n = t.name;
    if (n == "b" || n == "strong") s.fmt |= F_BOLD;
    else if (n == "i" || n == "em" || n == "cite" || n == "dfn") s.fmt |= F_ITALIC;
    else if (n == "u" || n == "ins") s.fmt |= F_UNDER;
    else if (n == "s" || n == "strike" || n == "del") s.fmt |= F_STRIKE;
    else if (n == "sub") s.fmt |= F_SUB;
    else if (n == "sup") s.fmt |= F_SUP;
    else if (n == "mark") s.fmt |= F_MARK;
    else if (n == "code" || n == "tt" || n == "kbd" || n == "samp") s.fmt |= F_CODE;
    else if (n == "a") { auto it = t.attrs.find("href"); if (it != t.attrs.end() && (startsWith(lower(it->second), "http") || startsWith(lower(it->second), "mailto:") || startsWith(lower(it->second), "doi:"))) s.link = it->second; }
    auto it = t.attrs.find("style");
    if (it != t.attrs.end()) {
      CssBits c = parseCss(it->second);
      if (c.bold) s.fmt |= F_BOLD;
      if (c.boldOff) s.fmt &= uint16_t(~F_BOLD);
      if (c.italic) s.fmt |= F_ITALIC;
      if (c.italicOff) s.fmt &= uint16_t(~F_ITALIC);
      if (c.under) s.fmt |= F_UNDER;
      if (c.strike) s.fmt |= F_STRIKE;
      if (c.sup) s.fmt |= F_SUP;
      if (c.sub) s.fmt |= F_SUB;
      if (c.mark) s.fmt |= F_MARK;
      if (!c.font.empty() && n == "span") s.font = c.font;
      if (c.size > 0 && n == "span") s.size = c.size;
      if (c.color) s.color = c.color;
    }
    st.push_back(s);
  };
  auto popInline = [&]() { if (st.size() > 1) st.pop_back(); };
  auto beginBlock = [&](PStyle style, int level, int numStart, PAlign align) {
    flushPara(false);
    cur = Para();
    cur.style = style;
    cur.level = level;
    cur.numStart = numStart;
    cur.align = align;
    curOpen = true;
  };
  auto paraStyleFor = [&](const Tag& t, PStyle& style, int& level, int& numStart) {
    style = PStyle::Body; level = 0; numStart = 0;
    const string& n = t.name;
    if (n == "h1") style = PStyle::H1;
    else if (n == "h2") style = PStyle::H2;
    else if (n == "h3") style = PStyle::H3;
    else if (n == "h4" || n == "h5" || n == "h6") style = PStyle::H4;
    else if (n == "blockquote") style = PStyle::Quote;
    else if (n == "li") { style = !lists.empty() && lists.back() == "ol" ? PStyle::Number : PStyle::Bullet; level = std::min(2, std::max(0, int(lists.size()) - 1)); }
    string cls;
    auto ci = t.attrs.find("class");
    if (ci != t.attrs.end()) cls = ci->second;
    string css;
    auto si = t.attrs.find("style");
    if (si != t.attrs.end()) css = si->second;
    CssBits cb = parseCss(css);
    if (cls.find("MsoTitle") != string::npos || cls == "Title") style = PStyle::Title;
    else if (cls.find("MsoSubtitle") != string::npos) style = PStyle::Subtitle;
    else if (cls.find("MsoCaption") != string::npos) style = PStyle::Caption;
    else if (cls.find("MsoQuote") != string::npos || cls.find("MsoIntenseQuote") != string::npos) style = PStyle::Quote;
    else if (cls.find("MsoBibliography") != string::npos) style = PStyle::Reference;
    else if (cls.find("MsoHeading") != string::npos || startsWith(cls, "Heading")) { int lv = 1; for (char c : cls) if (isdigit(uint8_t(c))) { lv = c - '0'; break; } style = headingStyle(lv); }
    else if (cls.find("MsoListBullet") != string::npos) { style = PStyle::Bullet; }
    else if (cls.find("MsoListNumber") != string::npos) { style = PStyle::Number; }
    else if (cls.find("MsoToc") != string::npos) style = PStyle::Body;
    if (!cb.msoList.empty()) {  // Word list paragraph: "l0 level2 lfo1"; the bullet text was hidden in <![if !supportLists]>
      int lv = 1;
      size_t lp = cb.msoList.find("level");
      if (lp != string::npos) lv = atoi(cb.msoList.c_str() + lp + 5);
      level = std::min(2, std::max(0, lv - 1));
      if (style != PStyle::Number && style != PStyle::Bullet) style = t.attrs.count("data-vs-numbered") ? PStyle::Number : PStyle::Bullet;
    }
  };
  auto alignFor = [&](const Tag& t) {
    PAlign a = PAlign::Left;
    auto ai = t.attrs.find("align");
    string v;
    if (ai != t.attrs.end()) v = lower(ai->second);
    auto si = t.attrs.find("style");
    if (si != t.attrs.end()) { CssBits cb = parseCss(si->second); if (!cb.textAlign.empty()) v = cb.textAlign; }
    if (v == "center") a = PAlign::Center;
    else if (v == "right") a = PAlign::Right;
    else if (v == "justify") a = PAlign::Justify;
    return a;
  };
  // Word marks numbered list paragraphs only through the hidden bullet text; peek at it before the tokenizer dropped
  // it: paragraphs whose raw HTML contains "<![if !supportLists]>" followed by a digit/letter and '.' or ')' are numbered
  {
    size_t p = 0;
    vector<bool> numbered;
    while ((p = html.find("<![if !supportLists]>", p)) != string::npos) {
      size_t e = html.find("<![endif]>", p);
      string inner = e == string::npos ? string() : html.substr(p, e - p);
      // strip tags
      string txt;
      bool inTag = false;
      for (char c : inner) { if (c == '<') inTag = true; else if (c == '>') inTag = false; else if (!inTag) txt += c; }
      txt = trim(decodeEntities(txt));
      bool num = !txt.empty() && (isdigit(uint8_t(txt[0])) || (isalpha(uint8_t(txt[0])) && txt.size() >= 2 && (txt[1] == '.' || txt[1] == ')')));
      numbered.push_back(num);
      p = e == string::npos ? html.size() : e;
    }
    // attach to the list-paragraph tags in order
    size_t idx = 0;
    for (auto& t : toks) {
      if (t.close || t.name != "p") continue;
      auto si = t.attrs.find("style");
      if (si == t.attrs.end() || si->second.find("mso-list") == string::npos) continue;
      if (idx < numbered.size() && numbered[idx]) t.attrs["data-vs-numbered"] = "1";
      idx++;
    }
  }

  for (size_t i = 0; i < toks.size(); i++) {
    const Tag& t = toks[i];
    if (t.name.empty()) { if (!t.text.empty()) { if (!curOpen && !cellOpen) { bool blank = true; for (char c : t.text) if (!isspace(uint8_t(c))) { blank = false; break; } if (blank) continue; beginBlock(PStyle::Body, 0, 0, PAlign::Left); } addText(t.text); } continue; }
    const string& n = t.name;
    if (n == "p" || n == "h1" || n == "h2" || n == "h3" || n == "h4" || n == "h5" || n == "h6" || n == "li" || n == "blockquote" || n == "div" || n == "pre" || n == "center" || n == "dd" || n == "dt") {
      if (n == "div" && !t.close) continue;  // containers: their paragraphs carry the text; a div with loose text opens a body paragraph on demand
      if (n == "div") { flushPara(false); continue; }
      if (cellOpen) {  // a paragraph inside a table cell: its alignment is the cell's; a second one continues after a soft break
        if (!t.close) { PAlign a = alignFor(t); if (a != PAlign::Left) cell.align = a; if (!cell.spans.empty()) addText("\xE2\x80\xA8"); }
        continue;
      }
      if (t.close) { flushPara(n != "li"); if (n == "pre") { preDepth = std::max(0, preDepth - 1); inPre = preDepth > 0; } continue; }
      PStyle style; int level, numStart;
      paraStyleFor(t, style, level, numStart);
      if (n == "pre") { preDepth++; inPre = true; style = PStyle::Code; }
      if (n == "li" && style == PStyle::Number && !listStartPending.empty() && listStartPending.back() > 0) { numStart = listStartPending.back(); listStartPending.back() = 0; }
      beginBlock(style, level, numStart, alignFor(t));
      continue;
    }
    if (n == "br") { if (curOpen || cellOpen) addText(inPre ? "\n" : "\xE2\x80\xA8"); continue; }  // U+2028: a soft break that survives the blank collapsing
    if (n == "ul" || n == "ol") {
      if (!t.close) { flushPara(false); lists.push_back(n); auto st0 = t.attrs.find("start"); listStartPending.push_back(n == "ol" && st0 != t.attrs.end() ? std::max(0, atoi(st0->second.c_str())) : 0); }
      else { flushPara(false); if (!lists.empty()) lists.pop_back(); if (!listStartPending.empty()) listStartPending.pop_back(); }
      continue;
    }
    if (n == "table") {
      if (!t.close) { flushPara(false); if (inTable) continue; inTable = true; tbl = Table(); row.clear(); rowIsHeader.clear(); }
      else {
        if (!inTable) continue;
        inTable = false;
        if (!row.empty()) { tbl.cells.push_back(row); row.clear(); }
        if (!tbl.cells.empty()) {
          tbl.fixup();
          tbl.header = !rowIsHeader.empty() && rowIsHeader[0];
          if (!tbl.header && !tbl.cells.empty()) {  // Word has no <th>: a first row that is all bold is a header
            bool allBold = true;
            for (auto& c : tbl.cells[0]) { if (c.spans.empty()) continue; for (auto& s : c.spans) if (!(s.fmt & F_BOLD) && !trim(s.text).empty()) allBold = false; }
            tbl.header = allBold;
          }
          if (tbl.header) for (auto& c : tbl.cells[0]) for (auto& s : c.spans) s.fmt &= uint16_t(~F_BOLD);
          Block b;
          b.kind = Block::TableBlock;
          b.tbl = tbl;
          blocks.push_back(b);
          sawContent = true;
        }
      }
      continue;
    }
    if (inTable && (n == "tr")) {
      if (!t.close) { if (!row.empty()) { tbl.cells.push_back(row); row.clear(); } rowIsHeader.push_back(false); }
      else { if (cellOpen) { cleanSpace(cell); row.push_back(cell); cellOpen = false; cell = Para(); } if (!row.empty()) { tbl.cells.push_back(row); row.clear(); } }
      continue;
    }
    if (inTable && (n == "td" || n == "th")) {
      if (!t.close) { if (cellOpen) { cleanSpace(cell); row.push_back(cell); } cellOpen = true; cell = Para(); cell.align = alignFor(t); if (n == "th" && !rowIsHeader.empty()) rowIsHeader.back() = true; if (n == "th") st.push_back(st.back()), st.back().fmt |= F_BOLD; }
      else { if (cellOpen) { cleanSpace(cell); row.push_back(cell); cellOpen = false; cell = Para(); } if (n == "th") popInline(); }
      continue;
    }
    if (n == "hr") { flushPara(false); Block b; b.kind = Block::Rule; blocks.push_back(b); continue; }
    if (n == "img" || n == "object" || n == "v:shape" || n == "v:imagedata" || n == "input" || n == "button") continue;
    if (n == "b" || n == "strong" || n == "i" || n == "em" || n == "u" || n == "s" || n == "strike" || n == "del" || n == "ins" || n == "sub" || n == "sup" || n == "mark" || n == "code" || n == "tt" || n == "kbd" || n == "samp" || n == "a" || n == "span" || n == "font" || n == "cite" || n == "dfn" || n == "small" || n == "big" || n == "label" || n == "abbr" || n == "q") {
      if (t.close) popInline();
      else if (!t.selfClose) pushInline(t);
      continue;
    }
    // anything else (body, html, head, thead, tbody, colgroup, o:p, w:sdt ...) is transparent
  }
  flushPara(false);
  if (blocks.empty()) return false;
  // drop trailing empty paragraphs
  while (!blocks.empty() && blocks.back().kind == Block::Paragraph && blocks.back().p.spans.empty()) blocks.pop_back();
  while (!blocks.empty() && blocks.front().kind == Block::Paragraph && blocks.front().p.spans.empty()) blocks.erase(blocks.begin());
  if (blocks.empty()) return false;
  // a single plain paragraph pastes inline (into the paragraph at the caret), as text does
  if (blocks.size() == 1 && blocks[0].kind == Block::Paragraph && blocks[0].p.style == PStyle::Body) { out.spans = blocks[0].p.spans; return !out.spans.empty(); }
  out.blocks = blocks;
  return true;
}

}  // namespace vs
