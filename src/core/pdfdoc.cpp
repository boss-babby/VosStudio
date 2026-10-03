#include "pdfdoc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pdfium.h"

namespace vs {
namespace pdf {

// ---------------------------------------------------------------- helpers
string utf16to8(const std::u16string& s) {
  string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
  }
  return out;
}

std::u16string utf8to16(const string& s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 7; extra = 3; }
    else if (c >= 0xE0) { cp = c & 15; extra = 2; }
    else if (c >= 0xC0) { cp = c & 31; extra = 1; }
    i++;
    for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((unsigned char)s[i] & 63);
    if (cp >= 0x10000) { cp -= 0x10000; out += char16_t(0xD800 + (cp >> 10)); out += char16_t(0xDC00 + (cp & 0x3FF)); }
    else out += char16_t(cp);
  }
  return out;
}

Box unionBox(const Box& a, const Box& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

namespace {
const Pdfium& A() { return api(); }

void appendUtf8(string& out, char32_t cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
  else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

// PDFium returns UTF-16LE strings through "call with a buffer, get the byte count" pairs
template <class F>
string utf16Text(F&& fetch) {
  unsigned long n = fetch(nullptr, 0);  // bytes including the terminator
  if (n < 2) return string();
  std::u16string buf(n / 2, u'\0');
  fetch(&buf[0], n);
  while (!buf.empty() && buf.back() == 0) buf.pop_back();
  return utf16to8(buf);
}

bool isWordChar(char32_t c) {
  if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '\'' || c == '-';
  return c != 0x2013 && c != 0x2014 && c != 0x2019 && c != 0x00A0 && c > 0xA0 && !(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x303F);
}

bool lowerLetter(char32_t c) {
  if (c >= U'a' && c <= U'z') return true;
  if ((c >= 0x00DF && c <= 0x00F6) || (c >= 0x00F8 && c <= 0x00FF) || c == 0x0131 || c == 0x017F) return true;
  if ((c >= 0x0101 && c <= 0x012F && (c & 1)) || (c >= 0x0133 && c <= 0x0137 && (c & 1)) ||
      (c >= 0x013A && c <= 0x0148 && !(c & 1)) || (c >= 0x014B && c <= 0x0177 && (c & 1))) return true;
  if ((c >= 0x03AC && c <= 0x03CE) || (c >= 0x03B1 && c <= 0x03CB) || c == 0x03C2) return true;
  if ((c >= 0x0430 && c <= 0x045F) || (c >= 0x0461 && c <= 0x052F && (c & 1))) return true;
  if (c >= 0x0560 && c <= 0x0588) return true;  // Armenian
  return false;
}

bool visualHyphen(char32_t c) { return c == U'-' || c == 0x00AD || c == 0x2010; }
bool visualSpace(char32_t c) { return c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == 0x00A0; }
}  // namespace

string joinSelectionLines(const string& text) {
  vector<char32_t> src;
  src.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    unsigned char b0 = static_cast<unsigned char>(text[i]);
    char32_t cp = b0;
    size_t n = 1;
    if (b0 >= 0xF0) { cp = b0 & 0x07; n = 4; }
    else if (b0 >= 0xE0) { cp = b0 & 0x0F; n = 3; }
    else if (b0 >= 0xC2) { cp = b0 & 0x1F; n = 2; }
    else if (b0 >= 0x80) { cp = U'\uFFFD'; }
    if (n > 1) {
      if (i + n > text.size()) { cp = U'\uFFFD'; n = 1; }
      else {
        bool valid = true;
        for (size_t k = 1; k < n; k++) {
          unsigned char bx = static_cast<unsigned char>(text[i + k]);
          if ((bx & 0xC0) != 0x80) { valid = false; break; }
          cp = (cp << 6) | (bx & 0x3F);
        }
        if (!valid || (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
          cp = U'\uFFFD'; n = 1;
        }
      }
    }
    src.push_back(cp);
    i += n;
  }

  vector<char32_t> out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size(); i++) {
    char32_t cp = src[i];
    if (cp == U'\r') continue;
    if (cp == U'\n') {
      size_t next = i + 1;
      while (next < src.size() && src[next] == U'\r') next++;
      while (next < src.size() && (src[next] == U' ' || src[next] == U'\t')) next++;
      if (!out.empty() && visualHyphen(out.back()) && next < src.size() && lowerLetter(src[next])) {
        out.pop_back();
        i = next - 1;
      } else if (!out.empty() && !visualSpace(out.back())) out.push_back(U' ');
      continue;
    }
    out.push_back(cp == 0x00A0 ? U' ' : cp);
  }
  string joined;
  joined.reserve(text.size());
  for (char32_t cp : out) appendUtf8(joined, cp);
  return collapseWs(trim(joined));
}

// ---------------------------------------------------------------- TextPage
int TextPage::lineOf(int ch) const {
  int lo = 0, hi = int(lines.size()) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (ch < lines[size_t(mid)][0]) hi = mid - 1;
    else if (ch > lines[size_t(mid)][1]) lo = mid + 1;
    else return mid;
  }
  return -1;
}

int TextPage::hit(float x, float y, bool nearest) const {
  if (chars.empty()) return -1;
  // exact: a character whose box contains the point (skip generated ones)
  for (size_t i = 0; i < chars.size(); i++) {
    const Char& c = chars[i];
    if (c.generated || c.b.empty()) continue;
    if (x >= c.b.x0 && x <= c.b.x1 && y >= c.b.y0 && y <= c.b.y1) return int(i);
  }
  if (!nearest) return -1;
  // the nearest line vertically, then the nearest character horizontally on it
  int bestLine = -1;
  float bestD = 1e30f;
  for (size_t li = 0; li < lines.size(); li++) {
    Box lb;
    for (int i = lines[li][0]; i <= lines[li][1]; i++) if (!chars[size_t(i)].generated) lb = unionBox(lb, chars[size_t(i)].b);
    if (lb.empty()) continue;
    float dy = y < lb.y0 ? lb.y0 - y : y > lb.y1 ? y - lb.y1 : 0;
    float dx = x < lb.x0 ? lb.x0 - x : x > lb.x1 ? x - lb.x1 : 0;
    float d = dy * 4 + dx;  // lines are what the eye follows: vertical distance weighs more
    if (d < bestD) { bestD = d; bestLine = int(li); }
  }
  if (bestLine < 0) return -1;
  int best = -1;
  float bx = 1e30f;
  for (int i = lines[size_t(bestLine)][0]; i <= lines[size_t(bestLine)][1]; i++) {
    const Char& c = chars[size_t(i)];
    if (c.generated || c.b.empty()) continue;
    float cx = (c.b.x0 + c.b.x1) / 2;
    float d = std::fabs(cx - x);
    if (d < bx) { bx = d; best = i; }
  }
  return best;
}

vector<Box> TextPage::rects(int a, int b) const {
  vector<Box> out;
  if (chars.empty()) return out;
  if (a > b) std::swap(a, b);
  a = std::max(0, a);
  b = std::min(int(chars.size()) - 1, b);
  int la = lineOf(a), lb = lineOf(b);
  if (la < 0 || lb < 0) return out;
  for (int li = la; li <= lb; li++) {
    int from = std::max(a, lines[size_t(li)][0]), to = std::min(b, lines[size_t(li)][1]);
    Box r;
    for (int i = from; i <= to; i++) if (!chars[size_t(i)].generated) r = unionBox(r, chars[size_t(i)].b);
    if (!r.empty()) out.push_back(r);
  }
  return out;
}

string TextPage::slice(int a, int b) const {
  if (chars.empty()) return string();
  if (a > b) std::swap(a, b);
  a = std::max(0, a);
  b = std::min(int(chars.size()) - 1, b);
  string out;
  for (int i = a; i <= b; i++) {
    char32_t cp = chars[size_t(i)].cp;
    if (cp == '\r') continue;
    if (cp == '\n') { if (!out.empty() && out.back() != '\n') out += '\n'; continue; }
    appendUtf8(out, cp);
  }
  return out;
}

std::array<int, 2> TextPage::word(int ch) const {
  std::array<int, 2> w{ch, ch};
  if (ch < 0 || ch >= int(chars.size())) return w;
  int li = lineOf(ch);
  int lo = li >= 0 ? lines[size_t(li)][0] : 0, hi = li >= 0 ? lines[size_t(li)][1] : int(chars.size()) - 1;
  if (!isWordChar(chars[size_t(ch)].cp)) return w;
  while (w[0] > lo && isWordChar(chars[size_t(w[0] - 1)].cp)) w[0]--;
  while (w[1] < hi && isWordChar(chars[size_t(w[1] + 1)].cp)) w[1]++;
  return w;
}

// ---------------------------------------------------------------- Document
struct Document::Access : FPDF_FILEACCESS {};

Document::Document() = default;
Document::~Document() { close(); }

int Document::getBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
  Document* d = static_cast<Document*>(param);
  if (!d->file_) return 0;
#ifdef _WIN32
  if (_fseeki64(d->file_, (long long)pos, SEEK_SET) != 0) return 0;
#else
  if (fseeko(d->file_, (off_t)pos, SEEK_SET) != 0) return 0;
#endif
  return fread(buf, 1, size, d->file_) == size ? 1 : 0;
}

bool Document::open(const string& path, string* err, const string& password) {
  close();
  if (!bound()) { if (err) *err = "The PDF engine is not loaded."; return false; }
  file_ = openFileUtf8(path, false);
  if (!file_) { if (err) *err = "Cannot open " + path; return false; }
  long long n = fileSizeU(path);
  if (n <= 0) { fclose(file_); file_ = nullptr; if (err) *err = "Empty file: " + path; return false; }
  fileLen_ = (unsigned long)n;
  path_ = path;
  access_.reset(new Access());
  access_->m_FileLen = fileLen_;
  access_->m_GetBlock = &Document::getBlock;
  access_->m_Param = this;
  doc_ = A().FPDF_LoadCustomDocument(access_.get(), password.empty() ? nullptr : password.c_str());
  return load(err, password);
}

bool Document::openMemory(string bytes, string* err, const string& password) {
  close();
  if (!bound()) { if (err) *err = "The PDF engine is not loaded."; return false; }
  bytes_ = std::move(bytes);
  doc_ = A().FPDF_LoadMemDocument64(bytes_.data(), bytes_.size(), password.empty() ? nullptr : password.c_str());
  return load(err, password);
}

bool Document::load(string* err, const string&) {
  if (!doc_) {
    unsigned long e = A().FPDF_GetLastError();
    needsPassword_ = e == FPDF_ERR_PASSWORD;
    if (err) *err = e == FPDF_ERR_PASSWORD ? "The PDF is password-protected." : e == FPDF_ERR_FORMAT ? "Not a PDF file, or a damaged one." : e == FPDF_ERR_SECURITY ? "The PDF's security settings do not allow opening it." : e == FPDF_ERR_FILE ? "The file could not be read." : "The PDF could not be opened.";
    if (file_) { fclose(file_); file_ = nullptr; }
    bytes_.clear();
    return false;
  }
  needsPassword_ = false;
  readInfo();
  return true;
}

void Document::close() {
  dropPages();
  if (doc_) { A().FPDF_CloseDocument(doc_); doc_ = nullptr; }
  if (file_) { fclose(file_); file_ = nullptr; }
  bytes_.clear();
  access_.reset();
  info_ = DocInfo();
  path_.clear();
}

void Document::readInfo() {
  info_ = DocInfo();
  info_.pages = A().FPDF_GetPageCount(doc_);
  info_.sizes.resize(size_t(std::max(0, info_.pages)));
  for (int i = 0; i < info_.pages; i++) {
    FS_SIZEF sz{612, 792};
    A().FPDF_GetPageSizeByIndexF(doc_, i, &sz);
    if (sz.width <= 1 || sz.height <= 1) { sz.width = 612; sz.height = 792; }
    info_.sizes[size_t(i)] = {sz.width, sz.height};
  }
  auto meta = [&](const char* tag) { return trim(utf16Text([&](void* buf, unsigned long n) { return A().FPDF_GetMetaText(doc_, tag, buf, n); })); };
  info_.title = meta("Title");
  info_.author = meta("Author");
  info_.subject = meta("Subject");
  info_.keywords = meta("Keywords");
  info_.creator = meta("Creator");
  info_.producer = meta("Producer");
  info_.created = meta("CreationDate");
  info_.modified = meta("ModDate");
  // the outline (bookmarks): depth-first, bounded
  std::function<void(void*, int)> walk = [&](void* parent, int depth) {
    if (depth > 8 || info_.outline.size() > 4000) return;
    int guard = 0;
    for (void* bm = A().FPDFBookmark_GetFirstChild(doc_, parent); bm && guard < 5000; bm = A().FPDFBookmark_GetNextSibling(doc_, bm), guard++) {
      OutlineItem it;
      it.title = trim(utf16Text([&](void* buf, unsigned long n) { return A().FPDFBookmark_GetTitle(bm, buf, n); }));
      it.depth = depth;
      void* dest = A().FPDFBookmark_GetDest(doc_, bm);
      if (!dest) { void* act = A().FPDFBookmark_GetAction(bm); if (act && A().FPDFAction_GetType(act) == PDFACTION_GOTO) dest = A().FPDFAction_GetDest(doc_, act); }
      if (dest) {
        it.page = A().FPDFDest_GetDestPageIndex(doc_, dest);
        FPDF_BOOL hx = 0, hy = 0, hz = 0;
        float x = 0, y = 0, z = 0;
        if (A().FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &y, &z) && hy && it.page >= 0 && it.page < info_.pages) it.y = std::max(0.f, info_.sizes[size_t(it.page)][1] - y);
      }
      info_.outline.push_back(it);
      walk(bm, depth + 1);
    }
  };
  walk(nullptr, 0);
  // page labels (only when the document defines any)
  bool any = false;
  vector<string> labels(size_t(std::max(0, info_.pages)));
  for (int i = 0; i < info_.pages && i < 2000; i++) {
    labels[size_t(i)] = utf16Text([&](void* buf, unsigned long n) { return A().FPDF_GetPageLabel(doc_, i, buf, n); });
    if (!labels[size_t(i)].empty() && labels[size_t(i)] != std::to_string(i + 1)) any = true;
  }
  if (any) { for (int i = 0; i < info_.pages; i++) if (labels[size_t(i)].empty()) labels[size_t(i)] = std::to_string(i + 1); info_.labels = labels; }
}

void Document::closePage(PageH& p) {
  if (p.tp) A().FPDFText_ClosePage(p.tp);
  if (p.page) A().FPDF_ClosePage(p.page);
  p = PageH();
}

void Document::dropPages() {
  for (auto& p : pages_) closePage(p);
  pages_.clear();
}

Document::PageH* Document::page(int idx, bool needText) {
  if (!doc_ || idx < 0 || idx >= info_.pages) return nullptr;
  for (size_t i = 0; i < pages_.size(); i++) {
    if (pages_[i].idx == idx) {
      if (i) { PageH p = pages_[i]; pages_.erase(pages_.begin() + long(i)); pages_.insert(pages_.begin(), p); }
      PageH& p = pages_.front();
      if (needText && !p.tp) p.tp = A().FPDFText_LoadPage(p.page);
      return &p;
    }
  }
  PageH p;
  p.idx = idx;
  p.page = A().FPDF_LoadPage(doc_, idx);
  if (!p.page) return nullptr;
  p.w = A().FPDF_GetPageWidthF(p.page);
  p.h = A().FPDF_GetPageHeightF(p.page);
  if (p.w > 1 && p.h > 1) info_.sizes[size_t(idx)] = {p.w, p.h};
  // user space -> display space: PDFium's page matrix (rotation + crop box origin) followed by the y flip
  FS_RECTF bb{0, p.h, p.w, 0};
  A().FPDF_GetPageBoundingBox(p.page, &bb);  // left, top, right, bottom in user space
  int rot = A().FPDFPage_GetRotation(p.page) & 3;
  Aff pm;
  switch (rot) {
    case 0: pm = {1, 0, 0, 1, -bb.left, -bb.bottom}; break;
    case 1: pm = {0, -1, 1, 0, -bb.bottom, bb.right}; break;
    case 2: pm = {-1, 0, 0, -1, bb.right, bb.top}; break;
    default: pm = {0, 1, -1, 0, bb.top, -bb.left}; break;
  }
  Aff flip{1, 0, 0, -1, 0, p.h};
  auto compose = [](const Aff& m1, const Aff& m2) {  // m1 then m2
    Aff r;
    r.a = m2.a * m1.a + m2.c * m1.b; r.b = m2.b * m1.a + m2.d * m1.b;
    r.c = m2.a * m1.c + m2.c * m1.d; r.d = m2.b * m1.c + m2.d * m1.d;
    r.e = m2.a * m1.e + m2.c * m1.f + m2.e; r.f = m2.b * m1.e + m2.d * m1.f + m2.f;
    return r;
  };
  p.toDisplay = compose(pm, flip);
  double det = p.toDisplay.a * p.toDisplay.d - p.toDisplay.b * p.toDisplay.c;
  if (std::fabs(det) > 1e-12) {
    const Aff& m = p.toDisplay;
    Aff inv;
    inv.a = m.d / det; inv.b = -m.b / det; inv.c = -m.c / det; inv.d = m.a / det;
    inv.e = (m.c * m.f - m.d * m.e) / det; inv.f = (m.b * m.e - m.a * m.f) / det;
    p.toUser = inv;
  }
  if (needText) p.tp = A().FPDFText_LoadPage(p.page);
  pages_.insert(pages_.begin(), p);
  while (pages_.size() > 3) { closePage(pages_.back()); pages_.pop_back(); }  // a loaded page keeps its decoded images and text; three cover the view and its neighbours
  return &pages_.front();
}

Box Document::toDisplay(const PageH& p, double x0, double y0, double x1, double y1) const {
  const Aff& m = p.toDisplay;
  double xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
  Box b{1e30f, 1e30f, -1e30f, -1e30f};
  for (int i = 0; i < 4; i++) {
    double dx = m.a * xs[i] + m.c * ys[i] + m.e, dy = m.b * xs[i] + m.d * ys[i] + m.f;
    b.x0 = std::min(b.x0, float(dx)); b.x1 = std::max(b.x1, float(dx));
    b.y0 = std::min(b.y0, float(dy)); b.y1 = std::max(b.y1, float(dy));
  }
  return b;
}

bool Document::render(int idx, double scale, int x, int y, int w, int h, Bitmap& out, bool withAnnots, bool dark, int rot) {
  PageH* p = page(idx, false);
  if (!p || w <= 0 || h <= 0 || scale <= 0) return false;
  out.w = w;
  out.h = h;
  out.bgra.assign(size_t(w) * size_t(h) * 4, 0);
  void* bm = A().FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, out.bgra.data(), w * 4);
  if (!bm) return false;
  A().FPDFBitmap_FillRect(bm, 0, 0, w, h, 0xFFFFFFFFu);
  // the engine maps the page onto [0, w] x [0, h] (y down); the view rotation is composed on top of that, then the
  // scale and the region offset: x' = a x + c y + e, y' = b x + d y + f
  const float pw = p->w, ph = p->h;
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  switch (rot & 3) {
    case 1: a = 0; b = 1; c = -1; d = 0; e = ph; f = 0; break;
    case 2: a = -1; b = 0; c = 0; d = -1; e = pw; f = ph; break;
    case 3: a = 0; b = -1; c = 1; d = 0; e = 0; f = pw; break;
    default: break;
  }
  const float sc = float(scale);
  FS_MATRIX m{a * sc, b * sc, c * sc, d * sc, e * sc - float(x), f * sc - float(y)};
  FS_RECTF clip{0, 0, float(w), float(h)};
  A().FPDF_RenderPageBitmapWithMatrix(bm, p->page, &m, &clip, (withAnnots ? FPDF_ANNOT : 0) | FPDF_RENDER_LIMITEDIMAGECACHE);
  A().FPDFBitmap_Destroy(bm);
  if (dark) {
    vector<ImageBox> ims;
    images(idx, ims);
    vector<PxRect> keep;
    for (auto& im : ims) {
      if (!im.photo) continue;
      const Box rb = rotateBox(rot, pw, ph, im.box);
      PxRect k{int(std::floor(rb.x0 * scale)) - x, int(std::floor(rb.y0 * scale)) - y, int(std::ceil(rb.x1 * scale)) - x, int(std::ceil(rb.y1 * scale)) - y};
      if (k.x1 <= 0 || k.y1 <= 0 || k.x0 >= w || k.y0 >= h) continue;
      keep.push_back(k);
    }
    darkenPage(out, keep);
  }
  return true;
}

// ------------------------------------------------------------------ images on a page, and the dark page
namespace {
void walkImages(const Pdfium& A, void* page, void* obj, const FS_MATRIX& acc, int depth, vector<std::pair<Box, FPDF_IMAGEOBJ_METADATA>>& out) {
  if (!obj || out.size() >= 400) return;
  int t = A.FPDFPageObj_GetType(obj);
  float l = 0, b = 0, r = 0, tp = 0;
  if (!A.FPDFPageObj_GetBounds(obj, &l, &b, &r, &tp)) return;
  auto map = [&](float x, float y, float& ox, float& oy) { ox = acc.a * x + acc.c * y + acc.e; oy = acc.b * x + acc.d * y + acc.f; };
  if (t == FPDF_PAGEOBJ_IMAGE) {
    float xs[4], ys[4];
    map(l, b, xs[0], ys[0]); map(r, b, xs[1], ys[1]); map(l, tp, xs[2], ys[2]); map(r, tp, xs[3], ys[3]);
    Box bx{1e30f, 1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 4; i++) { bx.x0 = std::min(bx.x0, xs[i]); bx.x1 = std::max(bx.x1, xs[i]); bx.y0 = std::min(bx.y0, ys[i]); bx.y1 = std::max(bx.y1, ys[i]); }
    FPDF_IMAGEOBJ_METADATA md{};
    A.FPDFImageObj_GetImageMetadata(obj, page, &md);
    out.push_back({bx, md});
  } else if (t == FPDF_PAGEOBJ_FORM && depth < 6) {
    // children's bounds are in the form's space; the form object's matrix places it on the page
    FS_MATRIX fm{1, 0, 0, 1, 0, 0};
    A.FPDFPageObj_GetMatrix(obj, &fm);
    FS_MATRIX n;
    n.a = fm.a * acc.a + fm.b * acc.c; n.b = fm.a * acc.b + fm.b * acc.d;
    n.c = fm.c * acc.a + fm.d * acc.c; n.d = fm.c * acc.b + fm.d * acc.d;
    n.e = fm.e * acc.a + fm.f * acc.c + acc.e; n.f = fm.e * acc.b + fm.f * acc.d + acc.f;
    int cnt = A.FPDFFormObj_CountObjects(obj);
    for (int i = 0; i < cnt; i++) walkImages(A, page, A.FPDFFormObj_GetObject(obj, (unsigned long)i), n, depth + 1, out);
  }
}
}  // namespace

bool Document::images(int idx, vector<ImageBox>& out) {
  out.clear();
  PageH* p = page(idx, false);
  if (!p) return false;
  if (!p->imagesDone) {
    p->imagesDone = true;
    vector<std::pair<Box, FPDF_IMAGEOBJ_METADATA>> raw;
    FS_MATRIX id{1, 0, 0, 1, 0, 0};
    int n = A().FPDFPage_CountObjects(p->page);
    for (int i = 0; i < n; i++) walkImages(A(), p->page, A().FPDFPage_GetObject(p->page, i), id, 0, raw);
    int classified = 0;
    for (auto& r : raw) {
      ImageBox ib;
      ib.box = toDisplay(*p, r.first.x0, r.first.y0, r.first.x1, r.first.y1);
      const FPDF_IMAGEOBJ_METADATA& md = r.second;
      // bilevel images are line art or scans; tiny ones are glyphs and icons; the rest is judged by its pixels
      bool candidate = md.bits_per_pixel != 1 && ib.box.w() >= 24 && ib.box.h() >= 24 && classified < 48;
      if (candidate) {
        classified++;
        double sc = std::min(1.0, 96.0 / std::max(ib.box.w(), ib.box.h()));
        int x0 = int(std::floor(ib.box.x0 * sc)), y0 = int(std::floor(ib.box.y0 * sc));
        int w = std::max(1, int(std::ceil(ib.box.x1 * sc)) - x0), h = std::max(1, int(std::ceil(ib.box.y1 * sc)) - y0);
        Bitmap tmp;
        if (render(idx, sc, x0, y0, w, h, tmp, false, false)) ib.photo = looksLikePhoto(tmp, 0, 0, w, h);
      }
      p->images.push_back(ib);
    }
  }
  out = p->images;
  return true;
}

bool looksLikePhoto(const Bitmap& bm, int x0, int y0, int x1, int y1) {
  x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(bm.w, x1); y1 = std::min(bm.h, y1);
  if (x1 - x0 < 4 || y1 - y0 < 4) return false;
  long n = 0, white = 0, grey = 0;
  vector<uint8_t> seen(4096, 0);
  int distinct = 0;
  const int stepx = std::max(1, (x1 - x0) / 96), stepy = std::max(1, (y1 - y0) / 96);
  for (int y = y0; y < y1; y += stepy)
    for (int x = x0; x < x1; x += stepx) {
      const uint8_t* px = &bm.bgra[(size_t(y) * size_t(bm.w) + size_t(x)) * 4];
      int b = px[0], g = px[1], r = px[2];
      int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
      n++;
      if (mn > 232) white++;
      if (mx - mn < 18) grey++;
      int q = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
      if (!seen[size_t(q)]) { seen[size_t(q)] = 1; distinct++; }
    }
  double wf = double(white) / double(n), gf = double(grey) / double(n);
  // a photograph: little white, many distinct colours — or, when grey, many grey levels (a micrograph)
  return wf < 0.30 && (distinct > 160 || (gf > 0.9 && distinct > 40));
}

// ---- marks burnt into a rendered page (printing)
void burnMarks(Bitmap& bm, double scale, int rot, float pageW, float pageH, int ox, int oy, const vector<Mark>& marks) {
  if (bm.w <= 0 || bm.h <= 0 || bm.bgra.size() < size_t(bm.w) * size_t(bm.h) * 4) return;
  auto px = [&](const Box& b, PxRect& r) {
    const Box d = rotateBox(rot, pageW, pageH, b);
    r = {int(std::floor(d.x0 * scale)) - ox, int(std::floor(d.y0 * scale)) - oy, int(std::ceil(d.x1 * scale)) - ox, int(std::ceil(d.y1 * scale)) - oy};
    r.x0 = std::max(0, r.x0); r.y0 = std::max(0, r.y0); r.x1 = std::min(bm.w, r.x1); r.y1 = std::min(bm.h, r.y1);
    return r.x1 > r.x0 && r.y1 > r.y0;
  };
  auto multiply = [&](const PxRect& r, uint32_t rgb) {
    const int mr = (rgb >> 16) & 255, mg = (rgb >> 8) & 255, mb = rgb & 255;
    for (int yy = r.y0; yy < r.y1; yy++) {
      uint8_t* row = bm.bgra.data() + (size_t(yy) * bm.w + r.x0) * 4;
      for (int xx = r.x0; xx < r.x1; xx++, row += 4) {
        row[0] = uint8_t(row[0] * mb / 255);
        row[1] = uint8_t(row[1] * mg / 255);
        row[2] = uint8_t(row[2] * mr / 255);
      }
    }
  };
  auto fill = [&](const PxRect& r, uint32_t rgb) {
    const uint8_t cr = (rgb >> 16) & 255, cg = (rgb >> 8) & 255, cb = rgb & 255;
    for (int yy = r.y0; yy < r.y1; yy++) {
      uint8_t* row = bm.bgra.data() + (size_t(yy) * bm.w + r.x0) * 4;
      for (int xx = r.x0; xx < r.x1; xx++, row += 4) { row[0] = cb; row[1] = cg; row[2] = cr; row[3] = 255; }
    }
  };
  auto frame = [&](const Box& b, float thick, uint32_t rgb) {
    PxRect r;
    if (!px(b, r)) return;
    const int t = std::max(1, int(std::lround(thick * scale)));
    fill({r.x0, r.y0, r.x1, std::min(r.y1, r.y0 + t)}, rgb);
    fill({r.x0, std::max(r.y0, r.y1 - t), r.x1, r.y1}, rgb);
    fill({r.x0, r.y0, std::min(r.x1, r.x0 + t), r.y1}, rgb);
    fill({std::max(r.x0, r.x1 - t), r.y0, r.x1, r.y1}, rgb);
  };
  // a darker shade of the mark colour for lines (a pale highlighter yellow would vanish on paper)
  auto ink = [](uint32_t rgb) {
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    return uint32_t((r * 55 / 100) << 16 | (g * 55 / 100) << 8 | (b * 55 / 100));
  };
  auto line = [&](const InkPoint& a, const InkPoint& b, float width, uint32_t rgb) {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) return;
    float ax, ay, bx, by;
    rotatePoint(rot, pageW, pageH, clampv(a.x, 0.f, pageW), clampv(a.y, 0.f, pageH), ax, ay);
    rotatePoint(rot, pageW, pageH, clampv(b.x, 0.f, pageW), clampv(b.y, 0.f, pageH), bx, by);
    int x0 = int(std::lround(ax * scale)) - ox, y0 = int(std::lround(ay * scale)) - oy;
    int x1 = int(std::lround(bx * scale)) - ox, y1 = int(std::lround(by * scale)) - oy;
    int thick = clampv(int(std::lround(std::max(0.25f, width) * scale)), 1, 128);
    int lo = (thick - 1) / 2, hi = thick / 2;
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
      PxRect brush{std::max(0, x0 - lo), std::max(0, y0 - lo), std::min(bm.w, x0 + hi + 1), std::min(bm.h, y0 + hi + 1)};
      if (brush.x1 > brush.x0 && brush.y1 > brush.y0) fill(brush, rgb);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  };
  const float lineW = 1.4f;  // points
  for (const Mark& m : marks) {
    PxRect r;
    switch (m.kind) {
      case 0:
        for (const Box& q : m.quads) if (px(q, r)) multiply(r, m.rgb);
        break;
      case 1:
        for (const Box& q : m.quads) if (px({q.x0, q.y1 - lineW, q.x1, q.y1}, r)) fill(r, ink(m.rgb));
        break;
      case 2:
        for (const Box& q : m.quads) {
          const float mid = q.y0 + q.h() * 0.55f;
          if (px({q.x0, mid - lineW / 2, q.x1, mid + lineW / 2}, r)) fill(r, ink(m.rgb));
        }
        break;
      case 3: {
        const float sz = 9;  // points
        Box b{m.rect.x0, m.rect.y0, m.rect.x0 + sz, m.rect.y0 + sz};
        if (px(b, r)) fill(r, m.rgb);
        frame(b, 0.8f, ink(m.rgb));
        break;
      }
      case 4: frame(m.rect, lineW, ink(m.rgb)); break;
      case 5:
        for (const InkStroke& stroke : m.strokes)
          for (size_t i = 1; i < stroke.points.size(); i++) line(stroke.points[i - 1], stroke.points[i], m.strokeWidth, ink(m.rgb));
        break;
      default: break;
    }
  }
}

void darkenPage(Bitmap& bm, const vector<PxRect>& keep) {
  static uint8_t lut[256];
  static bool init = false;
  constexpr float lo = 0.11f, hi = 0.90f;  // paper -> #1c1c1c, ink -> #e6e6e6
  if (!init) {
    for (int v = 0; v < 256; v++) lut[v] = uint8_t(std::lround((lo + (1 - v / 255.f) * (hi - lo)) * 255));
    init = true;
  }
  for (int y = 0; y < bm.h; y++) {
    uint8_t* row = &bm.bgra[size_t(y) * size_t(bm.w) * 4];
    // the kept spans of this row (photographs), dimmed a little against the dark paper
    int nk = 0;
    int kx0[32], kx1[32];
    for (auto& k : keep) if (nk < 32 && y >= k.y0 && y < k.y1 && k.x1 > 0 && k.x0 < bm.w) { kx0[nk] = std::max(0, k.x0); kx1[nk] = std::min(bm.w, k.x1); nk++; }
    for (int x = 0; x < bm.w; x++) {
      uint8_t* p = row + x * 4;
      bool kept = false;
      for (int i = 0; i < nk; i++) if (x >= kx0[i] && x < kx1[i]) { kept = true; break; }
      if (kept) { p[0] = uint8_t(p[0] * 220 / 255); p[1] = uint8_t(p[1] * 220 / 255); p[2] = uint8_t(p[2] * 220 / 255); continue; }
      const int b = p[0], g = p[1], r = p[2];
      if (r == g && g == b) { p[0] = p[1] = p[2] = lut[r]; continue; }
      // luma inverted, chroma kept (BT.709 YCbCr), then scaled into the gamut so that the hue survives
      const float rf = r / 255.f, gf = g / 255.f, bf = b / 255.f;
      const float Y = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
      const float cb = (bf - Y) / 1.8556f, cr = (rf - Y) / 1.5748f;
      const float Y2 = lo + (1 - Y) * (hi - lo);
      float dr = 1.5748f * cr, dg = -0.1873f * cb - 0.4681f * cr, db = 1.8556f * cb;
      float sc = 1.f;
      auto fit = [&](float d) { if (d > 0 && Y2 + d * sc > 1) sc = std::min(sc, (1 - Y2) / d); if (d < 0 && Y2 + d * sc < 0) sc = std::min(sc, -Y2 / d); };
      fit(dr); fit(dg); fit(db);
      p[2] = uint8_t(std::lround(std::min(255.f, std::max(0.f, (Y2 + dr * sc) * 255))));
      p[1] = uint8_t(std::lround(std::min(255.f, std::max(0.f, (Y2 + dg * sc) * 255))));
      p[0] = uint8_t(std::lround(std::min(255.f, std::max(0.f, (Y2 + db * sc) * 255))));
    }
  }
}

void filterPage(Bitmap& bm, int mode) {
  if (mode < 1 || mode > 2 || bm.w <= 0 || bm.h <= 0) return;
  const size_t width = size_t(bm.w), height = size_t(bm.h);
  if (width > (bm.bgra.size() / 4) / height) return;  // ignore a malformed/partial buffer rather than reading past it
  const size_t pixels = width * height;
  for (size_t i = 0; i < pixels; i++) {
    uint8_t* p = bm.bgra.data() + i * 4;
    if (mode == 1) {  // warm paper: preserve contrast, gently reduce green/blue; RGB order is p[2], p[1], p[0]
      p[1] = uint8_t(std::lround(p[1] * 0.985f));
      p[0] = uint8_t(std::lround(p[0] * 0.94f));
    } else {  // grayscale (BT.709); the rendered bitmap is opaque BGRA so alpha remains untouched
      const uint8_t luma = uint8_t(std::lround(0.2126f * p[2] + 0.7152f * p[1] + 0.0722f * p[0]));
      p[0] = p[1] = p[2] = luma;
    }
  }
}

bool Document::text(int idx, TextPage& out) {
  out = TextPage();
  PageH* p = page(idx, true);
  if (!p || !p->tp) return false;
  int n = A().FPDFText_CountChars(p->tp);
  if (n <= 0) return true;
  out.chars.resize(size_t(n));
  out.byteAt.resize(size_t(n) + 1);
  std::map<int, int> sizes;
  int lineStart = 0;
  for (int i = 0; i < n; i++) {
    Char& c = out.chars[size_t(i)];
    c.cp = A().FPDFText_GetUnicode(p->tp, i);
    c.generated = A().FPDFText_IsGenerated(p->tp, i) == 1;
    if (!c.generated) {
      FS_RECTF lb{};
      double l = 0, r = 0, b = 0, t = 0;
      bool loose = A().FPDFText_GetLooseCharBox(p->tp, i, &lb) && lb.right > lb.left && lb.top != lb.bottom;
      bool tight = A().FPDFText_GetCharBox(p->tp, i, &l, &r, &b, &t) && r > l;
      if (loose) c.b = toDisplay(*p, lb.left, std::min(lb.top, lb.bottom), lb.right, std::max(lb.top, lb.bottom));
      else if (tight) c.b = toDisplay(*p, l, b, r, t);
      if (loose && tight) {  // very tall loose boxes (fonts with big line gaps) are pulled towards the glyphs
        Box tb = toDisplay(*p, l, b, r, t);
        float fs = float(A().FPDFText_GetFontSize(p->tp, i));
        float maxH = std::max(fs * 1.35f, tb.h());
        if (c.b.h() > maxH && maxH > 0) { float cy = (tb.y0 + tb.y1) / 2; c.b.y0 = cy - maxH / 2; c.b.y1 = cy + maxH / 2; }
      }
      int fs = int(std::lround(A().FPDFText_GetFontSize(p->tp, i)));
      if (fs > 0) sizes[fs]++;
    }
    out.byteAt[size_t(i)] = uint32_t(out.text.size());
    if (c.cp != '\r') appendUtf8(out.text, c.cp == '\n' ? U'\n' : c.cp);
    bool nl = c.cp == '\n' || (c.cp == '\r' && (i + 1 >= n || A().FPDFText_GetUnicode(p->tp, i + 1) != '\n'));  // the engine writes "\r\n"
    if (nl) { out.lines.push_back({lineStart, i}); lineStart = i + 1; }
  }
  out.byteAt[size_t(n)] = uint32_t(out.text.size());
  if (lineStart < n) out.lines.push_back({lineStart, n - 1});
  int best = 0;
  for (auto& kv : sizes) if (kv.second > best) { best = kv.second; out.fontSize = float(kv.first); }
  return true;
}

bool Document::links(int idx, vector<Link>& out) {
  out.clear();
  PageH* p = page(idx, true);
  if (!p) return false;
  // URLs written as text
  if (p->tp) {
    void* pl = A().FPDFLink_LoadWebLinks(p->tp);
    if (pl) {
      int n = A().FPDFLink_CountWebLinks(pl);
      for (int i = 0; i < n && i < 500; i++) {
        int len = A().FPDFLink_GetURL(pl, i, nullptr, 0);
        if (len <= 1) continue;
        std::u16string u(size_t(len), u'\0');
        A().FPDFLink_GetURL(pl, i, reinterpret_cast<unsigned short*>(&u[0]), len);
        while (!u.empty() && u.back() == 0) u.pop_back();
        string url = utf16to8(u);
        int nr = A().FPDFLink_CountRects(pl, i);
        for (int r = 0; r < nr; r++) {
          double l, t, rr, b;
          if (!A().FPDFLink_GetRect(pl, i, r, &l, &t, &rr, &b)) continue;
          Link L;
          L.b = toDisplay(*p, l, b, rr, t);
          L.url = url;
          out.push_back(L);
        }
      }
      A().FPDFLink_CloseWebLinks(pl);
    }
  }
  // link annotations (internal destinations and URIs)
  int pos = 0;
  void* link = nullptr;
  int guard = 0;
  while (A().FPDFLink_Enumerate(p->page, &pos, &link) && link && guard++ < 2000) {
    FS_RECTF r{};
    if (!A().FPDFLink_GetAnnotRect(link, &r)) continue;
    Link L;
    L.b = toDisplay(*p, r.left, std::min(r.top, r.bottom), r.right, std::max(r.top, r.bottom));
    void* dest = A().FPDFLink_GetDest(doc_, link);
    if (!dest) {
      void* act = A().FPDFLink_GetAction(link);
      if (act) {
        unsigned long t = A().FPDFAction_GetType(act);
        if (t == PDFACTION_GOTO) dest = A().FPDFAction_GetDest(doc_, act);
        else if (t == PDFACTION_URI) {
          unsigned long n = A().FPDFAction_GetURIPath(doc_, act, nullptr, 0);
          if (n > 1) { string u(n, '\0'); A().FPDFAction_GetURIPath(doc_, act, &u[0], n); while (!u.empty() && u.back() == 0) u.pop_back(); L.url = u; }
        }
      }
    }
    if (dest) {
      L.page = A().FPDFDest_GetDestPageIndex(doc_, dest);
      FPDF_BOOL hx = 0, hy = 0, hz = 0;
      float x = 0, y = 0, z = 0;
      if (A().FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &y, &z) && hy && L.page >= 0 && L.page < info_.pages) L.y = std::max(0.f, info_.sizes[size_t(L.page)][1] - y);
    }
    if (L.page >= 0 || !L.url.empty()) out.push_back(L);
  }
  return true;
}

bool Document::find(int idx, const std::u16string& needle, bool matchCase, bool wholeWord, vector<Hit>& out, TextPage* cached) {
  if (needle.empty()) return false;
  PageH* p = page(idx, true);
  if (!p || !p->tp) return false;
  TextPage local;
  TextPage* tp = cached && !cached->empty() ? cached : nullptr;
  std::u16string z = needle;
  z.push_back(0);
  void* h = A().FPDFText_FindStart(p->tp, reinterpret_cast<const unsigned short*>(z.c_str()), (matchCase ? FPDF_MATCHCASE : 0) | (wholeWord ? FPDF_MATCHWHOLEWORD : 0), 0);
  if (!h) return false;
  int guard = 0;
  while (A().FPDFText_FindNext(h) && guard++ < 5000) {
    Hit hit;
    hit.page = idx;
    hit.start = A().FPDFText_GetSchResultIndex(h);
    hit.count = A().FPDFText_GetSchCount(h);
    if (hit.start < 0 || hit.count <= 0) continue;
    int nr = A().FPDFText_CountRects(p->tp, hit.start, hit.count);
    for (int r = 0; r < nr; r++) {
      double l, t, rr, b;
      if (A().FPDFText_GetRect(p->tp, r, &l, &t, &rr, &b)) hit.rects.push_back(toDisplay(*p, l, b, rr, t));
    }
    if (!tp) { text(idx, local); tp = &local; }
    if (!tp->empty()) {
      int a = std::max(0, hit.start - 40), b2 = std::min(int(tp->chars.size()) - 1, hit.start + hit.count - 1 + 40);
      hit.before = replaceAll(tp->slice(a, hit.start - 1), "\n", " ");
      hit.match = replaceAll(tp->slice(hit.start, hit.start + hit.count - 1), "\n", " ");
      hit.after = replaceAll(tp->slice(hit.start + hit.count, b2), "\n", " ");
      if (hit.start - 1 < a) hit.before.clear();
    }
    out.push_back(std::move(hit));
  }
  A().FPDFText_FindClose(h);
  return true;
}

bool Document::readAnnots(int idx, vector<FileAnnot>& out) {
  out.clear();
  PageH* p = page(idx, false);
  if (!p) return false;
  int n = A().FPDFPage_GetAnnotCount(p->page);
  size_t pageInkPoints = 0;  // bound optional path extraction for unusually annotation-heavy documents
  for (int i = 0; i < n && i < 2000; i++) {
    void* an = A().FPDFPage_GetAnnot(p->page, i);
    if (!an) continue;
    FileAnnot fa;
    fa.subtype = A().FPDFAnnot_GetSubtype(an);
    bool markup = fa.subtype == FPDF_ANNOT_HIGHLIGHT || fa.subtype == FPDF_ANNOT_UNDERLINE || fa.subtype == FPDF_ANNOT_STRIKEOUT || fa.subtype == FPDF_ANNOT_SQUIGGLY ||
                  fa.subtype == FPDF_ANNOT_TEXT || fa.subtype == FPDF_ANNOT_SQUARE || fa.subtype == FPDF_ANNOT_INK || fa.subtype == FPDF_ANNOT_FREETEXT;
    if (!markup) { A().FPDFPage_CloseAnnot(an); continue; }
    FS_RECTF r{};
    if (A().FPDFAnnot_GetRect(an, &r)) fa.rect = toDisplay(*p, r.left, std::min(r.top, r.bottom), r.right, std::max(r.top, r.bottom));
    if (fa.subtype == FPDF_ANNOT_INK && A().FPDFAnnot_GetInkListCount && A().FPDFAnnot_GetInkListPath) {
      const unsigned long pathCount = std::min<unsigned long>(A().FPDFAnnot_GetInkListCount(an), 128UL);
      size_t totalPoints = 0;
      for (unsigned long path = 0; path < pathCount && totalPoints < 4096 && pageInkPoints < 50000; path++) {
        unsigned long n = A().FPDFAnnot_GetInkListPath(an, path, nullptr, 0);
        if (n < 2 || n > 4096 - totalPoints || n > 50000 - pageInkPoints) continue;
        vector<FS_POINTF> src;
        src.resize(static_cast<size_t>(n));
        if (A().FPDFAnnot_GetInkListPath(an, path, src.data(), n) != n) continue;
        InkStroke stroke;
        stroke.points.reserve(size_t(n));
        const Aff& m = p->toDisplay;
        for (const FS_POINTF& point : src) stroke.points.push_back({float(m.a * point.x + m.c * point.y + m.e), float(m.b * point.x + m.d * point.y + m.f)});
        totalPoints += size_t(n);
        pageInkPoints += size_t(n);
        fa.strokes.push_back(std::move(stroke));
      }
    }
    float hr = 0, vr = 0, width = 0;
    if (A().FPDFAnnot_GetBorder && A().FPDFAnnot_GetBorder(an, &hr, &vr, &width) && std::isfinite(width) && width > 0)
      fa.strokeWidth = clampv(width, 0.25f, 12.f);
    size_t nq = A().FPDFAnnot_CountAttachmentPoints(an);
    for (size_t q = 0; q < nq && q < 500; q++) {
      FS_QUADPOINTSF qp{};
      if (!A().FPDFAnnot_GetAttachmentPoints(an, q, &qp)) continue;
      double xs[4] = {qp.x1, qp.x2, qp.x3, qp.x4}, ys[4] = {qp.y1, qp.y2, qp.y3, qp.y4};
      double x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
      for (int k = 1; k < 4; k++) { x0 = std::min(x0, xs[k]); x1 = std::max(x1, xs[k]); y0 = std::min(y0, ys[k]); y1 = std::max(y1, ys[k]); }
      fa.quads.push_back(toDisplay(*p, x0, y0, x1, y1));
    }
    unsigned int R = 255, G = 255, B = 0, Al = 255;
    if (!A().FPDFAnnot_GetColor(an, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &Al)) {
      // annotations that carry an appearance stream refuse GetColor: take the colour of the first drawn object
      bool got = false;
      int no = A().FPDFAnnot_GetObjectCount(an);
      for (int k = 0; k < no && k < 8 && !got; k++) {
        void* obj = A().FPDFAnnot_GetObject(an, k);
        if (!obj) continue;
        unsigned int r2, g2, b2, a2;
        if (A().FPDFPageObj_GetFillColor(obj, &r2, &g2, &b2, &a2) && a2) { R = r2; G = g2; B = b2; Al = 255; got = true; }
        else if (A().FPDFPageObj_GetStrokeColor(obj, &r2, &g2, &b2, &a2) && a2) { R = r2; G = g2; B = b2; Al = 255; got = true; }
      }
      if (!got) { bool hl = fa.subtype == FPDF_ANNOT_HIGHLIGHT; R = hl ? 255 : 0; G = hl ? 255 : 0; B = 0; Al = 255; }
    }
    fa.color = (uint32_t(Al) << 24) | (uint32_t(R) << 16) | (uint32_t(G) << 8) | uint32_t(B);
    auto str = [&](const char* key) { return utf16Text([&](void* buf, unsigned long len) { return A().FPDFAnnot_GetStringValue(an, key, static_cast<FPDF_WCHAR*>(buf), len); }); };
    fa.contents = str("Contents");
    fa.author = str("T");
    fa.name = str("NM");
    fa.modified = str("M");
    A().FPDFPage_CloseAnnot(an);
    out.push_back(std::move(fa));
  }
  return true;
}

bool Document::writeAnnots(int idx, const vector<FileAnnot>& annots, string* err) {
  PageH* p = page(idx, false);
  if (!p) { if (err) *err = "page not available"; return false; }
  auto toUser = [&](const Box& b, double& l, double& bt, double& r, double& t) {
    const Aff& m = p->toUser;
    double xs[4] = {b.x0, b.x1, b.x0, b.x1}, ys[4] = {b.y0, b.y0, b.y1, b.y1};
    l = bt = 1e30; r = t = -1e30;
    for (int i = 0; i < 4; i++) {
      double ux = m.a * xs[i] + m.c * ys[i] + m.e, uy = m.b * xs[i] + m.d * ys[i] + m.f;
      l = std::min(l, ux); r = std::max(r, ux); bt = std::min(bt, uy); t = std::max(t, uy);
    }
  };
  auto wide = [](const string& s) { std::u16string u = utf8to16(s); u.push_back(0); return u; };
  int written = 0;
  for (auto& fa : annots) {
    if (fa.subtype == FPDF_ANNOT_INK && !A().FPDFAnnot_AddInkStroke) {
      if (err) *err = "This PDFium version can read and display drawings but does not export Ink annotations. Update the PDF engine to save drawings into the PDF.";
      return false;
    }
    void* an = A().FPDFPage_CreateAnnot(p->page, fa.subtype);
    if (!an) continue;
    unsigned int R = (fa.color >> 16) & 255, G = (fa.color >> 8) & 255, B = fa.color & 255, Al = (fa.color >> 24) & 255;
    if (Al == 0) Al = 255;
    A().FPDFAnnot_SetColor(an, FPDFANNOT_COLORTYPE_Color, R, G, B, Al);
    Box all;
    for (auto& q : fa.quads) {
      double l, b, r, t;
      toUser(q, l, b, r, t);
      FS_QUADPOINTSF qp{float(l), float(t), float(r), float(t), float(l), float(b), float(r), float(b)};
      A().FPDFAnnot_AppendAttachmentPoints(an, &qp);
      all = unionBox(all, q);
    }
    if (fa.subtype == FPDF_ANNOT_INK) {
      const Aff& m = p->toUser;
      size_t total = 0;
      bool inkOk = true;
      for (const InkStroke& stroke : fa.strokes) {
        if (stroke.points.size() < 2) continue;
        if (stroke.points.size() > 20000 || total > 20000 - stroke.points.size()) { inkOk = false; break; }
        vector<FS_POINTF> pts;
        pts.reserve(stroke.points.size());
        for (const InkPoint& pt : stroke.points) pts.push_back({float(m.a * pt.x + m.c * pt.y + m.e), float(m.b * pt.x + m.d * pt.y + m.f)});
        if (A().FPDFAnnot_AddInkStroke(an, pts.data(), pts.size()) < 0) { inkOk = false; break; }
        total += pts.size();
      }
      if (!inkOk || total < 2) {
        A().FPDFPage_CloseAnnot(an);
        int last = A().FPDFPage_GetAnnotCount(p->page) - 1;
        if (last >= 0) A().FPDFPage_RemoveAnnot(p->page, last);
        if (err) *err = "The freehand Ink annotation could not be written to the PDF.";
        return false;
      }
    }
    if (all.empty()) all = fa.rect;
    if (!all.empty()) {
      double l, b, r, t;
      toUser(all, l, b, r, t);
      FS_RECTF rr{float(l), float(t), float(r), float(b)};
      A().FPDFAnnot_SetRect(an, &rr);
    }
    if (fa.subtype == FPDF_ANNOT_SQUARE) A().FPDFAnnot_SetBorder(an, 0, 0, 1.5f);
    if (fa.subtype == FPDF_ANNOT_INK) A().FPDFAnnot_SetBorder(an, 0, 0, clampv(fa.strokeWidth, 0.25f, 12.f));
    if (!fa.contents.empty()) { std::u16string u = wide(fa.contents); A().FPDFAnnot_SetStringValue(an, "Contents", reinterpret_cast<FPDF_WIDESTRING>(u.c_str())); }
    if (!fa.author.empty()) { std::u16string u = wide(fa.author); A().FPDFAnnot_SetStringValue(an, "T", reinterpret_cast<FPDF_WIDESTRING>(u.c_str())); }
    if (!fa.name.empty()) { std::u16string u = wide(fa.name); A().FPDFAnnot_SetStringValue(an, "NM", reinterpret_cast<FPDF_WIDESTRING>(u.c_str())); }
    if (!fa.modified.empty()) { std::u16string u = wide(fa.modified); A().FPDFAnnot_SetStringValue(an, "M", reinterpret_cast<FPDF_WIDESTRING>(u.c_str())); }
    A().FPDFAnnot_SetFlags(an, FPDF_ANNOT_FLAG_PRINT);
    A().FPDFPage_CloseAnnot(an);
    written++;
  }
  // rendering the page once makes the engine generate appearance streams for the new annotations, which is what
  // other viewers draw; the streams stay in the document and are saved with it
  if (written) { Bitmap tmp; render(idx, 0.02, 0, 0, 4, 4, tmp, true); }
  return true;
}

int Document::removeAnnots(int idx, const std::function<bool(const FileAnnot&)>& which) {
  PageH* p = page(idx, false);
  if (!p) return 0;
  // walks the raw list from the end so indexes stay valid while removing
  int removed = 0;
  int n = A().FPDFPage_GetAnnotCount(p->page);
  for (int i = n - 1; i >= 0; i--) {
    void* an = A().FPDFPage_GetAnnot(p->page, i);
    if (!an) continue;
    FileAnnot fa;
    fa.subtype = A().FPDFAnnot_GetSubtype(an);
    fa.name = utf16Text([&](void* buf, unsigned long len) { return A().FPDFAnnot_GetStringValue(an, "NM", static_cast<FPDF_WCHAR*>(buf), len); });
    fa.contents = utf16Text([&](void* buf, unsigned long len) { return A().FPDFAnnot_GetStringValue(an, "Contents", static_cast<FPDF_WCHAR*>(buf), len); });
    A().FPDFPage_CloseAnnot(an);
    if (which(fa) && A().FPDFPage_RemoveAnnot(p->page, i)) removed++;
  }
  return removed;
}

namespace {
struct Writer : FPDF_FILEWRITE {
  FILE* f = nullptr;
  bool ok = true;
  static int write(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    Writer* w = static_cast<Writer*>(self);
    if (!w->f) return 0;
    if (fwrite(data, 1, size, w->f) != size) { w->ok = false; return 0; }
    return 1;
  }
};
}  // namespace

bool Document::saveAs(const string& path, string* err) {
  if (!doc_) { if (err) *err = "no document"; return false; }
  Writer w;
  w.version = 1;
  w.WriteBlock = &Writer::write;
  w.f = openFileUtf8(path, true);
  if (!w.f) { if (err) *err = "Cannot write " + path; return false; }
  FPDF_BOOL ok = A().FPDF_SaveAsCopy(doc_, &w, FPDF_NO_INCREMENTAL);
  fclose(w.f);
  if (!ok || !w.ok) { if (err) *err = "The PDF could not be written."; removeFileU(path); return false; }
  return true;
}

// ---------------------------------------------------------------- Worker
void Worker::start(std::function<void(std::function<void()>)> post) {
  if (running_) return;
  post_ = std::move(post);
  stop_ = false;
  running_ = true;
  th_ = std::thread([this] { run(); });
}

void Worker::stop() {
  if (!running_) return;
  { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
  running_ = false;
}

Worker::Handle Worker::submit(int priority, Task task) {
  Handle h;
  h.flag = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t seq = seq_++;
    queue_.emplace(std::make_pair(priority, seq), Item{priority, seq, std::move(task), h.flag});
  }
  cv_.notify_one();
  return h;
}

size_t Worker::pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return queue_.size();
}

bool Worker::idle() const {
  std::lock_guard<std::mutex> lk(mu_);
  return queue_.empty() && !busy_;
}

Document& Worker::doc(int id) {
  auto& d = docs_[id];
  if (!d) d.reset(new Document());
  return *d;
}

void Worker::closeDoc(int id) { docs_.erase(id); }

// the library is initialised on the worker thread as soon as the engine is bound — which may happen inside the first
// task (the application loads the DLL from the worker so that the first use never blocks the UI)
void Worker::ensureLib() {
  if (lib_ || !bound()) return;
  FPDF_LIBRARY_CONFIG cfg{};
  cfg.version = 2;
  api().FPDF_InitLibraryWithConfig(&cfg);
  lib_ = true;
}

bool Worker::releaseLibrary() {
  if (!lib_ || !docs_.empty()) return false;
  api().FPDF_DestroyLibrary();
  lib_ = false;
  return true;
}

void Worker::run() {
  ensureLib();
  for (;;) {
    Item it;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
      if (stop_) break;
      auto first = queue_.begin();
      it = std::move(first->second);
      queue_.erase(first);
      current_.flag = it.flag;
      busy_ = true;
    }
    ensureLib();
    if (!(it.flag && *it.flag)) {
      try { it.task(*this); } catch (...) {}
    }
    current_ = Handle();
    { std::lock_guard<std::mutex> lk(mu_); busy_ = false; }
  }
  {  // tasks never run are dropped now, while the library is alive (what they captured may hold engine objects)
    std::multimap<std::pair<int, uint64_t>, Item> rest;
    { std::lock_guard<std::mutex> lk(mu_); rest.swap(queue_); }
    rest.clear();
  }
  docs_.clear();
  if (lib_) api().FPDF_DestroyLibrary();
  lib_ = false;
}

}  // namespace pdf
}  // namespace vs
