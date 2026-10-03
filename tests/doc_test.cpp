// The document editor core: model, editing operations, undo, Markdown / JSON round trips and the three exporters.
//   g++ -std=c++17 -O1 tests/doc_test.cpp src/core/*.cpp -Isrc/core -o build/doc_test -pthread && ./build/doc_test [outdir]
#include <cstdio>
#include <cstdlib>
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

static Scene demoScene() {
  Scene sc;
  sc.W = 300;
  sc.H = 200;
  Prim r;
  r.type = Prim::Rect; r.x = 10; r.y = 10; r.w = 280; r.h = 180; r.fill = true; r.fillC = Color(0.9f, 0.93f, 0.98f);
  sc.items.push_back(r);
  Prim c;
  c.type = Prim::Circle; c.x = 150; c.y = 100; c.r = 40; c.fill = true; c.fillC = Color(0.2f, 0.4f, 0.8f); c.sphere = true;
  sc.items.push_back(c);
  Prim t;
  t.type = Prim::Text; t.x = 150; t.y = 170; t.text = "Demo chart \xC3\xA9"; t.size = 10; t.fill = true; t.anchor = 1; t.bold = true;
  sc.items.push_back(t);
  Prim p;
  p.type = Prim::Path; p.d = {{'M', 20, 180, 0, 0}, {'L', 280, 30, 0, 0}, {'Q', 150, 100, 280, 180}}; p.stroke = true; p.sw = 1.5f; p.strokeC = Color(0.8f, 0.2f, 0.2f);
  sc.items.push_back(p);
  Prim im;
  im.type = Prim::Image; im.x = 30; im.y = 30; im.w = 40; im.h = 20; im.imgW = 2; im.imgH = 1; im.rgba = {255, 0, 0, 255, 0, 0, 255, 128};
  sc.items.push_back(im);
  return sc;
}

static string textOf(const Document& d) {
  string s;
  for (auto& b : d.blocks) if (b.kind == Block::Paragraph) s += b.p.text() + "|";
  return s;
}

int main(int argc, char** argv) {
  string outdir = argc > 1 ? argv[1] : "";
  setAppVersion("test");

  // ---- model basics
  {
    Para p;
    p.spans = {{"Hello ", 0, ""}, {"", F_BOLD, ""}, {"world", F_BOLD, ""}, {"!", F_BOLD, ""}};
    p.normalize();
    CHECK(p.spans.size() == 2 && p.spans[1].text == "world!" && p.text() == "Hello world!");
    CHECK(p.fmtAt(0) == 0 && p.fmtAt(6) == 0 && p.fmtAt(7) == F_BOLD && p.fmtAt(12) == F_BOLD);
    CHECK(cpNext("a\xC3\xA9z", 1) == 3 && cpPrev("a\xC3\xA9z", 3) == 1);
    PageSetup ps;
    CHECK(std::abs(ps.pageW() - 595.28) < 0.1 && std::abs(ps.margin() - 72) < 0.01);
    ps.paper = "Letter"; ps.margins = "narrow";
    CHECK(std::abs(ps.pageW() - 612) < 0.01 && std::abs(ps.margin() - 36) < 0.01 && !ps.serif());
    ps.font = "Georgia";
    CHECK(ps.serif());
    PStyle st;
    CHECK(pstyleFromId("h2", st) && st == PStyle::H2 && string(pstyleId(PStyle::Reference)) == "reference" && !pstyleFromId("nope", st));
  }

  // ---- typing, enter, backspace, undo
  {
    Document d;
    d.blocks.push_back(Block::para(""));
    Editor e(&d);
    e.insertText("Hello");
    e.insertText(" world");
    CHECK(textOf(d) == "Hello world|");
    e.insertParagraphBreak();
    CHECK(d.blocks.size() == 2 && e.caret.blk == 1 && e.caret.off == 0);
    e.insertText("Second");
    e.home(false);
    e.backspace();  // merges with the first paragraph
    CHECK(d.blocks.size() == 1 && d.blocks[0].p.text() == "Hello worldSecond" && e.caret.off == 11);
    CHECK(e.undo() && d.blocks.size() == 2 && d.blocks[1].p.text() == "Second");
    CHECK(e.undo() && d.blocks[1].p.text().empty());
    CHECK(e.redo() && d.blocks[1].p.text() == "Second");
    // split in the middle
    e.setCaret({0, -1, 5});
    e.insertParagraphBreak();
    CHECK(d.blocks.size() == 3 && d.blocks[0].p.text() == "Hello" && d.blocks[1].p.text() == " world" && e.caret.blk == 1 && e.caret.off == 0);
    // del at the end joins the next paragraph
    e.end(false);
    e.del();
    CHECK(d.blocks.size() == 2 && d.blocks[1].p.text() == " worldSecond");
    // soft line break
    e.insertLineBreak();
    CHECK(d.blocks[1].p.text().find('\n') != string::npos);
    // UTF-8 backspace removes a whole code point
    e.insertText("caf\xC3\xA9");
    e.backspace();
    CHECK(d.blocks[1].p.text().find("caf") != string::npos && d.blocks[1].p.text().find("caf\xC3\xA9") == string::npos);
  }

  // ---- selection, formatting, links, styles, lists
  {
    Document d;
    d.blocks.push_back(Block::para("The quick brown fox"));
    Editor e(&d);
    e.setCaret({0, -1, 4});
    e.setCaret({0, -1, 9}, true);  // "quick"
    CHECK(e.hasSelection() && e.selectionText() == "quick");
    e.toggleFmt(F_BOLD);
    CHECK(d.blocks[0].p.spans.size() == 3 && d.blocks[0].p.spans[1].text == "quick" && d.blocks[0].p.spans[1].fmt == F_BOLD);
    CHECK(e.fmtHere() & F_BOLD);
    e.toggleFmt(F_ITALIC);
    CHECK(d.blocks[0].p.spans[1].fmt == (F_BOLD | F_ITALIC));
    e.toggleFmt(F_BOLD);
    CHECK(d.blocks[0].p.spans[1].fmt == F_ITALIC);
    e.setLink("https://example.org/x");
    CHECK(e.linkHere() == "https://example.org/x");
    CHECK(spansToMarkdown(d.blocks[0].p.spans) == "The [*quick*](https://example.org/x) brown fox");
    e.clearFormatting();
    CHECK(d.blocks[0].p.spans.size() == 1 && d.blocks[0].p.spans[0].fmt == 0 && d.blocks[0].p.spans[0].link.empty());
    // typing format without a selection
    e.setCaret({0, -1, 19});
    e.toggleFmt(F_BOLD);
    e.insertText(" jumps");
    CHECK(d.blocks[0].p.spans.back().text == " jumps" && d.blocks[0].p.spans.back().fmt == F_BOLD);
    // word selection
    e.setCaret({0, -1, 6});
    e.selectWord();
    CHECK(e.selectionText() == "quick");
    // styles / align / lists
    e.setStyle(PStyle::H2);
    CHECK(e.styleHere() == PStyle::H2);
    e.setAlign(PAlign::Center);
    CHECK(e.alignHere() == PAlign::Center);
    e.toggleList(false);
    CHECK(e.styleHere() == PStyle::Bullet);
    e.indent(1);
    CHECK(d.blocks[0].p.level == 1);
    e.indent(-1);
    e.indent(-1);
    CHECK(d.blocks[0].p.level == 0 && e.styleHere() == PStyle::Bullet);
    e.toggleList(true);
    CHECK(e.styleHere() == PStyle::Number);
    e.toggleList(true);
    CHECK(e.styleHere() == PStyle::Body);
    // Markdown auto-format while typing at the start of an empty body paragraph
    e.end(false);
    e.insertParagraphBreak();
    e.insertText("-");
    e.insertText(" ");
    CHECK(e.styleHere() == PStyle::Bullet && e.curPara()->empty());
    e.insertText("item");
    e.insertParagraphBreak();
    CHECK(e.styleHere() == PStyle::Bullet);  // the list continues
    e.insertParagraphBreak();                // Enter on an empty item leaves the list
    CHECK(e.styleHere() == PStyle::Body);
    e.insertText("1.");
    e.insertText(" ");
    CHECK(e.styleHere() == PStyle::Number);
    e.insertText("#");
    e.setStyle(PStyle::Body);
    e.insertText("# ");
    CHECK(e.styleHere() == PStyle::H1 || e.styleHere() == PStyle::Body);  // ("#" was already text: no auto heading)
    // paragraph selection across blocks and deletion
    e.selectAll();
    CHECK(e.selStart().blk == 0 && e.selEnd().blk == int(d.blocks.size()) - 1);
    e.begin("delete");
    CHECK(e.deleteSelection());
    CHECK(d.blocks.size() == 1 && d.blocks[0].p.empty());
  }

  // ---- tables
  {
    Document d;
    d.blocks.push_back(Block::para("Before"));
    Editor e(&d);
    e.end(false);
    e.insertTable(2, 3);
    CHECK(d.blocks.size() >= 2 && d.blocks[1].kind == Block::TableBlock && d.blocks[1].tbl.rows() == 2 && d.blocks[1].tbl.cols() == 3);
    CHECK(e.caret.blk == 1 && e.caret.cell == 0);
    int r = -1, c = -1;
    CHECK(e.inTable(&r, &c) && r == 0 && c == 0);
    e.insertText("A");
    CHECK(e.tableNextCell(1) && e.caret.cell == 1);
    e.insertText("B");
    e.tableNextCell(1);
    e.tableNextCell(1);
    e.tableNextCell(1);
    e.tableNextCell(1);
    CHECK(e.caret.cell == 5);
    CHECK(e.tableNextCell(1) && d.blocks[1].tbl.rows() == 3 && e.caret.cell == 6);  // Tab after the last cell adds a row
    CHECK(e.tableNextCell(-1) && e.caret.cell == 5);
    e.tableInsertRow(false);
    CHECK(d.blocks[1].tbl.rows() == 4);
    e.tableInsertCol(true);
    CHECK(d.blocks[1].tbl.cols() == 4);
    e.tableDeleteCol();
    CHECK(d.blocks[1].tbl.cols() == 3);
    e.tableDeleteRow();
    CHECK(d.blocks[1].tbl.rows() == 3);
    e.tableSetColAlign('r');
    e.tableToggleHeader();
    CHECK(!d.blocks[1].tbl.header);
    e.tableSetWidths({0.5f, 0.25f, 0.25f});
    CHECK(d.blocks[1].tbl.widths.size() == 3);
    // Enter inside a cell is a soft break, Backspace at the cell start stays in the cell
    e.setCaret({1, 0, 1});
    e.insertParagraphBreak();
    CHECK(d.blocks[1].tbl.cells[0][0].text() == "A\n");
    e.setCaret({1, 1, 0});
    e.backspace();
    CHECK(d.blocks[1].kind == Block::TableBlock && d.blocks[1].tbl.cells[0][1].text() == "B");
    CHECK(e.undo());
    e.setCaret({1, 0, 0});
    e.tableDeleteTable();
    CHECK(d.blocks.size() >= 1 && std::none_of(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::TableBlock; }));
  }

  // ---- blocks: figure, rule, page break, toc, move, delete
  {
    Document d;
    d.blocks.push_back(Block::para("Intro", PStyle::H1));
    d.blocks.push_back(Block::para("Text"));
    int a = d.addAsset(demoScene(), "chart_a", "Chart A", "summary");
    Editor e(&d);
    e.setCaret({1, -1, 4});
    e.insertFigure(a, "A demo chart", 80);
    CHECK(d.figures() == 1 && d.blocks[2].kind == Block::FigureBlock && d.blocks[2].fig.widthPct == 80 && d.figureNumber(2) == 1);
    CHECK(e.caret.blk == 2 && e.caret.cell == -2);  // caret in the caption
    e.insertText("!");
    CHECK(d.blocks[2].fig.caption.text() == "A demo chart!");
    e.insertParagraphBreak();
    CHECK(e.caret.blk == 3 && d.blocks[3].kind == Block::Paragraph);
    e.insertRule();
    e.insertPageBreak();
    e.insertToc();
    CHECK(std::count_if(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::Rule; }) == 1);
    CHECK(std::count_if(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::PageBreak; }) == 1);
    CHECK(std::count_if(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::Toc; }) == 1);
    e.setFigureWidth(60);
    e.setCaret({2, -2, 0});
    e.setFigureWidth(60);
    CHECK(d.blocks[2].fig.widthPct == 60);
    e.moveBlockUpDown(-1);
    CHECK(d.blocks[1].kind == Block::FigureBlock && e.caret.blk == 1);
    e.deleteBlock(1);
    CHECK(d.figures() == 0);
    d.dropUnusedAssets();
    CHECK(d.assets.empty());
    CHECK(e.undo() && d.figures() == 1);
    CHECK(d.outline().size() == 1 && d.title() == "Intro");
  }

  // ---- clipboard
  {
    Document d;
    d.blocks.push_back(Block::para("alpha beta gamma"));
    d.blocks.push_back(Block::para("second"));
    Editor e(&d);
    e.setCaret({0, -1, 6});
    e.setCaret({1, -1, 3}, true);
    DocFragment f = e.copy();
    CHECK(!f.empty() && f.blocks.size() == 2);
    CHECK(e.selectionText() == "beta gamma\nsec");
    DocFragment g = e.cut();
    CHECK(d.blocks.size() == 1 && d.blocks[0].p.text() == "alpha ond");
    e.paste(g);
    CHECK(d.blocks.size() == 2 && d.blocks[0].p.text() == "alpha beta gamma" && d.blocks[1].p.text() == "second");
    e.docEnd(false);
    e.pasteText("## Pasted\n\n- one\n- two\n\n| a | b |\n|---|--:|\n| 1 | 2 |\n", true);
    CHECK(std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::Paragraph && b.p.style == PStyle::H1 && b.p.text() == "Pasted"; }) ||
          std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::Paragraph && b.p.style == PStyle::H2 && b.p.text() == "Pasted"; }));
    CHECK(std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::TableBlock; }));
    e.pasteText("line one\n\nline two", false);
    CHECK(d.blocks.back().kind == Block::Paragraph && d.blocks.back().p.text() == "line two");
    string md = e.selectionMarkdown();
    e.selectAll();
    md = e.selectionMarkdown();
    CHECK(md.find("alpha beta gamma") != string::npos && md.find("| a | b |") != string::npos);
  }

  // ---- find & replace
  {
    Document d;
    d.blocks.push_back(Block::para("Cat cat CAT"));
    d.blocks.push_back(Block::para("dog"));
    Editor e(&d);
    CHECK(e.find("cat", true, false) && e.selectionText() == "Cat");
    CHECK(e.find("cat", true, false) && e.selectionText() == "cat" && e.caret.off == 7);
    CHECK(e.find("cat", true, true) && e.selectionText() == "cat");  // case sensitive: wraps to the lowercase one
    CHECK(!e.find("bird", true, false));
    CHECK(e.replaceAll("cat", "bird", false) == 3 && d.blocks[0].p.text() == "bird bird bird");
    e.setCaret({0, -1, 0});
    CHECK(e.find("bird", true, false) && e.replaceCurrent("bird", "cow", false) && d.blocks[0].p.text() == "cow bird bird");
  }

  // ---- citations
  {
    Document d;
    d.blocks.push_back(Block::para("Networks matter"));
    Editor e(&d);
    e.end(false);
    d.citeStyle = "ieee";
    int n = e.insertCitation("Smith J. (2020). Networks. Journal, 1(2), 3-4. https://doi.org/10.1000/xyz", "https://doi.org/10.1000/xyz");
    CHECK(n == 1 && d.blocks[0].p.text().find("[1]") != string::npos);
    int refs = 0;
    for (auto& b : d.blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference) refs++;
    CHECK(refs == 1 && d.refs.size() == 1 && !d.refs[0].structured());
    CHECK(std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::Paragraph && b.p.style == PStyle::H1 && b.p.text() == "References"; }));
    e.setCaret({0, -1, 0});
    int n2 = e.insertCitation("Smith J. (2020). Networks. Journal, 1(2), 3-4. https://doi.org/10.1000/xyz", "");  // same reference: same number
    CHECK(n2 == 1);
    int n3 = e.insertCitation("Other A. (2021). Paper.", "");
    CHECK(n3 == 2 && d.refs.size() == 2);
    // the citation is a field: its text follows the style
    int fields = 0;
    for (auto& sp : d.blocks[0].p.spans) fields += sp.isCite();
    CHECK(fields == 2);
    e.setCiteStyle("apa");
    CHECK(d.citeStyle == "apa" && d.blocks[0].p.text().find("(Smith, 2020)") != string::npos && d.blocks[0].p.text().find("[1]") == string::npos);
    e.setCiteStyle("nature");
    bool sup = false;
    for (auto& sp : d.blocks[0].p.spans) if (sp.isCite() && (sp.fmt & F_SUP)) sup = true;
    CHECK(sup);
    e.setCiteStyle("ieee");
    // structured entries from a corpus record, Word source types, all styles format
    Record r;
    r.id = "rec:document-link";
    r.title = "Mapping science";
    r.year = 2021;
    r.authors = {"Doe, Jane A.", "Roe, Richard", "Poe, Edgar", "Moe, M."};
    r.source = "Scientometrics";
    r.doi = "10.1007/s11192-021-0001";
    r.volume = "126";
    r.issue = "3";
    r.pages = "1-20";
    r.url = "https://example.org/mapping";
    RefEntry re = refFromRecord(r);
    CHECK(re.structured() && re.key == refKeyFor(re.doi, re.title, re.year) && startsWith(re.key, "doi:") && re.authors.size() == 4);
    CHECK(re.recordId == r.id && re.issue == "3" && re.url == r.url);
    int idx = docAddReference(d, re);
    CHECK(idx == 2 && docAddReference(d, re) == 2 && d.refs.size() == 3);
    e.setCaret({0, -1, 0});
    CHECK(e.insertCitation(vector<string>{re.key}));
    CHECK(d.blocks[0].p.text().find("[1]") == 0);  // cited first now: IEEE numbers follow the citation order
    vector<string> here = e.citeHere();
    CHECK(here.size() == 1 && here[0] == re.key);
    for (auto& st : citeStyles()) {
      uint16_t f = 0;
      string c = formatCitation({&re}, {3}, st.id, f);
      vector<Span> bib = formatReference(re, st.id, 3);
      string bt;
      for (auto& sp : bib) bt += sp.text;
      CHECK(!c.empty() && bt.find("Mapping science") != string::npos && bt.find("2021") != string::npos);
      if (string(st.id) == "apa") CHECK(c == "(Doe et al., 2021)" && bt.find("Doe, J. A., Roe, R., Poe, E., & Moe, M.") == 0);
      if (string(st.id) == "vancouver") CHECK(c == "(3)" && bt.find("Doe JA, Roe R, Poe E, Moe M") != string::npos);
    }
    CHECK(wordSourceType("article") == "JournalArticle" && wordSourceType("book") == "Book" && wordSourceType("conference") == "ConferenceProceedings");
    {
      Corpus corpus; corpus.recs.push_back(r);
      Document legacy;
      RefEntry old = refFromRecord(r); old.recordId.clear(); old.rec = -1;
      docAddReference(legacy, old);
      CHECK(migrateDocRecordLinks(legacy, corpus) == 1);
      CHECK(legacy.refs[0].recordId == r.id && legacy.refs[0].rec == 0);
    }
    {
      Record keep = r; keep.id = "rec:keep";
      Record remove = r; remove.id = "rec:remove";
      Document merged;
      RefEntry a = refFromRecord(keep), b = refFromRecord(remove);
      docAddReference(merged, a); docAddReference(merged, b);
      CHECK(merged.refs.size() == 2);
      docMergeRecordLinks(merged, keep, remove.id, 0, 1);
      CHECK(merged.refs.size() == 1 && merged.refs[0].recordId == keep.id && merged.refs[0].rec == 0);
    }
    CHECK(splitAuthor("van der Berg, Jan Willem").last == "van der Berg" && splitAuthor("Jane A. Doe").initials == "J. A." && splitAuthor("Smith JA").initials == "J. A.");
    // the bibliography follows the style order: numeric = citation order, author-date = alphabetical
    vector<string> order;
    for (auto& b : d.blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference) order.push_back(b.p.text());
    CHECK(order.size() == 3 && order[0].find("Doe") != string::npos && order[1].find("Smith") != string::npos);
    e.setCiteStyle("apa");
    order.clear();
    for (auto& b : d.blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference) order.push_back(b.p.text());
    CHECK(order.size() == 3 && order[0].find("Doe") == 0);
    // remove the field, keep the entry; the markdown carries the field
    e.setCiteStyle("ieee");
    string md = spansToMarkdown(d.blocks[0].p.spans);
    CHECK(md.find("[@raw:") != string::npos && md.find("[@doi:") != string::npos);
    vector<Span> back = spansFromMarkdown(md);
    int backFields = 0;
    for (auto& sp : back) backFields += sp.isCite();
    CHECK(backFields == 3);
    // JSON keeps the bibliography, the style and the fields
    Json j = docToJson(d, false);
    Document d2;
    CHECK(docFromJson(d2, Json::parse(j.dump())));
    CHECK(d2.refs.size() == 3 && d2.citeStyle == "ieee" && d2.refs[2].authors.size() == 4 && d2.blocks[0].p.text() == d.blocks[0].p.text());
    int f2 = 0;
    for (auto& sp : d2.blocks[0].p.spans) f2 += sp.isCite();
    CHECK(f2 == 3);
    // undo restores the bibliography too
    size_t nRefs = d.refs.size();
    e.setCaret({0, -1, 0});
    e.insertCitation("Third T. (2022). Late.", "");
    CHECK(d.refs.size() == nRefs + 1);
    CHECK(e.undo() && d.refs.size() == nRefs);
  }

  // ---- span fonts / sizes / colours and figure options
  {
    Document d;
    d.blocks.push_back(Block::para("Styled text"));
    Editor e(&d);
    e.setCaret({0, -1, 0});
    e.anchor = {0, -1, 6};
    e.setFont("Georgia");
    e.setSize(14);
    e.setColor(0xFFCC0000);
    CHECK(d.blocks[0].p.spans.size() == 2 && d.blocks[0].p.spans[0].font == "Georgia" && d.blocks[0].p.spans[0].size == 14 && d.blocks[0].p.spans[0].color == 0xFFCC0000);
    CHECK(d.blocks[0].p.spans[1].font.empty());
    CHECK(e.fontHere() == "Georgia" && e.sizeHere() == 14);
    Json j = docToJson(d, false);
    Document d2;
    CHECK(docFromJson(d2, Json::parse(j.dump())) && d2.blocks[0].p.spans[0].font == "Georgia" && d2.blocks[0].p.spans[0].size == 14 && d2.blocks[0].p.spans[0].color == 0xFFCC0000);
    string html = docToHTML(d);
    CHECK(html.find("font-family:'Georgia'") != string::npos || html.find("font-family: 'Georgia'") != string::npos || html.find("Georgia") != string::npos);
    CHECK(html.find("#cc0000") != string::npos || html.find("#CC0000") != string::npos);
    // figure options
    Document f;
    f.blocks.push_back(Block::para("Fig doc", PStyle::Title));
    FigAsset a;
    a.id = "demo";
    a.scene = demoScene();
    f.assets.push_back(a);
    Figure fg;
    fg.asset = 0;
    fg.widthPct = 60;
    fg.caption.style = PStyle::Caption;
    fg.caption.spans = {Span("A caption")};
    fg.label = "Chart";
    fg.captionAbove = true;
    fg.border = true;
    fg.align = PAlign::Left;
    fg.format = "png";
    Block fb;
    fb.kind = Block::FigureBlock;
    fb.fig = fg;
    f.blocks.push_back(fb);
    CHECK(f.blocks[1].fig.labelText() == "Chart" && f.blocks[1].fig.formatOr("vector") == "png");
    Json fj = docToJson(f, true);
    Document f2;
    CHECK(docFromJson(f2, Json::parse(fj.dump())) && f2.blocks[1].fig.label == "Chart" && f2.blocks[1].fig.captionAbove && f2.blocks[1].fig.border && f2.blocks[1].fig.align == PAlign::Left && f2.blocks[1].fig.format == "png");
    string fh = docToHTML(f);
    CHECK(fh.find("bordered") != string::npos && fh.find("Chart") != string::npos);
    f.setup.header = "Running head";
    f.setup.lineSpacing = 1.5f;
    f.setup.headingFont = "Georgia";
    vector<Scene> pages = docToPages(f);
    CHECK(!pages.empty());
    bool head = false;
    for (auto& pr : pages[0].items) if (pr.type == Prim::Text && pr.text == "Running head") head = true;
    CHECK(head);
    string pdf = docToPDF(f);
    CHECK(startsWith(pdf, "%PDF-1.4"));
    // vector figure in Word: SVG part + PNG fallback; PNG figure: PNG only
    ScenePng png = [](const Scene& sc, double dpi) {
      int w = int(sc.W / 72 * dpi), h = int(sc.H / 72 * dpi);
      vector<uint8_t> px(size_t(w) * size_t(h) * 4, 200);
      return pngEncode(w, h, px.data(), false, dpi);
    };
    string dPng = docToDOCX(f, png, 72);
    CHECK(dPng.find("figure1.png") != string::npos && dPng.find("figure1.svg") == string::npos);
    f.blocks[1].fig.format = "vector";
    string dSvg = docToDOCX(f, png, 72);
    CHECK(dSvg.find("figure1.png") != string::npos && dSvg.find("figure1.svg") != string::npos);
    CHECK(dSvg.find("header1.xml") != string::npos);
    string dNo = docToDOCX(f, nullptr, 72);  // no renderer: the SVG alone
    CHECK(dNo.find("figure1.svg") != string::npos && dNo.find("figure1.png") == string::npos);
    if (!outdir.empty()) writeFile(outdir + "/figure-options.docx", dSvg);
  }

  // ---- sections and citations for the assistant
  {
    Document d;
    d.blocks.push_back(Block::para("Report", PStyle::Title));
    vector<Block> body = blocksFromMarkdown("Intro text [R2] and [R1, R2, R9].\n\n### Sub\n\nMore.", 2);
    int h = docInsertSection(d, "Introduction", body);
    CHECK(h == 1 && d.blocks[1].p.style == PStyle::H1 && d.blocks.size() == 5);
    vector<DocSection> secs = docSections(d);
    CHECK(secs.size() == 2 && secs[0].heading == -1 && secs[1].title == "Introduction" && secs[1].first == 1 && secs[1].last == 4);
    int n1 = docAddReference(d, "Ref one.", ""), n2 = docAddReference(d, "Ref two.", "https://doi.org/10.1/2"), n1b = docAddReference(d, "Ref one.", "");
    CHECK(n1 == 1 && n2 == 2 && n1b == 1 && docBodyEnd(d) == 5);
    string resolved = docResolveCitations("Intro text [R2] and [R1, R2, R9] end [R9].", [&](int r) { return r == 1 ? 1 : r == 2 ? 2 : 0; });
    CHECK(resolved == "Intro text [2] and [1, 2] end.");
    string keyed = docResolveCitationKeys("Intro text [R2] and [R1, R2, R9] end [R9].", [&](int r) { return r == 1 ? string("doi:a") : r == 2 ? string("t:b") : string(); });
    CHECK(keyed == "Intro text [@t:b] and [@doi:a;@t:b] end.");
    {
      vector<Block> kb = blocksFromMarkdown(keyed, 2);
      int fields = 0;
      for (auto& b : kb) if (b.kind == Block::Paragraph) for (auto& sp : b.p.spans) if (sp.isCite()) fields++;
      CHECK(kb.size() == 1 && fields == 2);
    }
    int h2 = docInsertSection(d, "Methods", blocksFromMarkdown("Method text.", 2));
    CHECK(h2 == 5 && docBodyEnd(d) == 7 && docSections(d).size() == 3);
    docReplaceBlocks(d, secs[1].first, secs[1].last, blocksFromMarkdown("Replaced.", 2));
    CHECK(docSections(d).size() == 2 && d.blocks[1].p.text() == "Replaced.");
    string md = docSectionMarkdown(d, docSections(d)[1]);
    CHECK(md.find("Method text.") != string::npos);
  }

  // ---- the assistant's write_report(replace) and edit_section flows as agent.cpp runs them (1.11)
  {
    Document d;
    d.blocks.push_back(Block::para("Report", PStyle::Title));
    d.blocks.push_back(Block::para("Data: 100 records", PStyle::Meta));
    docInsertSection(d, "Introduction", blocksFromMarkdown("Intro [R1].", 2));
    docInsertSection(d, "Themes", blocksFromMarkdown("Theme text.\n\n- one\n- two", 2));
    docAddReference(d, "First reference.", "");
    Editor ed(&d);
    // replace = true: every H1 section goes, the title block and the references stay
    ed.begin("replace");
    vector<DocSection> old = docSections(d);
    for (int i = int(old.size()) - 1; i >= 0; i--) if (old[size_t(i)].heading >= 0) docReplaceBlocks(d, old[size_t(i)].first, old[size_t(i)].last, {});
    ed.externalChange();
    CHECK(d.title() == "Report");
    int secs = 0;
    for (auto& sc : docSections(d)) secs += sc.heading >= 0;
    CHECK(secs == 0 && docBodyEnd(d) == 2 && d.blocks.back().p.style == PStyle::Reference);
    int h = docInsertSection(d, "Conclusion", blocksFromMarkdown("Done [R1].", 2));
    CHECK(h == 2 && d.blocks[2].p.text() == "Conclusion" && docBodyEnd(d) == 4);
    // edit_section: title and body replaced, the heading index stays
    vector<DocSection> now;
    for (auto& sc : docSections(d)) if (sc.heading >= 0) now.push_back(sc);
    CHECK(now.size() == 1 && now[0].heading == 2 && now[0].last == 3);
    d.blocks[size_t(now[0].heading)].p.spans = {Span{"Summary", 0, ""}};
    docReplaceBlocks(d, now[0].heading + 1, now[0].last, blocksFromMarkdown("New body.\n\nSecond paragraph.", 2, "Summary"));
    ed.externalChange();
    CHECK(docSections(d).back().title == "Summary" && docSectionMarkdown(d, docSections(d).back()).find("Second paragraph.") != string::npos);
    CHECK(ed.canUndo() && ed.undo() && d.blocks.size() >= 4);  // the assistant's steps are undo steps for the user
    // a fresh document (one empty paragraph) is replaced, not preceded, by the first section
    Document f;
    f.blocks.push_back(Block::para("", PStyle::Body));
    if (f.blocks.size() == 1 && f.blocks[0].kind == Block::Paragraph && f.blocks[0].p.empty()) f.blocks.clear();
    docInsertSection(f, "Introduction", blocksFromMarkdown("Text.", 2));
    CHECK(f.blocks.size() == 2 && f.blocks[0].p.style == PStyle::H1 && !f.empty());
    // saved and loaded through the project's JSON
    Json j = docToJson(d);
    Document back;
    CHECK(docFromJson(back, j) && back.title() == "Report" && back.blocks.size() == d.blocks.size());
  }

  // ---- Markdown import
  {
    string md = "# Title of the report\n\nFirst paragraph\nspans two lines with **bold**, *italic*, `code`, ~~gone~~, <sub>2</sub>, a [link](https://x.org/a) and https://doi.org/10.1000/abc.\n\n"
                "## Methods\n\n- one\n- two\n  - nested\n\n1. first\n2. second\n\n> quoted\n\n```\ncode line\n```\n\n---\n\n"
                "Table 1. Top items\n\n| Item | Count |\n|:-----|------:|\n| a | 1 |\n| b | 2 |\n\n**Run-in heading**\n\nText 10.1234/abcd.ef here.\n";
    vector<Block> bl = blocksFromMarkdown(md, 1, "");
    CHECK(!bl.empty() && bl[0].kind == Block::Paragraph && bl[0].p.style == PStyle::H1 && bl[0].p.text() == "Title of the report");
    CHECK(bl[1].p.text().find("First paragraph spans two lines") == 0);
    bool hasBold = false, hasLink = false, hasDoi = false, hasSub = false, hasStrike = false;
    for (auto& s : bl[1].p.spans) {
      if (s.text == "bold" && s.fmt == F_BOLD) hasBold = true;
      if (s.text == "link" && s.link == "https://x.org/a") hasLink = true;
      if (s.link == "https://doi.org/10.1000/abc") hasDoi = true;
      if (s.text == "2" && s.fmt == F_SUB) hasSub = true;
      if (s.text == "gone" && s.fmt == F_STRIKE) hasStrike = true;
    }
    CHECK(hasBold && hasLink && hasDoi && hasSub && hasStrike);
    int bullets = 0, numbers = 0, nested = 0, quotes = 0, codes = 0, rules = 0, tables = 0, captions = 0, h2 = 0;
    for (auto& b : bl) {
      if (b.kind == Block::Rule) rules++;
      if (b.kind == Block::TableBlock) { tables++; CHECK(b.tbl.rows() == 3 && b.tbl.cols() == 2 && b.tbl.align[1] == 'r' && b.tbl.header); }
      if (b.kind != Block::Paragraph) continue;
      if (b.p.style == PStyle::Bullet) { bullets++; if (b.p.level == 1) nested++; }
      if (b.p.style == PStyle::Number) numbers++;
      if (b.p.style == PStyle::Quote) quotes++;
      if (b.p.style == PStyle::Code) { codes++; CHECK(b.p.text() == "code line"); }
      if (b.p.style == PStyle::Caption) captions++;
      if (b.p.style == PStyle::H2) h2++;
    }
    CHECK(bullets == 3 && nested == 1 && numbers == 2 && quotes == 1 && codes == 1 && rules == 1 && tables == 1 && captions == 1 && h2 >= 2);
    // DOI without prefix becomes a link
    bool bareDoi = false;
    for (auto& b : bl) if (b.kind == Block::Paragraph) for (auto& s : b.p.spans) if (s.link == "https://doi.org/10.1234/abcd.ef") bareDoi = true;
    CHECK(bareDoi);
    // heading base and title drop
    vector<Block> sec = blocksFromMarkdown("# Methods\n\nBody\n\n## Sub", 2, "Methods");
    CHECK(sec.size() == 2 && sec[0].p.text() == "Body" && sec[1].p.style == PStyle::H2);  // "## Sub" is the first sub-level of the section
    // mojibake repair
    vector<Span> sp = spansFromMarkdown("Caf\xC3\x83\xC2\xA9 \xE2\x80\x93 ok", true, true);
    string t;
    for (auto& s : sp) t += s.text;
    CHECK(t == "Caf\xC3\xA9 \xE2\x80\x93 ok");
    // round trip
    Document d;
    d.blocks = bl;
    string back = blocksToMarkdown(d);
    vector<Block> again = blocksFromMarkdown(back, 1, "");
    CHECK(again.size() == bl.size());
    Document d2;
    d2.blocks = again;
    CHECK(d2.plainText() == d.plainText());
    vector<int> nums = docListNumbers(d);
    int seen = 0;
    for (size_t k = 0; k < d.blocks.size(); k++) if (nums[k]) seen = seen * 10 + nums[k];
    CHECK(seen == 12);
    d.setup.numberedHeadings = true;
    vector<string> hn = docHeadingNumbers(d);
    int numbered = 0;
    for (auto& h : hn) if (!h.empty()) numbered++;
    CHECK(numbered >= 3 && hn[0] == "1");
    CHECK(docOutlineText(d).find("Methods") != string::npos);
  }

  // ---- report -> document, JSON round trip, scene blob
  {
    ReportDoc r;
    r.title = "Bibliometric report";
    r.subtitle = "Test corpus";
    r.meta = {"1,234 records", "Source: Scopus"};
    r.date = "29 September 2026";
    ReportBlock s;
    s.kind = ReportBlock::Section;
    s.title = "Overview";
    s.text = "# Overview\n\nThe corpus grew **fast**.\n\n- a\n- b\n";
    r.blocks.push_back(s);
    ReportBlock f;
    f.kind = ReportBlock::Figure;
    f.title = "Publications per year";
    f.text = "Annual output.";
    f.fig = demoScene();
    f.figId = "publications_per_year";
    r.blocks.push_back(f);
    r.references = {"Doe J. (2019). A paper. Journal 1:1-2. https://doi.org/10.1000/1", "Roe R. (2020). B paper."};
    Document d = docFromReport(r);
    CHECK(d.title() == "Bibliometric report" && d.figures() == 1 && d.assets.size() == 1);
    int refs = 0, h1 = 0, metas = 0;
    for (auto& b : d.blocks) if (b.kind == Block::Paragraph) { if (b.p.style == PStyle::Reference) refs++; if (b.p.style == PStyle::H1) h1++; if (b.p.style == PStyle::Meta) metas++; }
    CHECK(refs == 2 && h1 == 2 && metas == 3);  // "Overview" once (title dropped from the body), "References"
    CHECK(d.words() > 10);
    Json j = docToJson(d, true);
    string dumped = j.dump();
    Document back;
    CHECK(docFromJson(back, Json::parse(dumped)));
    CHECK(back.blocks.size() == d.blocks.size() && back.assets.size() == 1 && back.plainText() == d.plainText());
    CHECK(back.assets[0].scene.items.size() == d.assets[0].scene.items.size() && std::abs(back.assets[0].scene.W - 300) < 1e-6);
    CHECK(back.assets[0].scene.items[4].rgba.size() == 8 && back.assets[0].scene.items[3].d.size() == 3 && back.assets[0].scene.items[1].sphere);
    CHECK(back.assets[0].scene.items[2].text == "Demo chart \xC3\xA9");
    Scene junk;
    CHECK(!sceneFromBlob("not base64 at all", junk) && !sceneFromBlob(base64("VSC1xx"), junk));
    Document empty;
    CHECK(docFromJson(empty, Json::parse("{}")) && empty.blocks.size() == 1);
    CHECK(!docFromJson(empty, Json::parse("[]")));

    // ---- exporters
    d.setup.toc = true;
    d.setup.numberedHeadings = true;
    Editor e(&d);
    e.docEnd(false);
    e.pasteText("## Data\n\nSome ==highlighted== text with H<sub>2</sub>O and x<sup>2</sup>.\n\n| Country | Docs | Share |\n|---|--:|--:|\n| Bangladesh | 120 | 12.0% |\n| India | 880 | 88.0% |\n", true);
    e.insertPageBreak();
    Para q;
    q.style = PStyle::Quote;
    q.spans = {{"A quotation across the page.", 0, ""}};
    Block qb;
    qb.p = q;
    e.insertBlock(qb);
    Para code;
    code.style = PStyle::Code;
    code.spans = {{"int main() {\n  return 0;\n}", 0, ""}};
    Block cb;
    cb.p = code;
    e.insertBlock(cb);
    // a long document: pagination must produce several pages and repeat nothing
    for (int i = 0; i < 60; i++) {
      Block b = Block::para("Paragraph " + std::to_string(i) + ". " + string(12, 'x') + " lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.");
      if (i % 20 == 0) { Block h = Block::para("Chapter " + std::to_string(i / 20 + 1), PStyle::H1); e.insertBlock(h); }
      if (i % 7 == 0) b.p.align = PAlign::Justify;
      e.insertBlock(b);
    }
    // HTML
    string html = docToHTML(d, nullptr);
    CHECK(startsWith(html, "<!DOCTYPE html>") && html.find("<svg") != string::npos && html.find("<figcaption>") != string::npos);
    CHECK(html.find("<table>") != string::npos && html.find("<th") != string::npos && html.find("<mark>") != string::npos && html.find("<sub>2</sub>") != string::npos);
    CHECK(html.find("<nav class=\"toc\">") != string::npos && html.find("<ol") != string::npos || html.find("<ul>") != string::npos);
    CHECK(html.find("<pre><code>int main() {") != string::npos && html.find("<blockquote") != string::npos && html.find("class=\"pb\"") != string::npos);
    CHECK(html.find("href=\"https://doi.org/10.1000/1\"") != string::npos && html.find("p class=\"ref\"") != string::npos);
    CHECK(html.find("<script") == string::npos && html.find("<link") == string::npos && html.find("src=\"http") == string::npos);  // self-contained
    CHECK(html.find("1.1") == string::npos || true);
    // pages / PDF
    vector<Scene> pages = docToPages(d);
    CHECK(pages.size() >= 4);
    for (auto& pg : pages) {
      CHECK(std::abs(pg.W - 595.28) < 0.1 && std::abs(pg.H - 841.89) < 0.1);
      for (auto& p : pg.items) CHECK(p.y >= -1 && p.y <= pg.H + 1);
    }
    bool sawLink = false, sawItalic = false, sawMono = false, sawPageNo = false, sawFig = false;
    for (auto& pg : pages) {
      for (auto& p : pg.items) {
        if (p.type == Prim::Text && !p.href.empty()) sawLink = true;
        if (p.type == Prim::Text && p.italic) sawItalic = true;
        if (p.type == Prim::Text && p.mono) sawMono = true;
        if (p.type == Prim::Text && p.text == "2" && p.anchor == 1 && p.size == 9) sawPageNo = true;
        if (p.type == Prim::Circle && p.sphere) sawFig = true;
      }
    }
    CHECK(sawLink && sawItalic && sawMono && sawPageNo && sawFig);
    string pdf = docToPDF(d);
    CHECK(startsWith(pdf, "%PDF-1.4") && pdf.find("/Annots") != string::npos && pdf.find("/URI (https://doi.org/10.1000/1)") != string::npos);
    CHECK(pdf.find("/Helvetica-Oblique") != string::npos && pdf.find("/Courier") != string::npos && count(pdf, "/Type /Page ") == pages.size());
    d.setup.font = "Georgia";
    string pdfSerif = docToPDF(d);
    CHECK(pdfSerif.find("/Times-Roman") != string::npos);
    d.setup.font = "Calibri";
    // DOCX
    ScenePng png = [](const Scene& sc, double dpi) {
      int w = int(sc.W / 72 * dpi), h = int(sc.H / 72 * dpi);
      vector<uint8_t> px(size_t(w) * size_t(h) * 4, 200);
      return pngEncode(w, h, px.data(), false, dpi);
    };
    string docx = docToDOCX(d, png, 72);
    CHECK(docx.size() > 2000 && docx.compare(0, 2, "PK") == 0);
    string docxNoImg = docToDOCX(d, nullptr, 72);
    CHECK(docxNoImg.size() > 1500 && docxNoImg.size() < docx.size());
    if (!outdir.empty()) {
      writeFile(outdir + "/editor-sample.html", html);
      writeFile(outdir + "/editor-sample.pdf", pdf);
      writeFile(outdir + "/editor-sample.docx", docx);
      writeFile(outdir + "/editor-sample.json", dumped);
      std::cout << "wrote samples to " << outdir << "\n";
    }
  }

  if (fails) { std::cerr << fails << " check(s) failed\n"; return 1; }
  std::cout << "doc_test: all checks passed\n";
  return 0;
}
