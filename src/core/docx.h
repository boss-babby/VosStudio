// Word (.docx) writer for reports. The output is native WordprocessingML (ECMA-376 / ISO 29500 transitional, the
// dialect Word itself writes): real paragraph and character styles (Title, Heading 1-4, Caption, List Paragraph,
// Quote, Hyperlink ...), Word lists from numbering.xml (bullets and restarting numbered lists, nested), tables with a
// grid, repeating header row and per-column alignment, inline pictures with SEQ-field captions (so "Insert Caption",
// cross-references and a table of figures work), clickable hyperlinks and DOIs, a footer with PAGE / NUMPAGES fields,
// document settings in compatibility mode 15 (Word 2013+), a font table and document properties. Pure C++17, no
// dependencies; the container is a ZIP with the XML parts deflated.
#pragma once
#include <functional>
#include <map>
#include <set>

#include "report.h"

namespace vs {

// Minimal ZIP archive writer (methods "stored" and "deflate", CRC-32). Enough for OOXML packages.
class ZipWriter {
 public:
  void add(const string& name, const string& data, bool deflate = false);
  string finish() const;  // the complete archive
  size_t entries() const { return entries_.size(); }

 private:
  struct Entry { string name; uint32_t crc, size, csize, offset; uint16_t method; };
  string body_;
  vector<Entry> entries_;
};

uint32_t crc32Bytes(const string& s);

// Renders a figure scene to PNG bytes at the given DPI (the caller decides how: GPU on Windows, tests can use
// pngEncode on a blank bitmap). When it returns an empty string the figure is written as its caption only.
using PngRenderer = std::function<string(const Scene& sc, double dpi)>;

// Builds the .docx package for a report. Section bodies use the same light Markdown as the PDF layout, read the way
// Word users expect it: consecutive lines form one paragraph (blank lines separate paragraphs, two trailing spaces or
// <br> break a line), "#".."####" headings, "- " / "* " bullets and "1. " numbered items with nesting by indentation,
// "> " quotes, ``` code fences, "---" rules, "| a | b |" tables (outer pipes optional; a |---|:--:|--:| line gives the
// header and column alignment), **bold**, *italic*, __bold__, _italic_, `code`, ~~strike~~, [text](url), bare URLs
// and DOIs (clickable), <b> <i> <sub> <sup> <br> and HTML entities. Citation numbers such as [3] stay as text; the
// reference list is numbered to match.
string toDOCX(const ReportDoc& d, const PngRenderer& png, double figureDpi = 200);

// State collected while the document part is written; the other parts are generated from it.
struct DocxCtx {
  vector<string> links;    // external hyperlink targets in order: relationship ids rIdHl1, rIdHl2, ...
  vector<int> listStarts;  // one entry per numbered list: its first number (numbering instance 2 + index)
  int nextDrawing = 1000;  // wp:docPr ids (unique per document)
  vector<string> svgParts; // 1.12: per image index, the SVG bytes when the figure is vector (word/media/figureN.svg with a PNG fallback)
  string sourcesXml;       // 1.12: Word bibliography sources (customXml/item1.xml); "" = the document has no citations
  string headerXml;        // 1.12: running head part; "" = none
  int nextSdtId = 700000;  // content-control ids
};

// Word's bibliography sources part (b:Sources) for a document with citations; fills key -> Word tag (Smi20). "" when no refs.
struct Document;
string docBibSourcesXml(const Document& d, std::map<string, string>& citeTags, const std::set<string>* only = nullptr);  // only: emit these keys (tags are still assigned over all refs)
// the same, one (tag, <b:Source>…</b:Source>) per reference — for Word's Bibliography.Sources.Add (automation)
vector<std::pair<string, string>> docBibSourceItems(const Document& d, std::map<string, string>& citeTags, const std::set<string>* only = nullptr);
const char* docBibSourcesPropsXml();  // the ds:datastoreItem part that goes with it

// Individual parts (exposed for tests).
string docxBodyXml(const string& markdown, DocxCtx& cx, const string& sectionTitle = "");
string docxDocumentXml(const ReportDoc& d, const vector<std::pair<int, int>>& figurePx, DocxCtx& cx);
string docxStylesXml(const string& font = "Calibri", int sizeHalfPt = 22, int lineTwips = 264, const string& headingFont = "");
string docxNumberingXml(const DocxCtx& cx);
string docxSettingsXml();
string docxFontTableXml(const string& extraFont = "");
string docxFooterXml();

}  // namespace vs
