#include "../src/core/doc.h"

#include <cstdio>
#include <iostream>

using namespace vs;

namespace {
int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; } } while (0)

size_t occurrences(const string& text, const string& needle) {
  size_t n = 0;
  for (size_t p = text.find(needle); p != string::npos; p = text.find(needle, p + needle.size())) ++n;
  return n;
}

Scene testScene() {
  Scene sc;
  sc.W = 120; sc.H = 70;
  Prim rect;
  rect.type = Prim::Rect; rect.x = 4; rect.y = 4; rect.w = 112; rect.h = 62;
  rect.fill = true; rect.fillC = Color(0.9f, 0.93f, 0.98f);
  sc.items.push_back(rect);
  Prim line;
  line.type = Prim::Path; line.d = {{'M', 12, 54, 0, 0}, {'L', 60, 18, 0, 0}, {'L', 104, 42, 0, 0}};
  line.stroke = true; line.sw = 2; line.strokeC = Color(0.1f, 0.35f, 0.8f);
  sc.items.push_back(line);
  return sc;
}
}

int main() {
  Document d;
  d.author = "Ada Lovelace & Charles Babbage";
  d.keywords = "bibliography; 100% reproducible";
  d.setup.paper = "Letter";
  d.setup.margins = "narrow";
  d.setup.font = "Georgia";
  d.setup.headingFont = "Arial";
  d.setup.baseSize = 11;
  d.setup.lineSpacing = 1.4f;
  d.setup.numberedHeadings = false;
  d.setup.toc = true;
  d.setup.header = "Review & Notes";
  d.setup.pageNumbers = false;

  d.blocks.push_back(Block::para("Visual & honest: 50%", PStyle::Title));
  d.blocks.push_back(Block::para("A structured document", PStyle::Subtitle));
  d.blocks.push_back(Block::para("Section with 100% coverage", PStyle::H1));
  Span colored("colored");
  colored.color = 0xFF12AB34;
  Block body = Block::para("");
  body.p.spans = {
      Span("Escapes: 50% & "),
      Span("x_1 #tag", F_BOLD),
      Span("; "),
      Span("emphasis", F_ITALIC | F_UNDER),
      Span("; "),
      Span("removed", F_STRIKE),
      Span("; "),
      Span("sub", F_SUB),
      Span(" and "),
      Span("super", F_SUP),
      Span("; "),
      Span("marked", F_MARK),
      Span("; "),
      colored,
      Span("; "),
      Span("linked", 0, "https://example.test/?a=1&b=2"),
      Span("; literal TeX: "),
      Span(R"(\end{document} % not a command)")};
  d.blocks.push_back(body);

  Block bullet = Block::para("Bullet item", PStyle::Bullet);
  bullet.p.level = 0; d.blocks.push_back(bullet);
  Block nested = Block::para("Nested bullet", PStyle::Bullet);
  nested.p.level = 1; d.blocks.push_back(nested);
  Block numbered = Block::para("Numbered item", PStyle::Number);
  numbered.p.numStart = 4; d.blocks.push_back(numbered);

  d.blocks.push_back(Block::para("Table notes & methods", PStyle::Caption));
  Block tableBlock;
  tableBlock.kind = Block::TableBlock;
  tableBlock.tbl.cells.resize(2, vector<Para>(2));
  tableBlock.tbl.cells[0][0].spans = {Span("Measure")};
  tableBlock.tbl.cells[0][1].spans = {Span("Value")};
  tableBlock.tbl.cells[1][0].spans = {Span("Percent & rate")};
  tableBlock.tbl.cells[1][1].spans = {Span("100%\nper year")};
  tableBlock.tbl.align = {'l', 'r'};
  tableBlock.tbl.widths = {0.65f, 0.35f};
  tableBlock.tbl.header = true;
  tableBlock.tbl.fixup();
  d.blocks.push_back(tableBlock);

  int asset = d.addAsset(testScene(), "test-chart", "Test chart", "synthetic vector fixture");
  Block figure;
  figure.kind = Block::FigureBlock;
  figure.fig.asset = asset;
  figure.fig.widthPct = 72;
  figure.fig.caption.spans = {Span("A vector figure & caption")};
  figure.fig.alt = "A line from A to B";
  figure.fig.label = "Chart";
  figure.fig.border = true;
  figure.fig.captionAbove = true;
  d.blocks.push_back(figure);

  RefEntry ref;
  ref.key = "doi:10.1000/demo_1";
  ref.kind = "article";
  ref.authors = {"Smith, Jane", "Lee, Kim"};
  ref.year = "2024";
  ref.title = "A 50% reliable result";
  ref.container = "Journal of Tests";
  ref.volume = "12";
  ref.issue = "3";
  ref.pages = "1-10";
  ref.doi = "10.1000/demo_1";
  ref.url = "https://doi.org/10.1000/demo_1";
  d.refs.push_back(ref);
  RefEntry collidingBibKey = ref;
  collidingBibKey.key = "doi/10.1000/demo_1";  // punctuation normalizes to the same BibTeX key as the DOI above
  collidingBibKey.title = "A second record with a colliding key";
  d.refs.push_back(collidingBibKey);
  Block citation = Block::para("");
  citation.p.spans = {Span("Prior work: ")};
  Span cite;
  cite.cite = ref.key;
  citation.p.spans.push_back(cite);
  d.blocks.push_back(citation);
  docRefreshCitations(d);

  LatexBundle bundle = docToLatex(d);
  CHECK(!bundle.source.empty());
  CHECK(bundle.source.find("\\documentclass[11pt,letterpaper]{article}") != string::npos);
  CHECK(bundle.source.find("paper=letterpaper,margin=0.500in") != string::npos);
  CHECK(bundle.source.find("\\sffamily") == string::npos);  // Georgia is mapped to the generic serif family.
  CHECK(bundle.source.find("\\rmfamily") != string::npos);
  CHECK(bundle.source.find("Visual \\& honest: 50\\%") != string::npos);
  CHECK(bundle.source.find("Escapes: 50\\% \\& \\textbf{x\\_1 \\#tag}") != string::npos);
  CHECK(bundle.source.find("\\uline{\\emph{emphasis}}") != string::npos);
  CHECK(bundle.source.find("\\sout{removed}") != string::npos);
  CHECK(bundle.source.find("\\textsubscript{sub}") != string::npos && bundle.source.find("\\textsuperscript{super}") != string::npos);
  CHECK(bundle.source.find("\\colorbox{yellow!40}{marked}") != string::npos);
  CHECK(bundle.source.find("\\textcolor[HTML]{12ab34}{colored}") != string::npos);
  CHECK(bundle.source.find("\\href{https://example.test/?a=1\\&b=2}{linked}") != string::npos);
  CHECK(bundle.source.find("\\begin{itemize}") != string::npos && bundle.source.find("\\begin{enumerate}") != string::npos);
  CHECK(bundle.source.find("\\setcounter{enumi}{3}") != string::npos);
  CHECK(bundle.source.find("\\tableofcontents") != string::npos);
  CHECK(bundle.source.find("\\begin{longtable}") != string::npos && bundle.source.find("\\endfirsthead") != string::npos);
  CHECK(bundle.source.find("\\captionof{table}") != string::npos);
  CHECK(bundle.source.find("\\includegraphics[width=0.72\\linewidth]{figures/figure-1.pdf}") != string::npos);
  CHECK(bundle.source.find("\\captionsetup{name={Chart}}") != string::npos);
  CHECK(bundle.source.find("\\end{document} % not a command") == string::npos);
  CHECK(occurrences(bundle.source, "\\end{document}") == 1);
  CHECK(bundle.source.find("\\section*{References}") != string::npos);
  CHECK(bundle.assets.size() == 1 && bundle.assets[0].first == "figures/figure-1.pdf");
  CHECK(bundle.assets[0].second.size() > 100 && bundle.assets[0].second.compare(0, 5, "%PDF-") == 0);
  CHECK(bundle.bibliography.find("@article{doi-10-1000-demo_1,") != string::npos);
  CHECK(bundle.bibliography.find("@article{doi-10-1000-demo_1-2,") != string::npos);
  CHECK(bundle.bibliography.find("title = {A 50\\% reliable result}") != string::npos);
  CHECK(bundle.bibliography.find("doi = {10.1000/demo\\_1}") != string::npos);
  CHECK(!bundle.warnings.empty());  // different heading family and untagged figure alt text are reported.
  LatexBundle customAssetDir = docToLatex(d, "../Unsafe/figures");
  CHECK(customAssetDir.assets.size() == 1 && customAssetDir.assets[0].first.find("..") == string::npos);
  CHECK(customAssetDir.source.find("unsafe-figures/figure-1.pdf") != string::npos);

  if (fails) { std::cerr << "latex_test: " << fails << " failures\n"; return 1; }
  std::puts("latex_test: all checks passed");
  return 0;
}
