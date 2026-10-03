// The PDF reader's state (reader.cpp draws and drives it; readlib.cpp is the Read page and the library flows).
// Everything the engine produces is cached here per open document: page text (selection, search, quotes), links,
// a low-resolution preview per page and the tiles of the pages at the current scale (GPU bitmaps, LRU-bounded).
#pragma once
#include <map>
#include <set>
#include <unordered_map>

#include "../core/pdfdoc.h"
#include "app.h"
#include "pdfload.h"

namespace vs {
namespace win {

struct RdTileKey {
  int page = 0, k = 0, dark = 0, filter = 0, rot = 0, tx = 0, ty = 0;  // k = scale in 1/1000 px per point; filter: 0 original, 1 warm, 2 grayscale
  bool operator<(const RdTileKey& o) const { return std::tie(page, k, dark, filter, rot, tx, ty) < std::tie(o.page, o.k, o.dark, o.filter, o.rot, o.tx, o.ty); }
  bool operator==(const RdTileKey& o) const { return page == o.page && k == o.k && dark == o.dark && filter == o.filter && rot == o.rot && tx == o.tx && ty == o.ty; }
};
struct RdTile {
  Com<ID2D1Bitmap> bmp;
  int w = 0, h = 0;
  uint32_t used = 0;  // frame of the last draw
  double usedT = 0;   // time of the last draw (idle tiles go first)
};
struct RdPage {
  float w = 612, h = 792;         // points, as displayed
  pdf::TextPage text;
  bool textReq = false, textDone = false;
  vector<pdf::Link> links;
  bool linksDone = false;
  Com<ID2D1Bitmap> preview;       // whole page at a small size (under the tiles until they arrive; the thumbnails)
  int pw = 0, ph = 0;
  bool previewReq = false;
  int previewTargetW = 0;         // adaptive preview resolution requested for this viewport
  bool previewDark = false;       // rendered as the dark page
  int previewFilter = 0;          // non-inverting reading filter in the preview
  int previewRot = 0;             // rendered under this view rotation
  vector<pdf::FileAnnot> fileAnnots;  // annotations already inside the file (drawn by the engine; listed, not editable)
  bool fileAnnotsDone = false;
};
struct RdDoc {
  int id = -1;                    // Worker document id
  string itemId, path;
  pdf::DocInfo info;
  vector<RdPage> pages;
  bool ready = false, failed = false, needsPassword = false;
  string error;
  uint32_t gen = 0;               // ReaderState::docGen at the open: results of an older document are dropped
};

struct ReaderState {
  pdf::Worker worker;
  bool workerStarted = false;
  bool engineTried = false, engineReady = false;
  PdfEngine engine;
  RdDoc doc;
  uint32_t docGen = 0;            // grows with every open, across documents and resets
  double closedT = 0;             // when the reader was last closed: the document is released after a while
  bool released = false;          // …and has been; the engine's own memory goes after a further while of quiet
  // tiles
  std::map<RdTileKey, RdTile> tiles;
  size_t tileBytes = 0;
  std::map<RdTileKey, pdf::Worker::Handle> tileReq;
  int tileK = 0, tileKPrev = 0;   // the scale the tiles are requested at, and the one before (drawn underneath meanwhile)
  // view
  float zoom = 0;                 // 0 = fit the width; else the scale relative to 100 % (96 dpi at the UI scale)
  bool dark = false;              // the dark page: pages rendered light-on-dark (a reading mode of the reader alone, kept in settings)
  int filterMode = 0;             // non-inverting reading appearance: 0 original, 1 warm paper, 2 grayscale
  bool darkLoaded = false;        // appearance, twoUp and coverAlone read from the settings once
  int rot = 0;                    // view rotation of this document, quarter turns clockwise (kept per PDF in the project)
  bool twoUp = false;             // two pages side by side (a setting of the reader)
  bool coverAlone = false;        // …with the first page alone on the right, as a book opens
  // the Print card
  int printPages = 0;             // 0 all, 1 the current page, 2 the range below
  string printRange;
  bool printMarks = true, printFit = true, printAutoRotate = true;
  float zoomShown = 0;            // eased
  double zoomT = 0, prevT = 0;
  float zoomAnchorX = -1, zoomAnchorY = -1;
  float scroll = 0, scrollTarget = 0, scrollX = 0;
  float maxScroll = 0, maxScrollX = 0;  // of the last frame (keys)
  int barDrag = 0;                // 1 = the vertical scrollbar's thumb is being dragged, 2 = the horizontal one
  float barGrab = 0;              // where in the thumb it was grabbed
  float kPrev = 0;
  float viewW = 0, viewH = 0;
  Rect pageArea{0, 0, 0, 0};
  int pendingPage = -1;           // navigate here once the document is ready
  float pendingY = -1;
  bool positionRestored = false;
  // selection (page + char range) and mouse gestures
  int selPage = -1, selA = -1, selB = -1;
  bool dragging = false, panning = false, selGesture = false;
  bool touchPanPending = false, touchPanMaySelect = false;
  float touchPanStartX = 0, touchPanStartY = 0;
  double touchPanStartT = 0;
  float panX = 0, panY = 0, panScroll = 0, panScrollX = 0;
  double clickT = 0;
  int clicks = 0;
  bool spaceDown = false;
  // mini toolbar / context
  bool miniShow = false;
  float miniX = 0, miniY = 0;
  Rect miniR{0, 0, 0, 0};
  float ctxX = 0, ctxY = 0;
  string ctxAnnot;                // the annotation the context menu is about ("" = the selection)
  int ctxPage = -1;               // the page under the right-click (and the point, in page points)
  float ctxPX = 0, ctxPY = 0;
  // annotations; the comment card floats next to the annotation it edits (its text is saved as it is typed)
  string hoverAnnot, curAnnot, cardAnnot, noteBuf, tagBuf;
  // area annotations (scanned pages, figures, tables): the Area tool, or Alt + drag, draws a rectangle on a page
  bool areaTool = false, areaDrag = false;
  int areaPage = -1;
  float areaX0 = 0, areaY0 = 0, areaX1 = 0, areaY1 = 0;  // page points
  // freehand Ink: points stay in unrotated page points so view rotation/zoom and PDF export use one stable geometry
  bool inkTool = false, inkDrag = false;
  int inkPage = -1;
  uint32_t inkColor = 0xFF1D4ED8;
  float inkWidth = 1.8f;
  vector<pdf::InkPoint> inkPoints;
  struct InkUndoAction { string id; bool created = false, inFile = false; vector<pdf::InkStroke> strokes; pdf::Box rect; };
  vector<InkUndoAction> inkUndo;  // creation and geometry edits; Ctrl+Z reverses the latest drawing action
  bool inkMove = false, inkMoveDragged = false;
  string inkMoveId;
  int inkMovePage = -1;
  float inkMoveX = 0, inkMoveY = 0, inkMoveScreenX = 0, inkMoveScreenY = 0;
  vector<pdf::InkStroke> inkMoveOrig;
  pdf::Box inkMoveOrigRect;
  bool inkMoveOrigInFile = false;
  string miniArea;                // the mini toolbar is about this area annotation (else about the text selection)
  bool cardFocus = false;         // give the card's text field the caret when it is next drawn
  Rect cardR{0, 0, 0, 0};
  // panes: 0 hidden, 1 notes, 2 marks, 3 find, 4 outline, 5 info
  int pane = 2;
  string findQ, findLast;
  bool findCase = false, findWord = false, findFocus = false;
  vector<pdf::Hit> hits;
  int hitCur = -1, findGen = 0, findPages = 0;
  bool findRunning = false;
  string pageBuf;                 // the "page n" box
  string linkQuery;               // record picker
  bool linkPick = false;
  int markFilter = 0;             // the Marks pane: 0 all, else Code
  bool libSel = false;            // the Read page: multi-select mode
  std::set<string> libSelected;
  string libQuery;                // the Read page's search box
  string paperTagCacheKey;
  uint64_t paperTagCacheVersion = 0;
  vector<string> paperTagCache;
  int libFilter = 0;              // 0 all, 1 to read, 2 reading, 3 read, 4 excluded
  int libSort = 0;                // 0 added, 1 title, 2 year, 3 status
  string libMenuItem;             // item the Read page's context menu is about
  float libMenuX = 0, libMenuY = 0;
  string statusLine;
  double statusT = 0;
  // long lists (outline, highlights): row tops measured once per list/width/scale, only the visible rows are drawn
  struct Rows { uint64_t sig = 0; vector<float> top; };  // top[i] = offset of row i, top.back() = total height
  Rows olRows, markRows;
  std::unordered_map<string, std::pair<bool, double>> exists;  // file existence, re-checked after a few seconds
  // page geometry cache for this frame
  vector<float> pageTop;          // screen y of every page's top (at the shown scale), pageArea-relative
  vector<float> pageLeft;         // x of every page's left edge, relative to the page column's left edge
  float kShown = 1;               // px per point drawn this frame
  float colWShown = 1;            // page-column width at kShown; cached for pointer hit testing
  string password;
  bool passwordFocus = false;
};

Color rdCodeColor(uint32_t argb);

}  // namespace win
}  // namespace vs
