// VOSStudio Native: the editable document model and its editing engine (see doc.h). Everything here is portable and
// covered by tests/doc_test.cpp; the Windows writer only adds drawing, hit-testing and the keyboard map.
#include "doc.h"

#include <algorithm>
#include <cstring>

namespace vs {

// ---------------------------------------------------------------- styles
const char* pstyleName(PStyle s) {
  switch (s) {
    case PStyle::Body: return "Body";
    case PStyle::Title: return "Title";
    case PStyle::Subtitle: return "Subtitle";
    case PStyle::Meta: return "Note";
    case PStyle::H1: return "Heading 1";
    case PStyle::H2: return "Heading 2";
    case PStyle::H3: return "Heading 3";
    case PStyle::H4: return "Heading 4";
    case PStyle::Bullet: return "Bulleted list";
    case PStyle::Number: return "Numbered list";
    case PStyle::Quote: return "Quote";
    case PStyle::Code: return "Code";
    case PStyle::Caption: return "Caption";
    case PStyle::Reference: return "Reference";
  }
  return "Body";
}

const char* pstyleId(PStyle s) {
  static const char* ids[] = {"body", "title", "subtitle", "meta", "h1", "h2", "h3", "h4", "bullet", "number", "quote", "code", "caption", "reference"};
  int k = int(s);
  return k >= 0 && k < 14 ? ids[k] : "body";
}

bool pstyleFromId(const string& id, PStyle& s) {
  for (int k = 0; k < 14; k++)
    if (id == pstyleId(PStyle(k))) { s = PStyle(k); return true; }
  return false;
}

// ---------------------------------------------------------------- UTF-8
size_t cpPrev(const string& s, size_t off) {
  if (off == 0) return 0;
  off = std::min(off, s.size());
  off--;
  while (off > 0 && (uint8_t(s[off]) & 0xC0) == 0x80) off--;
  return off;
}

size_t cpNext(const string& s, size_t off) {
  if (off >= s.size()) return s.size();
  off++;
  while (off < s.size() && (uint8_t(s[off]) & 0xC0) == 0x80) off++;
  return off;
}

static bool wordChar(const string& s, size_t k) { return k < s.size() && (isalnum(uint8_t(s[k])) || uint8_t(s[k]) >= 0x80 || s[k] == '_' || s[k] == '\''); }

// ---------------------------------------------------------------- Para
string Para::text() const {
  string t;
  for (auto& s : spans) t += s.text;
  return t;
}

size_t Para::size() const {
  size_t n = 0;
  for (auto& s : spans) n += s.text.size();
  return n;
}

void Para::normalize() {
  vector<Span> out;
  for (auto& s : spans) {
    if (s.text.empty()) continue;
    if (!out.empty() && out.back().sameStyle(s)) out.back().text += s.text;
    else out.push_back(s);
  }
  spans.swap(out);
}

uint16_t Para::fmtAt(size_t off) const {
  size_t pos = 0;
  for (auto& s : spans) {
    size_t e = pos + s.text.size();
    if (off <= e && (off > pos || pos == 0)) return s.fmt;
    pos = e;
  }
  return spans.empty() ? 0 : spans.back().fmt;
}

string Para::linkAt(size_t off) const {
  size_t pos = 0;
  for (auto& s : spans) {
    size_t e = pos + s.text.size();
    if (off >= pos && off <= e && !s.link.empty()) return s.link;
    pos = e;
  }
  return "";
}

// spans[0..i) cover exactly [0, off): splits the span that contains off; returns i
static size_t splitSpan(Para& p, size_t off) {
  size_t pos = 0;
  for (size_t i = 0; i < p.spans.size(); i++) {
    size_t e = pos + p.spans[i].text.size();
    if (off == pos) return i;
    if (off < e) {
      Span right = p.spans[i];
      right.text = p.spans[i].text.substr(off - pos);
      p.spans[i].text = p.spans[i].text.substr(0, off - pos);
      p.spans.insert(p.spans.begin() + long(i) + 1, right);
      return i + 1;
    }
    pos = e;
  }
  return p.spans.size();
}

static vector<Span> sliceSpans(const Para& p, size_t a, size_t b) {
  Para c = p;
  size_t i0 = splitSpan(c, std::min(a, c.size()));
  size_t i1 = splitSpan(c, std::min(b, c.size()));
  return vector<Span>(c.spans.begin() + long(i0), c.spans.begin() + long(std::max(i0, i1)));
}

static void eraseBytes(Para& p, size_t a, size_t b) {
  if (b <= a) return;
  size_t i0 = splitSpan(p, a), i1 = splitSpan(p, b);
  p.spans.erase(p.spans.begin() + long(i0), p.spans.begin() + long(std::max(i0, i1)));
  p.normalize();
}

static void insertSpans(Para& p, size_t off, const vector<Span>& spans) {
  size_t i = splitSpan(p, off);
  p.spans.insert(p.spans.begin() + long(i), spans.begin(), spans.end());
  p.normalize();
}

// ---------------------------------------------------------------- Table / Block / PageSetup / Document
void Table::fixup() {
  int c = 0;
  for (auto& r : cells) c = std::max<int>(c, int(r.size()));
  for (auto& r : cells) while (int(r.size()) < c) r.push_back(Para());
  align.resize(size_t(c), 'a');
  if (!widths.empty()) {
    widths.resize(size_t(c), 0);
    float tot = 0;
    for (float w : widths) tot += std::max(0.f, w);
    if (tot <= 0) widths.clear();
    else for (auto& w : widths) w = std::max(0.02f, w) / tot;
  }
}

Block Block::para(const string& text, PStyle st) {
  Block b;
  b.p.style = st;
  if (!text.empty()) b.p.spans.push_back({text, 0, ""});
  return b;
}

double PageSetup::pageW() const { return paper == "Letter" ? 612.0 : 595.28; }
double PageSetup::pageH() const { return paper == "Letter" ? 792.0 : 841.89; }
double PageSetup::margin() const { return margins == "narrow" ? 36.0 : margins == "wide" ? 108.0 : 72.0; }

bool Document::empty() const {
  for (auto& b : blocks) {
    if (b.kind != Block::Paragraph) return false;
    if (!b.p.empty()) return false;
  }
  return true;
}

string Document::title() const {
  for (auto& b : blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Title && !b.p.empty()) return b.p.text();
  for (auto& b : blocks) if (b.kind == Block::Paragraph && isHeading(b.p.style) && !b.p.empty()) return b.p.text();
  return "";
}

int Document::figures() const { int n = 0; for (auto& b : blocks) n += b.kind == Block::FigureBlock; return n; }
int Document::tables() const { int n = 0; for (auto& b : blocks) n += b.kind == Block::TableBlock; return n; }

static int countWords(const string& t) {
  int n = 0;
  bool in = false;
  for (unsigned char c : t) {
    bool w = !(c == ' ' || c == '\n' || c == '\t' || c == 0xA0);
    if (w && !in) n++;
    in = w;
  }
  return n;
}

static int countParaWords(const Para& p) {
  int n = 0;
  for (const Span& s : p.spans) if (!(s.fmt & F_MATH)) n += countWords(s.text);
  return n;
}

int Document::words() const {
  int n = 0;
  for (const auto& b : blocks) {
    if (b.kind == Block::Paragraph) n += countParaWords(b.p);
    else if (b.kind == Block::FigureBlock) n += countParaWords(b.fig.caption);
    else if (b.kind == Block::TableBlock) for (const auto& r : b.tbl.cells) for (const auto& c : r) n += countParaWords(c);
  }
  return n;
}

int Document::figureNumber(int blk) const {
  if (blk < 0 || blk >= int(blocks.size()) || blocks[size_t(blk)].kind != Block::FigureBlock) return 0;
  int n = 0;
  for (int k = 0; k <= blk; k++) n += blocks[size_t(k)].kind == Block::FigureBlock;
  return n;
}

int Document::tableNumber(int blk) const {
  if (blk < 0 || blk >= int(blocks.size()) || blocks[size_t(blk)].kind != Block::TableBlock) return 0;
  int n = 0;
  for (int k = 0; k <= blk; k++) n += blocks[size_t(k)].kind == Block::TableBlock;
  return n;
}

int Document::refNumber(int blk) const {
  if (blk < 0 || blk >= int(blocks.size()) || blocks[size_t(blk)].kind != Block::Paragraph || blocks[size_t(blk)].p.style != PStyle::Reference) return 0;
  int n = 0;
  for (int k = 0; k <= blk; k++) n += blocks[size_t(k)].kind == Block::Paragraph && blocks[size_t(k)].p.style == PStyle::Reference;
  return n;
}

vector<int> Document::outline() const {
  vector<int> o;
  for (size_t k = 0; k < blocks.size(); k++) if (blocks[k].kind == Block::Paragraph && isHeading(blocks[k].p.style)) o.push_back(int(k));
  return o;
}

int Document::addAsset(const Scene& sc, const string& id, const string& title, const string& summary) {
  FigAsset a;
  a.scene = sc;
  a.id = id;
  a.title = title;
  a.summary = summary;
  assets.push_back(std::move(a));
  return int(assets.size()) - 1;
}

void Document::dropUnusedAssets() {
  vector<int> map(assets.size(), -1);
  vector<FigAsset> keep;
  for (auto& b : blocks) {
    if (b.kind != Block::FigureBlock || b.fig.asset < 0 || b.fig.asset >= int(assets.size())) continue;
    if (map[size_t(b.fig.asset)] < 0) { map[size_t(b.fig.asset)] = int(keep.size()); keep.push_back(assets[size_t(b.fig.asset)]); }
    b.fig.asset = map[size_t(b.fig.asset)];
  }
  assets.swap(keep);
}

string Document::plainText() const {
  string o;
  for (auto& b : blocks) {
    if (b.kind == Block::Paragraph) o += b.p.text() + "\n";
    else if (b.kind == Block::FigureBlock) o += b.fig.caption.text() + "\n";
    else if (b.kind == Block::TableBlock) for (auto& r : b.tbl.cells) { for (size_t c = 0; c < r.size(); c++) o += (c ? "\t" : "") + r[c].text(); o += "\n"; }
  }
  return o;
}

// ---------------------------------------------------------------- positions
static int cellOrder(int cell) { return cell == -1 ? 0 : cell == -2 ? 1 : 2 + cell; }

bool docBefore(const DocPos& a, const DocPos& b) {
  if (a.blk != b.blk) return a.blk < b.blk;
  if (a.cell != b.cell) return cellOrder(a.cell) < cellOrder(b.cell);
  return a.off < b.off;
}

const Para* docPara(const Document& d, const DocPos& p) {
  if (p.blk < 0 || p.blk >= int(d.blocks.size())) return nullptr;
  const Block& b = d.blocks[size_t(p.blk)];
  if (b.kind == Block::Paragraph) return p.cell == -1 ? &b.p : nullptr;
  if (b.kind == Block::TableBlock) {
    int C = b.tbl.cols();
    if (p.cell < 0 || C <= 0 || p.cell >= b.tbl.rows() * C) return nullptr;
    return &b.tbl.cells[size_t(p.cell / C)][size_t(p.cell % C)];
  }
  if (b.kind == Block::FigureBlock) return p.cell == -2 ? &b.fig.caption : nullptr;
  return nullptr;
}

Para* docPara(Document& d, const DocPos& p) { return const_cast<Para*>(docPara(static_cast<const Document&>(d), p)); }

// every paragraph of a block with its cell id
static void blockParas(Block& b, const std::function<void(int cell, Para&)>& fn) {
  if (b.kind == Block::Paragraph) fn(-1, b.p);
  else if (b.kind == Block::TableBlock) {
    int C = b.tbl.cols();
    for (int r = 0; r < b.tbl.rows(); r++) for (int c = 0; c < C; c++) fn(r * C + c, b.tbl.cells[size_t(r)][size_t(c)]);
  } else if (b.kind == Block::FigureBlock) fn(-2, b.fig.caption);
}

// first / last text position of a block (a table's frame or a rule when it has no text)
static DocPos blockFirst(const Document& d, int blk) {
  const Block& b = d.blocks[size_t(blk)];
  if (b.kind == Block::Paragraph) return {blk, -1, 0};
  if (b.kind == Block::TableBlock && b.tbl.cols() > 0) return {blk, 0, 0};
  return {blk, -1, 0};
}

static DocPos blockLast(const Document& d, int blk) {
  const Block& b = d.blocks[size_t(blk)];
  if (b.kind == Block::Paragraph) return {blk, -1, int(b.p.size())};
  if (b.kind == Block::TableBlock && b.tbl.cols() > 0) { int last = b.tbl.rows() * b.tbl.cols() - 1; return {blk, last, int(b.tbl.cells.back().back().size())}; }
  if (b.kind == Block::FigureBlock) return {blk, -2, int(b.fig.caption.size())};
  return {blk, -1, 0};
}

// ---------------------------------------------------------------- Editor: positions
namespace {
// the citation field around byte `off` of `p` (strictly inside): [a, b) or false
bool citeAround(const Para& p, size_t off, size_t& a, size_t& b) {
  size_t pos = 0;
  for (auto& s : p.spans) {
    size_t e = pos + s.text.size();
    if (s.isCite() && off > pos && off < e) { a = pos; b = e; return true; }
    pos = e;
  }
  return false;
}
}  // namespace

void Editor::clampPos(DocPos& p) const {
  if (!d) return;
  if (d->blocks.empty()) const_cast<Document*>(d)->blocks.push_back(Block::para(""));
  p.blk = clampv(p.blk, 0, int(d->blocks.size()) - 1);
  const Block& b = d->blocks[size_t(p.blk)];
  if (b.kind == Block::Paragraph) p.cell = -1;
  else if (b.kind == Block::TableBlock) { int n = b.tbl.rows() * b.tbl.cols(); if (p.cell >= n) p.cell = n ? n - 1 : -1; if (p.cell < -1 || n == 0) p.cell = -1; }
  else if (b.kind == Block::FigureBlock) { if (p.cell != -2) p.cell = -1; }
  else p.cell = -1;
  const Para* q = docPara(*d, p);
  if (!q) { p.off = 0; return; }
  const string t = q->text();
  p.off = clampv(p.off, 0, int(t.size()));
  while (p.off > 0 && p.off < int(t.size()) && (uint8_t(t[size_t(p.off)]) & 0xC0) == 0x80) p.off--;
  size_t ca = 0, cb = 0;
  if (citeAround(*q, size_t(p.off), ca, cb)) p.off = int(size_t(p.off) - ca < cb - size_t(p.off) ? ca : cb);  // a citation field is one unit
}

void Editor::setCaret(const DocPos& p, bool extend) {
  caret = p;
  clampPos(caret);
  if (!extend) anchor = caret;
  else clampPos(anchor);
  typingSet = false;
  breakCoalescing();
}

bool Editor::inTable(int* row, int* col) const {
  if (!d || caret.blk >= int(d->blocks.size())) return false;
  const Block& b = d->blocks[size_t(caret.blk)];
  if (b.kind != Block::TableBlock || b.tbl.cols() == 0) return false;
  int c = std::max(0, caret.cell);
  if (row) *row = c / b.tbl.cols();
  if (col) *col = c % b.tbl.cols();
  return true;
}

// ---------------------------------------------------------------- Editor: undo
size_t Editor::snapBytes(const vector<Block>& blocks) {
  size_t n = blocks.size() * sizeof(Block);
  auto para = [&](const Para& p) { n += p.spans.size() * sizeof(Span); for (auto& sp : p.spans) n += sp.text.size() + sp.cite.size() + sp.link.size(); };
  for (auto& b : blocks) {
    para(b.p);
    para(b.fig.caption);
    for (auto& row : b.tbl.cells) for (auto& c : row) para(c);
  }
  return n;
}

// A snapshot is a full copy of the blocks, so a long document with many edits could hold hundreds of megabytes of
// history; the stack keeps at most 200 steps and about 24 MB (never fewer than 20 steps).
void Editor::trimUndo() {
  size_t total = 0;
  for (auto& u : undo_) total += u.bytes;
  while (undo_.size() > 200 || (undo_.size() > 20 && total > size_t(24) << 20)) { total -= undo_.front().bytes; undo_.erase(undo_.begin()); }
}

void Editor::begin(const string& label, bool coalesce) {
  if (coalesce && lastLabel_ == label && !undo_.empty()) return;
  undo_.push_back({d->blocks, caret, anchor, label, d->refs, d->citeStyle, 0});
  undo_.back().bytes = snapBytes(undo_.back().blocks);
  trimUndo();
  redo_.clear();
  lastLabel_ = coalesce ? label : string();
}

bool Editor::undo() {
  if (undo_.empty()) return false;
  redo_.push_back({d->blocks, caret, anchor, undo_.back().label, d->refs, d->citeStyle, undo_.back().bytes});
  d->blocks = undo_.back().blocks;
  d->refs = undo_.back().refs;
  d->citeStyle = undo_.back().citeStyle;
  caret = undo_.back().caret;
  anchor = undo_.back().anchor;
  undo_.pop_back();
  clampPos(caret);
  clampPos(anchor);
  lastLabel_.clear();
  typingSet = false;
  changed();
  return true;
}

bool Editor::redo() {
  if (redo_.empty()) return false;
  undo_.push_back({d->blocks, caret, anchor, redo_.back().label, d->refs, d->citeStyle, redo_.back().bytes});
  d->blocks = redo_.back().blocks;
  d->refs = redo_.back().refs;
  d->citeStyle = redo_.back().citeStyle;
  caret = redo_.back().caret;
  anchor = redo_.back().anchor;
  redo_.pop_back();
  clampPos(caret);
  clampPos(anchor);
  lastLabel_.clear();
  typingSet = false;
  changed();
  return true;
}

// ---------------------------------------------------------------- Editor: ranges
vector<int> Editor::selectedBlocks() const {
  vector<int> v;
  DocPos s = selStart(), e = selEnd();
  for (int k = s.blk; k <= e.blk && k < int(d->blocks.size()); k++) v.push_back(k);
  return v;
}

// calls fn(paragraph, a, b) for every paragraph the selection touches, with the covered byte range
static void forEachRange(Editor& ed, const std::function<void(Para&, size_t, size_t)>& fn) {
  Document& d = *ed.d;
  DocPos s = ed.selStart(), e = ed.selEnd();
  if (!ed.hasSelection()) { Para* p = ed.curPara(); if (p) fn(*p, size_t(s.off), size_t(s.off)); return; }
  for (int blk = s.blk; blk <= e.blk && blk < int(d.blocks.size()); blk++) {
    Block& B = d.blocks[size_t(blk)];
    bool rect = s.blk == e.blk && B.kind == Block::TableBlock && s.cell >= 0 && e.cell >= 0;
    int C = B.tbl.cols();
    int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    if (rect) { r0 = std::min(s.cell / C, e.cell / C); r1 = std::max(s.cell / C, e.cell / C); c0 = std::min(s.cell % C, e.cell % C); c1 = std::max(s.cell % C, e.cell % C); }
    blockParas(B, [&](int cell, Para& p) {
      if (rect) {
        int r = cell / C, c = cell % C;
        if (r < r0 || r > r1 || c < c0 || c > c1) return;
        fn(p, 0, p.size());
        return;
      }
      DocPos ps{blk, cell, 0}, pe{blk, cell, int(p.size())};
      if (docBefore(pe, s) || docBefore(e, ps)) return;
      size_t a = (blk == s.blk && cell == s.cell) ? size_t(s.off) : 0;
      size_t b = (blk == e.blk && cell == e.cell) ? size_t(e.off) : p.size();
      fn(p, a, std::max(a, b));
    });
  }
}

void Editor::forEachSelectedPara(const std::function<void(Para&)>& fn) {
  forEachRange(*this, [&](Para& p, size_t, size_t) { fn(p); });
}

void Editor::applyFmt(Para& p, size_t a, size_t b, const std::function<void(Span&)>& fn) {
  if (b <= a) return;
  size_t i0 = splitSpan(p, a), i1 = splitSpan(p, b);
  for (size_t i = i0; i < i1 && i < p.spans.size(); i++) fn(p.spans[i]);
  p.normalize();
}

void Editor::splitAt(const DocPos& pos, Para& left, Para& right) const {
  const Para* p = docPara(*d, pos);
  left = right = Para();
  if (!p) return;
  left = *p;
  right = *p;
  right.numStart = 0;
  left.spans = sliceSpans(*p, 0, size_t(pos.off));
  right.spans = sliceSpans(*p, size_t(pos.off), p->size());
}

void Editor::eraseRange(const DocPos& a0, const DocPos& b0) {
  DocPos a = a0, b = b0;
  if (!docBefore(a, b)) return;
  vector<Block>& bl = d->blocks;
  if (a.blk == b.blk) {
    Block& B = bl[size_t(a.blk)];
    if (a.cell == b.cell) {
      Para* p = docPara(*d, a);
      if (p) eraseBytes(*p, size_t(a.off), size_t(b.off));
      caret = anchor = a;
    } else if (B.kind == Block::TableBlock && a.cell >= 0 && b.cell >= 0) {  // a rectangle of cells: their contents
      int C = B.tbl.cols();
      int r0 = std::min(a.cell / C, b.cell / C), r1 = std::max(a.cell / C, b.cell / C), c0 = std::min(a.cell % C, b.cell % C), c1 = std::max(a.cell % C, b.cell % C);
      for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++) B.tbl.cells[size_t(r)][size_t(c)].spans.clear();
      caret = anchor = {a.blk, r0 * C + c0, 0};
    } else if (B.kind == Block::FigureBlock && a.cell == -1 && b.cell == -2) {
      eraseBytes(B.fig.caption, 0, size_t(b.off));
      caret = anchor = {a.blk, -2, 0};
    } else if (B.kind == Block::TableBlock && a.cell == -1) {  // frame to a cell: the table goes
      bl.erase(bl.begin() + a.blk);
      if (bl.empty()) bl.push_back(Block::para(""));
      caret = anchor = {std::min(a.blk, int(bl.size()) - 1), -1, 0};
      clampPos(caret);
      anchor = caret;
    } else caret = anchor = a;
    changed();
    return;
  }
  Block& A = bl[size_t(a.blk)];
  Block& Bk = bl[size_t(b.blk)];
  bool keepA = A.kind == Block::Paragraph && a.cell == -1;
  bool keepB = Bk.kind == Block::Paragraph && b.cell == -1;
  Para tail;
  if (keepB) { tail = Bk.p; tail.spans = sliceSpans(Bk.p, size_t(b.off), Bk.p.size()); }
  if (keepA) eraseBytes(A.p, size_t(a.off), A.p.size());
  int first = keepA ? a.blk + 1 : a.blk;
  bl.erase(bl.begin() + first, bl.begin() + b.blk + 1);
  if (keepB) {
    if (keepA) { Block& AA = bl[size_t(a.blk)]; AA.p.spans.insert(AA.p.spans.end(), tail.spans.begin(), tail.spans.end()); AA.p.normalize(); }
    else { Block nb; nb.p = tail; bl.insert(bl.begin() + first, nb); }
  }
  if (bl.empty()) bl.push_back(Block::para(""));
  if (keepA) caret = {a.blk, -1, a.off};
  else caret = {std::min(first, int(bl.size()) - 1), -1, 0};
  clampPos(caret);
  anchor = caret;
  changed();
}

bool Editor::deleteSelection() {
  if (!hasSelection()) return false;
  DocPos s = selStart(), e = selEnd();
  eraseRange(s, e);
  return true;
}

// ---------------------------------------------------------------- Editor: text
void Editor::insertText(const string& utf8) {
  if (!d || utf8.empty()) return;
  string s;
  for (char c : utf8) if (c != '\r' && (uint8_t(c) >= 0x20 || c == '\t' || c == '\n')) s += c == '\t' ? ' ' : c;
  if (s.empty()) return;
  begin("typing", true);
  if (hasSelection()) deleteSelection();
  Para* p = curPara();
  if (!p) {  // a rule, figure or table frame is selected: the text starts a paragraph after it
    Block nb = Block::para("");
    d->blocks.insert(d->blocks.begin() + caret.blk + 1, nb);
    caret = anchor = {caret.blk + 1, -1, 0};
    p = curPara();
  }
  // Markdown-style auto formatting at the start of a body paragraph: "- ", "1. ", "# "
  if (s == " " && !typingSet && caret.cell == -1 && p->style == PStyle::Body && caret.off == int(p->size()) && p->size() <= 4) {
    string t = p->text();
    if (t == "-" || t == "*") { p->spans.clear(); p->style = PStyle::Bullet; caret.off = 0; anchor = caret; changed(); return; }
    if (t.size() >= 2 && t.back() == '.' && isDigits(t.substr(0, t.size() - 1))) { int n = toInt(t.substr(0, t.size() - 1), 1); p->spans.clear(); p->style = PStyle::Number; p->numStart = n; caret.off = 0; anchor = caret; changed(); return; }
    if (!t.empty() && t.find_first_not_of('#') == string::npos && t.size() <= 4) { p->spans.clear(); p->style = headingStyle(int(t.size())); caret.off = 0; anchor = caret; changed(); return; }
  }
  size_t i = splitSpan(*p, size_t(caret.off));
  Span ns;
  ns.text = s;
  const Span* inherit = i > 0 ? &p->spans[i - 1] : (!p->spans.empty() ? &p->spans[0] : nullptr);
  if (inherit && inherit->isCite() && i > 0 && i < p->spans.size() && !p->spans[i].isCite()) inherit = &p->spans[i];  // after a citation: the text that follows it
  if (inherit) { ns.font = inherit->font; ns.size = inherit->size; ns.color = inherit->color; }
  if (typingSet) { ns.fmt = typing; ns.link = typingLink; }
  else if (i > 0 && !p->spans[i - 1].isCite()) {
    ns.fmt = p->spans[i - 1].fmt;
    ns.link = p->spans[i - 1].link;
    size_t endPrev = 0;
    for (size_t k = 0; k < i; k++) endPrev += p->spans[k].text.size();
    if (!ns.link.empty() && endPrev == size_t(caret.off) && (i >= p->spans.size() || p->spans[i].link != ns.link)) ns.link.clear();  // typing after a link leaves it
  } else if (!p->spans.empty() && i == 0) { ns.fmt = p->spans[0].fmt & uint16_t(~F_SUP); ns.link = p->spans[0].link; if (!ns.link.empty()) ns.link.clear(); if (p->spans[0].isCite()) ns.fmt = 0; }
  else if (i > 0) ns.fmt = uint16_t(p->spans[i - 1].fmt & ~uint16_t(F_SUP | F_SUB));  // after a citation field: plain
  if (typingPropsSet) { ns.font = typingFont; ns.size = typingSize; ns.color = typingColor; }
  p->spans.insert(p->spans.begin() + long(i), ns);
  p->normalize();
  caret.off += int(s.size());
  anchor = caret;
  changed();
}

string Editor::mathAtCaret() const {
  if (!d) return string();
  DocPos a = hasSelection() ? selStart() : caret;
  DocPos b = hasSelection() ? selEnd() : caret;
  if (a.blk != b.blk || a.cell != b.cell) return string();
  const Para* p = docPara(*d, a);
  if (!p) return string();
  size_t pos = 0;
  for (const Span& span : p->spans) {
    size_t end = pos + span.text.size();
    if (span.fmt & F_MATH) {
      bool exact = hasSelection() && size_t(a.off) == pos && size_t(b.off) == end;
      bool underCaret = !hasSelection() && size_t(a.off) >= pos && size_t(a.off) <= end;
      if (exact || underCaret) return span.text;
    }
    pos = end;
  }
  return string();
}

bool Editor::selectMathAtCaret() {
  if (!d) return false;
  if (hasSelection()) return !mathAtCaret().empty();
  const Para* p = curPara();
  if (!p) return false;
  size_t pos = 0;
  for (const Span& span : p->spans) {
    size_t end = pos + span.text.size();
    if ((span.fmt & F_MATH) && size_t(caret.off) >= pos && size_t(caret.off) <= end) {
      anchor = {caret.blk, caret.cell, int(pos)};
      caret = {caret.blk, caret.cell, int(end)};
      breakCoalescing();
      return true;
    }
    pos = end;
  }
  return false;
}

void Editor::insertMath(const string& latex) {
  if (!d || trim(latex).empty()) return;
  if (!hasSelection() && !mathAtCaret().empty()) selectMathAtCaret();
  begin("equation");
  if (hasSelection()) deleteSelection();
  Para* p = curPara();
  if (!p) {
    int at = clampv(caret.blk + 1, 0, int(d->blocks.size()));
    d->blocks.insert(d->blocks.begin() + at, Block::para(""));
    caret = anchor = {at, -1, 0};
    p = curPara();
  }
  size_t at = size_t(clampv(caret.off, 0, int(p->size())));
  size_t i = splitSpan(*p, at);
  Span equation;
  equation.text = latex;
  equation.fmt = F_MATH;
  p->spans.insert(p->spans.begin() + long(i), std::move(equation));
  p->normalize();
  caret.off += int(latex.size());
  anchor = caret;
  typingSet = false;
  changed();
}

void Editor::insertLineBreak() {
  Para* p = curPara();
  if (!p) { insertParagraphBreak(); return; }
  begin("line break");
  if (hasSelection()) deleteSelection();
  insertText("\n");
  lastLabel_.clear();
}

void Editor::insertParagraphBreak() {
  if (!d) return;
  begin("paragraph");
  if (hasSelection()) deleteSelection();
  Para* p = curPara();
  vector<Block>& bl = d->blocks;
  if (!p) {  // after a selected rule / figure / table
    bl.insert(bl.begin() + caret.blk + 1, Block::para(""));
    caret = anchor = {caret.blk + 1, -1, 0};
    changed();
    return;
  }
  if (caret.cell >= 0) { insertText("\n"); lastLabel_.clear(); return; }  // inside a table cell: a new line
  if (caret.cell == -2) {  // the caption: a paragraph after the figure
    bl.insert(bl.begin() + caret.blk + 1, Block::para(""));
    caret = anchor = {caret.blk + 1, -1, 0};
    changed();
    return;
  }
  if (p->style == PStyle::Code) {
    string t = p->text();
    if (caret.off == int(t.size()) && endsWith(t, "\n")) {  // Enter on an empty last line leaves the code block
      eraseBytes(*p, t.size() - 1, t.size());
      bl.insert(bl.begin() + caret.blk + 1, Block::para(""));
      caret = anchor = {caret.blk + 1, -1, 0};
    } else { insertText("\n"); lastLabel_.clear(); }
    changed();
    return;
  }
  if (isList(p->style) && p->empty()) {  // Enter on an empty item ends the list
    if (p->level > 0) p->level--;
    else { p->style = PStyle::Body; p->level = 0; p->numStart = 0; }
    changed();
    return;
  }
  Para left, right;
  splitAt(caret, left, right);
  if (right.empty()) {  // the next paragraph takes the style that follows this one
    if (isHeading(left.style) || left.style == PStyle::Title || left.style == PStyle::Subtitle || left.style == PStyle::Caption || left.style == PStyle::Meta) { right.style = left.style == PStyle::Title ? PStyle::Subtitle : PStyle::Body; right.level = 0; }
    right.align = right.style == PStyle::Body ? PAlign::Left : left.align;
  }
  bl[size_t(caret.blk)].p = left;
  Block nb;
  nb.p = right;
  bl.insert(bl.begin() + caret.blk + 1, nb);
  caret = anchor = {caret.blk + 1, -1, 0};
  changed();
}

void Editor::backspace() {
  if (!d) return;
  if (hasSelection()) { begin("delete"); deleteSelection(); refreshCitations(); return; }
  Para* p = curPara();
  vector<Block>& bl = d->blocks;
  if (p && caret.off > 0) {  // right after a citation field: remove the field
    size_t pos = 0;
    for (size_t i = 0; i < p->spans.size(); i++) {
      size_t e = pos + p->spans[i].text.size();
      if (p->spans[i].isCite() && e == size_t(caret.off)) {
        begin("delete");
        p->spans.erase(p->spans.begin() + long(i));
        if (p->spans.empty()) p->spans.push_back({"", 0, ""});
        p->normalize();
        caret.off = anchor.off = int(pos);
        refreshCitations();
        changed();
        return;
      }
      pos = e;
    }
  }
  if (!p) {  // a selected rule / figure / table frame
    begin("delete");
    int k = caret.blk;
    bl.erase(bl.begin() + k);
    if (bl.empty()) bl.push_back(Block::para(""));
    caret = k > 0 ? blockLast(*d, k - 1) : blockFirst(*d, 0);
    clampPos(caret);
    anchor = caret;
    changed();
    return;
  }
  if (caret.off > 0) {
    begin("typing", true);
    string t = p->text();
    size_t a = cpPrev(t, size_t(caret.off));
    eraseBytes(*p, a, size_t(caret.off));
    caret.off = int(a);
    anchor = caret;
    changed();
    return;
  }
  // at the start of the paragraph
  if (caret.cell >= 0) return;  // table cells do not merge
  begin("merge");
  if (caret.cell == -2) { caret = anchor = {caret.blk, -1, 0}; changed(); return; }  // caption start: select the figure
  if (p->level > 0) { p->level--; changed(); return; }
  if (p->style != PStyle::Body && p->style != PStyle::Title && p->style != PStyle::Reference) { p->style = PStyle::Body; p->numStart = 0; changed(); return; }
  if (caret.blk == 0) return;
  Block& prev = bl[size_t(caret.blk) - 1];
  if (prev.kind == Block::Paragraph) {
    size_t at = prev.p.size();
    prev.p.spans.insert(prev.p.spans.end(), p->spans.begin(), p->spans.end());
    prev.p.normalize();
    if (prev.p.empty() && !p->empty()) prev.p.style = p->style;
    bl.erase(bl.begin() + caret.blk);
    caret = anchor = {caret.blk - 1, -1, int(at)};
  } else if (prev.kind == Block::FigureBlock) {
    caret = anchor = {caret.blk - 1, -2, int(prev.fig.caption.size())};
    if (p->empty()) bl.erase(bl.begin() + caret.blk + 1);
  } else {
    caret = anchor = {caret.blk - 1, -1, 0};  // select the block before; a second Backspace removes it
    if (p->empty() && caret.blk + 2 < int(bl.size())) bl.erase(bl.begin() + caret.blk + 1);
  }
  changed();
}

void Editor::del() {
  if (!d) return;
  if (hasSelection()) { begin("delete"); deleteSelection(); refreshCitations(); return; }
  Para* p = curPara();
  vector<Block>& bl = d->blocks;
  if (!p) { backspace(); return; }
  {
    size_t pos = 0;
    for (size_t i = 0; i < p->spans.size(); i++) {  // right before a citation field: remove the field
      size_t e = pos + p->spans[i].text.size();
      if (p->spans[i].isCite() && pos == size_t(caret.off)) {
        begin("delete");
        p->spans.erase(p->spans.begin() + long(i));
        if (p->spans.empty()) p->spans.push_back({"", 0, ""});
        p->normalize();
        caret.off = anchor.off = int(pos);
        refreshCitations();
        changed();
        return;
      }
      pos = e;
    }
  }
  string t = p->text();
  if (caret.off < int(t.size())) {
    begin("typing", true);
    eraseBytes(*p, size_t(caret.off), cpNext(t, size_t(caret.off)));
    anchor = caret;
    changed();
    return;
  }
  if (caret.cell != -1 || caret.blk + 1 >= int(bl.size())) return;
  begin("merge");
  Block& next = bl[size_t(caret.blk) + 1];
  if (next.kind == Block::Paragraph) {
    p->spans.insert(p->spans.end(), next.p.spans.begin(), next.p.spans.end());
    p->normalize();
    bl.erase(bl.begin() + caret.blk + 1);
  } else caret = anchor = {caret.blk + 1, -1, 0};
  changed();
}

// ---------------------------------------------------------------- Editor: movement
void Editor::moveChar(int dir, bool extend) {
  if (!d) return;
  if (!extend && hasSelection()) { setCaret(dir < 0 ? selStart() : selEnd()); return; }
  const Para* p = docPara(*d, caret);
  DocPos np = caret;
  if (p) {
    string t = p->text();
    size_t ca = 0, cb = 0;
    if (dir < 0 && caret.off > 0) { np.off = int(cpPrev(t, size_t(caret.off))); if (citeAround(*p, size_t(np.off), ca, cb)) np.off = int(ca); }
    else if (dir > 0 && caret.off < int(t.size())) { np.off = int(cpNext(t, size_t(caret.off))); if (citeAround(*p, size_t(np.off), ca, cb)) np.off = int(cb); }
    else if (dir > 0) {
      // next cell, the caption, or the next block
      const Block& b = d->blocks[size_t(caret.blk)];
      if (b.kind == Block::TableBlock && caret.cell + 1 < b.tbl.rows() * b.tbl.cols()) np = {caret.blk, caret.cell + 1, 0};
      else if (caret.blk + 1 < int(d->blocks.size())) np = blockFirst(*d, caret.blk + 1);
    } else {
      const Block& b = d->blocks[size_t(caret.blk)];
      if (b.kind == Block::TableBlock && caret.cell > 0) { np = {caret.blk, caret.cell - 1, 0}; const Para* q = docPara(*d, np); np.off = q ? int(q->size()) : 0; }
      else if (b.kind == Block::FigureBlock && caret.cell == -2) np = {caret.blk, -1, 0};
      else if (caret.blk > 0) np = blockLast(*d, caret.blk - 1);
    }
  } else {  // on a block frame
    const Block& b = d->blocks[size_t(caret.blk)];
    if (dir > 0) {
      if (b.kind == Block::FigureBlock) np = {caret.blk, -2, 0};
      else if (b.kind == Block::TableBlock && b.tbl.cols() > 0) np = {caret.blk, 0, 0};
      else if (caret.blk + 1 < int(d->blocks.size())) np = blockFirst(*d, caret.blk + 1);
    } else if (caret.blk > 0) np = blockLast(*d, caret.blk - 1);
  }
  setCaret(np, extend);
}

void Editor::moveWord(int dir, bool extend) {
  const Para* p = docPara(*d, caret);
  if (!p) { moveChar(dir, extend); return; }
  string t = p->text();
  size_t k = size_t(caret.off);
  if (dir > 0) {
    if (k >= t.size()) { moveChar(1, extend); return; }
    while (k < t.size() && wordChar(t, k)) k = cpNext(t, k);
    while (k < t.size() && !wordChar(t, k)) k = cpNext(t, k);
  } else {
    if (k == 0) { moveChar(-1, extend); return; }
    while (k > 0 && !wordChar(t, cpPrev(t, k))) k = cpPrev(t, k);
    while (k > 0 && wordChar(t, cpPrev(t, k))) k = cpPrev(t, k);
  }
  setCaret({caret.blk, caret.cell, int(k)}, extend);
}

void Editor::moveBlock(int dir, bool extend) {
  int k = caret.blk;
  if (dir < 0) { if (caret.off > 0 || caret.cell != -1) { setCaret({k, caret.cell, 0}, extend); return; } if (k > 0) setCaret(blockFirst(*d, k - 1), extend); }
  else if (k + 1 < int(d->blocks.size())) setCaret(blockFirst(*d, k + 1), extend);
  else setCaret(blockLast(*d, k), extend);
}

void Editor::home(bool extend) {
  const Para* p = docPara(*d, caret);
  if (!p) return;
  string t = p->text();
  size_t k = size_t(caret.off);
  while (k > 0 && t[k - 1] != '\n') k--;
  setCaret({caret.blk, caret.cell, int(k)}, extend);
}

void Editor::end(bool extend) {
  const Para* p = docPara(*d, caret);
  if (!p) return;
  string t = p->text();
  size_t k = size_t(caret.off);
  while (k < t.size() && t[k] != '\n') k++;
  setCaret({caret.blk, caret.cell, int(k)}, extend);
}

void Editor::docStart(bool extend) { setCaret(blockFirst(*d, 0), extend); }
void Editor::docEnd(bool extend) { setCaret(blockLast(*d, int(d->blocks.size()) - 1), extend); }

void Editor::selectAll() {
  anchor = blockFirst(*d, 0);
  caret = blockLast(*d, int(d->blocks.size()) - 1);
  typingSet = false;
  breakCoalescing();
}

void Editor::selectWord() {
  const Para* p = docPara(*d, caret);
  if (!p) return;
  string t = p->text();
  size_t a = size_t(caret.off), b = a;
  while (a > 0 && wordChar(t, cpPrev(t, a))) a = cpPrev(t, a);
  while (b < t.size() && wordChar(t, b)) b = cpNext(t, b);
  if (a == b && b < t.size()) b = cpNext(t, b);
  anchor = {caret.blk, caret.cell, int(a)};
  caret = {caret.blk, caret.cell, int(b)};
  breakCoalescing();
}

void Editor::selectParagraph() {
  const Para* p = docPara(*d, caret);
  if (!p) { anchor = caret = {caret.blk, -1, 0}; return; }
  anchor = {caret.blk, caret.cell, 0};
  caret = {caret.blk, caret.cell, int(p->size())};
  breakCoalescing();
}

// ---------------------------------------------------------------- Editor: formatting
uint16_t Editor::fmtHere() const {
  if (!d) return 0;
  if (typingSet && !hasSelection()) return typing;
  DocPos s = hasSelection() ? selStart() : caret;
  const Para* p = docPara(*d, s);
  if (!p) return 0;
  return hasSelection() ? p->fmtAt(size_t(s.off) + 1) : p->fmtAt(size_t(s.off));
}

void Editor::toggleFmt(uint16_t f) {
  if (!d) return;
  if (!hasSelection()) {
    Para* p = curPara();
    if (!p) return;
    uint16_t cur = typingSet ? typing : p->fmtAt(size_t(caret.off));
    typing = uint16_t(cur ^ f);
    if (f == F_SUB) typing &= uint16_t(~F_SUP);
    if (f == F_SUP) typing &= uint16_t(~F_SUB);
    typingLink = typingSet ? typingLink : p->linkAt(size_t(caret.off));
    typingSet = true;
    return;
  }
  bool all = true;
  forEachRange(*this, [&](Para& p, size_t a, size_t b) {
    if (b <= a) return;
    size_t pos = 0;
    for (auto& s : p.spans) {
      size_t e = pos + s.text.size();
      if (e > a && pos < b && !(s.fmt & f)) all = false;
      pos = e;
    }
  });
  begin(all ? "remove formatting" : "formatting");
  forEachRange(*this, [&](Para& p, size_t a, size_t b) {
    applyFmt(p, a, b, [&](Span& s) {
      if (all) s.fmt &= uint16_t(~f);
      else { s.fmt |= f; if (f == F_SUB) s.fmt &= uint16_t(~F_SUP); if (f == F_SUP) s.fmt &= uint16_t(~F_SUB); }
    });
  });
  changed();
}

string Editor::linkHere() const {
  const Para* p = docPara(*d, hasSelection() ? selStart() : caret);
  return p ? p->linkAt(size_t((hasSelection() ? selStart() : caret).off) + (hasSelection() ? 1 : 0)) : string();
}

void Editor::setLink(const string& url0) {
  if (!d) return;
  string url = trim(url0);
  Para* p = curPara();
  if (!p) return;
  begin(url.empty() ? "remove link" : "link");
  if (!hasSelection()) {
    // extend over the existing link at the caret, else the word, else insert the URL as text
    size_t pos = 0;
    bool found = false;
    for (auto& s : p->spans) {
      size_t e = pos + s.text.size();
      if (!s.link.empty() && size_t(caret.off) >= pos && size_t(caret.off) <= e) { anchor = {caret.blk, caret.cell, int(pos)}; caret.off = int(e); found = true; break; }
      pos = e;
    }
    if (!found) selectWord();
    if (!hasSelection()) {
      if (url.empty()) return;
      insertText(url);
      anchor = {caret.blk, caret.cell, caret.off - int(url.size())};
      p = curPara();
    }
  }
  forEachRange(*this, [&](Para& q, size_t a, size_t b) { applyFmt(q, a, b, [&](Span& s) { s.link = url; }); });
  changed();
}

PStyle Editor::styleHere() const {
  const Para* p = docPara(*d, caret);
  return p ? p->style : PStyle::Body;
}

void Editor::setStyle(PStyle st) {
  if (!d) return;
  begin("style");
  bool any = false;
  forEachSelectedPara([&](Para& p) {
    any = true;
    p.style = st;
    if (!isList(st)) { p.numStart = 0; if (st != PStyle::Body && st != PStyle::Quote) p.level = 0; }
    if (st == PStyle::Title || st == PStyle::Subtitle) p.level = 0;
  });
  if (!any) { Block& b = curBlock(); if (b.kind == Block::FigureBlock) b.fig.caption.style = st; }
  changed();
}

PAlign Editor::alignHere() const {
  const Para* p = docPara(*d, caret);
  return p ? p->align : PAlign::Left;
}

void Editor::setAlign(PAlign a) {
  begin("alignment");
  forEachSelectedPara([&](Para& p) { p.align = a; });
  changed();
}

void Editor::toggleList(bool ordered) {
  PStyle want = ordered ? PStyle::Number : PStyle::Bullet;
  bool all = true, any = false;
  forEachSelectedPara([&](Para& p) { any = true; if (p.style != want) all = false; });
  if (!any) return;
  setStyle(all ? PStyle::Body : want);
}

void Editor::indent(int dir) {
  if (!d) return;
  if (inTable() && !hasSelection()) { tableNextCell(dir); return; }
  begin("indent");
  forEachSelectedPara([&](Para& p) {
    if (isList(p.style) || p.style == PStyle::Body || p.style == PStyle::Quote) p.level = clampv(p.level + dir, 0, 2);
  });
  changed();
}

void Editor::clearFormatting() {
  begin("clear formatting");
  forEachRange(*this, [&](Para& p, size_t a, size_t b) {
    if (b <= a) { for (auto& s : p.spans) s.fmt &= F_MATH; p.style = PStyle::Body; p.align = PAlign::Left; p.level = 0; return; }
    applyFmt(p, a, b, [](Span& s) { s.fmt &= F_MATH; s.link.clear(); });
  });
  typingSet = false;
  changed();
}

// ---------------------------------------------------------------- Editor: blocks
void Editor::insertBlock(const Block& nb) {
  if (!d) return;
  begin("insert");
  if (hasSelection()) deleteSelection();
  vector<Block>& bl = d->blocks;
  Para* p = curPara();
  int at;
  if (p && caret.cell == -1) {
    if (p->empty()) { at = caret.blk; bl[size_t(at)] = nb; }
    else if (caret.off == 0) { at = caret.blk; bl.insert(bl.begin() + at, nb); }
    else if (caret.off >= int(p->size())) { at = caret.blk + 1; bl.insert(bl.begin() + at, nb); }
    else {
      Para left, right;
      splitAt(caret, left, right);
      bl[size_t(caret.blk)].p = left;
      Block rb;
      rb.p = right;
      bl.insert(bl.begin() + caret.blk + 1, rb);
      at = caret.blk + 1;
      bl.insert(bl.begin() + at, nb);
    }
  } else { at = caret.blk + 1; bl.insert(bl.begin() + at, nb); }
  const Block& in = bl[size_t(at)];
  if (in.kind == Block::TableBlock && in.tbl.cols() > 0) caret = {at, 0, 0};
  else if (in.kind == Block::Paragraph) caret = {at, -1, int(in.p.size())};
  else if (in.kind == Block::FigureBlock) caret = {at, -2, int(in.fig.caption.size())};
  else caret = {at, -1, 0};
  if (at + 1 >= int(bl.size()) && in.kind != Block::Paragraph) bl.push_back(Block::para(""));  // room to keep typing
  if (in.kind == Block::Rule || in.kind == Block::PageBreak || in.kind == Block::Toc) caret = {at + 1, -1, 0};
  clampPos(caret);
  anchor = caret;
  changed();
}

void Editor::insertTable(int rows, int cols) {
  rows = clampv(rows, 1, 200);
  cols = clampv(cols, 1, 20);
  Block b;
  b.kind = Block::TableBlock;
  b.tbl.cells.assign(size_t(rows), vector<Para>(size_t(cols)));
  b.tbl.header = rows > 1;
  b.tbl.fixup();
  insertBlock(b);
}

void Editor::insertFigure(int asset, const string& caption, int widthPct) {
  Block b;
  b.kind = Block::FigureBlock;
  b.fig.asset = asset;
  b.fig.widthPct = clampv(widthPct, 20, 100);
  b.fig.caption.style = PStyle::Caption;
  if (!caption.empty()) b.fig.caption.spans.push_back({caption, 0, ""});
  b.fig.alt = caption;
  insertBlock(b);
}

void Editor::insertRule() { Block b; b.kind = Block::Rule; insertBlock(b); }
void Editor::insertPageBreak() { Block b; b.kind = Block::PageBreak; insertBlock(b); }
void Editor::insertToc() { Block b; b.kind = Block::Toc; insertBlock(b); }

void Editor::deleteBlock(int blk) {
  if (!d || blk < 0 || blk >= int(d->blocks.size())) return;
  begin("delete");
  d->blocks.erase(d->blocks.begin() + blk);
  if (d->blocks.empty()) d->blocks.push_back(Block::para(""));
  caret = blk > 0 ? blockLast(*d, std::min(blk - 1, int(d->blocks.size()) - 1)) : blockFirst(*d, 0);
  clampPos(caret);
  anchor = caret;
  refreshCitations();
  changed();
}

void Editor::moveBlockUpDown(int dir) {
  int k = caret.blk, j = k + dir;
  if (!d || j < 0 || j >= int(d->blocks.size())) return;
  begin("move");
  std::swap(d->blocks[size_t(k)], d->blocks[size_t(j)]);
  caret.blk = j;
  anchor = caret;
  refreshCitations();
  changed();
}

void Editor::setFigureWidth(int pct) {
  Block& b = curBlock();
  if (b.kind != Block::FigureBlock) return;
  begin("figure width");
  b.fig.widthPct = clampv(pct, 20, 100);
  changed();
}

// tables
void Editor::tableInsertRow(bool below) {
  int r, c;
  if (!inTable(&r, &c)) return;
  begin("table");
  Table& t = curBlock().tbl;
  int at = below ? r + 1 : r;
  t.cells.insert(t.cells.begin() + at, vector<Para>(size_t(t.cols())));
  caret = anchor = {caret.blk, at * t.cols() + c, 0};
  changed();
}

void Editor::tableInsertCol(bool right) {
  int r, c;
  if (!inTable(&r, &c)) return;
  begin("table");
  Table& t = curBlock().tbl;
  int at = right ? c + 1 : c;
  for (auto& row : t.cells) row.insert(row.begin() + at, Para());
  t.align.insert(t.align.begin() + at, 'a');
  if (!t.widths.empty()) { float w = 1.f / float(t.cols()); for (auto& x : t.widths) x *= (1 - w); t.widths.insert(t.widths.begin() + at, w); }
  t.fixup();
  caret = anchor = {caret.blk, r * t.cols() + at, 0};
  changed();
}

void Editor::tableDeleteRow() {
  int r, c;
  if (!inTable(&r, &c)) return;
  Table& t = curBlock().tbl;
  if (t.rows() <= 1) { tableDeleteTable(); return; }
  begin("table");
  t.cells.erase(t.cells.begin() + r);
  caret = anchor = {caret.blk, std::min(r, t.rows() - 1) * t.cols() + c, 0};
  changed();
}

void Editor::tableDeleteCol() {
  int r, c;
  if (!inTable(&r, &c)) return;
  Table& t = curBlock().tbl;
  if (t.cols() <= 1) { tableDeleteTable(); return; }
  begin("table");
  for (auto& row : t.cells) row.erase(row.begin() + c);
  t.align.erase(t.align.begin() + c);
  if (!t.widths.empty()) t.widths.erase(t.widths.begin() + c);
  t.fixup();
  caret = anchor = {caret.blk, r * t.cols() + std::min(c, t.cols() - 1), 0};
  changed();
}

void Editor::tableDeleteTable() {
  if (!inTable()) return;
  deleteBlock(caret.blk);
}

void Editor::tableToggleHeader() {
  if (!inTable()) return;
  begin("table");
  curBlock().tbl.header = !curBlock().tbl.header;
  changed();
}

void Editor::tableSetColAlign(char a) {
  int r, c;
  if (!inTable(&r, &c)) return;
  begin("table");
  curBlock().tbl.align[size_t(c)] = a;
  changed();
}

void Editor::tableSetWidths(const vector<float>& w) {
  if (!inTable()) return;
  begin("column width", true);
  curBlock().tbl.widths = w;
  curBlock().tbl.fixup();
  changed();
}

bool Editor::tableNextCell(int dir) {
  int r, c;
  if (!inTable(&r, &c)) return false;
  Table& t = curBlock().tbl;
  int idx = r * t.cols() + c + dir;
  if (idx < 0) return true;
  if (idx >= t.rows() * t.cols()) { begin("table"); t.cells.push_back(vector<Para>(size_t(t.cols()))); changed(); }
  const Para& p = t.cells[size_t(idx / t.cols())][size_t(idx % t.cols())];
  anchor = {caret.blk, idx, 0};
  caret = {caret.blk, idx, int(p.size())};
  typingSet = false;
  breakCoalescing();
  return true;
}

// ---------------------------------------------------------------- Editor: clipboard
DocFragment Editor::copy() const {
  DocFragment f;
  if (!d) return f;
  if (!hasSelection()) {  // a selected block (figure, rule, page break, table of contents, table frame) copies whole
    if (blockSelected() && caret.blk >= 0 && caret.blk < int(d->blocks.size())) f.blocks.push_back(d->blocks[size_t(caret.blk)]);
    return f;
  }
  DocPos s = selStart(), e = selEnd();
  const Para* p = docPara(*d, s);
  if (s.blk == e.blk && s.cell == e.cell && p) { f.spans = sliceSpans(*p, size_t(s.off), size_t(e.off)); return f; }
  const Block& B = d->blocks[size_t(s.blk)];
  if (s.blk == e.blk && B.kind == Block::TableBlock && s.cell >= 0 && e.cell >= 0) {
    int C = B.tbl.cols();
    int r0 = std::min(s.cell / C, e.cell / C), r1 = std::max(s.cell / C, e.cell / C), c0 = std::min(s.cell % C, e.cell % C), c1 = std::max(s.cell % C, e.cell % C);
    Block nb;
    nb.kind = Block::TableBlock;
    for (int r = r0; r <= r1; r++) { vector<Para> row; for (int c = c0; c <= c1; c++) row.push_back(B.tbl.cells[size_t(r)][size_t(c)]); nb.tbl.cells.push_back(row); }
    nb.tbl.header = B.tbl.header && r0 == 0;
    nb.tbl.fixup();
    f.blocks.push_back(nb);
    return f;
  }
  for (int k = s.blk; k <= e.blk; k++) {
    Block b = d->blocks[size_t(k)];
    if (k == s.blk && b.kind == Block::Paragraph && s.cell == -1) b.p.spans = sliceSpans(b.p, size_t(s.off), b.p.size());
    if (k == e.blk && b.kind == Block::Paragraph && e.cell == -1) b.p.spans = sliceSpans(b.p, 0, size_t(e.off));
    if (k == s.blk && b.kind == Block::FigureBlock && s.cell == -2) { b.kind = Block::Paragraph; b.p = b.fig.caption; b.p.spans = sliceSpans(b.fig.caption, size_t(s.off), b.fig.caption.size()); }
    f.blocks.push_back(b);
  }
  return f;
}

DocFragment Editor::cut() {
  DocFragment f = copy();
  if (f.empty()) return f;
  if (!hasSelection() && blockSelected()) { deleteBlock(caret.blk); return f; }  // its own undo step
  begin("cut");
  deleteSelection();
  refreshCitations();
  return f;
}

void Editor::paste(const DocFragment& f) {
  if (!d || f.empty()) return;
  begin("paste");
  if (hasSelection()) deleteSelection();
  vector<Block>& bl = d->blocks;
  Para* p = curPara();
  if (!f.spans.empty()) {
    if (!p) { bl.insert(bl.begin() + caret.blk + 1, Block::para("")); caret = anchor = {caret.blk + 1, -1, 0}; p = curPara(); }
    size_t n = 0;
    for (auto& s : f.spans) n += s.text.size();
    insertSpans(*p, size_t(caret.off), f.spans);
    caret.off += int(n);
    anchor = caret;
    changed();
    return;
  }
  if (p && caret.cell != -1) {  // inside a cell or caption: the text only
    string t;
    for (auto& b : f.blocks) { if (b.kind == Block::Paragraph) t += (t.empty() ? "" : " ") + b.p.text(); }
    lastLabel_.clear();
    insertText(t);
    return;
  }
  vector<Block> ins = f.blocks;
  int at;
  if (p) {
    Para left, right;
    splitAt(caret, left, right);
    Block& cur = bl[size_t(caret.blk)];
    // the first pasted paragraph joins the current one when both are plain text; a heading, list item or other
    // styled paragraph pasted into a non-empty paragraph stays its own block
    bool joinFirst = ins.front().kind == Block::Paragraph && (left.empty() || (ins.front().p.style == PStyle::Body && cur.p.style != PStyle::Code));
    if (joinFirst) {
      Para fp = ins.front().p;
      if (left.empty()) cur.p = fp;
      else { cur.p = left; cur.p.spans.insert(cur.p.spans.end(), fp.spans.begin(), fp.spans.end()); cur.p.normalize(); }
      ins.erase(ins.begin());
    } else cur.p = left;
    if (ins.empty()) {  // only one paragraph and it was joined
      if (!right.empty()) { cur.p.spans.insert(cur.p.spans.end(), right.spans.begin(), right.spans.end()); cur.p.normalize(); }
      caret = {caret.blk, -1, int(cur.p.size() - right.size())};
      anchor = caret;
      changed();
      return;
    }
    at = caret.blk;
    bl.insert(bl.begin() + at + 1, ins.begin(), ins.end());
    int lastIdx = at + int(ins.size());
    Block& last = bl[size_t(lastIdx)];
    if (last.kind == Block::Paragraph) {
      caret = {lastIdx, -1, int(last.p.size())};
      if (!right.empty()) { last.p.spans.insert(last.p.spans.end(), right.spans.begin(), right.spans.end()); last.p.normalize(); }
    } else {
      Block rb;
      rb.p = right;
      bl.insert(bl.begin() + lastIdx + 1, rb);
      caret = {lastIdx + 1, -1, 0};
    }
  } else {
    at = caret.blk + 1;
    bl.insert(bl.begin() + at, ins.begin(), ins.end());
    caret = blockLast(*d, at + int(ins.size()) - 1);
  }
  clampPos(caret);
  anchor = caret;
  changed();
}

void Editor::pasteText(const string& text0, bool markdown) {
  string text = vs::replaceAll(text0, "\r", "");
  if (trim(text).empty()) return;
  DocFragment f;
  if (markdown) f.blocks = blocksFromMarkdown(text, 2, "");
  else {
    vector<string> lines = split(text, '\n', true);
    vector<string> nonEmpty;
    for (auto& l : lines) if (!trim(l).empty()) nonEmpty.push_back(l);
    if (nonEmpty.size() <= 1) { lastLabel_.clear(); insertText(nonEmpty.empty() ? string() : nonEmpty[0]); return; }
    for (auto& l : nonEmpty) f.blocks.push_back(Block::para(l));
  }
  if (f.blocks.size() == 1 && f.blocks[0].kind == Block::Paragraph && f.blocks[0].p.style == PStyle::Body) { f.spans = f.blocks[0].p.spans; f.blocks.clear(); }
  paste(f);
}

string Editor::selectionText() const {
  DocFragment f = copy();
  string o;
  for (auto& s : f.spans) o += s.text;
  for (size_t k = 0; k < f.blocks.size(); k++) {
    const Block& b = f.blocks[k];
    if (k) o += "\n";
    if (b.kind == Block::Paragraph) o += b.p.text();
    else if (b.kind == Block::FigureBlock) o += b.fig.caption.text();
    else if (b.kind == Block::TableBlock) for (size_t r = 0; r < b.tbl.cells.size(); r++) { if (r) o += "\n"; for (size_t c = 0; c < b.tbl.cells[r].size(); c++) o += (c ? "\t" : "") + b.tbl.cells[r][c].text(); }
  }
  return o;
}

string Editor::selectionMarkdown() const {
  DocFragment f = copy();
  if (!f.spans.empty()) return spansToMarkdown(f.spans);
  Document tmp;
  tmp.blocks = f.blocks;
  tmp.assets = d->assets;
  return blocksToMarkdown(tmp);
}

// ---------------------------------------------------------------- Editor: search
static size_t findIn(const string& hay, const string& needle, size_t from, bool matchCase, bool forward) {
  if (needle.empty() || hay.size() < needle.size()) return string::npos;
  string h = matchCase ? hay : lower(hay), n = matchCase ? needle : lower(needle);
  if (h.size() != hay.size()) { h = hay; n = needle; }  // lower() changed lengths (non-ASCII): match exactly
  if (forward) return h.find(n, from);
  if (from == 0) return string::npos;
  return h.rfind(n, from - 1);
}

bool Editor::find(const string& what, bool forward, bool matchCase, bool wrap) {
  if (!d || what.empty()) return false;
  struct Slot { int blk, cell; };
  vector<Slot> slots;
  for (size_t k = 0; k < d->blocks.size(); k++) blockParas(d->blocks[k], [&](int cell, Para&) { slots.push_back({int(k), cell}); });
  if (slots.empty()) return false;
  int start = 0;
  for (size_t i = 0; i < slots.size(); i++) if (slots[i].blk == caret.blk && slots[i].cell == caret.cell) { start = int(i); break; }
  if (slots[size_t(start)].blk != caret.blk) for (size_t i = 0; i < slots.size(); i++) if (slots[i].blk >= caret.blk) { start = int(i); break; }
  int n = int(slots.size());
  for (int step = 0; step <= n; step++) {
    int i = ((forward ? start + step : start - step) % n + n) % n;
    if (step == n && !wrap) break;
    const Para* p = docPara(*d, {slots[size_t(i)].blk, slots[size_t(i)].cell, 0});
    if (!p) continue;
    string t = p->text();
    size_t from = step == 0 ? (forward ? size_t(selEnd().blk == slots[size_t(i)].blk && selEnd().cell == slots[size_t(i)].cell ? selEnd().off : 0) : size_t(selStart().off)) : (forward ? 0 : t.size());
    if (step == 0 && !forward && !(selStart().blk == slots[size_t(i)].blk && selStart().cell == slots[size_t(i)].cell)) from = t.size();
    size_t at = findIn(t, what, from, matchCase, forward);
    if (at == string::npos) continue;
    anchor = {slots[size_t(i)].blk, slots[size_t(i)].cell, int(at)};
    caret = {slots[size_t(i)].blk, slots[size_t(i)].cell, int(at + what.size())};
    typingSet = false;
    return true;
  }
  return false;
}

bool Editor::replaceCurrent(const string& what, const string& with, bool matchCase) {
  if (!hasSelection()) return find(what, true, matchCase);
  string sel = selectionText();
  bool same = matchCase ? sel == what : lower(sel) == lower(what);
  if (!same) return find(what, true, matchCase);
  begin("replace");
  Para* p = docPara(*d, selStart());
  DocPos s = selStart();
  uint16_t f = p ? p->fmtAt(size_t(s.off) + 1) : 0;
  string link = p ? p->linkAt(size_t(s.off) + 1) : string();
  deleteSelection();
  if (p && !with.empty()) { insertSpans(*p, size_t(s.off), {Span{with, f, link}}); caret = anchor = {s.blk, s.cell, s.off + int(with.size())}; }
  changed();
  return find(what, true, matchCase);
}

int Editor::replaceAll(const string& what, const string& with, bool matchCase) {
  if (!d || what.empty()) return 0;
  begin("replace all");
  int n = 0;
  for (auto& b : d->blocks) blockParas(b, [&](int, Para& p) {
    string t = p.text();
    vector<size_t> hits;
    for (size_t at = 0; (at = findIn(t, what, at, matchCase, true)) != string::npos; at += what.size()) hits.push_back(at);
    for (size_t k = hits.size(); k-- > 0;) {
      uint16_t f = p.fmtAt(hits[k] + 1);
      string link = p.linkAt(hits[k] + 1);
      eraseBytes(p, hits[k], hits[k] + what.size());
      if (!with.empty()) insertSpans(p, hits[k], {Span{with, f, link}});
      n++;
    }
  });
  clampPos(caret);
  clampPos(anchor);
  if (n) changed();
  return n;
}

// ---------------------------------------------------------------- Editor: citations
int Editor::insertCitation(const string& referenceText, const string& url) {
  if (!d) return 0;
  string text = trim(referenceText);
  if (text.empty()) return 0;
  RefEntry e;
  e.raw = text;
  e.url = url;
  begin("citation");  // before the entry is added, so one undo removes both
  lastLabel_ = "citation";  // lets the inner insertCitation reuse this snapshot
  int at = docAddReference(*d, e);
  if (at < 0) { lastLabel_.clear(); return 0; }
  string key = d->refs[size_t(at)].key;
  if (!insertCitation(vector<string>{key})) { lastLabel_.clear(); return 0; }
  int n = 0;
  for (size_t k = 0; k < d->blocks.size(); k++) if (d->blocks[k].kind == Block::Paragraph && d->blocks[k].p.style == PStyle::Reference && d->blocks[k].p.refKey == key) n = d->refNumber(int(k));
  return n;
}

bool Editor::insertCitation(const vector<string>& keys0) {
  if (!d) return false;
  vector<string> keys;
  for (auto& k : keys0) if (d->findRef(k)) keys.push_back(k);
  if (keys.empty()) return false;
  begin("citation", true);  // coalesces with the snapshot insertCitation(text, url) just took; the label is cleared below
  if (hasSelection()) deleteSelection();
  Para* p = curPara();
  if (!p) {
    Block nb = Block::para("");
    d->blocks.insert(d->blocks.begin() + caret.blk + 1, nb);
    caret = anchor = {caret.blk + 1, -1, 0};
    p = curPara();
  }
  // inside or right after an existing field: merge the keys into it
  size_t pos = 0;
  for (auto& s : p->spans) {
    size_t e = pos + s.text.size();
    if (s.isCite() && size_t(caret.off) > pos && size_t(caret.off) <= e) {
      vector<string> have = split(s.cite, ';');
      for (auto& k : keys) if (std::find(have.begin(), have.end(), k) == have.end()) have.push_back(k);
      s.cite = join(have, ";");
      string merged = s.cite;
      refreshCitations();  // rebuilds the blocks: p and s are gone
      p = curPara();
      size_t q = 0;
      if (p) for (auto& t : p->spans) { q += t.text.size(); if (t.cite == merged) break; }
      if (p) caret.off = anchor.off = int(std::min(q, p->size()));
      changed();
      return true;
    }
    pos = e;
  }
  size_t i = splitSpan(*p, size_t(caret.off));
  Span ns;
  ns.cite = join(keys, ";");
  ns.text = "[?]";
  if (i > 0) { ns.font = p->spans[i - 1].font; ns.size = p->spans[i - 1].size; ns.color = p->spans[i - 1].color; }
  p->spans.insert(p->spans.begin() + long(i), ns);
  p->normalize();
  caret.off += int(ns.text.size());
  anchor = caret;
  refreshCitations();  // writes the real text (and rebuilds the blocks: p is gone); the caret sits after the field
  p = curPara();
  size_t q = 0;
  if (p) for (auto& t : p->spans) { q += t.text.size(); if (t.cite == ns.cite) break; }
  if (p) caret.off = anchor.off = int(std::min(q, p->size()));
  lastLabel_.clear();
  changed();
  return true;
}

vector<string> Editor::citeHere() const {
  const Para* p = d ? docPara(*d, caret) : nullptr;
  if (!p) return {};
  size_t pos = 0;
  int off = hasSelection() ? selStart().off : caret.off;
  for (auto& s : p->spans) {
    size_t e = pos + s.text.size();
    if (s.isCite() && (size_t(off) > pos || (hasSelection() && size_t(off) >= pos)) && size_t(off) <= e) {
      if (!hasSelection() && size_t(off) == e && &s != &p->spans.back()) { /* boundary: the field before the caret */ }
      vector<string> out;
      for (auto& k : split(s.cite, ';')) if (!trim(k).empty()) out.push_back(trim(k));
      return out;
    }
    pos = e;
  }
  return {};
}

bool Editor::removeCitationHere() {
  Para* p = curPara();
  if (!p) return false;
  size_t pos = 0;
  for (size_t i = 0; i < p->spans.size(); i++) {
    Span& s = p->spans[i];
    size_t e = pos + s.text.size();
    if (s.isCite() && size_t(caret.off) >= pos && size_t(caret.off) <= e) {
      begin("remove citation");
      p->spans.erase(p->spans.begin() + long(i));
      if (p->spans.empty()) p->spans.push_back({"", 0, ""});
      p->normalize();
      caret.off = anchor.off = int(pos);
      refreshCitations();
      changed();
      return true;
    }
    pos = e;
  }
  return false;
}

void Editor::setCiteStyle(const string& id) {
  if (!d || d->citeStyle == id) return;
  begin("citation style");
  d->citeStyle = citeStyleInfo(id).id;
  refreshCitations();
  changed();
}

void Editor::refreshCitations() {
  if (!d) return;
  docRefreshCitations(*d, &caret, &anchor);
  clampPos(caret);
  clampPos(anchor);
}

// ---- character properties beyond the format bits
namespace {
template <class F>
void setProp(Editor& ed, const char* label, F fn) {
  if (!ed.d) return;
  if (!ed.hasSelection()) {
    Para* p = ed.curPara();
    if (!p) return;
    if (!ed.typingPropsSet) { ed.typingFont = ed.fontHere(); ed.typingSize = ed.sizeHere(); ed.typingColor = ed.colorHere(); ed.typingPropsSet = true; }
    Span tmp;
    tmp.font = ed.typingFont; tmp.size = ed.typingSize; tmp.color = ed.typingColor;
    fn(tmp);
    ed.typingFont = tmp.font; ed.typingSize = tmp.size; ed.typingColor = tmp.color;
    return;
  }
  ed.begin(label);
  forEachRange(ed, [&](Para& p, size_t a, size_t b) { ed.applyFmtPublic(p, a, b, fn); });
  ed.version++;
}
}  // namespace

void Editor::applyFmtPublic(Para& p, size_t a, size_t b, const std::function<void(Span&)>& fn) { applyFmt(p, a, b, fn); }

void Editor::setFont(const string& family) { setProp(*this, "font", [&](Span& s) { s.font = family; }); }
void Editor::setSize(float pt) { setProp(*this, "font size", [&](Span& s) { s.size = pt > 0 ? clampv(pt, 5.f, 96.f) : 0.f; }); }
void Editor::setColor(uint32_t argb) { setProp(*this, "text colour", [&](Span& s) { s.color = argb; }); }

namespace {
const Span* spanHere(const Editor& ed) {
  if (!ed.d) return nullptr;
  DocPos s = ed.hasSelection() ? ed.selStart() : ed.caret;
  const Para* p = docPara(*ed.d, s);
  if (!p || p->spans.empty()) return nullptr;
  size_t off = size_t(s.off) + (ed.hasSelection() ? 1 : 0);
  size_t pos = 0;
  for (auto& sp : p->spans) {
    size_t e = pos + sp.text.size();
    if (off <= e && (off > pos || &sp == &p->spans.front())) return &sp;
    pos = e;
  }
  return &p->spans.back();
}
}  // namespace

string Editor::fontHere() const { if (typingPropsSet && !hasSelection()) return typingFont; const Span* s = spanHere(*this); return s ? s->font : string(); }
float Editor::sizeHere() const { if (typingPropsSet && !hasSelection()) return typingSize; const Span* s = spanHere(*this); return s ? s->size : 0.f; }
uint32_t Editor::colorHere() const { if (typingPropsSet && !hasSelection()) return typingColor; const Span* s = spanHere(*this); return s ? s->color : 0u; }

// ---- figures
const Figure* Editor::figureHere() const {
  if (!d || caret.blk < 0 || caret.blk >= int(d->blocks.size())) return nullptr;
  const Block& b = d->blocks[size_t(caret.blk)];
  return b.kind == Block::FigureBlock ? &b.fig : nullptr;
}

void Editor::setFigure(const Figure& f) {
  if (!d || caret.blk < 0 || caret.blk >= int(d->blocks.size())) return;
  Block& b = d->blocks[size_t(caret.blk)];
  if (b.kind != Block::FigureBlock) return;
  begin("figure");
  int asset = b.fig.asset;
  Para cap = b.fig.caption;
  b.fig = f;
  b.fig.asset = asset;
  b.fig.caption = cap;
  b.fig.widthPct = clampv(b.fig.widthPct, 20, 100);
  changed();
}

}  // namespace vs
