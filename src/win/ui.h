// VOSStudio Native — immediate-mode UI toolkit on Direct2D/DirectWrite
#pragma once
#include <unordered_map>
#include <array>
#include "netview.h"

namespace vs {
namespace win {

struct UiColors {
  Color bg, rail, panel, panel2, card, border, borderStrong, text, textDim, textFaint, accent, accentHover, accentText, hover, active, input, danger, warn, ok, shadow, sel;
};

struct Input {
  float mx = -1, my = -1;
  bool down[3] = {false, false, false}, pressed[3] = {false, false, false}, released[3] = {false, false, false};
  bool dbl = false;
  bool touch = false;       // this frame came from a Windows touch contact
  bool touchCancel = false; // a second contact cancelled the primary one-finger tap/drag
  float pinch = 0;           // pinch zoom, in wheel-like notches (positive = zoom in)
  float wheel = 0, hwheel = 0;  // notches; hwheel > 0 = right (a touchpad's sideways swipe, a tilt wheel)
  std::u32string chars;
  vector<int> keys;  // VK codes pressed this frame
  bool ctrl = false, shift = false, alt = false;
  bool key(int vk) const { return std::find(keys.begin(), keys.end(), vk) != keys.end(); }
};

enum Align { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };
enum BtnKind { BTN_NORMAL = 0, BTN_PRIMARY = 1, BTN_GHOST = 2, BTN_DANGER = 3 };

struct Toast { string title, msg; int kind = 0; double t0 = 0, dur = 4; };

class Ui {
 public:
  Gfx* g = nullptr;
  HWND hwnd = nullptr;
  float s = 1;  // DPI scale
  bool dark = true;
  UiColors c;
  Input in;
  double time = 0;
  bool animating = false;   // request another frame
  uint64_t hot = 0, active = 0, focus = 0;
  string cursor = "arrow";

  void init(Gfx& gfx, HWND h);
  void setTheme(bool darkTheme);
  static UiColors palette(bool darkTheme);  // the colours of either theme (the theme reveal paints with the old one)
  // partial = an overlay-only frame (the Live pop-up redrawn over the cached window): the base layer is not
  // drawn, so the caption hit test keeps the widget rects of the last full frame (see widgetAt)
  void beginFrame(const Input& input, double t, bool partial = false);
  void fullFrameAfterAll() { partial_ = false; hitRects_.clear(); }  // a frame begun as partial is drawn in full after all
  void endFrame();  // draws popups, tooltips, toasts
  uint64_t hoverTargetAt(float x, float y) const;  // last-frame hit map; lets WM_MOUSEMOVE skip unchanged hover targets
  double wakeAt = 0;  // next one-shot UI wake (e.g. delayed tooltip); 0 means none

  // drawing
  ID2D1DeviceContext* dc() const { return g->dc.get(); }
  void fill(const Rect& r, const Color& col, float radius = 0);
  void stroke(const Rect& r, const Color& col, float radius = 0, float w = 1);
  void line(float x0, float y0, float x1, float y1, const Color& col, float w = 1);
  void circle(float cx, float cy, float r, const Color& col, bool filled = true, float w = 1);
  float textW(const string& t, float size, int weight = 400, bool mono = false);
  void text(const Rect& r, const string& t, float size, const Color& col, Align al = AL_LEFT, int weight = 400, bool mono = false);
  float textWrap(const Rect& r, const string& t, float size, const Color& col, int weight = 400, bool draw = true);  // returns height
  void icon(const string& name, float cx, float cy, float size, const Color& col, float strokeW = 1.8f);
  void shadow(const Rect& r, float radius, float spread = 10);
  void pushClip(const Rect& r);
  void popClip();
  Rect clipRect() const { return clips_.empty() ? Rect{0, 0, 1e6f, 1e6f} : clips_.back(); }

  // ids
  uint64_t id(const string& s) const;
  void pushId(const string& s);
  void popId();

  // interaction
  bool mouseIn(const Rect& r) const;
  bool behave(uint64_t id, const Rect& r, bool* hovered = nullptr, bool* held = nullptr);  // returns clicked
  void tip(const string& t) { if (hotNext_ == lastId_) tipNext_ = t; }
  void tipFor(uint64_t idv, const string& t) { if (hotNext_ == idv) tipNext_ = t; }
  // immediate tooltip card at the mouse (first line bold) — chart data points, map hover
  bool wantsCaret = false;  // a text field has focus this frame (caret blink)
  void richTip(const string& t) { richTip_ = t; richBold_ = true; }
  // "?" help icon: small circled question mark; hovering shows `text` at once. Returns true while hovered.
  bool help(float x, float cy, const string& msg);
  // label -> help text registry: toggle / checkbox / slider / field labels with an entry get a "?" icon automatically
  std::unordered_map<string, string> helpTexts;
  const string* helpOf(const string& label) const { auto it = helpTexts.find(label); return it == helpTexts.end() ? nullptr : &it->second; }
  // draws the "?" after a label drawn at x with the given size; returns true while hovered
  bool helpAfterLabel(float x, float cy, const string& label, float size);

  // widgets
  bool button(const Rect& r, const string& label, int kind = BTN_NORMAL, const string& iconName = "", bool enabled = true);
  bool iconButton(const Rect& r, const string& iconName, const string& tooltip, bool toggled = false, bool enabled = true);
  bool checkbox(const Rect& r, const string& label, bool& v);
  bool toggle(const Rect& r, const string& label, bool& v);
  bool slider(const Rect& r, const string& label, float& v, float lo, float hi, const char* fmt = "%.2f", float step = 0);
  bool sliderInt(const Rect& r, const string& label, int& v, int lo, int hi);
  bool segmented(const Rect& r, const vector<string>& opts, int& sel, const string& idKey = "");
  bool combo(const Rect& r, const string& key, const vector<string>& opts, int& sel);
  bool textInput(const Rect& r, const string& key, string& buf, const string& placeholder = "", bool* submitted = nullptr, const string& iconName = "");
  // Multi-line, wrapping text box. Enter submits (Shift+Enter inserts a line break). Returns true when the text changed.
  // *contentH receives the height the text needs (to grow the box up to a maximum).
  bool textArea(const Rect& r, const string& key, string& buf, const string& placeholder, bool* submitted, float* contentH, float fontSize = 13.5f);
  // Wrapped text with inline Markdown: **bold**, *italic*, `code`. Returns the height.
  float richText(const Rect& r, const string& md, float size, const Color& col, int weight = 400, bool draw = true, float lineSpacing = 1.35f);
  void scrollSet(const string& key, float y);  // jump a root-level scroll area (no animation); y is clamped
  float scrollGet(const string& key) const;   // current offset of a root-level scroll area (0 when unknown)
  bool numberInput(const Rect& r, const string& key, int& v, int lo, int hi, int step = 1);
  bool numberInputD(const Rect& r, const string& key, double& v, double lo, double hi, double step, int dec);
  bool header(const Rect& r, const string& title, bool& open, const string& badge = "");
  bool colorSwatch(const Rect& r, const string& key, Color& col);
  void label(const Rect& r, const string& t, bool dim = true) { text(r, t, 12.f * s, dim ? c.textDim : c.text); }
  void progress(const Rect& r, double p, const Color& col);
  bool tab(const Rect& r, const string& label, bool selected);
  bool listRow(const Rect& r, const string& key, bool selected);

  // popups (drawn at end of frame, above everything)
  void openPopup(const string& key) { popup_ = id(key); popupOpenedAt_ = time; }
  bool isPopupOpen(const string& key) const { return popup_ == id(key); }
  void closePopup() { popup_ = 0; }
  void overlay(std::function<void()> fn) { overlays_.push_back({std::move(fn), modalLayer_}); }
  // modal layer (dialogs such as Settings): separate from the transient popup slot, so a dialog can host
  // combos, colour pickers and menus. While a modal is open, only widgets drawn inside the modal layer react.
  void openModal(const string& key) { modal_ = id(key); popup_ = 0; focus = 0; }
  bool isModalOpen(const string& key) const { return modal_ == id(key); }
  void closeModal() { modal_ = 0; popup_ = 0; focus = 0; }
  bool anyModal() const { return modal_ != 0; }
  void beginModalLayer() { modalLayer_ = true; }
  void endModalLayer() { modalLayer_ = false; }
  bool maskNext = false;  // the next textInput shows its text masked while not being edited (API keys)
  void popupRect(const Rect& r) { popupRectNext_ = r; }  // mark area owned by the popup this frame
  bool inOverlay() const { return inOverlay_; }
  bool hasClip() const { return !clips_.empty(); }
  Rect activeClip() const { return clips_.empty() ? Rect{} : clips_.back(); }
  // a floating panel (the Live AI pop-up) owns this area for the rest of the frame: base-layer widgets, scroll areas
  // and the canvas under it ignore the mouse; the panel calls unblock() before drawing its own widgets
  void block(const Rect& r) { blockRect_ = r; }
  void unblock() { blockRect_ = {-1, -1, 0, 0}; }
  bool blocked(float x, float y) const { return !inOverlay_ && blockRect_.w > 0 && blockRect_.has(x, y); }
  bool anyPopup() const { return popup_ != 0; }
  bool tipShowing() const { return !tipNext_.empty() && hot && !in.down[0] && time - hotSince_ > 0.45; }  // a tooltip is drawn at the end of this frame

  // scroll areas
  void beginScroll(const string& key, const Rect& r);
  void endScroll(float contentH);
  void addHitRect(const Rect& r) { hitRects_.push_back(r); }
  // true when the last frame placed an interactive widget at (x, y) outside any scroll area
  bool widgetAt(float x, float y) const {
    for (auto& r : hitPrev_) if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return true;
    return false;
  }
  float scrollY() const { return scrollStack_.empty() ? 0 : scrollStack_.back().y; }
  void scrollTo(const string& key, float y);

  // toasts
  void toast(const string& title, const string& msg, int kind = 0, double dur = 4);
  vector<Toast> toasts;

  // text editing state (public so the app can know whether a field has focus)
  bool editing() const { return focus != 0; }
  void focusText(uint64_t idv, const string& current);  // give a text input the caret (with its current text) from outside

 private:
  struct HoverHit { uint64_t id = 0; Rect r, clip; bool clipped = false, overlay = false, modal = false; };
  vector<HoverHit> hoverHits_, hoverBase_;
  void recordHover(uint64_t id, const Rect& r);
  bool touchHit(uint64_t id, const Rect& r) const;  // expands sparse targets without stealing a neighbouring control
  vector<uint64_t> idStack_;
  vector<Rect> clips_;
  uint64_t lastId_ = 0, hotNext_ = 0, popup_ = 0, touchTargetId_ = 0;
  double popupOpenedAt_ = 0;
  vector<std::pair<std::function<void()>, bool>> overlays_;  // (draw fn, queued from the modal layer)
  uint64_t modal_ = 0;
  bool modalLayer_ = false;
  bool inOverlay_ = false;
  Rect popupRectPrev_{-1, -1, 0, 0}, popupRectNext_{-1, -1, 0, 0}, blockRect_{-1, -1, 0, 0};
  string tipNext_, tipCur_, richTip_;
  bool richBold_ = false;
  uint64_t tipId_ = 0;
  double hotSince_ = 0;
  struct Scroll { uint64_t id; Rect r; float y; };
  vector<Scroll> scrollStack_;
  vector<Rect> hitRects_, hitPrev_;  // interactive rects of the root layer (window caption hit testing)
  vector<Rect> hitBase_;             // rects of the last full frame (kept through overlay-only frames)
  bool partial_ = false;
  uint64_t hotKeep_ = 0;
  string tipKeep_;
  std::map<uint64_t, std::pair<float, float>> scroll_;  // id -> (y, contentH)
  std::map<uint64_t, float> scrollTarget_;
  float wheelLeft_ = 0;
  uint64_t touchScrollId_ = 0;
  float touchScrollStartY_ = 0, touchScrollLastY_ = 0;
  bool touchScrollMoved_ = false;
  // editing
  std::u32string ebuf_;
  int caret_ = 0, anchor_ = 0;
  float escroll_ = 0;
  double caretT_ = 0;
  bool mouseSel_ = false;
  std::map<string, Com<ID2D1PathGeometry>> icons_;
  ID2D1PathGeometry* iconGeo(const string& name);
  Com<IDWriteTextLayout> mkLayout(const string& t, float size, int weight, bool mono, float maxW = 100000);
  // Text layout cache. DirectWrite shaping is the costly part of drawing a string; a panel draws hundreds per frame
  // and they hardly change, so finished layouts are kept for a while, keyed by text, font, box and alignment.
  struct TextEntry {
    Com<IDWriteTextLayout> L;
    float w = 0, h = 0;                       // metrics
    vector<std::array<float, 4>> boxes;       // rich text: code-span backgrounds (relative to the origin)
    uint32_t seen = 0;                        // frame of the last use
  };
  std::unordered_map<uint64_t, TextEntry> textCache_;
  std::map<uint64_t, Com<IDWriteInlineObject>> ellipsis_;  // trimming signs per format
  uint32_t frameNo_ = 0;
  TextEntry* textLookup(uint64_t key);      // nullptr when absent or the cache is off
  TextEntry& textStore(uint64_t key, TextEntry&& e);
  void textSweep();                          // drops layouts not used for a while
  static uint64_t textKey(int kind, const string& t, float size, int weight, bool mono, float w, float h, int extra);
  IDWriteInlineObject* ellipsisFor(float size, int weight, bool mono);
 public:
  bool textCacheOn = true;                   // setting "textCache" (script: textcache 0|1)
  uint32_t textHits = 0, textMisses = 0;     // this frame (performance overlay)
  uint32_t textHitsPrev = 0, textMissesPrev = 0;  // previous frame
  size_t textCacheSize() const { return textCache_.size(); }
  void textCacheClear() { textCache_.clear(); ellipsis_.clear(); }
};

// Vertical layout helper
struct Lay {
  float x = 0, y = 0, w = 0, gap = 6;
  Rect row(float h) { Rect r{x, y, w, h}; y += h + gap; return r; }
  void space(float h) { y += h; }
};
inline Rect splitL(const Rect& r, float w, float gap = 6) { return {r.x, r.y, w, r.h}; }
inline Rect splitR(const Rect& r, float w, float gap = 6) { (void)gap; return {r.x + r.w - w, r.y, w, r.h}; }
inline Rect inset(const Rect& r, float dx, float dy) { return {r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy}; }
inline vector<Rect> cols(const Rect& r, int n, float gap) {
  vector<Rect> v;
  float w = (r.w - gap * float(n - 1)) / float(n);
  for (int i = 0; i < n; i++) v.push_back({r.x + float(i) * (w + gap), r.y, w, r.h});
  return v;
}

std::u32string toU32(const string& s);
string fromU32(const std::u32string& s);

}  // namespace win
}  // namespace vs
