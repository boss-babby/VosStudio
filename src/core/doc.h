// VOSStudio Native: the editable document. One model serves the writer (the in-application editor), the assistant
// (it reads and appends Markdown) and the three exporters (Word, PDF, HTML), so what is edited is what is exported.
//
// A document is a list of blocks: paragraphs of styled spans (title, headings, body, list items, quotes, code,
// captions, references), tables of paragraphs, figures (vector scenes drawn by the application), rules, page breaks
// and a table of contents. Positions are (block, cell, byte offset); the Editor applies every change through a
// small set of operations with undo, and the platform layer only adds drawing, hit-testing and the keyboard map.
#pragma once
#include "figure.h"
#include "json.h"
#include "report.h"

namespace vs {

// ---------------------------------------------------------------- content
enum Fmt : uint16_t { F_BOLD = 1, F_ITALIC = 2, F_UNDER = 4, F_STRIKE = 8, F_CODE = 16, F_SUB = 32, F_SUP = 64, F_MARK = 128, F_MATH = 256 };

struct Span {
  Span() = default;
  Span(string t, uint16_t f = 0, string l = string()) : text(std::move(t)), fmt(f), link(std::move(l)) {}
  string text;
  uint16_t fmt = 0;
  string link;         // absolute URL; "" = none
  string font;         // font family override; "" = the document font
  float size = 0;      // point size override; 0 = the paragraph style's size
  uint32_t color = 0;  // 0xAARRGGBB text colour; 0 = the style's colour
  string cite;         // citation field: reference keys separated by ';' — the text is generated from the citation style
  bool sameStyle(const Span& o) const { return fmt == o.fmt && link == o.link && font == o.font && size == o.size && color == o.color && cite.empty() && o.cite.empty(); }
  bool isCite() const { return !cite.empty(); }
};

enum class PStyle { Body, Title, Subtitle, Meta, H1, H2, H3, H4, Bullet, Number, Quote, Code, Caption, Reference };
enum class PAlign { Left, Center, Right, Justify };
const char* pstyleName(PStyle s);         // "Heading 1"
const char* pstyleId(PStyle s);           // "h1" (saved files, scripts)
bool pstyleFromId(const string& id, PStyle& s);
inline bool isHeading(PStyle s) { return s == PStyle::H1 || s == PStyle::H2 || s == PStyle::H3 || s == PStyle::H4; }
inline int headingLevel(PStyle s) { return s == PStyle::H1 ? 1 : s == PStyle::H2 ? 2 : s == PStyle::H3 ? 3 : s == PStyle::H4 ? 4 : 0; }
inline PStyle headingStyle(int level) { return level <= 1 ? PStyle::H1 : level == 2 ? PStyle::H2 : level == 3 ? PStyle::H3 : PStyle::H4; }
inline bool isList(PStyle s) { return s == PStyle::Bullet || s == PStyle::Number; }

struct Para {
  PStyle style = PStyle::Body;
  PAlign align = PAlign::Left;
  int level = 0;     // list nesting 0..2
  int numStart = 0;  // Number: 0 = continues the numbered list above, n = starts a new list at n
  string refKey;     // Reference paragraphs generated from Document::refs carry their entry's key (a bibliography field)
  uint64_t refGen = 0;  // hash of the generated text: an entry the user edited by hand is left alone by the style
  vector<Span> spans;
  string text() const;
  size_t size() const;  // bytes
  bool empty() const { return size() == 0; }
  void normalize();     // merges equal neighbouring spans, drops empty ones (keeps one empty span's format)
  uint16_t fmtAt(size_t off) const;  // format of the character before off (what typing there inherits)
  string linkAt(size_t off) const;
};

struct Table {
  vector<vector<Para>> cells;  // rows x cols (every row has cols() cells)
  vector<char> align;          // per column: 'l' 'c' 'r' or 'a' (auto: numbers right)
  vector<float> widths;        // per column, fractions of the text width; empty = equal
  bool header = true;          // the first row repeats on every page
  int rows() const { return int(cells.size()); }
  int cols() const { return cells.empty() ? 0 : int(cells[0].size()); }
  void fixup();                // makes every row cols() long, align/widths sized
};

struct Figure {
  int asset = -1;      // index into Document::assets (-1 = missing)
  int widthPct = 100;  // of the text width
  Para caption;        // Caption style; the exporters put "Figure n." in front
  string alt;          // alternative text (Word, HTML)
  string format;       // "vector" (SVG / native PDF drawing) | "png" (picture); "" = the document default
  string label;        // caption label: "" = "Figure"; "Fig.", "Chart", "Map", or any word; "-" = no label and no number
  bool captionAbove = false, border = false;
  PAlign align = PAlign::Center;  // of the image
  string formatOr(const string& def) const { return format.empty() ? def : format; }
  string labelText() const { return label.empty() ? string("Figure") : label; }
};

struct Block {
  enum Kind { Paragraph, TableBlock, FigureBlock, Rule, PageBreak, Toc } kind = Paragraph;
  Para p;      // Paragraph
  Table tbl;   // TableBlock
  Figure fig;  // FigureBlock
  bool isText() const { return kind == Paragraph; }
  static Block para(const string& text, PStyle st = PStyle::Body);
};

struct FigAsset {
  Scene scene;          // vector content, any size (scaled to the text width)
  string id, title;     // chart id ("publications_per_year", "map_network") and title
  string summary;       // data summary for the assistant
};

struct PageSetup {
  string paper = "A4";        // "A4" | "Letter"
  string margins = "normal";  // "normal" (2.54 cm) | "narrow" (1.27 cm) | "wide" (3.81 cm)
  string font = "Calibri";    // body font family (any installed family; the PDF maps it to its Helvetica / Times / Courier class)
  string headingFont;         // "" = the body font
  float baseSize = 11;        // pt
  float lineSpacing = 1.15f;  // 1.0 single … 2.0 double
  bool numberedHeadings = false, toc = false, pageNumbers = true;
  string header;              // running head printed at the top of every page ("" = none)
  string figureFormat = "vector";  // default for new figures: "vector" | "png"
  double pageW() const, pageH() const, margin() const;  // points
  double textW() const { return pageW() - 2 * margin(); }
  bool serif() const { return serifFamily(font); }
  static bool serifFamily(const string& family);  // Cambria, Georgia, Times New Roman, Garamond, … (also "…Serif" without "Sans")
  static bool monoFamily(const string& family);   // Consolas, Courier New, Cascadia …
  string headingFamily() const { return headingFont.empty() ? font : headingFont; }
};

// ---------------------------------------------------------------- references
struct RefEntry {
  string key;               // citation field key: "doi:10.…" | "t:<hash of title+year>" | "raw:<hash>"
  string recordId;          // durable corpus identity, survives DOI/title/year edits
  string kind = "article";  // article | conference | book | chapter | thesis | report | web | software | other
  vector<string> authors;   // as given ("Smith, J. A." / "Smith JA" / "John A. Smith" are all understood)
  string year, title, container, volume, issue, pages, publisher, place, doi, url, raw;
  int rec = -1;             // corpus record index the entry came from (-1 = none)
  bool structured() const { return raw.empty(); }
};
struct CiteStyleInfo { const char* id; const char* name; bool numeric; const char* wordXsl; const char* wordName; };
const vector<CiteStyleInfo>& citeStyles();          // apa, mla, chicago, harvard, ieee, vancouver, nature
const CiteStyleInfo& citeStyleInfo(const string& id);  // unknown -> apa
struct RefName { string last, given, initials; };
RefName splitAuthor(const string& name);
vector<Span> formatReference(const RefEntry& e, const string& style, int number);   // one bibliography entry (italics, DOI link)
string formatCitation(const vector<const RefEntry*>& refs, const vector<int>& numbers, const string& style, uint16_t& fmt);  // in-text; fmt gets F_SUP for superscript styles
string refSortKey(const RefEntry& e);                // alphabetical order for author-date styles
string refKeyFor(const string& doi, const string& title, const string& year);  // the stable key
string wordSourceType(const string& kind);           // Word bibliography SourceType

struct Document {
  vector<Block> blocks;
  vector<FigAsset> assets;
  vector<RefEntry> refs;      // the bibliography (Reference paragraphs and citation texts are generated from it)
  string citeStyle = "apa";   // see citeStyles()
  PageSetup setup;
  string author, keywords, created;
  const RefEntry* findRef(const string& key) const;
  vector<string> citedKeys() const;  // in order of first citation
  bool empty() const;
  string title() const;  // first Title paragraph (or first heading)
  int figures() const, tables() const, words() const;
  int figureNumber(int blk) const;  // 1-based number of the figure block, 0 if not a figure
  int tableNumber(int blk) const;
  int refNumber(int blk) const;     // 1-based number of a Reference paragraph among all Reference paragraphs
  vector<int> outline() const;      // heading block indices
  int addAsset(const Scene& sc, const string& id, const string& title, const string& summary);
  void dropUnusedAssets();
  string plainText() const;
};

// ---------------------------------------------------------------- positions
struct DocPos {
  int blk = 0, cell = -1, off = 0;  // cell: -1 the block's own paragraph, r * cols + c a table cell, -2 a figure caption
  bool operator==(const DocPos& o) const { return blk == o.blk && cell == o.cell && off == o.off; }
  bool operator!=(const DocPos& o) const { return !(*this == o); }
};
bool docBefore(const DocPos& a, const DocPos& b);        // document order (cells in row-major order after the block itself)
Para* docPara(Document& d, const DocPos& p);             // nullptr for a rule, page break, toc or a table's own position
const Para* docPara(const Document& d, const DocPos& p);
size_t cpPrev(const string& s, size_t off);              // UTF-8 boundaries
size_t cpNext(const string& s, size_t off);

// ---------------------------------------------------------------- editing
struct DocFragment {  // clipboard content: spans of one paragraph, or whole blocks
  vector<Span> spans;
  vector<Block> blocks;
  bool empty() const { return spans.empty() && blocks.empty(); }
};

class Editor {
 public:
  Document* d = nullptr;
  DocPos caret, anchor;
  uint16_t typing = 0;        // format toggled without a selection: applies to the next typed characters
  bool typingSet = false;
  string typingLink;

  explicit Editor(Document* doc = nullptr) : d(doc) {}
  void attach(Document* doc) { d = doc; caret = anchor = DocPos(); clampPos(caret); clampPos(anchor); undo_.clear(); redo_.clear(); }
  bool hasSelection() const { return caret != anchor; }
  DocPos selStart() const { return docBefore(caret, anchor) ? caret : anchor; }
  DocPos selEnd() const { return docBefore(caret, anchor) ? anchor : caret; }
  void clampPos(DocPos& p) const;
  void setCaret(const DocPos& p, bool extend = false);
  Para* curPara() { return docPara(*d, caret); }
  Block& curBlock() { return d->blocks[size_t(caret.blk)]; }
  bool inTable(int* row = nullptr, int* col = nullptr) const;
  bool blockSelected() const { return !hasSelection() && !docPara(*d, caret); }  // caret on a rule / page break / toc / table frame

  // text
  void insertText(const string& utf8);      // typed or pasted plain text (no newlines)
  void insertMath(const string& latex);      // insert/replace a first-class editable LaTeX math span
  string mathAtCaret() const;                // formula under the caret or exactly selected
  bool selectMathAtCaret();                  // selects the complete formula under the caret
  void insertParagraphBreak();              // Enter
  void insertLineBreak();                   // Shift+Enter: a soft break ("\n" inside the paragraph)
  void backspace();
  void del();
  bool deleteSelection();
  void moveChar(int dir, bool extend);
  void moveWord(int dir, bool extend);
  void moveBlock(int dir, bool extend);     // to the start of the previous / next paragraph
  void home(bool extend), end(bool extend); // paragraph start / end (the platform layer does lines)
  void docStart(bool extend), docEnd(bool extend);
  void selectAll(), selectWord(), selectParagraph();

  // formatting
  uint16_t fmtHere() const;                 // format at the caret (selection: of its first character), plus `typing`
  void toggleFmt(uint16_t f);
  void setLink(const string& url);          // "" removes; without a selection the word at the caret (or url as text)
  string linkHere() const;
  PStyle styleHere() const;
  void setStyle(PStyle s);                  // every paragraph in the selection
  PAlign alignHere() const;
  void setAlign(PAlign a);
  void toggleList(bool ordered);
  void indent(int dir);                     // list level, or Tab in a table (moves cells)
  void clearFormatting();

  // blocks
  void insertBlock(const Block& b);         // after the current block (or splits the paragraph at the caret)
  void insertTable(int rows, int cols);
  void insertFigure(int asset, const string& caption, int widthPct = 100);
  void insertRule(), insertPageBreak(), insertToc();
  void deleteBlock(int blk);
  void moveBlockUpDown(int dir);
  void setFigureWidth(int pct);
  // tables (the caret's table)
  void tableInsertRow(bool below), tableInsertCol(bool right), tableDeleteRow(), tableDeleteCol(), tableDeleteTable();
  void tableToggleHeader();
  void tableSetColAlign(char a);
  void tableSetWidths(const vector<float>& w);
  bool tableNextCell(int dir);              // Tab / Shift+Tab; adds a row after the last cell

  // clipboard
  DocFragment copy() const;
  DocFragment cut();
  void paste(const DocFragment& f);
  void pasteText(const string& text, bool markdown);  // plain text: paragraphs at blank lines; markdown: parsed
  string selectionText() const;
  string selectionMarkdown() const;

  // search
  bool find(const string& what, bool forward, bool matchCase, bool wrap = true);
  int replaceAll(const string& what, const string& with, bool matchCase);
  bool replaceCurrent(const string& what, const string& with, bool matchCase);

  // undo
  void begin(const string& label, bool coalesce = false);  // snapshot before a change (typing coalesces)
  bool undo(), redo();
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  void breakCoalescing() { lastLabel_.clear(); }
  uint64_t version = 0;  // bumps on every change (layout caches key on it)
  void externalChange() { changed(); breakCoalescing(); refreshCitations(); clampPos(caret); clampPos(anchor); }  // the document was edited directly (assistant tools): count it, keep the caret valid

  // character properties beyond the format bits
  void setFont(const string& family);   // "" = the document font
  void setSize(float pt);               // 0 = the style's size
  void setColor(uint32_t argb);         // 0 = the style's colour
  string fontHere() const;
  float sizeHere() const;
  uint32_t colorHere() const;
  string typingFont;                    // pending properties for the next typed characters (like `typing`)
  float typingSize = 0;
  uint32_t typingColor = 0;
  bool typingPropsSet = false;

  // figures
  void setFigure(const Figure& f);      // properties of the caret's figure block (asset and caption are kept)
  const Figure* figureHere() const;

  // references and citations
  int insertCitation(const string& referenceText, const string& url);  // free-text entry; returns its number
  bool insertCitation(const vector<string>& keys);  // keys of Document::refs (added first with docAddReference); one field
  vector<string> citeHere() const;      // keys of the citation field at the caret ("" when none)
  bool removeCitationHere();
  void setCiteStyle(const string& id);
  void refreshCitations();              // docRefreshCitations + caret fix-up (called by the edits that can move citations)
  void applyFmtPublic(Para& p, size_t a, size_t b, const std::function<void(Span&)>& fn);

 private:
  struct Snap { vector<Block> blocks; DocPos caret, anchor; string label; vector<RefEntry> refs; string citeStyle; size_t bytes = 0; };
  vector<Snap> undo_, redo_;
  static size_t snapBytes(const vector<Block>& blocks);  // rough size of a snapshot (text of every span); the undo history is bounded by bytes, not only by count
  void trimUndo();
  string lastLabel_;
  void changed() { version++; }
  void splitAt(const DocPos& p, Para& left, Para& right) const;
  void eraseRange(const DocPos& a, const DocPos& b);
  vector<int> selectedBlocks() const;  // block indices touched by the selection (caret block when none)
  void forEachSelectedPara(const std::function<void(Para&)>& fn);
  void applyFmt(Para& p, size_t a, size_t b, const std::function<void(Span&)>& fn);
};

// ---------------------------------------------------------------- import / export helpers
// Markdown (the assistant's dialect: paragraphs, #/##/### headings, - and 1. lists with nesting, > quotes, ``` code,
// --- rules, pipe tables, **bold** *italic* `code` ~~strike~~ links, <sub>/<sup>/<br>, entities, bare URLs and DOIs).
// headingBase: the style a single '#' maps to (1 = H1); the smallest level used in the text maps there.
// dropTitle: a first heading equal to this title is not repeated.
void docAppendMarkdown(Document& d, const string& markdown, int headingBase = 1, const string& dropTitle = "");
vector<Span> spansFromMarkdown(const string& line, bool markdown = true, bool autolink = true);
vector<Block> blocksFromMarkdown(const string& markdown, int headingBase = 1, const string& dropTitle = "");
string spansToMarkdown(const vector<Span>& spans);
string blocksToMarkdown(const Document& d);   // whole document (figures as "![Figure n](...)" lines)
string paraToMarkdown(const Para& p);
Document docFromReport(const ReportDoc& r);   // the assistant's staging report: title block, sections, figures, references
string docOutlineText(const Document& d);     // for the assistant: numbered headings, figures and tables
vector<int> docListNumbers(const Document& d);     // per block: the number of a numbered-list paragraph (0 otherwise)
vector<string> docHeadingNumbers(const Document& d);  // per block: "2.1" for headings (numbered headings), "" otherwise

// References and the assistant's view of the document as sections (H1 headings; "References" is not a section).
int docBodyEnd(const Document& d);                                        // index of the References heading, or blocks.size()
int docAddReference(Document& d, const string& text, const string& url);  // free-text entry: find or append; returns its number
int docAddReference(Document& d, const RefEntry& e);                       // find by key or durable record ID, else append
struct Record;
struct Corpus;
int migrateDocRecordLinks(Document& d, const Corpus& corpus);  // cautiously adopts legacy DOI/title/year/record-index links
void docMergeRecordLinks(Document& d, const Record& keep, const string& removedRecordId, int keepIndex, int removedIndex);
void docUpdateRecordReference(Document& d, const Record& r, int recIndex = -1);  // refreshes citations after record metadata edits
RefEntry refFromRecord(const Record& r);
void docRefreshCitations(Document& d, DocPos* caret = nullptr, DocPos* anchor = nullptr);  // numbering/order, citation texts, Reference paragraphs
int docRefIndexOf(const Document& d, const string& key);
string docResolveCitations(const string& markdown, const std::function<int(int)>& numberFor);  // "[R12, R4]" -> "[3, 4]" (0 = dropped)
string docResolveCitationKeys(const string& markdown, const std::function<string(int)>& keyFor);  // "[R12, R4]" -> "[@key;@key]" citation fields ("" = dropped)
struct DocSection { int heading = -1, first = 0, last = -1; string title; };  // blocks first..last (inclusive); heading -1 = front matter
vector<DocSection> docSections(const Document& d);
string docSectionMarkdown(const Document& d, const DocSection& s);
void docReplaceBlocks(Document& d, int first, int last, const vector<Block>& with);  // last inclusive; empty `with` removes
int docInsertSection(Document& d, const string& title, const vector<Block>& body, int before = -1);  // before the References (or at `before`); returns the heading index

Json docToJson(const Document& d, bool withAssets = true);
bool docFromJson(Document& d, const Json& j);
string sceneToBlob(const Scene& sc);          // compact binary, base64 (project files)
bool sceneFromBlob(const string& blob, Scene& sc);

// Exporters. `png` renders a scene at the dpi to PNG bytes (the platform layer draws with the GPU); it may be null,
// then figures are left out of Word / HTML files (the PDF is vector and needs no renderer).
using ScenePng = std::function<string(const Scene&, double dpi)>;
string docToHTML(const Document& d, const ScenePng& png = nullptr);       // one self-contained file (inline CSS, inline SVG or base64 PNG figures)
vector<Scene> docToPages(const Document& d);                              // paginated (page size from the setup)
string docToPDF(const Document& d);                                       // toPDF(docToPages(d))
string docToDOCX(const Document& d, const ScenePng& png, double dpi = 200);  // docx.cpp
struct LatexBundle {
  string source;                                      // standalone, visual-model-derived main.tex
  string bibliography;                               // companion BibTeX for library interoperability
  vector<std::pair<string, string>> assets;           // relative filename + bytes (vector figures are PDF)
  vector<string> warnings;                            // content the exporter could not represent faithfully
};
LatexBundle docToLatex(const Document& d, const string& assetDirectory = "figures");  // visual-model export; no code editor required

// Clipboard exchange with Word (docclip.cpp). The HTML is the dialect Word writes itself: Mso paragraph classes, real
// lists and grid tables, pictures as files, CITATION / SEQ / BIBLIOGRAPHY fields in Word's conditional comments and
// the bibliography sources as a linked dataStoreItem — so a paste into Word keeps the formatting and the citations
// are Word's own (References > Manage Sources knows them; Insert Bibliography lists them).
struct ClipOptions {
  string fileDir;      // folder for the pictures and the sources part; "" = pictures as data URIs, no sources part
  string fileUrlBase;  // the same folder as a file:///C:/... URL (no trailing slash)
  bool bibliography = false;  // append a References heading + BIBLIOGRAPHY field for the cited sources
  double dpi = 192;
  ScenePng png;        // renders figures; null = figures left out
  using SceneEmf = std::function<string(const Scene&, double wPt, double hPt)>;
  SceneEmf emf;        // vector figures for Word (an Enhanced Metafile, sized in points); null = PNG only
};
struct ClipHtml {
  string html;                                  // a complete HTML document with <!--StartFragment--> / <!--EndFragment-->
  vector<std::pair<string, string>> files;      // (file name, bytes) to write into fileDir before the clipboard is set
  int figures = 0, citations = 0;
};
ClipHtml docFragmentToWordHtml(const Document& d, const DocFragment& f, const ClipOptions& o);
std::set<string> docFragmentCitedKeys(const DocFragment& f);  // the reference keys cited inside a fragment
bool docFragmentFromHtml(const string& html, DocFragment& out);  // Word / browser HTML (or a CF_HTML buffer) -> blocks; false = nothing usable
string cfHtmlWrap(const string& html);                           // the CF_HTML ("HTML Format") header around the document
string cfHtmlFragment(const string& cfHtml);                     // the fragment out of a CF_HTML buffer (or the body of plain HTML)

}  // namespace vs
