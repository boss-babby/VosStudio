#include "ui.h"

#include <cstdio>

namespace vs {
namespace win {

std::u32string toU32(const string& s) {
  std::u32string o;
  for (size_t i = 0; i < s.size();) {
    uint8_t c = uint8_t(s[i]);
    char32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 31; n = 2; } else if ((c >> 4) == 14) { cp = c & 15; n = 3; } else { cp = c & 7; n = 4; }
    for (int k = 1; k < n && i + size_t(k) < s.size(); k++) cp = (cp << 6) | (uint8_t(s[i + size_t(k)]) & 63);
    o.push_back(cp);
    i += size_t(n);
  }
  return o;
}
string fromU32(const std::u32string& s) {
  string o;
  for (char32_t cp : s) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 63)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
  }
  return o;
}

void Ui::init(Gfx& gfx, HWND h) {
  g = &gfx;
  hwnd = h;
  setTheme(true);
}

void Ui::setTheme(bool d) {
  dark = d;
  c = palette(d);
}

UiColors Ui::palette(bool d) {
  // Neutral desktop palette: grey chrome, white content, hairline separators, one accent used sparingly.
  UiColors c;
  if (d) {
    c.bg = Color::hex(0x1e1e1e); c.rail = Color::hex(0x2a2a2a); c.panel = Color::hex(0x252525); c.panel2 = Color::hex(0x3a3a3c); c.card = Color::hex(0x2c2c2e);
    c.border = Color::hex(0x3a3a3a); c.borderStrong = Color::hex(0x4d4d50); c.text = Color::hex(0xf2f2f4); c.textDim = Color::hex(0xa1a1a6); c.textFaint = Color::hex(0x6e6e73);
    c.accent = Color::hex(0x0a84ff); c.accentHover = Color::hex(0x3396ff); c.accentText = Color::hex(0xffffff); c.hover = Color(1, 1, 1, 0.06f); c.active = Color(1, 1, 1, 0.1f);
    c.input = Color::hex(0x1c1c1e); c.danger = Color::hex(0xff453a); c.warn = Color::hex(0xff9f0a); c.ok = Color::hex(0x32d74b); c.shadow = Color(0, 0, 0, 0.5f);
    c.sel = Color(0.04f, 0.52f, 1.f, 0.24f);
  } else {
    c.bg = Color::hex(0xffffff); c.rail = Color::hex(0xf0f0f0); c.panel = Color::hex(0xf7f7f7); c.panel2 = Color::hex(0xffffff); c.card = Color::hex(0xffffff);
    c.border = Color::hex(0xdedede); c.borderStrong = Color::hex(0xc6c6c8); c.text = Color::hex(0x1d1d1f); c.textDim = Color::hex(0x6e6e73); c.textFaint = Color::hex(0xa1a1a6);
    c.accent = Color::hex(0x007aff); c.accentHover = Color::hex(0x0066d6); c.accentText = Color::hex(0xffffff); c.hover = Color(0, 0, 0, 0.045f); c.active = Color(0, 0, 0, 0.08f);
    c.input = Color::hex(0xffffff); c.danger = Color::hex(0xd70015); c.warn = Color::hex(0xb25000); c.ok = Color::hex(0x248a3d); c.shadow = Color(0, 0, 0, 0.14f);
    c.sel = Color(0.f, 0.48f, 1.f, 0.13f);
  }
  return c;
}

void Ui::beginFrame(const Input& input, double t, bool partial) {
  in = input;
  time = t;
  frameNo_++;
  textHitsPrev = textHits;
  textMissesPrev = textMisses;
  textHits = textMisses = 0;
  if (frameNo_ % 120 == 0) textSweep();
  partial_ = partial;
  wakeAt = 0;
  if (partial) {
    // only a floating layer is redrawn: the top bar's widgets are still where the last full frame put them, so
    // WM_NCHITTEST must keep seeing the widget rects from the last full frame.
    hitPrev_ = hitRects_;
    hitRects_ = hitBase_;
    hoverHits_ = hoverBase_;
  } else {
    hitPrev_.swap(hitRects_);
    hitRects_.clear();
    hoverHits_.clear();
  }
  animating = false;
  wantsCaret = false;
  // a partial frame happens only while the pointer rests: the hovered base widget, its tooltip and the cursor stay
  // as the last full frame left them unless a widget of the floating layer takes over
  if (!partial) cursor = "arrow";
  if (!partial || hotNext_) { if (hotNext_ != hot) { hot = hotNext_; hotSince_ = t; } }
  hotKeep_ = hot;
  hotNext_ = 0;
  overlays_.clear();
  popupRectPrev_ = popupRectNext_;
  popupRectNext_ = {-1, -1, 0, 0};
  blockRect_ = {-1, -1, 0, 0};
  if (!popup_) popupRectPrev_ = {-1, -1, 0, 0};
  touchTargetId_ = 0;
  if (in.touch) {
    const bool inPopup = (popupRectPrev_.w > 0 && popupRectPrev_.has(in.mx, in.my)) || (popupRectNext_.w > 0 && popupRectNext_.has(in.mx, in.my));
    for (auto it = hoverBase_.rbegin(); it != hoverBase_.rend(); ++it) {
      if (modal_ && !it->modal) continue;
      if (inPopup && !it->overlay) continue;
      if (it->clipped && !it->clip.has(in.mx, in.my)) continue;
      if (it->r.has(in.mx, in.my)) { touchTargetId_ = it->id; break; }
    }
  }
  tipKeep_ = partial ? tipNext_ : string();
  tipNext_.clear();
  if (!partial) richTip_.clear();
  wheelLeft_ = in.wheel;
  idStack_.clear();
  clips_.clear();
  if (in.released[0] && !in.down[0]) {
    // release clears active at end of frame (so widgets can see the release first)
  }
}

void Ui::endFrame() {
  if (!partial_) { hitBase_ = hitRects_; hoverBase_ = hoverHits_; }  // remembered for overlay-only frames
  inOverlay_ = true;
  // overlays may queue further overlays (a combo inside a dialog): run passes until the queue is empty
  for (int pass = 0; pass < 4 && !overlays_.empty(); pass++) {
    auto ov = std::move(overlays_);
    overlays_.clear();
    for (auto& f : ov) { modalLayer_ = f.second; f.first(); }
  }
  modalLayer_ = false;
  inOverlay_ = false;
  // close popups on outside click / escape
  if (popup_ && time - popupOpenedAt_ > 0.05) {
    bool inside = popupRectNext_.w > 0 && popupRectNext_.has(in.mx, in.my);
    if ((in.pressed[0] || in.pressed[1]) && !inside) popup_ = 0;
    if (in.key(VK_ESCAPE)) popup_ = 0;
  }
  // Commit this frame's hit target once. Hover feedback is already drawn from the current pointer; storing it now
  // avoids a second frame solely to discover the same target, and starts the delayed-tooltip timer accurately.
  if (!partial_ || hotNext_) {
    if (hotNext_ != hot) { hot = hotNext_; hotSince_ = time; }
  }
  // immediate rich tooltip (charts, help icons)
  if (!richTip_.empty()) {
    vector<string> lines;
    size_t a = 0;
    while (true) { size_t b = richTip_.find('\n', a); lines.push_back(richTip_.substr(a, b == string::npos ? string::npos : b - a)); if (b == string::npos) break; a = b + 1; }
    float fs = 12 * s, maxW = 340 * s, tw = 0;
    for (size_t i = 0; i < lines.size(); i++) tw = std::max(tw, textW(lines[i], fs, (richBold_ && i == 0) ? 650 : 400));
    tw = std::min(tw + 20 * s, maxW);
    float th = 8 * s;
    vector<float> lh;
    for (size_t i = 0; i < lines.size(); i++) { lh.push_back(textWrap({0, 0, tw - 20 * s, 1000}, lines[i], fs, c.text, (richBold_ && i == 0) ? 650 : 400, false) + 2 * s); th += lh.back(); }
    th += 4 * s;
    float x = in.mx + 14 * s, y = in.my + 18 * s;
    if (x + tw > g->W - 4) x = in.mx - tw - 10 * s;
    if (y + th > g->H - 4) y = in.my - th - 10 * s;
    x = std::max(4.f, x); y = std::max(4.f, y);
    Rect r{x, y, tw, th};
    shadow(r, 7 * s, 10 * s);
    fill(r, dark ? Color::hex(0x252a33) : Color::hex(0xffffff), 7 * s);
    stroke(r, c.borderStrong, 7 * s);
    float yy = y + 6 * s;
    for (size_t i = 0; i < lines.size(); i++) {
      bool b = richBold_ && i == 0;
      textWrap({x + 10 * s, yy, tw - 20 * s, lh[i]}, lines[i], fs, b ? c.text : c.textDim, b ? 650 : 400, true);
      yy += lh[i];
    }
    tipNext_.clear();
  }
  // tooltip
  if (partial_ && tipNext_.empty() && hot == hotKeep_) tipNext_ = tipKeep_;  // base-layer tooltip survives overlay-only frames
  if (!tipNext_.empty() && hot && !in.down[0]) {
    if (time - hotSince_ > 0.45) {
      float tw = std::min(textW(tipNext_, 12 * s) + 16 * s, 320 * s);
      float th = textWrap({0, 0, tw - 16 * s, 1000}, tipNext_, 12 * s, c.text, 400, false) + 10 * s;
      float x = clampv(in.mx + 12 * s, 4.f, float(g->W) - tw - 4), y = in.my + 20 * s;
      if (y + th > g->H - 4) y = in.my - th - 8 * s;
      Rect r{x, y, tw, th};
      shadow(r, 6 * s, 8 * s);
      fill(r, dark ? Color::hex(0x2a303a) : Color::hex(0x1f232a), 6 * s);
      textWrap({x + 8 * s, y + 5 * s, tw - 16 * s, th}, tipNext_, 12 * s, Color::hex(0xf2f4f8), 400, true);
    } else wakeAt = hotSince_ + 0.45;
  }
  // toasts (bottom-right)
  float y = float(g->H) - 44 * s;
  for (size_t i = toasts.size(); i-- > 0;) {
    Toast& t = toasts[i];
    double age = time - t.t0;
    if (age > t.dur) { toasts.erase(toasts.begin() + long(i)); continue; }
    animating = true;
    float a = float(std::min(1.0, std::min(age / 0.18, (t.dur - age) / 0.3)));
    float w = 340 * s;
    float mh = t.msg.empty() ? 0 : textWrap({0, 0, w - 50 * s, 1000}, t.msg, 12 * s, c.textDim, 400, false);
    float h = 30 * s + mh + (t.msg.empty() ? 0 : 4 * s);
    float x = float(g->W) - w - 16 * s + (1 - a) * 30 * s;
    Rect r{x, y - h, w, h};
    Color sh = c.shadow; sh.a *= a;
    for (int k = 3; k >= 1; k--) fill({r.x - k * 1.5f * s, r.y - k * 0.5f * s, r.w + k * 3 * s, r.h + k * 3 * s}, sh.withA(sh.a * 0.12f), 12 * s + k * 1.5f * s);
    fill(r, (dark ? Color::hex(0x2c2c2e) : Color::hex(0xfbfbfb)).withA(0.98f * a), 11 * s);
    stroke(r, (dark ? Color(1, 1, 1, 0.12f) : Color(0, 0, 0, 0.1f)).withA(a * (dark ? 0.12f : 0.1f)), 11 * s);
    Color kc = t.kind == 1 ? c.ok : (t.kind == 2 ? c.warn : (t.kind == 3 ? c.danger : c.accent));
    icon(t.kind == 1 ? "check" : (t.kind >= 2 ? "warn" : "info"), r.x + 20 * s, r.y + 16 * s, 14 * s, kc.withA(a), 2.f);
    text({r.x + 36 * s, r.y + 6 * s, w - 48 * s, 20 * s}, t.title, 12.5f * s, c.text.withA(a), AL_LEFT, 600);
    if (!t.msg.empty()) textWrap({r.x + 36 * s, r.y + 26 * s, w - 50 * s, mh}, t.msg, 12 * s, c.textDim.withA(a));
    y -= h + 8 * s;
  }
  if (!in.down[0] && !in.down[1] && !in.down[2]) active = 0;
  if (in.touchCancel || (in.touch && in.released[0] && !in.down[0])) {
    touchScrollId_ = 0;
    touchScrollMoved_ = false;
  }
  if (in.pressed[0] && focus && !mouseSel_) {
    // clicking outside a focused text box blurs it (the box re-claims focus on its own click)
  }
}

// ------------------------------------------------------------ drawing
void Ui::fill(const Rect& r, const Color& col, float radius) {
  if (col.a <= 0.001f) return;
  auto* b = g->br(col);
  if (radius > 0) dc()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.r(), r.b()), radius, radius), b);
  else dc()->FillRectangle(D2D1::RectF(r.x, r.y, r.r(), r.b()), b);
}
void Ui::stroke(const Rect& r, const Color& col, float radius, float w) {
  auto* b = g->br(col);
  D2D1_RECT_F rr = D2D1::RectF(r.x + w / 2, r.y + w / 2, r.r() - w / 2, r.b() - w / 2);
  if (radius > 0) dc()->DrawRoundedRectangle(D2D1::RoundedRect(rr, radius, radius), b, w);
  else dc()->DrawRectangle(rr, b, w);
}
void Ui::line(float x0, float y0, float x1, float y1, const Color& col, float w) { dc()->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), g->br(col), w); }
void Ui::circle(float cx, float cy, float r, const Color& col, bool filled, float w) {
  auto e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
  if (filled) dc()->FillEllipse(e, g->br(col));
  else dc()->DrawEllipse(e, g->br(col), w);
}
void Ui::shadow(const Rect& r, float radius, float spread) {
  for (int k = 4; k >= 1; k--) {
    float e = spread * float(k) / 4;
    Color sc = c.shadow;
    sc.a *= 0.1f;
    fill({r.x - e * 0.6f, r.y - e * 0.3f, r.w + e * 1.2f, r.h + e * 1.2f}, sc, radius + e);
  }
}
void Ui::pushClip(const Rect& r) {
  Rect cr = r;
  if (!clips_.empty()) {
    const Rect& p = clips_.back();
    float x0 = std::max(cr.x, p.x), y0 = std::max(cr.y, p.y), x1 = std::min(cr.r(), p.r()), y1 = std::min(cr.b(), p.b());
    cr = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
  }
  clips_.push_back(cr);
  dc()->PushAxisAlignedClip(D2D1::RectF(cr.x, cr.y, cr.x + cr.w, cr.y + cr.h), D2D1_ANTIALIAS_MODE_ALIASED);  // the intersection: never inverted, even fully outside the parent
}
void Ui::popClip() {
  if (clips_.empty()) return;
  clips_.pop_back();
  dc()->PopAxisAlignedClip();
}

Com<IDWriteTextLayout> Ui::mkLayout(const string& t, float size, int weight, bool mono, float maxW) {
  std::wstring w = widen(t);
  Com<IDWriteTextLayout> L;
  g->dw->CreateTextLayout(w.c_str(), UINT32(w.size()), g->format(size, weight, mono), maxW, 10000, L.put());
  return L;
}

// ---- layout cache
uint64_t Ui::textKey(int kind, const string& t, float size, int weight, bool mono, float w, float h, int extra) {
  uint64_t hh = 1469598103934665603ull ^ uint64_t(kind);
  auto mix = [&](uint64_t v) { hh ^= v; hh *= 1099511628211ull; hh ^= hh >> 31; };
  for (unsigned char ch : t) { hh ^= ch; hh *= 1099511628211ull; }
  mix(uint64_t(std::lround(size * 100)));
  mix(uint64_t(weight));
  mix(mono ? 1 : 0);
  mix(uint64_t(int64_t(std::lround(w * 4))) + 0x100000);   // quarter pixels
  mix(uint64_t(int64_t(std::lround(h * 4))) + 0x100000);
  mix(uint64_t(extra) + 0x1000);
  return hh ? hh : 1;
}
Ui::TextEntry* Ui::textLookup(uint64_t key) {
  if (!textCacheOn) return nullptr;
  auto it = textCache_.find(key);
  if (it == textCache_.end()) return nullptr;
  it->second.seen = frameNo_;
  textHits++;
  return &it->second;
}
Ui::TextEntry& Ui::textStore(uint64_t key, TextEntry&& e) {
  textMisses++;
  e.seen = frameNo_;
  if (!textCacheOn) { static TextEntry scratch; scratch = std::move(e); return scratch; }
  if (textCache_.size() > 3000) textSweep();
  if (textCache_.size() > 3000) textCache_.clear();  // pathological churn: start again rather than grow
  return textCache_[key] = std::move(e);
}
void Ui::textSweep() {
  for (auto it = textCache_.begin(); it != textCache_.end();) it = frameNo_ - it->second.seen > 90 ? textCache_.erase(it) : std::next(it);
}
IDWriteInlineObject* Ui::ellipsisFor(float size, int weight, bool mono) {
  uint64_t key = (uint64_t(std::lround(size * 100)) << 16) ^ (uint64_t(weight) << 1) ^ (mono ? 1 : 0);
  auto it = ellipsis_.find(key);
  if (it != ellipsis_.end()) return it->second.get();
  Com<IDWriteInlineObject> ell;
  g->dw->CreateEllipsisTrimmingSign(g->format(size, weight, mono), ell.put());
  ellipsis_[key] = ell;
  return ell.get();
}

float Ui::textW(const string& t, float size, int weight, bool mono) {
  if (t.empty()) return 0;
  uint64_t key = textKey(2, t, size, weight, mono, 0, 0, 0);
  if (TextEntry* e = textLookup(key)) return e->w;
  TextEntry e;
  e.L = mkLayout(t, size, weight, mono);
  if (!e.L) return 0;
  DWRITE_TEXT_METRICS m;
  e.L->GetMetrics(&m);
  e.w = m.widthIncludingTrailingWhitespace;
  e.h = m.height;
  return textStore(key, std::move(e)).w;
}
void Ui::text(const Rect& r, const string& t, float size, const Color& col, Align al, int weight, bool mono) {
  if (t.empty()) return;
  float bw = std::max(1.f, r.w), bh = std::max(1.f, r.h);
  uint64_t key = textKey(1, t, size, weight, mono, bw, bh, int(al));
  TextEntry* e = textLookup(key);
  if (!e) {
    TextEntry n;
    n.L = mkLayout(t, size, weight, mono, bw);
    if (!n.L) return;
    n.L->SetTextAlignment(al == AL_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : (al == AL_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING));
    n.L->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    n.L->SetMaxHeight(bh);
    DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    n.L->SetTrimming(&tr, ellipsisFor(size, weight, mono));
    e = &textStore(key, std::move(n));
  }
  dc()->DrawTextLayout(D2D1::Point2F(r.x, r.y), e->L.get(), g->br(col), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
float Ui::textWrap(const Rect& r, const string& t, float size, const Color& col, int weight, bool draw) {
  if (t.empty()) return 0;
  float bw = std::max(10.f, r.w);
  uint64_t key = textKey(3, t, size, weight, false, bw, 0, 0);
  TextEntry* e = textLookup(key);
  if (!e) {
    TextEntry n;
    std::wstring w = widen(t);
    IDWriteTextFormat* f = g->format(size, weight, false, false, true);
    if (!f) return 0;
    g->dw->CreateTextLayout(w.c_str(), UINT32(w.size()), f, bw, 100000, n.L.put());
    if (!n.L) return 0;
    DWRITE_TEXT_METRICS m;
    n.L->GetMetrics(&m);
    n.w = m.widthIncludingTrailingWhitespace;
    n.h = m.height;
    e = &textStore(key, std::move(n));
  }
  if (draw) dc()->DrawTextLayout(D2D1::Point2F(r.x, r.y), e->L.get(), g->br(col));
  return e->h;
}


// ------------------------------------------------------------ rich text (inline Markdown)
namespace {
struct RichRun { UINT32 a, n; int kind; };  // kind: 1 bold, 2 italic, 3 code
// strips **, *, _ and ` markers and returns the plain UTF-16 text with style ranges
std::wstring richParse(const string& md, vector<RichRun>& runs) {
  std::wstring src = widen(md), out;
  out.reserve(src.size());
  bool bold = false, ital = false, code = false;
  UINT32 bA = 0, iA = 0, cA = 0;
  for (size_t i = 0; i < src.size(); i++) {
    wchar_t ch = src[i];
    if (ch == L'`') {
      if (code) { runs.push_back({cA, UINT32(out.size()) - cA, 3}); code = false; }
      else if (src.find(L'`', i + 1) != std::wstring::npos) { code = true; cA = UINT32(out.size()); }
      else out += ch;
      continue;
    }
    if (!code && ch == L'*' && i + 1 < src.size() && src[i + 1] == L'*') {
      if (bold) { runs.push_back({bA, UINT32(out.size()) - bA, 1}); bold = false; i++; continue; }
      if (src.find(L"**", i + 2) != std::wstring::npos) { bold = true; bA = UINT32(out.size()); i++; continue; }
    }
    if (!code && (ch == L'*' || ch == L'_')) {
      bool prevAlnum = i > 0 && iswalnum(src[i - 1]);
      bool nextSpace = i + 1 >= src.size() || iswspace(src[i + 1]);
      if (ital && !(i > 0 && iswspace(src[i - 1]))) { runs.push_back({iA, UINT32(out.size()) - iA, 2}); ital = false; continue; }
      if (!ital && !prevAlnum && !nextSpace && src.find(ch, i + 1) != std::wstring::npos) { ital = true; iA = UINT32(out.size()); continue; }
    }
    out += ch;
  }
  return out;
}
}  // namespace

float Ui::richText(const Rect& r, const string& md, float size, const Color& col, int weight, bool draw, float lineSpacing) {
  if (md.empty()) return 0;
  float bw = std::max(10.f, r.w);
  uint64_t key = textKey(4, md, size, weight, false, bw, 0, int(std::lround(lineSpacing * 100)));
  TextEntry* e = textLookup(key);
  if (!e) {
    TextEntry n;
    vector<RichRun> runs;
    std::wstring w = richParse(md, runs);
    Com<IDWriteTextFormat> f;
    g->dw->CreateTextFormat(widen(g->uiFamily).c_str(), nullptr, DWRITE_FONT_WEIGHT(weight), DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put());
    if (!f) return 0;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * lineSpacing, size * lineSpacing * 0.8f);
    g->dw->CreateTextLayout(w.c_str(), UINT32(w.size()), f.get(), bw, 100000, n.L.put());
    if (!n.L) return 0;
    for (auto& ru : runs) {
      DWRITE_TEXT_RANGE tr{ru.a, ru.n};
      if (ru.kind == 1) n.L->SetFontWeight(DWRITE_FONT_WEIGHT(std::max(600, weight + 200)), tr);
      else if (ru.kind == 2) n.L->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, tr);
      else if (ru.kind == 3) { n.L->SetFontFamilyName(L"Consolas", tr); n.L->SetFontSize(size * 0.92f, tr); }
    }
    DWRITE_TEXT_METRICS m;
    n.L->GetMetrics(&m);
    n.w = m.widthIncludingTrailingWhitespace;
    n.h = m.height;
    // code spans get a subtle background: their boxes are part of the cached entry
    for (auto& ru : runs) {
      if (ru.kind != 3 || ru.n == 0) continue;
      UINT32 cnt = 0;
      n.L->HitTestTextRange(ru.a, ru.n, 0, 0, nullptr, 0, &cnt);
      if (!cnt) continue;
      vector<DWRITE_HIT_TEST_METRICS> hm(cnt);
      if (SUCCEEDED(n.L->HitTestTextRange(ru.a, ru.n, 0, 0, hm.data(), cnt, &cnt)))
        for (auto& h : hm) n.boxes.push_back({h.left, h.top, h.width, h.height});
    }
    e = &textStore(key, std::move(n));
  }
  if (draw) {
    for (auto& b : e->boxes) fill({r.x + b[0] - 2 * s, r.y + b[1] + 1 * s, b[2] + 4 * s, b[3] - 1 * s}, c.text.withA(dark ? 0.1f : 0.06f), 3 * s);
    dc()->DrawTextLayout(D2D1::Point2F(r.x, r.y), e->L.get(), g->br(col));
  }
  return e->h;
}

float Ui::scrollGet(const string& key) const {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char ch : "scroll:" + key) { h ^= ch; h *= 1099511628211ull; }
  auto it = scroll_.find(h);
  return it == scroll_.end() ? 0.f : it->second.first;
}

void Ui::scrollSet(const string& key, float y) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char ch : "scroll:" + key) { h ^= ch; h *= 1099511628211ull; }
  auto& st = scroll_[h];
  st.first = std::min(y, std::max(0.f, st.second));
  scrollTarget_.erase(h);
}

// ------------------------------------------------------------ multi-line text area
static UINT32 u16Index(const std::u32string& s, int k) {
  UINT32 n = 0;
  for (int i = 0; i < k && i < int(s.size()); i++) n += s[size_t(i)] >= 0x10000 ? 2 : 1;
  return n;
}
static int u32Index(const std::u32string& s, UINT32 pos16) {
  UINT32 n = 0;
  for (int i = 0; i < int(s.size()); i++) {
    if (n >= pos16) return i;
    n += s[size_t(i)] >= 0x10000 ? 2 : 1;
  }
  return int(s.size());
}

bool Ui::textArea(const Rect& r, const string& key, string& buf, const string& placeholder, bool* submitted, float* contentH, float fontSize) {
  uint64_t idv = id("ta:" + key);
  recordHover(idv, r);
  if (scrollStack_.empty()) hitRects_.push_back(r);
  bool hov = touchHit(idv, r);
  if (hov) { hotNext_ = idv; cursor = "ibeam"; }
  bool changed = false;
  float fs = fontSize * s, pad = 10 * s;
  Rect tr{r.x + pad, r.y + 8 * s, r.w - 2 * pad, r.h - 16 * s};
  bool foc = focus == idv;
  std::u32string cur = foc ? ebuf_ : toU32(buf);
  auto layoutOf = [&](const std::u32string& t) {
    std::wstring w = widen(fromU32(t) + " ");  // trailing space keeps the caret line of a final newline
    Com<IDWriteTextLayout> L;
    Com<IDWriteTextFormat> f;
    g->dw->CreateTextFormat(widen(g->uiFamily).c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs, L"en-us", f.put());
    if (f) { f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); g->dw->CreateTextLayout(w.c_str(), UINT32(w.size()), f.get(), std::max(10.f, tr.w), 100000, L.put()); }
    return L;
  };
  float& sy = escroll_;  // vertical scroll of the focused area
  auto posFromPt = [&](const Com<IDWriteTextLayout>& L, float mx, float my) {
    if (!L) return 0;
    BOOL trail = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS hm;
    L->HitTestPoint(mx - tr.x, my - tr.y + (foc ? sy : 0), &trail, &inside, &hm);
    return u32Index(ebuf_, hm.textPosition + (trail ? hm.length : 0));
  };
  if (in.pressed[0] && !in.touchCancel) {
    if (hov) {
      if (focus != idv) { focus = idv; ebuf_ = toU32(buf); sy = 0; foc = true; }
      auto L = layoutOf(ebuf_);
      caret_ = anchor_ = std::min(int(ebuf_.size()), posFromPt(L, in.mx, in.my));
      if (in.dbl) { anchor_ = 0; caret_ = int(ebuf_.size()); }
      mouseSel_ = true;
      caretT_ = time;
    } else if (focus == idv) { focus = 0; foc = false; }
  }
  if (foc && mouseSel_ && in.down[0]) { auto L = layoutOf(ebuf_); caret_ = std::min(int(ebuf_.size()), posFromPt(L, in.mx, in.my)); }
  if (!in.down[0]) mouseSel_ = false;
  if (foc) {
    if (ebuf_.empty() && !buf.empty() && toU32(buf) != ebuf_) ebuf_ = toU32(buf);
    if (buf.empty() && !ebuf_.empty() && !changed && !in.chars.size() && !in.keys.size() && !mouseSel_) {}  // cleared from outside: handled below
    auto delSel = [&]() {
      if (caret_ == anchor_) return false;
      int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
      ebuf_.erase(size_t(a), size_t(b - a));
      caret_ = anchor_ = a;
      return true;
    };
    auto insert = [&](const std::u32string& u) { delSel(); ebuf_.insert(size_t(caret_), u); caret_ += int(u.size()); anchor_ = caret_; changed = true; };
    for (char32_t ch : in.chars) { if (ch < 32 || ch == 127) continue; insert(std::u32string(1, ch)); }
    auto vertical = [&](int dir) {
      auto L = layoutOf(ebuf_);
      if (!L) return;
      FLOAT px, py;
      DWRITE_HIT_TEST_METRICS hm;
      L->HitTestTextPosition(u16Index(ebuf_, caret_), FALSE, &px, &py, &hm);
      BOOL trail, inside;
      DWRITE_HIT_TEST_METRICS h2;
      float ny = py + (dir > 0 ? hm.height * 1.5f : -hm.height * 0.5f);
      if (ny < 0) { caret_ = 0; return; }
      L->HitTestPoint(px, ny, &trail, &inside, &h2);
      caret_ = std::min(int(ebuf_.size()), u32Index(ebuf_, h2.textPosition + (trail ? h2.length : 0)));
    };
    for (int k : in.keys) {
      bool sh = in.shift;
      if (k == VK_LEFT) { if (caret_ > 0) caret_--; if (!sh) anchor_ = caret_; }
      else if (k == VK_RIGHT) { if (caret_ < int(ebuf_.size())) caret_++; if (!sh) anchor_ = caret_; }
      else if (k == VK_UP) { vertical(-1); if (!sh) anchor_ = caret_; }
      else if (k == VK_DOWN) { vertical(1); if (!sh) anchor_ = caret_; }
      else if (k == VK_HOME) { while (caret_ > 0 && ebuf_[size_t(caret_ - 1)] != U'\n') caret_--; if (in.ctrl) caret_ = 0; if (!sh) anchor_ = caret_; }
      else if (k == VK_END) { while (caret_ < int(ebuf_.size()) && ebuf_[size_t(caret_)] != U'\n') caret_++; if (in.ctrl) caret_ = int(ebuf_.size()); if (!sh) anchor_ = caret_; }
      else if (k == VK_BACK) { if (!delSel() && caret_ > 0) { ebuf_.erase(size_t(caret_ - 1), 1); caret_--; anchor_ = caret_; } changed = true; }
      else if (k == VK_DELETE) { if (!delSel() && caret_ < int(ebuf_.size())) ebuf_.erase(size_t(caret_), 1); changed = true; }
      else if (k == 'A' && in.ctrl) { anchor_ = 0; caret_ = int(ebuf_.size()); }
      else if ((k == 'C' || k == 'X') && in.ctrl && caret_ != anchor_) {
        int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
        setClipboardText(hwnd, replaceAll(fromU32(ebuf_.substr(size_t(a), size_t(b - a))), "\n", "\r\n"));
        if (k == 'X') { delSel(); changed = true; }
      } else if (k == 'V' && in.ctrl) insert(toU32(replaceAll(getClipboardText(hwnd), "\r", "")));
      else if (k == VK_RETURN) {
        if (sh) insert(U"\n");
        else if (submitted) *submitted = true;
      } else if (k == VK_ESCAPE) { focus = 0; }
      caretT_ = time;
    }
    if (changed) buf = fromU32(ebuf_);
    else if (toU32(buf) != ebuf_ && !mouseSel_) { ebuf_ = toU32(buf); caret_ = anchor_ = std::min(caret_, int(ebuf_.size())); }  // changed by the app (e.g. cleared after sending)
    wantsCaret = true;
    cur = ebuf_;
  }
  foc = focus == idv;
  // frame
  fill(r, c.input, 10 * s);
  if (foc) stroke(inset(r, -1.5f * s, -1.5f * s), c.accent.withA(0.3f), 11.5f * s, 3 * s);
  stroke(r, foc ? c.accent : c.borderStrong.withA(0.75f), 10 * s, 1.f);
  auto L = layoutOf(cur);
  float th = fs * 1.3f;
  if (L) { DWRITE_TEXT_METRICS m; L->GetMetrics(&m); th = m.height; }
  if (contentH) *contentH = th + 16 * s;
  pushClip(tr);
  if (foc && L) {
    FLOAT px, py;
    DWRITE_HIT_TEST_METRICS hm;
    L->HitTestTextPosition(u16Index(cur, caret_), FALSE, &px, &py, &hm);
    if (py - sy + hm.height > tr.h) sy = py + hm.height - tr.h;
    if (py - sy < 0) sy = py;
    sy = clampv(sy, 0.f, std::max(0.f, th - tr.h));
    if (caret_ != anchor_) {
      UINT32 a = u16Index(cur, std::min(caret_, anchor_)), b = u16Index(cur, std::max(caret_, anchor_));
      UINT32 cnt = 0;
      L->HitTestTextRange(a, b - a, 0, 0, nullptr, 0, &cnt);
      if (cnt) {
        vector<DWRITE_HIT_TEST_METRICS> v(cnt);
        if (SUCCEEDED(L->HitTestTextRange(a, b - a, 0, 0, v.data(), cnt, &cnt)))
          for (auto& h : v) fill({tr.x + h.left, tr.y + h.top - sy, std::max(h.width, 3 * s), h.height}, c.accent.withA(0.28f));
      }
    }
    if (std::fmod(time - caretT_, 1.0) < 0.55) line(tr.x + px, tr.y + py - sy + 1 * s, tr.x + px, tr.y + py - sy + hm.height - 1 * s, c.text, 1.2f * s);
  }
  if (cur.empty() && !placeholder.empty()) textWrap(tr, placeholder, fs, c.textFaint);
  else if (L) dc()->DrawTextLayout(D2D1::Point2F(tr.x, tr.y - (foc ? sy : 0)), L.get(), g->br(c.text));
  popClip();
  return changed;
}

// ------------------------------------------------------------ icons
static const std::map<string, const char*>& iconPaths() {
  static const std::map<string, const char*> P = {
      {"folder", "M3 7 L3 18 C3 19 4 20 5 20 L19 20 C20 20 21 19 21 18 L21 9 C21 8 20 7 19 7 L12 7 L10 4 L5 4 C4 4 3 5 3 6 Z"},
      {"list", "M9 6 L20 6 M9 12 L20 12 M9 18 L20 18 O 4.5 6 1.3 O 4.5 12 1.3 O 4.5 18 1.3"},
      {"font", "M4 19 L11 4 L18 19 M6.6 13.5 L15.4 13.5 M20 10 L20 19"},
      {"textcolor", "M6 15 L11 4 L16 15 M8 11 L14 11 M4 20 L20 20"},
      {"spacing", "M7 4 L7 20 M4.5 6.5 L7 4 L9.5 6.5 M4.5 17.5 L7 20 L9.5 17.5 M13 6 L21 6 M13 12 L21 12 M13 18 L21 18"},
      {"caption", "R 4 3.5 16 11 1.5 M7 18.5 L17 18.5 M9 21 L15 21"},
      {"sliders", "M4 7 L20 7 M4 12 L20 12 M4 17 L20 17 O 9 7 2 O 15 12 2 O 8 17 2"},
      {"newdoc", "M6 3 L14 3 L19 8 L19 21 L6 21 Z M14 3 L14 8 L19 8 M9.5 15 L15.5 15 M12.5 12 L12.5 18"},
      {"pages", "R 7 3 12 15 1.5 M7 7 L4 7 L4 21 L15 21 L15 18"},
      {"palette", "O 12 12 9 O 8.5 10 1.2 O 12 7.5 1.2 O 15.5 10 1.2 M12 21 C9.5 21 9.8 17.5 12.6 17.2 C15 17 15.8 15 15.8 15 L21 15"},
      {"save", "R 4 4 16 16 2 M8 4 L8 9 L15 9 L15 4 M8 20 L8 14 L16 14 L16 20"},
      {"data", "M4 6 C4 3.5 20 3.5 20 6 C20 8.5 4 8.5 4 6 Z M4 6 L4 18 C4 20.5 20 20.5 20 18 L20 6 M4 12 C4 14.5 20 14.5 20 12"},
      {"network", "O 6 6 2.6 O 18 7 2.6 O 12 18 2.6 O 19 17 1.6 M8.6 6.2 L15.4 6.8 M7.3 8.3 L10.8 15.7 M16.8 9.3 L13.3 15.7 M14.6 18 L17.4 17.2"},
      {"analyse", "M4 20 L4 4 M4 20 L20 20 M8.5 16 L8.5 11 M12.5 16 L12.5 7 M16.5 16 L16.5 13"},
      {"trends", "M3 17 L9 11 L13 15 L21 7 M15 7 L21 7 L21 13"},
      {"actors", "O 9 8 3.5 M3 20 C3 15.2 15 15.2 15 20 M16 4.6 C19 5.2 19 10.8 16 11.4 M17.5 15.2 C20 15.8 21 17.6 21 20"},
      {"publish", "R 3 4 18 16 2 O 9 10 2 M21 16 L16 11 L6 20"},
      {"look", "O 12 12 9 O 8 9.5 1.3 O 12 7 1.3 O 16 9.5 1.3 M12 21 C9.8 21 10 17 13 16.6 C15.4 16.3 16.2 14.4 16.2 14.4"},
      {"search", "O 11 11 7 M16.2 16.2 L21 21"},
      {"chat", "M4 5 C4 4.4 4.4 4 5 4 L19 4 C19.6 4 20 4.4 20 5 L20 15 C20 15.6 19.6 16 19 16 L10 16 L6 20 L6 16 L5 16 C4.4 16 4 15.6 4 15 Z"},
      {"send", "M12 19 L12 5 M6 11 L12 5 L18 11"},
      {"win-min", "M6 12 L18 12"},
      {"win-max", "R 6.5 6.5 11 11 0.5"},
      {"win-restore", "R 6.5 8.5 9 9 0.5 M9 8.5 L9 6.5 L17.5 6.5 L17.5 15 L15.5 15"},
      {"win-close", "M7 7 L17 17 M17 7 L7 17"},
      {"agent", "M4.5 6 L11 6 M4.5 12 L13 12 M4.5 18 L9 18 M16.5 13 L17.6 16.4 L21 17.5 L17.6 18.6 L16.5 22 L15.4 18.6 L12 17.5 L15.4 16.4 Z"},
      {"text", "M6 6.5 L18 6.5 M12 6.5 L12 19"},
      {"pen", "M4 20 L9 19 L19.5 8.5 C21 7 17 3 15.5 4.5 L5 15 Z M13.5 6.5 L17.5 10.5"},
      {"undo", "M9 14 L4 9 L9 4 M4 9 L15 9 C18.5 9 21 11.5 21 15 C21 18.5 18.5 21 15 21 L11 21"},
      {"redo", "M15 14 L20 9 L15 4 M20 9 L9 9 C5.5 9 3 11.5 3 15 C3 18.5 5.5 21 9 21 L13 21"},
      {"fit", "M4 9 L4 4 L9 4 M15 4 L20 4 L20 9 M20 15 L20 20 L15 20 M9 20 L4 20 L4 15"},
      {"plus", "M12 5 L12 19 M5 12 L19 12"},
      {"settings", "M4 7 L6.8 7 M11.2 7 L20 7 M4 17 L12.8 17 M17.2 17 L20 17 O 9 7 2.2 O 15 17 2.2"},
      {"minus", "M5 12 L19 12"},
      {"play", "M7 4.5 L19 12 L7 19.5 Z"},
      {"stop", "R 6 6 12 12 1.5"},
      {"x", "M6 6 L18 18 M18 6 L6 18"},
      {"check", "M5 12.5 L10 17.5 L19 7"},
      {"chev-down", "M6 9 L12 15 L18 9"},
      {"chev-up", "M6 15 L12 9 L18 15"},
      {"chev-right", "M9 6 L15 12 L9 18"},
      {"chev-left", "M15 6 L9 12 L15 18"},
      {"eye", "M2 12 C5 5.5 19 5.5 22 12 C19 18.5 5 18.5 2 12 Z O 12 12 3"},
      {"eye-off", "M2 12 C5 5.5 19 5.5 22 12 C19 18.5 5 18.5 2 12 Z O 12 12 3 M3 3 L21 21"},
      {"pin", "M12 17 L12 22 M8 3 L16 3 M9 3 L9 10 L6 14 L18 14 L15 10 L15 3"},
      {"bookmark", "M6 3 L18 3 L18 21 L12 17 L6 21 Z"},
      {"info", "O 12 12 9 M12 11 L12 16.5 M12 7.6 L12 7.9"},
      {"warn", "M12 3.5 L21.5 20 L2.5 20 Z M12 9.5 L12 14 M12 16.8 L12 17.1"},
      {"sun", "O 12 12 4 M12 2 L12 4 M12 20 L12 22 M2 12 L4 12 M20 12 L22 12 M4.9 4.9 L6.3 6.3 M17.7 17.7 L19.1 19.1 M4.9 19.1 L6.3 17.7 M17.7 6.3 L19.1 4.9"},
      {"moon", "M20 14.5 C18.5 18.5 14 21 9.5 19.5 C5 18 2.5 13 4 8.5 C5 5.5 7.5 3.5 10.5 3 C8 6.5 8.5 11.5 12 14 C14.5 15.8 17.5 16 20 14.5 Z"},
      {"download", "M12 3 L12 15 M7 10 L12 15 L17 10 M4 17 L4 20 L20 20 L20 17"},
      {"copy", "R 8 8 12 12 2 M16 8 L16 6 C16 5 15 4 14 4 L6 4 C5 4 4 5 4 6 L4 14 C4 15 5 16 6 16 L8 16"},
      {"trash", "M4 7 L20 7 M9 7 L9 4 L15 4 L15 7 M6 7 L7 20 L17 20 L18 7"},
      {"layers", "M12 3 L21 8 L12 13 L3 8 Z M3 12.5 L12 17.5 L21 12.5 M3 16.5 L12 21.5 L21 16.5"},
      {"cube", "M12 2.5 L20.5 7 L20.5 17 L12 21.5 L3.5 17 L3.5 7 Z M3.5 7 L12 11.5 L20.5 7 M12 11.5 L12 21.5"},
      {"sparkle", "M12 3 L13.8 10.2 L21 12 L13.8 13.8 L12 21 L10.2 13.8 L3 12 L10.2 10.2 Z"},
      {"mic", "R 9 2.5 6 12 3 M6 11 C6 15.4 8.7 18 12 18 C15.3 18 18 15.4 18 11 M12 18 L12 21.5 M8.5 21.5 L15.5 21.5"},
      {"live", "O 12 12 2.4 M8.2 8.2 C6.1 10.3 6.1 13.7 8.2 15.8 M15.8 8.2 C17.9 10.3 17.9 13.7 15.8 15.8 M5.4 5.4 C1.8 9 1.8 15 5.4 18.6 M18.6 5.4 C22.2 9 22.2 15 18.6 18.6"},
      {"shield", "M12 3 L19 6 L19 11.5 C19 16 16 19.5 12 21 C8 19.5 5 16 5 11.5 L5 6 Z M9 12 L11 14 L15 10"},
      {"orb", "O 12 12 3.5 M4.5 12 C4.5 7.9 7.9 4.5 12 4.5 M19.5 12 C19.5 16.1 16.1 19.5 12 19.5"},
      {"expand", "M14 10 L20 4 M15 4 L20 4 L20 9 M10 14 L4 20 M9 20 L4 20 L4 15"},
      {"camera", "M4 8 C4 7 5 6 6 6 L8.5 6 L10 3.5 L14 3.5 L15.5 6 L18 6 C19 6 20 7 20 8 L20 18 C20 19 19 20 18 20 L6 20 C5 20 4 19 4 18 Z O 12 13 3.5"},
      {"settings", "O 12 12 3 O 12 12 7.5 M12 2 L12 4.5 M12 19.5 L12 22 M2 12 L4.5 12 M19.5 12 L22 12 M4.9 4.9 L6.7 6.7 M17.3 17.3 L19.1 19.1 M4.9 19.1 L6.7 17.3 M17.3 6.7 L19.1 4.9"},
      {"grid", "R 3.5 3.5 7 7 1.2 R 13.5 3.5 7 7 1.2 R 3.5 13.5 7 7 1.2 R 13.5 13.5 7 7 1.2"},
      {"table", "R 3 4 18 16 2 M3 10 L21 10 M3 15 L21 15 M10 4 L10 20"},
      {"file", "M14 3 L6 3 C5 3 4 4 4 5 L4 19 C4 20 5 21 6 21 L18 21 C19 21 20 20 20 19 L20 9 Z M14 3 L14 9 L20 9"},
      {"globe", "O 12 12 9 M3 12 L21 12 M12 3 C8 7 8 17 12 21 C16 17 16 7 12 3"},
      {"clock", "O 12 12 9 M12 7 L12 12 L15.5 14"},
      {"density", "O 12 12 9 O 12 12 5.5 O 12 12 2"},
      {"tag", "M3 12 L12 3 L21 3 L21 12 L12 21 Z O 16.5 7.5 1.3"},
      {"refresh", "M20 11 C19.5 6.5 15.5 3 11 3.5 C7.5 4 4.5 6.5 3.8 10 M20 4 L20 11 L13 11 M4 13 C4.5 17.5 8.5 21 13 20.5 C16.5 20 19.5 17.5 20.2 14 M4 20 L4 13 L11 13"},
      {"shuffle", "M3 7 L7 7 C12 7 12 17 17 17 L21 17 M18 14 L21 17 L18 20 M3 17 L7 17 C9 17 10 15.5 11 14 M13 10 C14 8.5 15 7 17 7 L21 7 M18 4 L21 7 L18 10"},
      {"filter", "M3 5 L21 5 L14 13 L14 20 L10 18 L10 13 Z"},
      {"legend", "O 6 7 1.6 M10.5 7 L19 7 O 6 12 1.6 M10.5 12 L19 12 O 6 17 1.6 M10.5 17 L16 17"},
      {"help", "O 12 12 9 M9.5 9.5 C9.5 7.3 14.5 7.3 14.5 10 C14.5 12 12 12 12 14 M12 17 L12 17.3"},
      {"menu", "M4 6 L20 6 M4 12 L20 12 M4 18 L20 18"},
      {"bundle", "M3 18 C10 18 10 7 21 6 M3 12 C10 12 11 7.5 21 7.5 M3 6 C10 6 12 9 21 9"},
      {"hull", "M5 8 C5 4 19 3 20 8 C21 13 18 20 12 20 C6 20 5 13 5 8 Z O 9 10 1.2 O 14 9 1.2 O 12 15 1.2"},
      {"type", "M5 7 L5 4 L19 4 L19 7 M9 20 L15 20 M12 4 L12 20"},
      {"sankey", "M3 5 L7 5 C12 5 12 12 17 12 L21 12 M3 12 L7 12 C12 12 12 19 17 19 L21 19"},
      {"target", "O 12 12 8 O 12 12 3 M12 2 L12 5 M12 19 L12 22 M2 12 L5 12 M19 12 L22 12"},
      {"link", "M10 14 C8 12 8 9 10 7 L12.5 4.5 C14.5 2.5 17.5 2.5 19.5 4.5 C21.5 6.5 21.5 9.5 19.5 11.5 L18 13 M14 10 C16 12 16 15 14 17 L11.5 19.5 C9.5 21.5 6.5 21.5 4.5 19.5 C2.5 17.5 2.5 14.5 4.5 12.5 L6 11"},
      {"command", "R 3 3 18 18 4 M8 12 L16 12 M12 8 L12 16"},
      {"keyboard", "R 2.5 6 19 12 2 M6 10 L6 10.2 M10 10 L10 10.2 M14 10 L14 10.2 M18 10 L18 10.2 M7 14 L17 14"},
      {"book", "M4 19 L4 5 C4 4 5 3 6 3 L20 3 L20 21 L6 21 C5 21 4 20 4 19 C4 18 5 17 6 17 L20 17"},
      {"cursor", "M5 3 L19 12 L12 13.5 L9 20 Z"},
      {"hand", "M8 13 L8 5.5 C8 4 10 4 10 5.5 L10 11 M10 10 L10 3.8 C10 2.4 12 2.4 12 3.8 L12 11 M12 10 L12 4.8 C12 3.3 14 3.3 14 4.8 L14 11 M14 10 L14 6.8 C14 5.3 16 5.3 16 6.8 L16 15 C16 19 14 21 11 21 C8 21 7 19 5.5 16.5 L4 14 C3.3 12.8 5 11.8 6 13 L8 15"},
      {"lasso", "R 4 4 16 16 1.5"},
      {"area", "M4 8 L4 4 L8 4 M16 4 L20 4 L20 8 M20 16 L20 20 L16 20 M8 20 L4 20 L4 16 R 8.5 8.5 7 7 1"},
      // reader (1.19): two facing pages, a turn, a printer
      {"twopage", "R 3 5 8 14 1 R 13 5 8 14 1 M6 9 L8 9 M6 12 L8 12 M16 9 L18 9 M16 12 L18 12"},
      {"rotate", "M19.5 12 C19.5 16.1 16.1 19.5 12 19.5 C7.9 19.5 4.5 16.1 4.5 12 C4.5 7.9 7.9 4.5 12 4.5 C14.6 4.5 16.9 5.8 18.2 7.8 M18.6 3.5 L18.6 8.2 L13.9 8.2"},
      {"print", "M7 8.5 L7 3.5 L17 3.5 L17 8.5 M7 17 L4 17 L4 10.5 C4 9.4 4.9 8.5 6 8.5 L18 8.5 C19.1 8.5 20 9.4 20 10.5 L20 17 L17 17 M7 13.5 L17 13.5 L17 20.5 L7 20.5 Z"},
      {"external", "M14 4 L20 4 L20 10 M20 4 L11 13 M18 14 L18 19 C18 19.6 17.6 20 17 20 L5 20 C4.4 20 4 19.6 4 19 L4 7 C4 6.4 4.4 6 5 6 L10 6"},
      {"arrow-left", "M19 12 L5 12 M11 6 L5 12 L11 18"},
      {"quote", "M5 11 L9 11 L9 17 L5 17 Z M5 11 C5 8 6.5 6.5 9 6 M14 11 L18 11 L18 17 L14 17 Z M14 11 C14 8 15.5 6.5 18 6"},
      {"history", "M3.5 12 C3.5 7 7.5 3.5 12 3.5 C16.7 3.5 20.5 7.3 20.5 12 C20.5 16.7 16.7 20.5 12 20.5 C8.8 20.5 6 18.8 4.6 16.2 M3.5 5.5 L3.5 12 L9.5 12 M12 7.5 L12 12 L15 14"},
      {"map", "M3 6 L9 3.5 L15 6 L21 3.5 L21 18 L15 20.5 L9 18 L3 20.5 Z M9 3.5 L9 18 M15 6 L15 20.5"},
      // writer (1.11)
      {"writer", "M5 3 L14 3 L19 8 L19 21 L5 21 Z M14 3 L14 8 L19 8 M8 12 L16 12 M8 15.5 L16 15.5 M8 19 L13 19"},
      {"bold", "M7 4 L13 4 C16 4 18 5.5 18 8 C18 10.5 16 12 13 12 L7 12 M7 12 L14 12 C17 12 19 13.5 19 16 C19 18.5 17 20 14 20 L7 20 Z"},
      {"italic", "M13 4 L19 4 M5 20 L11 20 M15 4 L9 20"},
      {"underline", "M6 4 L6 11 C6 14.5 8.5 17 12 17 C15.5 17 18 14.5 18 11 L18 4 M5 21 L19 21"},
      {"strike", "M4 12 L20 12 M8 7.5 C8 5.5 10 4 12 4 C14 4 16 5 16 7 M8 17 C8 19 10 20 12 20 C14 20 16 19 16 17"},
      {"sub", "M4 6 L11 14 M11 6 L4 14 M15 13 C15 12 16 11.5 17 11.5 C18.5 11.5 19.5 12.5 19 14 L15 19 L19.5 19"},
      {"sup", "M4 10 L11 18 M11 10 L4 18 M15 5 C15 4 16 3.5 17 3.5 C18.5 3.5 19.5 4.5 19 6 L15 11 L19.5 11"},
      {"code", "M8 6 L3 12 L8 18 M16 6 L21 12 L16 18 M14 4 L10 20"},
      {"mark", "M6 15 L14.5 6.5 L18.5 10.5 L10 19 L6 19 Z M6 19 L4 21 M4 22 L20 22"},
      {"clearfmt", "M5 4 L18 4 M11.5 4 L8 17 M14 15 L20 21 M20 15 L14 21"},
      {"alignleft", "M4 6 L20 6 M4 10 L14 10 M4 14 L20 14 M4 18 L14 18"},
      {"aligncenter", "M4 6 L20 6 M7 10 L17 10 M4 14 L20 14 M7 18 L17 18"},
      {"alignright", "M4 6 L20 6 M10 10 L20 10 M4 14 L20 14 M10 18 L20 18"},
      {"justify", "M4 6 L20 6 M4 10 L20 10 M4 14 L20 14 M4 18 L20 18"},
      {"listbullet", "M9 6 L20 6 M9 12 L20 12 M9 18 L20 18 O 5 6 1.3 O 5 12 1.3 O 5 18 1.3"},
      {"listnum", "M10 6 L20 6 M10 12 L20 12 M10 18 L20 18 M4 4.5 L5.5 3.5 L5.5 8.5 M4 11 C4 10 5 9.5 5.8 9.5 C7 9.5 7.2 10.7 6.5 11.5 L4 14 L7.2 14 M4 16.5 L7 16.5 L5.5 18.5 C7 18.5 7.5 19.5 7 20.3 C6.5 21 4.5 21 4 20.3"},
      {"indent", "M10 6 L20 6 M10 12 L20 12 M10 18 L20 18 M4 6 L4 18 M3 9 L7 12 L3 15"},
      {"outdent", "M10 6 L20 6 M10 12 L20 12 M10 18 L20 18 M4 6 L4 18 M7 9 L3 12 L7 15"},
      {"image", "M4 5 L20 5 L20 19 L4 19 Z M4 16 L9 11 L13 15 L16 12 L20 16 O 15.5 8.5 1.5"},
      {"pagebreak", "M4 4 L4 9 L20 9 L20 4 M4 20 L4 15 L20 15 L20 20 M3 12 L6 12 M9 12 L12 12 M15 12 L18 12 M21 12 L21.5 12"},
      {"toc", "M4 5 L9 5 M12 5 L20 5 M6 10 L9 10 M12 10 L20 10 M6 15 L9 15 M12 15 L20 15 M4 20 L9 20 M12 20 L20 20"},
  };
  return P;
}

ID2D1PathGeometry* Ui::iconGeo(const string& name) {
  auto it = icons_.find(name);
  if (it != icons_.end()) return it->second.get();
  auto pit = iconPaths().find(name);
  Com<ID2D1PathGeometry> geo;
  g->d2f->CreatePathGeometry(geo.put());
  Com<ID2D1GeometrySink> sk;
  geo->Open(sk.put());
  if (pit != iconPaths().end()) {
    vector<string> t;
    {
      const string& src = pit->second;
      string cur;
      auto flush = [&]() { if (!cur.empty()) { t.push_back(cur); cur.clear(); } };
      for (size_t k = 0; k < src.size(); k++) {
        char ch = src[k];
        if (ch == ' ' || ch == ',') flush();
        else if (std::isalpha(static_cast<unsigned char>(ch))) { flush(); t.push_back(string(1, ch)); }
        else if (ch == '-' && !cur.empty() && cur.back() != 'e') { flush(); cur += ch; }
        else cur += ch;
      }
      flush();
    }
    size_t i = 0;
    bool open = false;
    auto num = [&]() { return i < t.size() ? float(toDouble(t[i++])) : 0.f; };
    auto endFig = [&](bool closed) { if (open) { sk->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN); open = false; } };
    while (i < t.size()) {
      string op = t[i++];
      if (op == "M") { endFig(false); float x = num(), y = num(); sk->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW); open = true; }
      else if (op == "L") { float x = num(), y = num(); sk->AddLine(D2D1::Point2F(x, y)); }
      else if (op == "C") { float a = num(), b = num(), cc = num(), d = num(), e = num(), f = num(); sk->AddBezier(D2D1::BezierSegment(D2D1::Point2F(a, b), D2D1::Point2F(cc, d), D2D1::Point2F(e, f))); }
      else if (op == "Z") endFig(true);
      else if (op == "O") {
        endFig(false);
        float cx = num(), cy = num(), r = num();
        const float kk = 0.5523f * r;  // cubic approximation of a circle (robust on every D2D implementation)
        sk->BeginFigure(D2D1::Point2F(cx + r, cy), D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + r, cy + kk), D2D1::Point2F(cx + kk, cy + r), D2D1::Point2F(cx, cy + r)));
        sk->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - kk, cy + r), D2D1::Point2F(cx - r, cy + kk), D2D1::Point2F(cx - r, cy)));
        sk->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - r, cy - kk), D2D1::Point2F(cx - kk, cy - r), D2D1::Point2F(cx, cy - r)));
        sk->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + kk, cy - r), D2D1::Point2F(cx + r, cy - kk), D2D1::Point2F(cx + r, cy)));
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
      } else if (op == "R") {
        endFig(false);
        float x = num(), y = num(), w = num(), h = num(), r = num();
        sk->BeginFigure(D2D1::Point2F(x + r, y), D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddLine(D2D1::Point2F(x + w - r, y));
        sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w, y + r), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine(D2D1::Point2F(x + w, y + h - r));
        sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w - r, y + h), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine(D2D1::Point2F(x + r, y + h));
        sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y + h - r), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine(D2D1::Point2F(x, y + r));
        sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + r, y), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
      }
    }
    endFig(false);
  }
  sk->Close();
  icons_[name] = geo;
  return geo.get();
}

void Ui::icon(const string& name, float cx, float cy, float size, const Color& col, float strokeW) {
  ID2D1PathGeometry* geo = iconGeo(name);
  if (!geo) return;
  float k = size / 24.f;
  D2D1_MATRIX_3X2_F old;
  dc()->GetTransform(&old);
  dc()->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(cx - size / 2, cy - size / 2) * old);
  static Com<ID2D1StrokeStyle> ss;
  if (!ss) g->d2f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, ss.put());
  if (name == "play" || name == "stop") dc()->FillGeometry(geo, g->br(col));
  else dc()->DrawGeometry(geo, g->br(col), strokeW * 1.15f, ss.get());
  dc()->SetTransform(old);
}

// ------------------------------------------------------------ ids & interaction
uint64_t Ui::id(const string& str) const {
  uint64_t h = idStack_.empty() ? 1469598103934665603ull : idStack_.back();
  for (unsigned char ch : str) { h ^= ch; h *= 1099511628211ull; }
  return h ? h : 1;
}
void Ui::pushId(const string& str) { idStack_.push_back(id(str)); }
void Ui::popId() { if (!idStack_.empty()) idStack_.pop_back(); }
void Ui::focusText(uint64_t idv, const string& current) {
  focus = idv;
  ebuf_ = toU32(current);
  caret_ = anchor_ = int(ebuf_.size());
  escroll_ = 0;
  mouseSel_ = false;
}

bool Ui::mouseIn(const Rect& r) const {
  if (!r.has(in.mx, in.my)) return false;
  if (modal_ && !modalLayer_) return false;  // a dialog is open: everything beneath is inert
  if (!clips_.empty() && !clips_.back().has(in.mx, in.my)) return false;
  if (!inOverlay_ && popupRectPrev_.w > 0 && popupRectPrev_.has(in.mx, in.my)) return false;
  if (blocked(in.mx, in.my)) return false;
  return true;
}

bool Ui::touchHit(uint64_t idv, const Rect& r) const {
  if (!in.touch) return mouseIn(r);

  // The prior frame's z-ordered hit map is sampled once in beginFrame. Honor it so enlarged controls never steal a
  // tap from a neighbour; overlay widgets update this cached target as they are drawn above the base layer.
  if (touchTargetId_) return touchTargetId_ == idv && mouseIn(r);

  // Keep small icon controls tappable at roughly 44 logical pixels, while retaining their actual visual geometry.
  const float padX = std::max(0.f, 22 * s - r.w * 0.5f);
  const float padY = std::max(0.f, 22 * s - r.h * 0.5f);
  return mouseIn({r.x - padX, r.y - padY, r.w + 2 * padX, r.h + 2 * padY});
}

void Ui::recordHover(uint64_t idv, const Rect& r) {
  hoverHits_.push_back({idv, r, clips_.empty() ? Rect{} : clips_.back(), !clips_.empty(), inOverlay_, modalLayer_});
  if (in.touch && inOverlay_ && r.has(in.mx, in.my) && (clips_.empty() || clips_.back().has(in.mx, in.my))) touchTargetId_ = idv;
}

uint64_t Ui::hoverTargetAt(float x, float y) const {
  const bool inPopup = popupRectNext_.w > 0 && popupRectNext_.has(x, y);
  const bool inBlocked = blockRect_.w > 0 && blockRect_.has(x, y);
  for (auto it = hoverHits_.rbegin(); it != hoverHits_.rend(); ++it) {
    if (modal_ && !it->modal) continue;
    if (inPopup && !it->overlay) continue;
    if (inBlocked && !it->overlay) continue;
    if (it->clipped && !it->clip.has(x, y)) continue;
    if (it->r.has(x, y)) return it->id;
  }
  return 0;
}

bool Ui::behave(uint64_t idv, const Rect& r, bool* hovered, bool* held) {
  lastId_ = idv;
  recordHover(idv, r);
  if (scrollStack_.empty()) hitRects_.push_back(r);
  bool over = touchHit(idv, r) && (active == 0 || active == idv);
  if (over) hotNext_ = idv;
  if (over && in.pressed[0] && !in.touchCancel) active = idv;
  bool clicked = false;
  if (active == idv && in.released[0] && !in.touchCancel) { clicked = touchHit(idv, r); }
  if (hovered) *hovered = over;
  if (held) *held = active == idv && in.down[0];
  if (over) cursor = "hand";
  return clicked;
}

// ------------------------------------------------------------ widgets
bool Ui::button(const Rect& r0, const string& lbl, int kind, const string& ic, bool enabled) {
  uint64_t idv = id("btn:" + lbl + ic + std::to_string(int(r0.x)) + ":" + std::to_string(int(r0.y)));
  bool hov = false, held = false;
  bool clicked = enabled && behave(idv, r0, &hov, &held);
  if (!enabled) hov = held = false;
  // compact desktop push button: the visual height is capped, the hit area keeps the full row
  float vh = std::min(r0.h, 28 * s);
  Rect r{r0.x, std::round(r0.y + (r0.h - vh) / 2), r0.w, vh};
  Color bg, fg = c.text, bd = c.borderStrong;
  float rad = 6 * s;
  if (kind == BTN_PRIMARY) { bg = hov ? c.accentHover : c.accent; fg = c.accentText; bd = Color(0, 0, 0, 0); }
  else if (kind == BTN_DANGER) { bg = hov ? c.danger.withA(0.1f) : c.panel2; fg = c.danger; }
  else if (kind == BTN_GHOST) { bg = hov ? c.hover : Color(0, 0, 0, 0); bd = Color(0, 0, 0, 0); fg = hov ? c.text : c.textDim; }
  else { bg = hov ? (dark ? Color::hex(0x444446) : Color::hex(0xf5f5f5)) : c.panel2; }
  if (held) bg = bg.mix(dark ? Color(1, 1, 1) : Color(0, 0, 0), 0.08f);
  if (!enabled) { fg = c.textFaint; if (kind == BTN_PRIMARY) { bg = c.active; } }
  fill(r, bg, rad);
  if (bd.a > 0) stroke(r, dark ? bd.withA(0.6f) : bd.withA(0.75f), rad, 1);
  float fs = 12.5f * s;
  // text buttons carry no icon; icon-only buttons keep theirs
  bool iconOnly = lbl.empty() && !ic.empty();
  if (iconOnly) { icon(ic, r.x + r.w / 2, r.y + r.h / 2, 15 * s, fg, 1.6f); return clicked; }
  float tw = textW(lbl, fs, kind == BTN_PRIMARY ? 600 : 400);
  text({r.x + (r.w - tw) / 2 - 1, r.y, tw + 4, r.h}, lbl, fs, fg, AL_LEFT, kind == BTN_PRIMARY ? 600 : 400);
  return clicked;
}

bool Ui::iconButton(const Rect& r, const string& ic, const string& tooltip, bool toggled, bool enabled) {
  uint64_t idv = id("ib:" + ic + tooltip + std::to_string(int(r.x)) + ":" + std::to_string(int(r.y)));
  bool hov = false, held = false;
  bool clicked = enabled && behave(idv, r, &hov, &held);
  if (toggled) fill(r, c.active, 6 * s);
  else if (hov && enabled) fill(r, c.hover, 6 * s);
  if (held) fill(r, c.active, 6 * s);
  Color fg = !enabled ? c.textFaint : (toggled ? c.accent : (hov ? c.text : c.textDim));
  icon(ic, r.x + r.w / 2, r.y + r.h / 2, std::min(17 * s, r.h * 0.58f), fg, 1.6f);
  if (!tooltip.empty()) tipFor(idv, tooltip);
  return clicked;
}

bool Ui::help(float x, float cy, const string& msg) {
  float r = 7 * s;
  Rect hr{x - 2 * s, cy - r - 2 * s, 2 * r + 4 * s, 2 * r + 4 * s};
  uint64_t hid = id("help:" + msg + ":" + std::to_string(int(x)) + ":" + std::to_string(int(cy)));
  recordHover(hid, hr);
  bool hov = touchHit(hid, hr) && !in.down[0];
  Color col = hov ? c.accent : c.textFaint.withA(0.85f);
  circle(x + r, cy, r - 1.f * s, col, false, 1.f * s);
  text({x, cy - r, 2 * r, 2 * r}, "?", 9.5f * s, col, AL_CENTER, 600);
  if (hov) { richTip_ = msg; richBold_ = false; cursor = "help"; }
  return hov;
}

bool Ui::helpAfterLabel(float x, float cy, const string& label, float size) {
  const string* h = helpOf(label);
  if (!h) return false;
  return help(x + textW(label, size) + 5 * s, cy, *h);
}

bool Ui::checkbox(const Rect& r, const string& lbl, bool& v) {
  uint64_t idv = id("cb:" + lbl);
  bool hov = false;
  float bs = 15 * s;
  bool overHelp = helpAfterLabel(r.x + bs + 8 * s, r.y + r.h / 2, lbl, 12.5f * s);
  bool clicked = behave(idv, r, &hov) && !overHelp;
  if (clicked) v = !v;
  Rect b{r.x, r.y + (r.h - bs) / 2, bs, bs};
  fill(b, v ? c.accent : c.input, 3.5f * s);
  if (!v) stroke(b, hov ? c.textFaint : c.borderStrong, 3.5f * s);
  if (v) icon("check", b.x + bs / 2, b.y + bs / 2, 11 * s, c.accentText, 2.2f);
  text({r.x + bs + 8 * s, r.y, r.w - bs - 8 * s, r.h}, lbl, 12.5f * s, c.text);
  return clicked;
}

bool Ui::toggle(const Rect& r, const string& lbl, bool& v) {
  uint64_t idv = id("tg:" + lbl);
  bool hov = false;
  bool overHelp = helpAfterLabel(r.x, r.y + r.h / 2, lbl, 12.5f * s);
  bool clicked = behave(idv, r, &hov) && !overHelp;
  if (clicked) v = !v;
  float tw = 28 * s, th = 16 * s;
  Rect t{r.r() - tw, r.y + (r.h - th) / 2, tw, th};
  fill(t, v ? c.accent : (dark ? Color::hex(0x48484a) : Color::hex(0xe0e0e2)), th / 2);
  float kx = v ? t.r() - th / 2 : t.x + th / 2;
  circle(kx, t.y + th / 2 + 0.6f * s, th / 2 - 1.5f * s, Color(0, 0, 0, 0.12f));
  circle(kx, t.y + th / 2, th / 2 - 1.8f * s, Color(1, 1, 1));
  text({r.x, r.y, r.w - tw - 8 * s, r.h}, lbl, 12.5f * s, c.text);
  return clicked;
}

bool Ui::slider(const Rect& r, const string& lbl, float& v, float lo, float hi, const char* fmt, float step) {
  uint64_t idv = id("sl:" + lbl);
  float lw = lbl.empty() ? 0 : std::min(r.w * 0.42f, 130 * s);
  float vw = 46 * s;
  Rect tr{r.x + lw, r.y, r.w - lw - vw - 6 * s, r.h};
  bool hov = false, held = false;
  behave(idv, tr, &hov, &held);
  float old = v;
  if (held) {
    float t = clampv((in.mx - tr.x) / std::max(1.f, tr.w), 0.f, 1.f);
    v = lo + t * (hi - lo);
    if (step > 0) v = std::round(v / step) * step;
    cursor = "hand";
  }
  if (hov && in.wheel != 0 && !held) {
    float st = step > 0 ? step : (hi - lo) / 100;
    v = clampv(v + (in.wheel > 0 ? st : -st), lo, hi);
    wheelLeft_ = 0;
  }
  if (!lbl.empty()) {
    text({r.x, r.y, lw - 6 * s, r.h}, lbl, 12.5f * s, c.text);
    if (const string* h = helpOf(lbl)) help(std::min(r.x + textW(lbl, 12.5f * s) + 5 * s, r.x + lw - 20 * s), r.y + r.h / 2, *h);
  }
  float t = clampv((v - lo) / std::max(1e-9f, hi - lo), 0.f, 1.f);
  float cy = tr.y + tr.h / 2, th = 3 * s;
  fill({tr.x, cy - th / 2, tr.w, th}, dark ? Color::hex(0x48484a) : Color::hex(0xdcdcde), th / 2);
  fill({tr.x, cy - th / 2, tr.w * t, th}, c.accent, th / 2);
  float kr = 7 * s;
  circle(tr.x + tr.w * t, cy + 0.7f * s, kr, Color(0, 0, 0, dark ? 0.35f : 0.1f));
  circle(tr.x + tr.w * t, cy, kr, dark ? Color::hex(0xd8d8da) : Color(1, 1, 1));
  circle(tr.x + tr.w * t, cy, kr, held ? c.accent : c.borderStrong, false, 1 * s);
  char buf[32];
  snprintf(buf, sizeof buf, fmt, double(v));
  text({r.r() - vw, r.y, vw, r.h}, buf, 12 * s, c.textDim, AL_RIGHT, 400);
  return v != old;
}

bool Ui::sliderInt(const Rect& r, const string& lbl, int& v, int lo, int hi) {
  float f = float(v);
  bool ch = slider(r, lbl, f, float(lo), float(hi), "%.0f", 1);
  v = int(std::lround(f));
  return ch;
}

bool Ui::segmented(const Rect& r, const vector<string>& opts, int& sel, const string& key) {
  bool changed = false;
  fill(r, dark ? Color(1, 1, 1, 0.07f) : Color(0, 0, 0, 0.055f), 7 * s);
  float w = (r.w - 4 * s) / float(std::max<size_t>(1, opts.size()));
  for (size_t i = 0; i < opts.size(); i++) {
    Rect b{r.x + 2 * s + w * float(i), r.y + 2 * s, w, r.h - 4 * s};
    uint64_t idv = id("seg:" + key + opts[i] + std::to_string(i));
    bool hov = false;
    if (behave(idv, b, &hov) && sel != int(i)) { sel = int(i); changed = true; }
    if (sel == int(i)) {
      fill({b.x, b.y + 0.8f * s, b.w, b.h}, Color(0, 0, 0, dark ? 0.3f : 0.07f), 5.5f * s);
      fill(b, dark ? Color::hex(0x636366) : Color::hex(0xffffff), 5.5f * s);
    } else if (hov) fill(b, c.hover, 5.5f * s);
    else if (i > 0 && sel != int(i) - 1) line(b.x, b.y + 5 * s, b.x, b.b() - 5 * s, c.border);
    text(b, opts[i], 12 * s, sel == int(i) ? c.text : c.text.withA(0.8f), AL_CENTER, sel == int(i) ? 600 : 400);
  }
  return changed;
}

bool Ui::combo(const Rect& r, const string& key, const vector<string>& opts, int& sel) {
  uint64_t idv = id("cmb:" + key);
  bool hov = false;
  if (behave(idv, r, &hov)) { if (popup_ == id("cmbp:" + key)) popup_ = 0; else openPopup("cmbp:" + key); }
  bool open = isPopupOpen("cmbp:" + key);
  fill(r, hov ? (dark ? Color::hex(0x444446) : Color::hex(0xf7f7f7)) : c.panel2, 6 * s);
  stroke(r, open ? c.accent : c.borderStrong.withA(0.75f), 6 * s);
  string cur = sel >= 0 && sel < int(opts.size()) ? opts[size_t(sel)] : "";
  text({r.x + 10 * s, r.y, r.w - 34 * s, r.h}, cur, 12.5f * s, c.text);
  icon("chev-down", r.r() - 15 * s, r.y + r.h / 2, 12 * s, c.textDim, 1.8f);
  bool changed = false;
  if (open) {
    int* selp = &sel;
    bool* chp = &changed;
    (void)chp;
    vector<string> o = opts;
    Rect anchor = r;
    string k = key;
    // The overlay runs after the frame; selection is applied through a pending map.
    static std::map<uint64_t, int> pending;
    auto pit = pending.find(idv);
    if (pit != pending.end()) { if (sel != pit->second) { sel = pit->second; changed = true; } pending.erase(pit); popup_ = 0; }
    else {
      int cur2 = *selp;
      overlay([this, o, anchor, idv, cur2, k]() {
        float rh = 28 * s;
        float h = std::min(float(o.size()) * rh + 8 * s, 360 * s);
        float y = anchor.b() + 4 * s;
        if (y + h > g->H - 8) y = anchor.y - h - 4 * s;
        float wantW = 0;
        for (auto& op : o) wantW = std::max(wantW, textW(op, 12.5f * s) + 40 * s);
        Rect pr{anchor.x, y, std::max({anchor.w, 160 * s, std::min(wantW, 420 * s)}), h};
        if (pr.r() > g->W - 8) pr.x = std::max(8.f, anchor.r() - pr.w);
        popupRect(pr);
        shadow(pr, 8 * s, 12 * s);
        fill(pr, c.card, 8 * s);
        stroke(pr, c.border, 8 * s);
        beginScroll("cmbscroll:" + k, inset(pr, 4 * s, 4 * s));
        float yy = pr.y + 4 * s - scrollY();
        for (size_t i = 0; i < o.size(); i++) {
          Rect ir{pr.x + 4 * s, yy, pr.w - 8 * s, rh};
          uint64_t iid = id("cmbi:" + k + std::to_string(i));
          bool h2 = false;
          if (behave(iid, ir, &h2)) { pending[idv] = int(i); animating = true; }
          if (h2) fill(ir, c.accent, 5 * s);
          if (int(i) == cur2) icon("check", ir.x + 12 * s, ir.y + rh / 2, 11 * s, h2 ? c.accentText : c.text, 2.f);
          text({ir.x + 24 * s, ir.y, ir.w - 30 * s, ir.h}, o[i], 12.5f * s, h2 ? c.accentText : c.text);
          yy += rh;
        }
        endScroll(float(o.size()) * rh);
      });
    }
  }
  return changed;
}

bool Ui::textInput(const Rect& r, const string& key, string& buf, const string& placeholder, bool* submitted, const string& ic) {
  uint64_t idv = id("ti:" + key);
  recordHover(idv, r);
  if (scrollStack_.empty()) hitRects_.push_back(r);
  bool mask = maskNext;
  maskNext = false;
  bool hov = touchHit(idv, r);
  if (hov) { hotNext_ = idv; cursor = "ibeam"; }
  bool changed = false;
  float padL = (ic.empty() ? 9 : 30) * s;
  float fs = 13 * s;
  auto caretX = [&](int k) { return textW(fromU32(ebuf_.substr(0, size_t(k))), fs); };
  auto posFromX = [&](float mx) {
    float x = mx - (r.x + padL) + escroll_;
    int best = 0;
    float bd = 1e9f;
    for (int k = 0; k <= int(ebuf_.size()); k++) { float d = std::fabs(caretX(k) - x); if (d < bd) { bd = d; best = k; } }
    return best;
  };
  if (in.pressed[0]) {
    if (hov) {
      if (focus != idv) { focus = idv; ebuf_ = toU32(buf); escroll_ = 0; }
      caret_ = anchor_ = posFromX(in.mx);
      if (in.dbl) { anchor_ = 0; caret_ = int(ebuf_.size()); }
      mouseSel_ = true;
      caretT_ = time;
    } else if (focus == idv) focus = 0;
  }
  if (focus == idv && mouseSel_ && in.down[0]) { caret_ = posFromX(in.mx); }
  if (!in.down[0]) mouseSel_ = false;
  if (focus == idv) {
    if (toU32(buf) != ebuf_ && !changed && ebuf_.empty() && !buf.empty()) ebuf_ = toU32(buf);
    auto delSel = [&]() {
      if (caret_ == anchor_) return false;
      int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
      ebuf_.erase(size_t(a), size_t(b - a));
      caret_ = anchor_ = a;
      return true;
    };
    for (char32_t ch : in.chars) {
      if (ch < 32 || ch == 127) continue;
      delSel();
      ebuf_.insert(size_t(caret_), 1, ch);
      caret_++;
      anchor_ = caret_;
      changed = true;
    }
    for (int k : in.keys) {
      bool sh = in.shift;
      if (k == VK_LEFT) { if (caret_ > 0) caret_--; if (!sh) anchor_ = caret_; }
      else if (k == VK_RIGHT) { if (caret_ < int(ebuf_.size())) caret_++; if (!sh) anchor_ = caret_; }
      else if (k == VK_HOME) { caret_ = 0; if (!sh) anchor_ = caret_; }
      else if (k == VK_END) { caret_ = int(ebuf_.size()); if (!sh) anchor_ = caret_; }
      else if (k == VK_BACK) { if (!delSel() && caret_ > 0) { ebuf_.erase(size_t(caret_ - 1), 1); caret_--; anchor_ = caret_; } changed = true; }
      else if (k == VK_DELETE) { if (!delSel() && caret_ < int(ebuf_.size())) ebuf_.erase(size_t(caret_), 1); changed = true; }
      else if (k == 'A' && in.ctrl) { anchor_ = 0; caret_ = int(ebuf_.size()); }
      else if ((k == 'C' || k == 'X') && in.ctrl && caret_ != anchor_) {
        int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
        setClipboardText(hwnd, fromU32(ebuf_.substr(size_t(a), size_t(b - a))));
        if (k == 'X') { delSel(); changed = true; }
      } else if (k == 'V' && in.ctrl) {
        delSel();
        string clip = getClipboardText(hwnd);
        clip = replaceAll(replaceAll(clip, "\r", ""), "\n", " ");
        auto u = toU32(clip);
        ebuf_.insert(size_t(caret_), u);
        caret_ += int(u.size());
        anchor_ = caret_;
        changed = true;
      } else if (k == VK_RETURN) { if (submitted) *submitted = true; }
      else if (k == VK_ESCAPE) { focus = 0; }
      caretT_ = time;
    }
    if (changed) buf = fromU32(ebuf_);
    wantsCaret = true;  // the app wakes every ~0.5 s for the caret instead of rendering continuously
  }
  bool foc = focus == idv;
  fill(r, c.input, 6 * s);
  if (foc) stroke(inset(r, -1.5f * s, -1.5f * s), c.accent.withA(0.35f), 7.5f * s, 3 * s);
  stroke(r, foc ? c.accent : c.borderStrong.withA(0.75f), 6 * s, 1.f);
  if (!ic.empty()) icon(ic, r.x + 16 * s, r.y + r.h / 2, 15 * s, c.textFaint);
  Rect tr{r.x + padL, r.y, r.w - padL - 8 * s, r.h};
  pushClip(tr);
  string shown = foc ? fromU32(ebuf_) : buf;
  if (mask && !foc && shown.size() > 4) { size_t n = std::min<size_t>(shown.size() - 4, 16); shown = shown.substr(0, 4); for (size_t i = 0; i < n; i++) shown += "\xE2\x80\xA2"; }
  if (foc) {
    float cx = caretX(caret_);
    if (cx - escroll_ > tr.w - 4) escroll_ = cx - tr.w + 4;
    if (cx - escroll_ < 0) escroll_ = cx;
    if (caret_ != anchor_) {
      float a = caretX(std::min(caret_, anchor_)), b = caretX(std::max(caret_, anchor_));
      fill({tr.x + a - escroll_, r.y + 5 * s, b - a, r.h - 10 * s}, c.accent.withA(0.3f));
    }
  }
  if (shown.empty() && !placeholder.empty()) text({tr.x, tr.y, tr.w, tr.h}, placeholder, fs, c.textFaint);
  else {
    auto L = mkLayout(shown, fs, 400, false);
    if (L) {
      DWRITE_TEXT_METRICS m;
      L->GetMetrics(&m);
      dc()->DrawTextLayout(D2D1::Point2F(tr.x - (foc ? escroll_ : 0), r.y + (r.h - m.height) / 2), L.get(), g->br(c.text));
    }
  }
  if (foc && std::fmod(time - caretT_, 1.0) < 0.55) {
    float cx = tr.x + caretX(caret_) - escroll_;
    line(cx, r.y + 6 * s, cx, r.b() - 6 * s, c.text, 1.2f * s);
  }
  popClip();
  return changed;
}

bool Ui::numberInput(const Rect& r, const string& key, int& v, int lo, int hi, int step) {
  float bw = 24 * s;
  bool ch = false;
  Rect mid{r.x + bw, r.y, r.w - 2 * bw, r.h};
  fill(r, c.input, 6 * s);
  stroke(r, c.border, 6 * s);
  if (iconButton({r.x + 2 * s, r.y + 2 * s, bw - 4 * s, r.h - 4 * s}, "minus", "", false, v > lo)) { v = std::max(lo, v - step); ch = true; }
  if (iconButton({r.r() - bw + 2 * s, r.y + 2 * s, bw - 4 * s, r.h - 4 * s}, "plus", "", false, v < hi)) { v = std::min(hi, v + step); ch = true; }
  string sbuf = std::to_string(v);
  uint64_t fid = id("ti:" + key);
  static std::map<uint64_t, string> edit;
  if (focus == fid) { if (!edit.count(fid)) edit[fid] = sbuf; sbuf = edit[fid]; }
  bool sub = false;
  Rect tr{mid.x, mid.y + 1, mid.w, mid.h - 2};
  bool changed = textInput(tr, key, sbuf, "", &sub);
  if (focus == fid) {
    edit[fid] = sbuf;
    if (changed && isDigits(sbuf) && !sbuf.empty()) { int nv = clampv(toInt(sbuf), lo, hi); if (nv != v) { v = nv; ch = true; } }
  } else edit.erase(fid);
  if (mouseIn(r) && in.wheel != 0 && focus != fid) { int nv = clampv(v + (in.wheel > 0 ? step : -step), lo, hi); if (nv != v) { v = nv; ch = true; } wheelLeft_ = 0; }
  return ch;
}

bool Ui::numberInputD(const Rect& r, const string& key, double& v, double lo, double hi, double step, int dec) {
  float bw = 24 * s;
  bool ch = false;
  fill(r, c.input, 6 * s);
  stroke(r, c.border, 6 * s);
  if (iconButton({r.x + 2 * s, r.y + 2 * s, bw - 4 * s, r.h - 4 * s}, "minus", "", false, v > lo + 1e-12)) { v = std::max(lo, v - step); ch = true; }
  if (iconButton({r.r() - bw + 2 * s, r.y + 2 * s, bw - 4 * s, r.h - 4 * s}, "plus", "", false, v < hi - 1e-12)) { v = std::min(hi, v + step); ch = true; }
  text({r.x + bw, r.y, r.w - 2 * bw, r.h}, fmtFixed(v, dec), 13 * s, c.text, AL_CENTER, 500, true);
  if (mouseIn(r) && in.wheel != 0) { v = clampv(v + (in.wheel > 0 ? step : -step), lo, hi); ch = true; wheelLeft_ = 0; }
  return ch;
}

bool Ui::header(const Rect& r, const string& title, bool& open, const string& badge) {
  uint64_t idv = id("hd:" + title);
  bool hov = false;
  bool clicked = behave(idv, r, &hov);
  if (clicked) open = !open;
  text({r.x, r.y, r.w - 60 * s, r.h}, title, 12 * s, hov ? c.text : c.textDim, AL_LEFT, 600);
  icon(open ? "chev-down" : "chev-right", r.r() - 10 * s, r.y + r.h / 2, 11 * s, hov ? c.text : c.textFaint, 1.8f);
  if (!badge.empty()) text({r.x, r.y, r.w - 26 * s, r.h}, badge, 11 * s, c.textFaint, AL_RIGHT);
  return clicked;
}

bool Ui::colorSwatch(const Rect& r, const string& key, Color& col) {
  uint64_t idv = id("sw:" + key);
  bool hov = false;
  if (behave(idv, r, &hov)) { if (isPopupOpen("swp:" + key)) closePopup(); else openPopup("swp:" + key); }
  fill(r, col, 4 * s);
  stroke(r, hov ? c.text : c.borderStrong, 4 * s);
  bool changed = false;
  static std::map<uint64_t, Color> pending;
  auto pit = pending.find(idv);
  if (pit != pending.end()) { col = pit->second; pending.erase(pit); changed = true; closePopup(); }
  else if (isPopupOpen("swp:" + key)) {
    Rect anchor = r;
    overlay([this, anchor, idv]() {
      static const uint32_t P[] = {0xd9453d, 0xf0913a, 0xc8c436, 0x4cae4c, 0x3fbfbf, 0x3f7ec5, 0x4b5dbf, 0x9b6fd1, 0xe57fb5, 0xa0674b, 0x8c8c8c, 0x222222,
                                   0x0072B2, 0xE69F00, 0x009E73, 0xCC79A7, 0x56B4E9, 0xD55E00, 0xF0E442, 0x999999, 0x4f8ef7, 0xf2a33c, 0x4ec98a, 0xe168a8,
                                   0x4477AA, 0xEE6677, 0x228833, 0xCCBB44, 0x66CCEE, 0xAA3377, 0xBBBBBB, 0xffffff};
      float cs = 22 * s, gp = 5 * s;
      int per = 8;
      float w = per * (cs + gp) + gp, h = 4 * (cs + gp) + gp;
      float x = std::min(anchor.x, float(g->W) - w - 8), y = anchor.b() + 4 * s;
      if (y + h > g->H - 8) y = anchor.y - h - 4 * s;
      Rect pr{x, y, w, h};
      popupRect(pr);
      shadow(pr, 8 * s, 12 * s);
      fill(pr, c.card, 8 * s);
      stroke(pr, c.border, 8 * s);
      for (int k = 0; k < 32; k++) {
        Rect cr{x + gp + float(k % per) * (cs + gp), y + gp + float(k / per) * (cs + gp), cs, cs};
        bool h2 = false;
        if (behave(id("swc:" + std::to_string(idv) + ":" + std::to_string(k)), cr, &h2)) { pending[idv] = Color::hex(P[k]); animating = true; }
        fill(cr, Color::hex(P[k]), 4 * s);
        stroke(cr, h2 ? c.text : c.border, 4 * s, h2 ? 2 * s : 1);
      }
    });
  }
  return changed;
}

void Ui::progress(const Rect& r, double p, const Color& col) {
  fill(r, c.active, r.h / 2);
  if (p < 0) {  // indeterminate
    float t = float(std::fmod(time * 0.8, 1.0));
    float w = r.w * 0.3f, x = r.x + (r.w + w) * t - w;
    pushClip(r);
    fill({x, r.y, w, r.h}, col, r.h / 2);
    popClip();  // indeterminate: the app refreshes ~10x per second while a job runs
  } else fill({r.x, r.y, float(r.w * clampv(p, 0.0, 1.0)), r.h}, col, r.h / 2);
}

bool Ui::tab(const Rect& r, const string& lbl, bool selected) {
  uint64_t idv = id("tab:" + lbl);
  bool hov = false;
  bool clicked = behave(idv, r, &hov);
  if (selected) fill(inset(r, 1 * s, 4 * s), c.active, 6 * s);
  else if (hov) fill(inset(r, 1 * s, 4 * s), c.hover, 6 * s);
  text(r, lbl, 12.5f * s, selected ? c.text : c.textDim, AL_CENTER, selected ? 600 : 400);
  return clicked;
}

bool Ui::listRow(const Rect& r, const string& key, bool selected) {
  uint64_t idv = id("row:" + key);
  bool hov = false;
  bool clicked = behave(idv, r, &hov);
  if (selected) fill(r, c.accent.withA(0.16f), 4 * s);
  else if (hov) fill(r, c.hover, 4 * s);
  return clicked;
}

// ------------------------------------------------------------ scroll
void Ui::beginScroll(const string& key, const Rect& r) {
  uint64_t idv = id("scroll:" + key);
  auto& st = scroll_[idv];
  auto tit = scrollTarget_.find(idv);
  if (tit != scrollTarget_.end()) {
    float d = tit->second - st.first;
    if (std::fabs(d) < 0.5f) { st.first = tit->second; scrollTarget_.erase(tit); }
    else { st.first += d * 0.35f; animating = true; }
  }
  float maxY = std::max(0.f, st.second - r.h);
  st.first = clampv(st.first, 0.f, maxY);
  scrollStack_.push_back({idv, r, st.first});
  pushClip(r);
}

void Ui::endScroll(float contentH) {
  if (scrollStack_.empty()) return;
  Scroll sc = scrollStack_.back();
  scrollStack_.pop_back();
  popClip();
  auto& st = scroll_[sc.id];
  st.second = contentH;
  float maxY = std::max(0.f, contentH - sc.r.h);
  bool over = sc.r.has(in.mx, in.my) && !(modal_ && !modalLayer_) && (inOverlay_ || !(popupRectPrev_.w > 0 && popupRectPrev_.has(in.mx, in.my))) && !blocked(in.mx, in.my);
  if (!touchScrollId_ && in.touch && in.pressed[0] && over && maxY > 0) {
    touchScrollId_ = sc.id;
    touchScrollStartY_ = touchScrollLastY_ = in.my;
    touchScrollMoved_ = false;
  }
  if (touchScrollId_ == sc.id) {
    if (in.touchCancel) {
      touchScrollId_ = 0;
      touchScrollMoved_ = false;
    } else if (in.touch && in.down[0]) {
      if (!touchScrollMoved_ && std::fabs(in.my - touchScrollStartY_) >= 8 * s) {
        touchScrollMoved_ = true;
        active = 0;  // cancel a row/button press that became a pan gesture
      }
      if (touchScrollMoved_ && maxY > 0) {
        st.first = clampv(st.first - (in.my - touchScrollLastY_), 0.f, maxY);
        scrollTarget_.erase(sc.id);
      }
      touchScrollLastY_ = in.my;
    } else if (in.touch && in.released[0] && !in.down[0]) {
      touchScrollId_ = 0;
      touchScrollMoved_ = false;
    }
  }
  if (over && wheelLeft_ != 0 && maxY > 0) {
    float tgt = scrollTarget_.count(sc.id) ? scrollTarget_[sc.id] : st.first;
    scrollTarget_[sc.id] = clampv(tgt - wheelLeft_ * 64 * s, 0.f, maxY);
    wheelLeft_ = 0;
    animating = true;
  }
  if (maxY > 0) {
    float th = std::max(24 * s, sc.r.h * sc.r.h / contentH);
    float ty = sc.r.y + (sc.r.h - th) * (st.first / maxY);
    Rect bar{sc.r.r() - 6 * s, ty, 4 * s, th};
    uint64_t bid = sc.id ^ 0x5bd1e995;
    bool hov = false, held = false;
    behave(bid, {sc.r.r() - 10 * s, sc.r.y, 10 * s, sc.r.h}, &hov, &held);
    if (held) { float t = clampv((in.my - sc.r.y - th / 2) / std::max(1.f, sc.r.h - th), 0.f, 1.f); st.first = t * maxY; scrollTarget_.erase(sc.id); }
    fill(bar, (hov || held) ? c.textFaint : c.textFaint.withA(0.45f), 2 * s);
  }
}

// root-level scroll areas (left panel, inspector) can be scrolled from anywhere, whatever the id stack
void Ui::scrollTo(const string& key, float y) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char ch : "scroll:" + key) { h ^= ch; h *= 1099511628211ull; }
  scrollTarget_[h ? h : 1] = std::max(0.f, y);
}

void Ui::toast(const string& title, const string& msg, int kind, double dur) {
  Toast t;
  t.title = title; t.msg = msg; t.kind = kind; t.t0 = time; t.dur = dur;
  toasts.push_back(t);
  if (toasts.size() > 4) toasts.erase(toasts.begin());
}

}  // namespace win
}  // namespace vs
