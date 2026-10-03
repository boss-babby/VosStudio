#include "common.h"
#include "doc.h"
#include "docx.h"
#include "equation.h"
#include "inflate.h"

#include <cstdio>
#include <iostream>

using namespace vs;

namespace {
int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; } } while (0)

uint32_t u32(const string& s, size_t p) {
  return uint32_t(uint8_t(s[p])) | (uint32_t(uint8_t(s[p + 1])) << 8) | (uint32_t(uint8_t(s[p + 2])) << 16) | (uint32_t(uint8_t(s[p + 3])) << 24);
}
uint16_t u16(const string& s, size_t p) {
  return uint16_t(uint8_t(s[p])) | uint16_t(uint8_t(s[p + 1]) << 8);
}

string zipPart(const string& archive, const string& name) {
  for (size_t p = 0; p + 30 <= archive.size();) {
    if (u32(archive, p) != 0x04034B50u) break;
    uint16_t method = u16(archive, p + 8);
    uint32_t compressed = u32(archive, p + 18), plain = u32(archive, p + 22);
    uint16_t nameLen = u16(archive, p + 26), extraLen = u16(archive, p + 28);
    size_t data = p + 30 + nameLen + extraLen;
    if (data + compressed > archive.size()) return string();
    if (archive.compare(p + 30, nameLen, name) == 0) {
      string out;
      if (method == 0) out.assign(archive.data() + data, compressed);
      else if (method == 8 && !inflateRaw(reinterpret_cast<const uint8_t*>(archive.data() + data), compressed, out, plain)) return string();
      else if (method != 8) return string();
      return out;
    }
    p = data + compressed;
  }
  return string();
}
}

int main() {
  string error;
  string omml = mathToOmml(R"(\frac{x_1^2}{\sqrt[3]{y}}+\alpha)", &error);
  CHECK(error.empty() && !omml.empty());
  CHECK(omml.find("<m:oMath") != string::npos && omml.find("<m:f>") != string::npos);
  CHECK(omml.find("<m:sSubSup>") != string::npos && omml.find("<m:rad>") != string::npos);
  CHECK(omml.find("α") != string::npos);

  string mathml = mathToMathML(R"(\hat{\theta}+\text{where})", &error);
  CHECK(error.empty() && mathml.find("<math xmlns=") != string::npos);
  CHECK(mathml.find("<mover") != string::npos && mathml.find("where") != string::npos);
  CHECK(mathPreviewText(R"(\frac{a}{b}+x^2+\infty)").find("a") != string::npos);
  CHECK(mathPreviewText(R"(\frac{a}{b}+x^2+\infty)").find("⁄") != string::npos);

  error.clear();
  CHECK(mathToOmml(R"(\frac{a}{)", &error).empty() && !error.empty());
  CHECK(mathPreviewText(R"(\frac{a}{)") == R"(\frac{a}{)");  // malformed source is not lost
  CHECK(mathToOmml(string(32769, 'x'), &error).empty() && !error.empty());
  error.clear();
  CHECK(mathToOmml(R"(\unsupported{z})", &error).empty() && error.find("Unsupported LaTeX command") != string::npos);
  CHECK(mathPreviewText(R"(\unsupported{z})") == R"(\unsupported{z})");  // unsupported source stays visible, not falsely rendered

  Document d;
  d.blocks.push_back(Block::para("A "));
  Span formula;
  formula.text = R"(\frac{x_1^2}{2})";
  formula.fmt = F_MATH;
  d.blocks[0].p.spans.push_back(formula);
  d.blocks[0].p.spans.push_back(Span(" B"));
  CHECK(d.words() == 2);  // formula source is not counted as ordinary prose
  string html = docToHTML(d);
  CHECK(html.find("<math xmlns=") != string::npos);
  string latex = docToLatex(d).source;
  CHECK(latex.find(R"(\(\frac{x_1^2}{2}\))") != string::npos);
  vector<Span> mdSpans = spansFromMarkdown(spansToMarkdown({formula}));
  CHECK(mdSpans.size() == 1 && (mdSpans[0].fmt & F_MATH) && mdSpans[0].text == formula.text);

  Json saved = docToJson(d);
  Document restored;
  CHECK(docFromJson(restored, saved));
  CHECK(restored.blocks[0].p.spans.size() == 3 && (restored.blocks[0].p.spans[1].fmt & F_MATH));

  Document editDoc;
  editDoc.blocks.push_back(Block::para("Before after"));
  Editor editor(&editDoc);
  editor.setCaret({0, -1, 7});
  editor.insertMath(R"(x^2+y^2=z^2)");
  CHECK(editDoc.blocks[0].p.spans.size() == 3);
  CHECK((editDoc.blocks[0].p.spans[1].fmt & F_MATH) != 0);
  CHECK(editor.mathAtCaret() == R"(x^2+y^2=z^2)");
  CHECK(editor.selectMathAtCaret() && editor.hasSelection());
  editor.insertMath(R"(\sqrt{x})");
  CHECK(editor.mathAtCaret() == R"(\sqrt{x})");
  CHECK(editDoc.blocks[0].p.text().find("x^2+y^2") == string::npos);
  editor.undo();
  CHECK(editor.mathAtCaret() == R"(x^2+y^2=z^2)");

  string docx = docToDOCX(d, nullptr);
  string documentXml = zipPart(docx, "word/document.xml");
  CHECK(!documentXml.empty());
  CHECK(documentXml.find("<m:oMath") != string::npos && documentXml.find("<m:f>") != string::npos);
  CHECK(documentXml.find("xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"") != string::npos);

  Document fallbackDoc;
  fallbackDoc.blocks.push_back(Block::para(""));
  Span unsupported;
  unsupported.text = R"(\unsupported{z})";
  unsupported.fmt = F_MATH;
  fallbackDoc.blocks[0].p.spans.push_back(unsupported);
  string fallbackXml = zipPart(docToDOCX(fallbackDoc, nullptr), "word/document.xml");
  CHECK(fallbackXml.find(R"(\unsupported{z})") != string::npos);  // unsupported content survives DOCX as visible source

  if (fails) { std::cerr << "equation_test: " << fails << " failures\n"; return 1; }
  std::puts("equation_test: all checks passed");
  return 0;
}
