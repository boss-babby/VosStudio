// Word export: a report with Markdown sections (paragraphs over several lines, nested lists, tables, quotes, code,
// links, DOIs, HTML bits), figures and references becomes a valid, native .docx package. Run:
//   g++ -std=c++17 -O1 tests/docx_test.cpp src/core/*.cpp -Isrc/core -o build/docx_test -pthread && ./build/docx_test [out.docx]
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "common.h"
#include "docx.h"

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

// well-formedness on the cheap: tags balance and no raw control characters
static bool balanced(const string& xml) {
  vector<string> stack;
  for (size_t i = 0; i < xml.size(); i++) {
    if (xml[i] != '<') continue;
    if (xml.compare(i, 2, "<?") == 0) { i = xml.find("?>", i); continue; }
    size_t e = xml.find('>', i);
    if (e == string::npos) return false;
    string tag = xml.substr(i + 1, e - i - 1);
    bool close = tag[0] == '/', self = tag.back() == '/';
    string name = tag.substr(close ? 1 : 0);
    size_t sp = name.find_first_of(" /");
    if (sp != string::npos) name = name.substr(0, sp);
    if (close) { if (stack.empty() || stack.back() != name) return false; stack.pop_back(); }
    else if (!self) stack.push_back(name);
    i = e;
  }
  return stack.empty();
}

int main(int argc, char** argv) {
  // --- zip + crc
  CHECK(crc32Bytes("123456789") == 0xCBF43926u);
  ZipWriter z;
  z.add("a.txt", "hello");
  string arc = z.finish();
  CHECK(arc.size() == 30 + 5 + 5 + 46 + 5 + 22);
  CHECK(arc.compare(0, 4, "PK\x03\x04") == 0);
  {
    ZipWriter zd;
    string big(4000, 'x');
    zd.add("b.txt", big, true);
    string a2 = zd.finish();
    CHECK(a2.size() < 1000);  // deflated
    CHECK(uint8_t(a2[8]) == 8);  // method in the local header
  }

  // --- inline Markdown → runs
  DocxCtx cx;
  string x = docxBodyXml("Plain **bold** and *italic* and `code` and ~~gone~~, p < 0.05* and 2 * 3 and snake_case_name.", cx);
  CHECK(count(x, "<w:b/>") == 1 && count(x, "<w:i/>") == 1 && count(x, "<w:strike/>") == 1 && count(x, "CodeChar") == 1);
  CHECK(x.find("0.05*") != string::npos && x.find("2 * 3") != string::npos && x.find("snake_case_name") != string::npos);
  CHECK(x.find("**") == string::npos && x.find("~~") == string::npos);
  x = docxBodyXml("See [the site](https://example.org/a b?x=1&y=2) and https://doi.org/10.1000/xyz123. Also doi:10.1234/abc.def, and www.example.com.", cx);
  CHECK(cx.links.size() == 4);
  CHECK(cx.links[0] == "https://example.org/a%20b?x=1&y=2");
  CHECK(cx.links[1] == "https://doi.org/10.1000/xyz123" && cx.links[2] == "https://doi.org/10.1234/abc.def" && cx.links[3] == "https://www.example.com");
  CHECK(count(x, "<w:hyperlink r:id=\"rIdHl") == 4 && count(x, "w:val=\"Hyperlink\"") == 4);
  CHECK(x.find("the site") != string::npos && x.find("[the site]") == string::npos);
  x = docxBodyXml("[plain](not a url) and [wiki](https://en.wikipedia.org/wiki/A_(b)) end", cx);
  CHECK(cx.links.size() == 5 && cx.links[4] == "https://en.wikipedia.org/wiki/A_(b)");
  CHECK(x.find("plain") != string::npos && count(x, "w:val=\"Hyperlink\"") == 1 && x.find("> end<") != string::npos && x.find(") end") == string::npos);
  x = docxBodyXml("A&amp;B &lt;x&gt; H<sub>2</sub>O and E=mc<sup>2</sup>, line<br>break, <b>strong</b>, [12] and [3, 4].", cx);
  CHECK(x.find("A&amp;B &lt;x&gt; H") != string::npos && x.find("subscript") != string::npos && x.find("superscript") != string::npos);
  CHECK(count(x, "<w:br/>") == 1 && x.find("[12] and [3, 4]") != string::npos && count(x, "<w:b/>") == 1);
  x = docxBodyXml("Tab\there and \\*not italic\\* and 100 % \xE2\x80\x93 done.", cx);
  CHECK(x.find("<w:tab/>") != string::npos && x.find("\t") == string::npos && x.find("*not italic*") != string::npos);
  CHECK(x.find("\xE2\x80\x93") != string::npos);
  // invalid UTF-8 is repaired, control characters dropped (Word rejects a part that is not well-formed XML)
  x = docxBodyXml(string("bad \xC3(byte\x01 here \xFF end"), cx);
  CHECK(x.find("\xEF\xBF\xBD") != string::npos && x.find('\x01') == string::npos && x.find("\xFF") == string::npos && balanced(x));

  // --- blocks: paragraphs join across lines, blank lines separate, headings adapt, lists nest and restart
  cx = DocxCtx();
  x = docxBodyXml("First line of a paragraph\nsecond line of the same paragraph.\n\nSecond paragraph  \nwith a hard break.\n\n### Sub heading ###\ntext\n#### Deeper\nmore", cx, "Section");
  CHECK(count(x, "<w:p>") == 6);
  CHECK(x.find("paragraph second line") != string::npos);
  CHECK(count(x, "<w:br/>") == 1);
  CHECK(x.find("Heading2") != string::npos && x.find("Heading3") != string::npos);  // ### is the top level used here
  CHECK(x.find("Sub heading</w:t>") != string::npos);
  cx = DocxCtx();
  x = docxBodyXml("Intro:\n- one\n- two **bold**\n  - nested a\n  - nested b\n    - deep\n- three\n\n1. first\n2. second\n   continued line\n3. third\n\nBreak.\n\n3. starts at three\n4. four\n\n1. new list\n2. again", cx);
  CHECK(count(x, "<w:numId w:val=\"1\"/>") == 6);
  CHECK(count(x, "<w:ilvl w:val=\"0\"/>") == 10 && count(x, "<w:ilvl w:val=\"1\"/>") == 2 && count(x, "<w:ilvl w:val=\"2\"/>") == 1);
  CHECK(cx.listStarts.size() == 3 && cx.listStarts[0] == 1 && cx.listStarts[1] == 3 && cx.listStarts[2] == 1);
  CHECK(count(x, "<w:numId w:val=\"2\"/>") == 3 && count(x, "<w:numId w:val=\"3\"/>") == 2 && count(x, "<w:numId w:val=\"4\"/>") == 2);
  CHECK(x.find("second continued line") != string::npos);
  CHECK(x.find("ListParagraph") != string::npos && x.find("\xE2\x80\xA2") == string::npos);  // bullets come from numbering.xml, not text
  string num = docxNumberingXml(cx);
  CHECK(count(num, "<w:num ") == 4 && count(num, "<w:startOverride w:val=\"3\"/>") == 1 && count(num, "<w:abstractNum ") == 2);
  CHECK(num.find("w:numFmt w:val=\"bullet\"") != string::npos && num.find("w:numFmt w:val=\"decimal\"") != string::npos);
  // quotes, rules, code, bold-line headings, section title repeated
  cx = DocxCtx();
  x = docxBodyXml("## Introduction\n> quoted line one\n> quoted line two\n\n---\n\n```\nfor x in y:\n    print(x)\n```\n\n**Key findings**\n\nBody.", cx, "Introduction");
  CHECK(x.find("Introduction") == string::npos);  // the repeated title is dropped
  CHECK(count(x, "w:val=\"Quote\"") == 1 && x.find("quoted line one quoted line two") != string::npos);
  CHECK(x.find("<w:pBdr><w:bottom") != string::npos);
  CHECK(count(x, "w:val=\"SourceCode\"") == 2 && x.find("    print(x)") != string::npos);
  CHECK(x.find("<w:pStyle w:val=\"Heading3\"/>") != string::npos && x.find("Key findings") != string::npos);

  // --- tables: with and without outer pipes, alignment row, numbers right-aligned, escaped pipes, caption
  cx = DocxCtx();
  x = docxBodyXml("**Table 1.** Top keywords\n\n| Keyword | Occurrences | Share |\n|:---|---:|:---:|\n| machine learning | 42 | 35% |\n| a \\| b | 17 | 14% |\n\n"
                  "Country | Papers\n--- | ---\nChina | 120\nUSA | 98\n\nAfter.", cx);
  CHECK(count(x, "<w:tbl>") == 2 && count(x, "<w:tblHeader/>") == 2 && count(x, "<w:cantSplit/>") == 6);
  CHECK(x.find("machine learning") != string::npos && x.find("a | b") != string::npos);
  CHECK(count(x, "<w:jc w:val=\"right\"/>") == 3 + 3 && count(x, "<w:jc w:val=\"center\"/>") == 3);  // Occurrences (explicit) + Papers (numeric) ; Share centred
  CHECK(x.find("w:val=\"Caption\"") != string::npos && x.find("<w:keepNext/>") != string::npos);
  CHECK(x.find("<w:tblW w:w=\"5000\" w:type=\"pct\"/>") != string::npos && count(x, "<w:gridCol ") == 5);
  CHECK(x.find("w:fill=\"EEF2F7\"") != string::npos);
  CHECK(balanced(x));

  // --- a whole report
  ReportDoc d;
  d.title = "Machine learning in <bibliometrics> & beyond";
  d.subtitle = "A test report";
  d.date = "Prepared on 1 January 2026";
  d.meta.push_back("Data: 120 records, 2015\xE2\x80\x93" "2024");
  ReportBlock s;
  s.kind = ReportBlock::Section;
  s.title = "Introduction";
  s.text = "The field grew quickly [1]. **Bold** and *italic* and `code` runs.\n\n## Trends\n- first bullet\n- second bullet with [2]\n1. numbered\n2. second\n\n"
           "| Keyword | Occurrences | Share |\n|---|---|---|\n| machine learning | 42 | 35% |\n| deep learning | 17 | 14% |\n\nA closing paragraph.";
  d.blocks.push_back(s);
  ReportBlock f;
  f.kind = ReportBlock::Figure;
  f.title = "Publications per year";
  f.text = "Bars show output; the line is the cumulative share.";
  f.fig.W = 640;
  f.fig.H = 400;
  d.blocks.push_back(f);
  ReportBlock f2 = f;
  f2.title = "A tall figure";
  f2.fig.W = 400;
  f2.fig.H = 900;
  d.blocks.push_back(f2);
  d.references = {"Smith, J. (2020). A study. Journal, 1(2), 3-4. https://doi.org/10.1000/abc", "Doe, A. (2021). Another *study*. Journal, 5(6), 7-8."};

  int calls = 0;
  auto png = [&](const Scene& sc, double dpi) {
    calls++;
    int w = int(sc.W * dpi / 72), h = int(sc.H * dpi / 72);
    vector<uint8_t> rgba(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; y++)
      for (int xx = 0; xx < w; xx++) {  // a visible test pattern: frame, diagonal gradient
        uint8_t* p = &rgba[(size_t(y) * w + xx) * 4];
        bool frame = xx < 3 || y < 3 || xx >= w - 3 || y >= h - 3;
        p[0] = frame ? 40 : uint8_t(60 + 190 * xx / w);
        p[1] = frame ? 40 : uint8_t(90 + 140 * y / h);
        p[2] = frame ? 40 : 200;
      }
    return pngEncode(w, h, rgba.data(), false, dpi);
  };
  string bytes = toDOCX(d, png, 96);
  CHECK(calls == 2);
  CHECK(bytes.size() > 4000);
  DocxCtx dcx;
  string doc = docxDocumentXml(d, {{853, 533}, {533, 1200}}, dcx);
  CHECK(balanced(doc));
  CHECK(doc.find("&lt;bibliometrics&gt; &amp; beyond") != string::npos);
  CHECK(doc.find("<w:pStyle w:val=\"Title\"/>") != string::npos && doc.find("<w:pStyle w:val=\"Subtitle\"/>") != string::npos);
  CHECK(doc.find("<w:pStyle w:val=\"Heading1\"/>") != string::npos);
  CHECK(doc.find("<w:pStyle w:val=\"Heading2\"/>") != string::npos);
  CHECK(doc.find("<w:tbl>") != string::npos && doc.find("<w:tblHeader/>") != string::npos);
  CHECK(doc.find("<w:b/>") != string::npos && doc.find("<w:i/>") != string::npos && doc.find("CodeChar") != string::npos);
  CHECK(doc.find("r:embed=\"rId101\"") != string::npos && doc.find("r:embed=\"rId102\"") != string::npos);
  CHECK(count(doc, "SEQ Figure \\* ARABIC") == 2);  // captions are Word captions
  CHECK(doc.find("<wp:docPr id=\"1000\"") != string::npos && doc.find("<wp:docPr id=\"1001\"") != string::npos);
  CHECK(doc.find("descr=\"A tall figure. Bars show output; the line is the cumulative share.\"") != string::npos);
  CHECK(doc.find("[2]</w:t><w:tab/>") != string::npos && doc.find("Another *study*") != string::npos);  // references: tab element, no Markdown
  CHECK(dcx.links.size() == 1 && dcx.links[0] == "https://doi.org/10.1000/abc");
  CHECK(doc.find("cy=\"7560000\"") != string::npos);  // the tall figure is capped at 21 cm
  CHECK(doc.find("<w:footerReference") != string::npos && doc.find("<w:docGrid") != string::npos);
  CHECK(doc.find("\t") == string::npos);  // never a raw tab in text
  // parts
  string styles = docxStylesXml(), settings = docxSettingsXml();
  CHECK(balanced(styles) && balanced(settings) && balanced(docxNumberingXml(dcx)) && balanced(docxFontTableXml()) && balanced(docxFooterXml()));
  CHECK(styles.find("w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/>") != string::npos);
  CHECK(styles.find("<w:next w:val=\"Normal\"/>") != string::npos);
  CHECK(styles.find("w:styleId=\"DefaultParagraphFont\"") != string::npos && styles.find("w:styleId=\"NoList\"") != string::npos);
  CHECK(settings.find("w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"") != string::npos);
  CHECK(docxFooterXml().find(" PAGE ") != string::npos && docxFooterXml().find(" NUMPAGES ") != string::npos);
  CHECK(bytes.find("word/numbering.xml") != string::npos && bytes.find("word/settings.xml") != string::npos && bytes.find("word/fontTable.xml") != string::npos);
  // a figure without an image still gets its caption
  DocxCtx ncx;
  string noimg = docxDocumentXml(d, {{0, 0}, {0, 0}}, ncx);
  CHECK(noimg.find("<w:drawing>") == string::npos && count(noimg, "SEQ Figure") == 2);
  // renderer failure: no media part, still a valid package
  string bytes2 = toDOCX(d, [](const Scene&, double) { return string(); }, 96);
  CHECK(bytes2.size() < bytes.size() && bytes2.find("word/media") == string::npos);

  if (argc > 1) {
    FILE* fp = fopen(argv[1], "wb");
    if (fp) { fwrite(bytes.data(), 1, bytes.size(), fp); fclose(fp); std::cout << "wrote " << argv[1] << " (" << bytes.size() << " bytes)\n"; }
  }
  if (argc > 2) {  // a stress document shaped like real model output, for viewing in Word / LibreOffice
    ReportDoc r;
    r.title = "Large language models in education: a bibliometric review";
    r.subtitle = "Generated stress document \xE2\x80\x94 every Markdown construct the assistant produces";
    r.meta = {"Data: OpenAlex, 486 works, 2019\xE2\x80\x93" "2025", "Query: \"large language model*\" AND (education OR learning)", "Map: all keywords, 62 items, 5 clusters"};
    r.date = "Prepared on 29 September 2026";
    auto sec = [&](const string& title, const string& text) { ReportBlock b; b.kind = ReportBlock::Section; b.title = title; b.text = text; r.blocks.push_back(b); };
    auto figb = [&](const string& title, const string& cap, double W, double H) { ReportBlock b; b.kind = ReportBlock::Figure; b.title = title; b.text = cap; b.fig.W = W; b.fig.H = H; r.blocks.push_back(b); };
    sec("Introduction",
        "## Introduction\n"
        "Large language models (LLMs) have moved from a niche of natural language processing into\n"
        "everyday educational practice within three years [1, 2]. This report maps the 486 works\n"
        "retrieved from OpenAlex (2019\xE2\x80\x93" "2025) and describes the structure of the field, its most\n"
        "cited contributions and the themes that are rising or fading. Growth is steep: 71 % of the\n"
        "corpus appeared after 2023, and the median citation count is 4 (mean 11.3, SD 34.8).\n\n"
        "The analysis follows three questions:\n\n"
        "1. **Structure** \xE2\x80\x94 which topics form coherent research fronts?\n"
        "2. **Dynamics** \xE2\x80\x94 which topics grow, which stabilise?\n"
        "   Sub-questions concern the role of *ChatGPT* after November 2022.\n"
        "3. **Actors** \xE2\x80\x94 which countries, sources and authors carry the field?\n\n"
        "> Note: citation counts are OpenAlex counts as of the retrieval date and favour older works.\n"
        "> They should be read as a rough indicator only.\n\n"
        "Key terms are written as in the data (e.g. *generative AI*, `gpt-4`), significance is reported as p < 0.05* where marked, and\n"
        "H<sub>2</sub>O or x<sup>2</sup> appear only to test formatting. Unbalanced markers like a lone * or an _underscore_ in snake_case_names stay literal.\n"
        "Non-Latin text renders too: \xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\x82\xE0\xA6\xB2\xE0\xA6\xBE, \xE4\xB8\xAD\xE6\x96\x87, \xF0\x9F\x93\x8A emoji, and a URL with brackets: https://en.wikipedia.org/wiki/Language_model_(disambiguation).");
    figb("Publications per year", "Bars show the number of works per year; the line is the cumulative share.", 720, 400);
    sec("Research themes",
        "### Cluster 1: Assessment and feedback (18 items)\n"
        "The largest cluster gathers work on **automated assessment**, *formative feedback* and essay scoring [3\xE2\x80\x93" "5].\n"
        "Typical studies compare model-written feedback with teacher feedback on:\n"
        "- accuracy and specificity,\n"
        "- tone and encouragement,\n"
        "  - measured with rubrics,\n"
        "  - or by student ratings,\n"
        "- and time saved for the instructor.\n\n"
        "### Cluster 2: Academic integrity (14 items)\n"
        "Detection tools, policy and plagiarism dominate [6]. See https://doi.org/10.1016/j.caeai.2023.100146 for a survey.\n\n"
        "**Table 1.** The five clusters with their size and most frequent terms\n\n"
        "| # | Cluster | Items | Docs | Share | Top terms |\n"
        "|:-:|---|---:|---:|---:|---|\n"
        "| 1 | Assessment and feedback | 18 | 142 | 29.2% | automated assessment; feedback; essay scoring; rubric |\n"
        "| 2 | Academic integrity | 14 | 97 | 20.0% | plagiarism; detection; policy; ethics |\n"
        "| 3 | Programming education | 12 | 88 | 18.1% | code generation; novice programmers; copilot |\n"
        "| 4 | Medical education | 10 | 81 | 16.7% | USMLE; clinical reasoning; examinations |\n"
        "| 5 | Language learning | 8 | 78 | 16.0% | writing; EFL; conversational agents |\n\n"
        "A wide table without outer pipes follows.\n\n"
        "Year | Works | Cited | Mean | Median | Open access | Countries | Sources | Authors\n"
        "---|---|---|---|---|---|---|---|---\n"
        "2019 | 3 | 210 | 70.0 | 41 | 33% | 2 | 3 | 9\n"
        "2020 | 7 | 340 | 48.6 | 22 | 43% | 5 | 6 | 21\n"
        "2021 | 15 | 402 | 26.8 | 12 | 47% | 9 | 12 | 48\n"
        "2022 | 41 | 611 | 14.9 | 6 | 54% | 17 | 30 | 132\n"
        "2023 | 168 | 2 907 | 17.3 | 5 | 61% | 41 | 96 | 540\n"
        "2024 | 194 | 1 480 | 7.6 | 3 | 66% | 47 | 118 | 655\n"
        "2025 | 58 | 91 | 1.6 | 0 | 70% | 25 | 44 | 199\n\n"
        "---\n\n"
        "Example prompt used in one study:\n\n"
        "```\nSystem: You are a patient tutor.\nUser: Explain recursion with one example.\n    (indentation kept)\n```\n\n"
        "#### A fourth-level heading\nText under it, with a [named link](https://openalex.org \"OpenAlex\") and a bare one: www.example.org/path?a=1&b=2.");
    figb("Keyword co-occurrence map", "Network view, 62 items in 5 clusters; circle size = occurrences.", 900, 700);
    figb("A very tall figure", "Height is capped so that figure and caption share a page.", 300, 1200);
    sec("Trends and emerging topics",
        "Keyword bursts (Kleinberg, \xCE\xB3 = 0.5) mark *ChatGPT* (2023\xE2\x80\x93" "2024), *prompt engineering* (2024) and *retrieval-augmented generation* (2025) as bursting.\n"
        "Fading terms are *chatbot* and *intelligent tutoring system*.\n\n"
        "1. ChatGPT \xE2\x80\x94 strength 12.4\n"
        "2. prompt engineering \xE2\x80\x94 strength 6.1\n"
        "3. retrieval-augmented generation \xE2\x80\x94 strength 4.8\n\n"
        "A second, independent list should restart at one:\n\n"
        "1. first of the second list\n"
        "2. second of the second list\n");
    sec("Conclusion", "The field is young, fast and concentrated in a few venues. Figures 1\xE2\x80\x93" "3 and Table 1 support the three findings above; the reference list gives the 12 most cited works.");
    for (int i = 1; i <= 12; i++)
      r.references.push_back("Author" + std::to_string(i) + ", A., & Coauthor, B. (202" + std::to_string(i % 6) + "). Title of work number " + std::to_string(i) +
                             " with a fairly long title that wraps onto a second line to test the hanging indent. Journal of Testing, " + std::to_string(i) + "(2), " +
                             std::to_string(10 * i) + "-" + std::to_string(10 * i + 9) + ". https://doi.org/10.1000/test." + std::to_string(i));
    string b2 = toDOCX(r, png, 96);
    FILE* fp = fopen(argv[2], "wb");
    if (fp) { fwrite(b2.data(), 1, b2.size(), fp); fclose(fp); std::cout << "wrote " << argv[2] << " (" << b2.size() << " bytes)\n"; }
  }
  std::cout << (fails ? "FAILED" : "docx_test OK") << " (" << fails << " failures)\n";
  return fails ? 1 : 0;
}
