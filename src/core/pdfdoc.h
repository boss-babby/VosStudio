// The PDF engine wrapper: one document = one PDFium handle plus a small cache of loaded pages, with everything the
// reader needs expressed in *display space* — points, origin at the page's top-left corner as drawn, the page's
// /Rotate and CropBox already applied — so the UI never sees PDF user space. Rendering produces top-down opaque
// BGRA buffers for any region of a page at any scale (tiles). Text comes out as characters with boxes and lines,
// which gives selection, search hits, links and highlight rectangles without further engine calls.
//
// PDFium is single-threaded: `Worker` owns the only thread that touches it, runs tasks by priority and hands the
// results back through a caller-supplied poster (the shell's UI queue). Documents live inside the worker and are
// addressed by id; tasks receive the Document by reference.
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "common.h"

namespace vs {
namespace pdf {

struct Box { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; float w() const { return x1 - x0; } float h() const { return y1 - y0; } bool empty() const { return x1 <= x0 || y1 <= y0; } };

// ---- view rotation (quarter turns clockwise, 0..3) of a page displayed w x h points; everything the engine reports
// (text boxes, links, annotations) stays in the unrotated display space, the rotation is a pure view transform
inline void rotatePoint(int rot, float w, float h, float x, float y, float& X, float& Y) {
  switch (rot & 3) {
    case 1: X = h - y; Y = x; break;
    case 2: X = w - x; Y = h - y; break;
    case 3: X = y; Y = w - x; break;
    default: X = x; Y = y; break;
  }
}
inline void unrotatePoint(int rot, float w, float h, float X, float Y, float& x, float& y) {
  switch (rot & 3) {
    case 1: x = Y; y = h - X; break;
    case 2: x = w - X; y = h - Y; break;
    case 3: x = w - Y; y = X; break;
    default: x = X; y = Y; break;
  }
}
inline Box rotateBox(int rot, float w, float h, const Box& b) {
  float ax, ay, bx, by;
  rotatePoint(rot, w, h, b.x0, b.y0, ax, ay);
  rotatePoint(rot, w, h, b.x1, b.y1, bx, by);
  return {std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
}

struct OutlineItem { string title; int page = -1; float y = -1; int depth = 0; };
struct DocInfo {
  int pages = 0;
  vector<std::array<float, 2>> sizes;  // points, as displayed (rotation applied)
  string title, author, subject, keywords, creator, producer, created, modified;
  vector<OutlineItem> outline;
  vector<string> labels;  // page labels when the document defines them (roman numerals for front matter…), else empty
};
struct Bitmap { int w = 0, h = 0; vector<uint8_t> bgra; };  // premultiplied (opaque) BGRA, top-down, stride = w*4

// an image drawn on a page (display space, points); `photo`: a photograph / micrograph, which the dark page keeps as
// it is, unlike graphics and scans, which it inverts with the rest of the page
struct ImageBox { Box box; bool photo = false; };
struct PxRect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };  // bitmap pixels

// ---- reader appearance (nothing is written back to the file)
// Turns a rendered region into its dark version: luma inverted (paper -> dark grey, ink -> light grey), hue kept,
// colours fitted back into the gamut (a blue link becomes a light blue, not a yellow); the rectangles in `keep` are
// left as they are, only dimmed a little (photographs).
void darkenPage(Bitmap& bm, const vector<PxRect>& keep);
// Non-inverting reading filters: 0 = unchanged, 1 = warm paper, 2 = grayscale. Applied to the rendered pixels only.
void filterPage(Bitmap& bm, int mode);
// Join visual line breaks in a selected passage. A hyphen-like character is removed only when it is a likely
// end-of-line word break (followed by a lowercase letter); paragraph breaks and semantic dashes retain spacing.
string joinSelectionLines(const string& text);
// Pixel statistics of a rendered region: a photograph (many colours or many grey levels, little white) rather than
// a graphic, a plot or a scanned page.
bool looksLikePhoto(const Bitmap& bm, int x0, int y0, int x1, int y1);

struct InkPoint { float x = 0, y = 0; };  // display-space page points
struct InkStroke { vector<InkPoint> points; };

// ---- the project's marks drawn onto a rendered page (printing): kind 0 highlight (multiplied, like a marker),
// 1 underline, 2 strikeout, 3 note (a small square), 4 area (an outline), 5 freehand Ink; boxes in unrotated display points
struct Mark { int kind = 0; uint32_t rgb = 0xFFE066; vector<Box> quads; vector<InkStroke> strokes; float strokeWidth = 1.8f; Box rect; };
void burnMarks(Bitmap& bm, double scale, int rot, float pageW, float pageH, int x, int y, const vector<Mark>& marks);

struct Char { Box b; char32_t cp = 0; bool generated = false; };  // generated = inserted by the engine (line breaks, spaces)
struct TextPage {
  vector<Char> chars;
  string text;               // UTF-8 of all characters ('\r' dropped)
  vector<uint32_t> byteAt;   // byte offset of chars[i] in text; size chars.size() + 1
  vector<std::array<int, 2>> lines;  // [first, last] char index per line
  float fontSize = 0;        // the most common size (for the mini bar / word gaps)
  bool empty() const { return chars.empty(); }
  int lineOf(int ch) const;
  // the character under (x, y), or the nearest one on the nearest line (for drag selection); -1 when there is no text
  int hit(float x, float y, bool nearest) const;
  // the rectangles covered by [a, b] (inclusive char indexes), one per line, tightened to the characters' boxes
  vector<Box> rects(int a, int b) const;
  string slice(int a, int b) const;  // UTF-8 text of [a, b], line breaks as '\n', hyphenation at line ends kept
  std::array<int, 2> word(int ch) const;  // the word around a character
};

struct Link { Box b; int page = -1; float y = -1; string url; };
struct Hit { int page = 0, start = 0, count = 0; vector<Box> rects; string before, match, after; };

// an annotation as PDFium reads / writes it (subtypes: 9 highlight, 10 underline, 12 strikeout, 1 text note, 5 square, 15 ink)
struct FileAnnot {
  int subtype = 0;
  vector<Box> quads;  // text markup: one box per line (display space)
  vector<InkStroke> strokes;  // freehand InkList paths (display-space page points)
  float strokeWidth = 1.8f;  // points
  Box rect;
  uint32_t color = 0xFFFFFF00;  // ARGB
  string contents, author;
  string name;      // the annotation's /NM (ours are "vs:<id>")
  string modified;  // /M, a PDF date string
};

class Document {
 public:
  Document();
  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  bool open(const string& path, string* err = nullptr, const string& password = "");
  bool openMemory(string bytes, string* err = nullptr, const string& password = "");
  void close();
  bool isOpen() const { return doc_ != nullptr; }
  const DocInfo& info() const { return info_; }
  const string& path() const { return path_; }
  bool passwordNeeded() const { return needsPassword_; }

  // region [x, y, x+w, y+h] (device pixels) of the page drawn at `scale` pixels per point; annotations included;
  // `dark`: the dark page (darkenPage over the region, photographs kept — see images()); `rot`: view rotation in
  // quarter turns clockwise — the region is measured on the rotated page (its width and height swap for 1 and 3)
  bool render(int page, double scale, int x, int y, int w, int h, Bitmap& out, bool withAnnots = true, bool dark = false, int rot = 0);
  // the images drawn on the page (form XObjects walked), each classified once by a small render (cached per page)
  bool images(int page, vector<ImageBox>& out);
  bool text(int page, TextPage& out);
  bool links(int page, vector<Link>& out);
  bool find(int page, const std::u16string& needle, bool matchCase, bool wholeWord, vector<Hit>& out, TextPage* cached = nullptr);
  bool readAnnots(int page, vector<FileAnnot>& out);
  // writes the annotations into the page (appearance streams generated by rendering once) — call saveAs after
  bool writeAnnots(int page, const vector<FileAnnot>& annots, string* err = nullptr);
  int removeAnnots(int page, const std::function<bool(const FileAnnot&)>& which);
  bool saveAs(const string& path, string* err = nullptr);  // a copy; the open document keeps reading the original
  string pageLabel(int page) const { return page >= 0 && page < int(info_.labels.size()) ? info_.labels[size_t(page)] : std::to_string(page + 1); }
  void dropPages();  // release the page cache (memory)

 private:
  struct Aff { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };  // x' = a x + c y + e ; y' = b x + d y + f
  struct PageH { int idx = -1; void* page = nullptr; void* tp = nullptr; float w = 0, h = 0; Aff toDisplay, toUser; vector<ImageBox> images; bool imagesDone = false; };
  PageH* page(int idx, bool needText);
  void closePage(PageH& p);
  bool load(string* err, const string& password);
  void readInfo();
  Box toDisplay(const PageH& p, double x0, double y0, double x1, double y1) const;

  void* doc_ = nullptr;
  string path_, bytes_;
  FILE* file_ = nullptr;
  unsigned long fileLen_ = 0;
  struct Access;  // FPDF_FILEACCESS
  std::unique_ptr<Access> access_;
  DocInfo info_;
  vector<PageH> pages_;  // small LRU (front = most recent)
  bool needsPassword_ = false;
  static int getBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size);
};

// The engine thread. Tasks run in priority order (0 first), FIFO within a priority; a task that finds its handle
// cancelled should return at once. `post` delivers a closure to the UI thread (set before start).
class Worker {
 public:
  using Task = std::function<void(Worker&)>;
  struct Handle {
    std::shared_ptr<std::atomic<bool>> flag;
    void cancel() { if (flag) *flag = true; }
    bool cancelled() const { return flag && *flag; }
  };
  ~Worker() { stop(); }
  void start(std::function<void(std::function<void()>)> post);
  void stop();
  bool running() const { return running_; }
  Handle submit(int priority, Task task);
  void toUi(std::function<void()> fn) { if (post_) post_(std::move(fn)); }
  size_t pending() const;
  bool idle() const;  // nothing queued and no task running
  Handle current() const { return current_; }  // the handle of the task now running (inside a task: its own)

  // documents live on the worker thread; ids are handed out on the UI thread
  int newDocId() { return nextDoc_++; }
  Document& doc(int id);              // worker thread only (creates on first use)
  void closeDoc(int id);              // worker thread only
  bool hasDoc(int id) const { return docs_.count(id) > 0; }
  size_t openDocs() const { return docs_.size(); }
  // worker thread only, inside a task: gives the engine's own memory back (its allocator only returns it when the
  // library is destroyed); nothing happens while a document is open; the next task initialises the library again
  bool releaseLibrary();
  bool libraryLoaded() const { return lib_; }

 private:
  struct Item { int prio; uint64_t seq; Task task; std::shared_ptr<std::atomic<bool>> flag; };
  void run();
  std::thread th_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::multimap<std::pair<int, uint64_t>, Item> queue_;
  uint64_t seq_ = 0;
  bool stop_ = false;
  std::atomic<bool> running_{false};
  std::function<void(std::function<void()>)> post_;
  Handle current_;
  std::atomic<bool> busy_{false};
  std::map<int, std::unique_ptr<Document>> docs_;
  int nextDoc_ = 1;
  std::atomic<bool> lib_{false};
  void ensureLib();
};

// small helpers shared with the UI
string utf16to8(const std::u16string& s);
std::u16string utf8to16(const string& s);
Box unionBox(const Box& a, const Box& b);

}  // namespace pdf
}  // namespace vs
