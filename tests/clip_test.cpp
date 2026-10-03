// Clipboard exchange with Word: the Word-dialect HTML the writer copies (styles, lists, tables, pictures, CITATION /
// SEQ / BIBLIOGRAPHY fields, the dataStoreItem with the sources), the CF_HTML framing, and the importer that reads
// Word's and browsers' HTML back into blocks.
//   g++ -std=c++17 -O1 tests/clip_test.cpp src/core/*.cpp -Isrc/core -o build/clip_test -pthread && ./build/clip_test [outdir]
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>

#include "common.h"
#include "doc.h"
#include "docx.h"
#include "model.h"

using namespace vs;

static int fails = 0;
#define CHECK(c)                                                                     \
  do {                                                                               \
    if (!(c)) { fails++; std::cerr << "FAIL " << __LINE__ << ": " #c << "\n"; }      \
  } while (0)

static size_t count(const string& s, const string& sub) {
  size_t n = 0;
  for (size_t p = s.find(sub); p != string::npos; p = s.find(sub, p + sub.size())) n++;
  return n;
}

static string plain(const Para& p) { return p.text(); }

static Document sample() {
  Document d;
  d.setup.font = "Calibri";
  d.citeStyle = "apa";
  d.blocks.clear();
  d.blocks.push_back(Block::para("Mapping science", PStyle::Title));
  d.blocks.push_back(Block::para("1 Introduction", PStyle::H1));
  {
    Block b = Block::para("Networks matter ", PStyle::Body);
    b.p.spans.push_back(Span("a lot", F_BOLD));
    b.p.spans.push_back(Span(" and ", 0));
    b.p.spans.push_back(Span("links", F_ITALIC | F_UNDER, "https://example.org/x"));
    b.p.spans.push_back(Span(" x", F_SUB));
    b.p.spans.push_back(Span(" CO", 0));
    b.p.spans.push_back(Span("2", F_SUB));
    d.blocks.push_back(b);
  }
  d.blocks.push_back(Block::para("first point", PStyle::Bullet));
  d.blocks.push_back(Block::para("second point", PStyle::Bullet));
  { Block b = Block::para("nested", PStyle::Bullet); b.p.level = 1; d.blocks.push_back(b); }
  d.blocks.push_back(Block::para("step one", PStyle::Number));
  d.blocks.push_back(Block::para("step two", PStyle::Number));
  d.blocks.push_back(Block::para("Table 1. Counts", PStyle::Caption));
  {
    Block b;
    b.kind = Block::TableBlock;
    b.tbl.cells = {{Para(), Para()}, {Para(), Para()}};
    b.tbl.cells[0][0].spans = {Span("Country", 0)};
    b.tbl.cells[0][1].spans = {Span("Papers", 0)};
    b.tbl.cells[1][0].spans = {Span("Bangladesh", 0)};
    b.tbl.cells[1][1].spans = {Span("42", 0)};
    b.tbl.fixup();
    b.tbl.header = true;
    d.blocks.push_back(b);
  }
  {
    FigAsset a;
    a.scene.W = 300;
    a.scene.H = 200;
    Prim r;
    r.type = Prim::Rect; r.x = 10; r.y = 10; r.w = 280; r.h = 180; r.fill = true; r.fillC = Color(0.9f, 0.93f, 0.98f);
    a.scene.items.push_back(r);
    a.id = "publications_per_year";
    d.assets.push_back(a);
    Block f;
    f.kind = Block::FigureBlock;
    f.fig.asset = 0;
    f.fig.widthPct = 80;
    f.fig.caption.spans = {Span("Publications per year", 0)};
    d.blocks.push_back(f);
  }
  d.blocks.push_back(Block::para("A quote.", PStyle::Quote));
  d.blocks.push_back(Block::para("x = 1\ny = 2", PStyle::Code));
  return d;
}

// a document with two citations and a generated reference list
static Document cited(Editor& e) {
  Document d = sample();
  e.attach(&d);
  Record r;
  r.title = "Mapping science";
  r.year = 2021;
  r.authors = {"Doe, Jane A.", "Roe, Richard"};
  r.source = "Scientometrics";
  r.doi = "10.1007/s11192-021-0001";
  RefEntry re = refFromRecord(r);
  Record r2;
  r2.title = "Second paper";
  r2.year = 2019;
  r2.authors = {"Smith, John"};
  r2.source = "Journal of Things";
  RefEntry re2 = refFromRecord(r2);
  docAddReference(d, re);
  docAddReference(d, re2);
  e.setCaret({2, -1, int(d.blocks[2].p.text().size())});
  CHECK(e.insertCitation(vector<string>{re.key}));
  e.setCaret({10, -1, 0});
  CHECK(e.insertCitation(vector<string>{re2.key, re.key}));
  e.refreshCitations();
  return d;
}

int main(int argc, char** argv) {
  string outdir = argc > 1 ? argv[1] : "";
  auto png = [](const Scene&, double) { return string("\x89PNG\r\n\x1a\n-fake-", 14); };

  // ---- 1. the Word HTML of a whole document (as "select all, copy" produces)
  {
    Document d = sample();
    DocFragment f;
    f.blocks = d.blocks;
    ClipOptions o;
    o.fileDir = "C:\\Temp\\clip";
    o.fileUrlBase = "file:///C:/Temp/clip";
    o.png = png;
    ClipHtml c = docFragmentToWordHtml(d, f, o);
    const string& h = c.html;
    CHECK(startsWith(h, "<html xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\""));
    CHECK(h.find("<meta name=ProgId content=Word.Document>") != string::npos);
    CHECK(h.find("<!--StartFragment-->") != string::npos && h.find("<!--EndFragment-->") != string::npos && h.find("<!--StartFragment-->") < h.find("<!--EndFragment-->"));
    CHECK(h.find("p.MsoNormal, li.MsoNormal, div.MsoNormal") != string::npos && h.find("font-family:\"Calibri\",sans-serif") != string::npos);
    CHECK(h.find("<p class=MsoTitle>Mapping science<o:p></o:p></p>") != string::npos);
    CHECK(h.find("<h1>1 Introduction<o:p></o:p></h1>") != string::npos);
    CHECK(h.find("<b>a lot</b>") != string::npos && h.find("<a href=\"https://example.org/x\"><u><i>links</i></u></a>") != string::npos && h.find("<sub>2</sub>") != string::npos);
    CHECK(count(h, "<ul type=disc>") == 1 && count(h, "<ul type=circle>") == 1 && count(h, "</ul>") == 2);
    CHECK(count(h, "<ol type=1>") == 1 && count(h, "<li class=MsoNormal>") == 5);
    CHECK(h.find("<table class=MsoTableGrid border=1 cellspacing=0 cellpadding=0") != string::npos && count(h, "<td width=") == 4 && h.find("<b>Country</b>") != string::npos);
    CHECK(h.find("<p class=MsoCaption>Table 1. Counts<o:p></o:p></p>") != string::npos);  // already labelled: no SEQ added
    {
      int wpx = int(std::lround(d.setup.textW() / 72.0 * 96.0 * 0.8)), hpx = int(std::lround(wpx * 200.0 / 300.0));
      CHECK(h.find("<img width=" + std::to_string(wpx) + " height=" + std::to_string(hpx) + " src=\"file:///C:/Temp/clip/clip_image001.png\" alt=\"Publications per year\">") != string::npos);
    }
    CHECK(h.find("Figure <!--[if supportFields]><span style='mso-element:field-begin'></span> SEQ Figure \\* ARABIC <span style='mso-element:field-separator'></span><![endif]-->1<!--[if supportFields]><span style='mso-element:field-end'></span><![endif]-->. Publications per year") != string::npos);
    CHECK(h.find("<p class=MsoQuote>A quote.<o:p></o:p></p>") != string::npos);
    CHECK(h.find("<pre>x = 1\ny = 2</pre>") != string::npos);
    CHECK(c.figures == 1 && c.files.size() == 1 && c.files[0].first == "clip_image001.png" && c.files[0].second.size() == 14);
    CHECK(h.find("dataStoreItem") == string::npos && c.citations == 0);  // no citations: no sources part
    // CF_HTML framing: the offsets point exactly at the fragment
    string cf = cfHtmlWrap(h);
    CHECK(startsWith(cf, "Version:0.9\r\nStartHTML:"));
    auto num = [&](const char* key) { size_t p = cf.find(key); return size_t(atol(cf.c_str() + p + strlen(key))); };
    size_t sh = num("StartHTML:"), eh = num("EndHTML:"), sf = num("StartFragment:"), ef = num("EndFragment:");
    CHECK(cf.substr(sh, 5) == "<html" && eh == cf.size());
    CHECK(cf.substr(sf - 20, 20) == "<!--StartFragment-->" && cf.substr(ef, 18) == "<!--EndFragment-->");
    CHECK(cfHtmlFragment(cf) == cf.substr(sf, ef - sf));
    if (!outdir.empty()) { FILE* fp = fopen((outdir + "/clip-sample.html").c_str(), "wb"); if (fp) { fwrite(h.data(), 1, h.size(), fp); fclose(fp); } }

    // ---- 2. round trip through the importer keeps the structure
    DocFragment back;
    CHECK(docFragmentFromHtml(cf, back));
    CHECK(back.spans.empty() && back.blocks.size() >= 12);
    vector<Block>& b = back.blocks;
    CHECK(b[0].kind == Block::Paragraph && b[0].p.style == PStyle::Title && plain(b[0].p) == "Mapping science");
    CHECK(b[1].p.style == PStyle::H1 && plain(b[1].p) == "1 Introduction");
    CHECK(b[2].p.style == PStyle::Body && plain(b[2].p) == "Networks matter a lot and links x CO2");
    bool bold = false, link = false, sub = false;
    for (auto& s : b[2].p.spans) { if (s.text == "a lot" && (s.fmt & F_BOLD)) bold = true; if (s.text == "links" && s.link == "https://example.org/x" && (s.fmt & F_ITALIC) && (s.fmt & F_UNDER)) link = true; if (s.text == "2" && (s.fmt & F_SUB)) sub = true; }
    CHECK(bold && link && sub);
    CHECK(b[3].p.style == PStyle::Bullet && b[3].p.level == 0 && plain(b[3].p) == "first point");
    CHECK(b[5].p.style == PStyle::Bullet && b[5].p.level == 1 && plain(b[5].p) == "nested");
    CHECK(b[6].p.style == PStyle::Number && plain(b[6].p) == "step one" && b[7].p.style == PStyle::Number);
    CHECK(b[8].p.style == PStyle::Caption && plain(b[8].p) == "Table 1. Counts");
    CHECK(b[9].kind == Block::TableBlock && b[9].tbl.rows() == 2 && b[9].tbl.cols() == 2 && b[9].tbl.header && plain(b[9].tbl.cells[1][1]) == "42" && plain(b[9].tbl.cells[0][0]) == "Country");
    bool hdrBold = false;
    for (auto& s : b[9].tbl.cells[0][0].spans) if (s.fmt & F_BOLD) hdrBold = true;
    CHECK(!hdrBold);  // the header row is a table property, not bold runs
    // the picture paragraph is skipped (no image import) but its caption survives as a Caption with the number as text
    bool cap = false, quote = false, code = false;
    for (auto& x : b) { if (x.kind == Block::Paragraph && x.p.style == PStyle::Caption && plain(x.p) == "Figure 1. Publications per year") cap = true; if (x.kind == Block::Paragraph && x.p.style == PStyle::Quote && plain(x.p) == "A quote.") quote = true; if (x.kind == Block::Paragraph && x.p.style == PStyle::Code && plain(x.p).find("x = 1") == 0) code = true; }
    CHECK(cap && quote && code);
  }

  // ---- 3. citations: CITATION fields, the sources dataStoreItem, the BIBLIOGRAPHY field
  {
    Editor e;
    Document d = cited(e);
    CHECK(d.refs.size() == 2);
    int refParas = 0;
    for (auto& b : d.blocks) refParas += b.kind == Block::Paragraph && b.p.style == PStyle::Reference && !b.p.refKey.empty();
    CHECK(refParas == 2);
    // a) a body paragraph with one citation, plain copy: the field, the sources link, no bibliography
    DocFragment f;
    f.blocks = {d.blocks[2]};
    ClipOptions o;
    o.fileDir = "C:\\Temp\\clip";
    o.fileUrlBase = "file:///C:/Temp/clip";
    o.png = png;
    ClipHtml c = docFragmentToWordHtml(d, f, o);
    CHECK(c.citations == 1 && c.figures == 0);
    CHECK(c.html.find("<link rel=dataStoreItem href=\"file:///C:/Temp/clip/clip_sources.xml\" target=\"file:///C:/Temp/clip/clip_sourcesprops.xml\">") != string::npos);
    CHECK(c.files.size() == 2 && c.files[0].first == "clip_sources.xml" && c.files[1].first == "clip_sourcesprops.xml");
    const string& src = c.files[0].second;
    CHECK(src.find("<b:Sources SelectedStyle=\"\\APASixthEditionOfficeOnline.xsl\"") != string::npos || src.find("<b:Sources SelectedStyle=\"\\APA.XSL\"") != string::npos || src.find("<b:Sources SelectedStyle=") != string::npos);
    CHECK(count(src, "<b:Source>") == 1 && src.find("<b:Tag>Doe21</b:Tag>") != string::npos && src.find("<b:Title>Mapping science</b:Title>") != string::npos);  // only the cited source travels
    CHECK(c.files[1].second.find("<ds:datastoreItem") != string::npos && c.files[1].second.find("officeDocument/2006/bibliography") != string::npos);
    string fld = "<!--[if supportFields]><span style='mso-element:field-begin'></span> CITATION Doe21 \\l 1033 <span style='mso-element:field-separator'></span><![endif]-->(Doe &amp; Roe, 2021)<!--[if supportFields]><span style='mso-element:field-end'></span><![endif]-->";
    CHECK(c.html.find(fld) != string::npos);
    CHECK(c.html.find("BIBLIOGRAPHY") == string::npos);
    // b) the same with the bibliography appended
    o.bibliography = true;
    ClipHtml cb = docFragmentToWordHtml(d, f, o);
    CHECK(cb.html.find("<h1>References<o:p></o:p></h1>") != string::npos);
    CHECK(count(cb.html, " BIBLIOGRAPHY ") == 1 && count(cb.html, "<p class=MsoBibliography>") == 1 && cb.html.find("Doe, J. A., &amp; Roe, R. (2021). Mapping science") != string::npos);
    // c) a multi-key citation and the document's own reference list (copied with the text): one BIBLIOGRAPHY field around both entries
    DocFragment g;
    g.blocks = d.blocks;
    o.bibliography = false;
    ClipHtml cc = docFragmentToWordHtml(d, g, o);
    CHECK(cc.citations == 2 && cc.html.find(" CITATION Smi19 \\m Doe21 \\l 1033 ") != string::npos);
    CHECK(count(cc.html, " BIBLIOGRAPHY ") == 1 && count(cc.html, "<p class=MsoBibliography>") == 2 && count(cc.html, "mso-element:field-end") >= 3);
    CHECK(count(cc.files[0].second, "<b:Source>") == 2);
    // the sources tags are stable across copies from the same document (a second paste must not collide)
    CHECK(c.files[0].second.find("<b:Tag>Doe21</b:Tag>") != string::npos && cc.files[0].second.find("<b:Tag>Doe21</b:Tag>") != string::npos && cc.files[0].second.find("<b:Tag>Smi19</b:Tag>") != string::npos);
    // the field result is what a reader sees when fields are not supported: importing our own HTML keeps the text only
    DocFragment back;
    CHECK(docFragmentFromHtml(cc.html, back));
    bool citeText = false, codeLeak = false;
    for (auto& b : back.blocks) if (b.kind == Block::Paragraph) { string t = plain(b.p); if (t.find("(Doe & Roe, 2021)") != string::npos) citeText = true; if (t.find("CITATION") != string::npos || t.find("field-begin") != string::npos) codeLeak = true; }
    CHECK(citeText && !codeLeak);
    // data-URI mode (no folder): the picture is inline, no sources part
    ClipOptions o2;
    o2.png = png;
    DocFragment fig;
    fig.blocks = {d.blocks[10]};
    CHECK(d.blocks[10].kind == Block::FigureBlock);
    ClipHtml ci = docFragmentToWordHtml(d, fig, o2);
    CHECK(ci.html.find("src=\"data:image/png;base64,") != string::npos && ci.files.empty());
    if (!outdir.empty()) { FILE* fp = fopen((outdir + "/clip-cited.html").c_str(), "wb"); if (fp) { fwrite(cc.html.data(), 1, cc.html.size(), fp); fclose(fp); } }
  }

  // ---- 4. Word's own clipboard HTML (as Word 365 writes it) comes back as blocks
  {
    string w =
        "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000009999\r\nStartFragment:0000000141\r\nEndFragment:0000009960\r\nSourceURL:file:///C:/x.docx\r\n"
        "<html xmlns:o=\"urn:schemas-microsoft-com:office:office\"><head><meta charset=utf-8><style><!-- p.MsoNormal {font-size:11.0pt;} --></style></head>"
        "<body lang=EN-US style='tab-interval:.5in'>\n<!--StartFragment-->\n"
        "<h1>Results<o:p></o:p></h1>\n"
        "<p class=MsoNormal>The corpus grew <b>quickly</b> after 2015 <i style='mso-bidi-font-style:normal'>(see Figure 1)</i>.<span style='mso-spacerun:yes'>&nbsp;</span>"
        "<!--[if supportFields]><span style='mso-element:field-begin'></span><span style='mso-spacerun:yes'> </span>CITATION Doe21 \\l 1033 <span style='mso-element:field-separator'></span><![endif]-->(Doe &amp; Roe, 2021)<!--[if supportFields]><span style='mso-element:field-end'></span><![endif]--><o:p></o:p></p>\n"
        "<p class=MsoListParagraphCxSpFirst style='text-indent:-.25in;mso-list:l0 level1 lfo1'><![if !supportLists]><span style='font-family:Symbol;mso-fareast-font-family:Symbol'>\xC2\xB7<span style='font:7.0pt \"Times New Roman\"'>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; </span></span><![endif]>Alpha item<o:p></o:p></p>\n"
        "<p class=MsoListParagraphCxSpLast style='margin-left:1.0in;text-indent:-.25in;mso-list:l0 level2 lfo1'><![if !supportLists]><span style='font-family:\"Courier New\"'>o<span style='font:7.0pt \"Times New Roman\"'>&nbsp;&nbsp; </span></span><![endif]>Beta sub-item<o:p></o:p></p>\n"
        "<p class=MsoListParagraph style='text-indent:-.25in;mso-list:l1 level1 lfo2'><![if !supportLists]><span style='mso-bidi-font-family:Calibri'>1.<span style='font:7.0pt \"Times New Roman\"'>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; </span></span><![endif]>First step<o:p></o:p></p>\n"
        "<p class=MsoCaption>Table <!--[if supportFields]><span style='mso-element:field-begin'></span> SEQ Table \\* ARABIC <span style='mso-element:field-separator'></span><![endif]-->1<!--[if supportFields]><span style='mso-element:field-end'></span><![endif]-->: Counts<o:p></o:p></p>\n"
        "<table class=MsoTableGrid border=1 cellspacing=0 cellpadding=0 style='border-collapse:collapse;border:none'>\n"
        " <tr style='mso-yfti-irow:0;mso-yfti-firstrow:yes'>\n  <td width=312 valign=top style='width:233.75pt;border:solid windowtext 1.0pt'><p class=MsoNormal><b>Country<o:p></o:p></b></p></td>\n  <td width=312 valign=top><p class=MsoNormal align=right style='text-align:right'><b>Papers</b><o:p></o:p></p></td>\n </tr>\n"
        " <tr style='mso-yfti-irow:1;mso-yfti-lastrow:yes'>\n  <td width=312 valign=top><p class=MsoNormal>Bangladesh<o:p></o:p></p></td>\n  <td width=312 valign=top><p class=MsoNormal align=right style='text-align:right'>42<o:p></o:p></p></td>\n </tr>\n</table>\n"
        "<p class=MsoNormal><o:p>&nbsp;</o:p></p>\n"
        "<p class=MsoNormal><span style='color:#C00000'>Red</span> and <span style='background:yellow;mso-highlight:yellow'>marked</span> and <span style='font-family:\"Georgia\",serif;font-size:14.0pt'>big serif</span> and H<sub>2</sub>O and a <a href=\"https://doi.org/10.1/x\"><span class=MsoHyperlink>DOI link</span></a>.<o:p></o:p></p>\n"
        "<!--EndFragment-->\n</body></html>";
    DocFragment f;
    CHECK(docFragmentFromHtml(w, f));
    vector<Block>& b = f.blocks;
    CHECK(f.spans.empty() && b.size() == 9);
    if (b.size() >= 9) {
      CHECK(b[0].p.style == PStyle::H1 && plain(b[0].p) == "Results");
      CHECK(b[1].p.style == PStyle::Body && plain(b[1].p) == "The corpus grew quickly after 2015 (see Figure 1). (Doe & Roe, 2021)");
      bool bold = false, ital = false;
      for (auto& s : b[1].p.spans) { if (s.text == "quickly" && (s.fmt & F_BOLD)) bold = true; if (s.text == "(see Figure 1)" && (s.fmt & F_ITALIC)) ital = true; }
      CHECK(bold && ital);
      CHECK(b[2].p.style == PStyle::Bullet && b[2].p.level == 0 && plain(b[2].p) == "Alpha item");
      CHECK(b[3].p.style == PStyle::Bullet && b[3].p.level == 1 && plain(b[3].p) == "Beta sub-item");
      CHECK(b[4].p.style == PStyle::Number && b[4].p.level == 0 && plain(b[4].p) == "First step");
      CHECK(b[5].p.style == PStyle::Caption && plain(b[5].p) == "Table 1: Counts");
      CHECK(b[6].kind == Block::TableBlock && b[6].tbl.rows() == 2 && b[6].tbl.cols() == 2 && b[6].tbl.header && plain(b[6].tbl.cells[0][1]) == "Papers" && plain(b[6].tbl.cells[1][0]) == "Bangladesh" && b[6].tbl.cells[1][1].align == PAlign::Right);
      CHECK(b[7].kind == Block::Paragraph && b[7].p.spans.empty());  // the empty paragraph between
      CHECK(b[8].p.style == PStyle::Body && plain(b[8].p) == "Red and marked and big serif and H2O and a DOI link.");
      bool red = false, mark = false, serif = false, sub = false, link = false;
      for (auto& s : b[8].p.spans) {
        if (s.text == "Red" && s.color == 0xFFC00000u) red = true;
        if (s.text == "marked" && (s.fmt & F_MARK)) mark = true;
        if (s.text == "big serif" && s.font == "Georgia" && s.size == 14) serif = true;
        if (s.text == "2" && (s.fmt & F_SUB)) sub = true;
        if (s.text == "DOI link" && s.link == "https://doi.org/10.1/x") link = true;
      }
      CHECK(red && mark && serif && sub && link);
    }
  }

  // ---- 5. browser HTML and plain fragments
  {
    DocFragment f;
    CHECK(docFragmentFromHtml("<div><p>Just <strong>one</strong> paragraph &amp; entities &#8212; &eacute;.</p></div>", f));
    CHECK(f.blocks.empty() && f.spans.size() == 3 && f.spans[1].text == "one" && (f.spans[1].fmt & F_BOLD) && f.spans[2].text == " paragraph & entities \xE2\x80\x94 \xC3\xA9.");
    DocFragment g;
    CHECK(docFragmentFromHtml("<h2>Title</h2><ul><li>a</li><li>b<ul><li>c</li></ul></li></ul><ol start=3><li>three</li></ol><p style=\"text-align:center\">centred</p><hr><pre>raw  code\n  kept</pre>", g));
    CHECK(g.blocks.size() == 8);
    if (g.blocks.size() == 8) {
      CHECK(g.blocks[0].p.style == PStyle::H2 && g.blocks[1].p.style == PStyle::Bullet && g.blocks[2].p.style == PStyle::Bullet && plain(g.blocks[2].p) == "b");
      CHECK(g.blocks[3].p.style == PStyle::Bullet && g.blocks[3].p.level == 1 && plain(g.blocks[3].p) == "c");
      CHECK(g.blocks[4].p.style == PStyle::Number && plain(g.blocks[4].p) == "three" && g.blocks[4].p.numStart == 3);
      CHECK(g.blocks[5].p.align == PAlign::Center && plain(g.blocks[5].p) == "centred");
      CHECK(g.blocks[6].kind == Block::Rule);
      CHECK(g.blocks[7].p.style == PStyle::Code && plain(g.blocks[7].p) == "raw  code\n  kept");
    }
    DocFragment n;
    CHECK(!docFragmentFromHtml("<html><head><style>p{}</style></head><body>   </body></html>", n));
    CHECK(!docFragmentFromHtml("", n));
    // a paste into the editor: blocks land in the document with their styles
    Document d;
    d.blocks.push_back(Block::para("", PStyle::Body));
    Editor e(&d);
    e.paste(g);
    CHECK(d.blocks.size() >= 8);
    int bullets = 0;
    for (auto& b : d.blocks) bullets += b.kind == Block::Paragraph && b.p.style == PStyle::Bullet;
    CHECK(bullets == 3);
  }

  // ---- 1.14: a selected figure copies (and cuts) as a block; vector figures travel as EMF in a VML shape for Word
  {
    Document d;
    d.blocks.push_back(Block::para("Before", PStyle::Body));
    Block fb;
    fb.kind = Block::FigureBlock;
    fb.fig.asset = 0;
    fb.fig.widthPct = 60;
    fb.fig.caption = Block::para("Trend", PStyle::Caption).p;
    FigAsset a;
    a.title = "Trend";
    a.scene.W = 300; a.scene.H = 200;
    d.assets.push_back(a);
    d.blocks.push_back(fb);
    d.blocks.push_back(Block::para("After", PStyle::Body));
    Editor e(&d);
    e.setCaret({1, -1, 0});  // on the figure: a block selection, no range
    CHECK(e.blockSelected() && !e.hasSelection());
    DocFragment f = e.copy();
    CHECK(f.blocks.size() == 1 && f.blocks[0].kind == Block::FigureBlock && f.blocks[0].fig.asset == 0);
    CHECK(docFragmentCitedKeys(f).empty());
    // the Word HTML of that fragment: EMF via VML when the figure is vector, the PNG as the fallback <img>
    ClipOptions o;
    o.fileDir = "C:\\Temp\\clip";
    o.fileUrlBase = "file:///C:/Temp/clip";
    o.png = [](const Scene&, double) { return string("\x89PNG\r\n\x1a\n" "......", 14); };
    const double sc0 = d.setup.textW();
    o.emf = [sc0](const Scene& sc, double w, double h) { CHECK(sc.W == 300 && std::fabs(w - 0.6 * sc0) < 1.0 && std::fabs(h - w * 2.0 / 3.0) < 0.01); return string("EMF!"); };
    ClipHtml c = docFragmentToWordHtml(d, f, o);
    CHECK(c.figures == 1 && c.files.size() == 2 && c.files[0].first == "clip_image001.png" && c.files[1].first == "clip_image001.emf" && c.files[1].second == "EMF!");
    CHECK(c.html.find("<v:shapetype id=\"_x0000_t75\"") != string::npos && c.html.find("<v:shape id=\"Picture_x0020_1\"") != string::npos);
    CHECK(c.html.find("<v:imagedata src=\"file:///C:/Temp/clip/clip_image001.emf\"") != string::npos && c.html.find("<![if !vml]><img width=") != string::npos);
    CHECK(c.html.find("xmlns:v=\"urn:schemas-microsoft-com:vml\"") != string::npos);
    // a PNG figure: no metafile, no VML
    d.blocks[1].fig.format = "png";
    f = e.copy();
    c = docFragmentToWordHtml(d, f, o);
    CHECK(c.files.size() == 1 && c.html.find("<v:shape ") == string::npos && c.html.find("<img width=") != string::npos);
    // the round trip through the importer still yields the picture-less caption paragraph, not the VML
    DocFragment back;
    CHECK(docFragmentFromHtml(c.html, back) && !back.blocks.empty());
    for (auto& b : back.blocks) CHECK(b.p.text().find("v:shape") == string::npos);
    // cut removes the block, in its own undo step
    size_t n = d.blocks.size();
    DocFragment cutF = e.cut();
    CHECK(cutF.blocks.size() == 1 && d.blocks.size() == n - 1 && d.blocks[1].p.text() == "After");
    e.undo();
    CHECK(d.blocks.size() == n && d.blocks[1].kind == Block::FigureBlock);
  }
  // ---- 1.14: one <b:Source> per reference for Word's Sources.Add, the tags as in the CITATION fields
  {
    Document d;
    RefEntry r1; r1.key = "doe2021"; r1.title = "On things"; r1.year = "2021"; r1.authors = {"Doe, Jane"}; r1.container = "J. Things"; r1.kind = "article";
    RefEntry r2; r2.key = "roe2019"; r2.title = "Other things"; r2.year = "2019"; r2.authors = {"Roe, Richard"}; r2.kind = "article";
    docAddReference(d, r1);
    docAddReference(d, r2);
    d.blocks.push_back(Block::para("We cite ", PStyle::Body));
    Editor e(&d);
    e.setCaret({0, -1, 8});
    CHECK(e.insertCitation({"roe2019"}));
    e.setCaret({0, -1, 0});
    e.selectParagraph();  // the first paragraph only (the whole document would also carry the reference list's keys)
    DocFragment f = e.copy();
    std::set<string> cited = docFragmentCitedKeys(f);
    CHECK(cited.size() == 1 && cited.count("roe2019"));
    std::map<string, string> tags;
    auto items = docBibSourceItems(d, tags, &cited);
    CHECK(items.size() == 1 && items[0].first == tags["roe2019"] && items[0].first == "Roe19");
    CHECK(items[0].second.find("<b:Source>") == 0 && items[0].second.find("<b:Tag>Roe19</b:Tag>") != string::npos && items[0].second.find("<b:Last>Roe</b:Last>") != string::npos);
    CHECK(tags.size() == 2 && tags["doe2021"] == "Doe21");  // tags are assigned over the whole document
    string all = docBibSourcesXml(d, tags, &cited);
    CHECK(all.find("<b:Sources ") != string::npos && count(all, "<b:Source>") == 1);
    // the clipboard HTML uses the same tag in its field code
    ClipOptions o;
    o.fileDir = "C:\\Temp\\clip";
    o.fileUrlBase = "file:///C:/Temp/clip";
    ClipHtml c = docFragmentToWordHtml(d, f, o);
    CHECK(c.citations == 1 && c.html.find(" CITATION Roe19 \\l 1033 ") != string::npos);
  }

  if (fails) { std::cerr << fails << " check(s) failed\n"; return 1; }
  std::cout << "clip_test: all checks passed\n";
  return 0;
}
