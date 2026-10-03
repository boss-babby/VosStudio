// VOSStudio Native: the split view — the main area in two or four panes, each with a chart of the loaded data, one
// of them optionally the live map. Analysts compare what interests them side by side (publications per year next to
// the top sources, the map next to the strategic diagram ...). Each pane renders through the same scene cache as the
// full-size charts (a chart is rebuilt only when the data or the pane size changes, then drawn from a bitmap), so a
// dashboard costs no more per frame than one chart; the map pane is the ordinary map, only smaller.
//
// 1.16 linked views: a pane can also be a second live view of the map ("live:overlay", "live:density", ...). Each
// linked view is its own NetView — own camera, own view kind, rendered by the GPU in the same pass as the main map —
// on the same network, style and positions. Selection, cluster filter, search hits and the hovered item are shared,
// so pointing at an item in one view highlights it in all of them; a click selects it everywhere, a double-click
// brings the main map to it. Panning and zooming are per pane. The main map keeps its full tool set (dragging items,
// box selection, minimap, legends); linked views are for looking, comparing and navigating.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.h"

namespace vs {
namespace win {

const vector<ChartChoice>& chartChoices() {
  static const vector<ChartChoice> k = {
      {"live:network", "Linked view: network", true},
      {"live:overlay", "Linked view: overlay (average year)", true},
      {"live:density", "Linked view: density", true},
      {"live:timeline", "Linked view: timeline", true},
      {"live:geo", "Linked view: geography", true},
      {"live:3d", "Linked view: 3D", true},
      {"publications_per_year", "Publications per year", false},
      {"top_sources", "Top sources", false},
      {"top_authors", "Top authors", false},
      {"top_countries", "Top countries", false},
      {"top_organisations", "Top organisations", false},
      {"most_cited", "Most cited papers", false},
      {"citation_classes", "Citation classes", false},
      {"trend_topics", "Trend topics", false},
      {"keyword_bursts", "Keyword bursts", false},
      {"thematic_evolution", "Thematic evolution", false},
      {"three_field", "Three-field plot", false},
      {"country_collaboration", "Country collaboration", false},
      {"production_over_time", "Authors' production over time", false},
      {"bradford", "Bradford's law", false},
      {"lotka", "Lotka's law", false},
      {"map_network", "Map picture: network", true},
      {"map_overlay", "Map picture: overlay (average year)", true},
      {"map_density", "Map picture: density", true},
      {"map_timeline", "Map picture: timeline", true},
      {"map_geo", "Map picture: geography", true},
      {"map_3d", "Map picture: 3D", true},
      {"map_matrix", "Map picture: matrix", true},
      {"strategic_diagram", "Strategic diagram", true}};
  return k;
}

namespace {
const char* paneLabel(const string& id) {
  if (id == "map") return "Live map";
  for (auto& c : chartChoices()) if (id == c.id) return c.label;
  return id.c_str();
}
const char* liveSuffix(ViewKind k) {
  switch (k) {
    case ViewKind::Overlay: return "overlay";
    case ViewKind::Density: return "density";
    case ViewKind::Timeline: return "timeline";
    case ViewKind::Geo: return "geo";
    case ViewKind::ThreeD: return "3d";
    default: return "network";
  }
}
// The pane's content picker (a popup under its chip): the live map, the linked views, the map pictures, the charts.
void panePicker(App& A, const Rect& anchor, int pane) {
  const float s = A.ui.s;
  A.ui.overlay([&A, anchor, pane, s]() {
    Ui& ui = A.ui;
    const auto& ch = chartChoices();
    const float rowH = 24 * s, headH = 22 * s;
    struct Row { string id; string label; bool head; };
    vector<Row> rows;
    rows.push_back({"map", "Live map", false});
    auto section = [&](const char* title, int group) {  // 0 linked views, 1 map pictures, 2 charts
      bool any = false;
      for (auto& c : ch) {
        string id = c.id;
        int g = startsWith(id, "live:") ? 0 : startsWith(id, "map_") || id == "strategic_diagram" ? 1 : 2;
        if (g != group) continue;
        if (!any) { rows.push_back({"", title, true}); any = true; }
        rows.push_back({id, c.label, false});
      }
    };
    section("Linked views of the map (live)", 0);
    section("Map pictures", 1);
    section("Charts", 2);
    float hgt = 8 * s;
    for (auto& r : rows) hgt += r.head ? headH : rowH;
    const float maxH = float(A.g.H) - 16 * s;
    Rect pr{anchor.r() - 280 * s, anchor.b() + 4 * s, 280 * s, std::min(hgt, maxH)};
    pr.y = std::min(pr.y, float(A.g.H) - pr.h - 8 * s);
    pr.x = std::max(pr.x, 4 * s);
    pr.y = std::max(pr.y, 4 * s);
    ui.shadow(pr, 8 * s);
    ui.fill(pr, ui.c.panel2, 8 * s);
    ui.stroke(pr, ui.c.border, 8 * s);
    ui.popupRect(pr);
    Rect inner{pr.x, pr.y + 4 * s, pr.w, pr.h - 8 * s};
    ui.beginScroll("panepick", inner);
    float yy = inner.y - ui.scrollY();
    const float y0 = yy;
    for (auto& r : rows) {
      if (r.head) {
        ui.text({pr.x + 12 * s, yy + 4 * s, pr.w - 24 * s, headH - 4 * s}, r.label, 10.5f * s, ui.c.textFaint, AL_LEFT, 650);
        yy += headH;
        continue;
      }
      Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, rowH - 1};
      bool live = r.id == "map" || startsWith(r.id, "live:");
      bool en = r.id == "map" ? A.hasMap() : true;
      for (auto& c : ch) if (r.id == c.id && c.needsMap && !A.hasMap()) en = false;
      if (r.id == "live:geo" && !A.geoAvailable()) en = false;
      bool cur = A.splitChart[pane] == r.id;
      if (en && ui.listRow(rr, "panepick:" + r.id, cur)) { ui.closePopup(); A.setSplitChart(pane, r.id); }
      if (live && en) ui.circle(rr.x + 11 * s, rr.y + rr.h / 2, 3 * s, ui.c.accent);
      ui.text({rr.x + (live ? 20 * s : 10 * s), rr.y, rr.w - 24 * s, rr.h}, r.label, 12.5f * s, en ? ui.c.text : ui.c.textFaint);
      yy += rowH;
    }
    ui.endScroll(yy - y0 + 4 * s);
  });
}
}  // namespace

bool App::splitActive() const {
  return splitMode > 0 && hasMap() && !writerOpen && !(papersOpen && hasCorpus()) && page != PG_AI && !mainChartOpen && view != ViewKind::Matrix;
}

int App::splitMapPane() const {
  if (!splitActive()) return 0;
  int n = splitPanes();
  if (splitMax >= 0 && splitMax < n) return splitChart[splitMax] == "map" ? splitMax : -1;
  for (int i = 0; i < n; i++) if (splitChart[i] == "map") return i;
  return -1;
}

Rect App::splitRect(int pane) const {
  const Rect& m = mainR;
  if (!splitActive()) return m;
  int n = splitPanes();
  if (splitMax >= 0 && splitMax < n) return pane == splitMax ? m : Rect{m.x, m.y, 0, 0};
  const float gap = 1;  // a hairline between panes
  if (splitMode == 1) { float w = (m.w - gap) / 2; return pane == 0 ? Rect{m.x, m.y, std::floor(w), m.h} : Rect{m.x + std::floor(w) + gap, m.y, m.w - std::floor(w) - gap, m.h}; }
  if (splitMode == 2) { float h = (m.h - gap) / 2; return pane == 0 ? Rect{m.x, m.y, m.w, std::floor(h)} : Rect{m.x, m.y + std::floor(h) + gap, m.w, m.h - std::floor(h) - gap}; }
  float w = std::floor((m.w - gap) / 2), h = std::floor((m.h - gap) / 2);
  float x = pane % 2 ? m.x + w + gap : m.x, y = pane / 2 ? m.y + h + gap : m.y;
  return {x, y, pane % 2 ? m.w - w - gap : w, pane / 2 ? m.h - h - gap : h};
}

void App::setSplit(int mode) {
  mode = clampv(mode, 0, 3);
  if (mode == splitMode) return;
  splitMode = mode;
  splitMax = -1;
  if (mode) { mainChartOpen = false; papersOpen = false; if (writerOpen) closeWriter(); if (page == PG_AI) page = PG_TRENDS; }
  static const char* kDefaults[4] = {"map", "live:density", "publications_per_year", "top_sources"};
  for (int i = 0; i < 4; i++) if (splitChart[i].empty()) splitChart[i] = kDefaults[i];
  splitSave();
  nv.nodesDirty = nv.linksDirty = true;
  needFrame = true;
}

void App::setSplitChart(int pane, const string& id) {
  if (pane < 0 || pane >= 4) return;
  string want = id;
  // one live map (it is the app's own canvas); the pane that had it gets this pane's previous content. Linked views
  // ("live:…") are separate views and can be used in any number of panes.
  if (want == "map") for (int i = 0; i < 4; i++) if (i != pane && splitChart[i] == "map") splitChart[i] = splitChart[pane].empty() || splitChart[pane] == "map" ? string("live:network") : splitChart[pane];
  splitChart[pane] = want;
  paneState_[pane] = PaneState();
  livePane_[pane].fitted = false;
  livePane_[pane].hover = -1;
  livePane_[pane].drag = 0;
  splitSave();
  needFrame = true;
}

void App::splitSave() {
  Json j = Json::object();
  j.set("mode", double(splitMode));
  Json arr = Json::array();
  for (int i = 0; i < 4; i++) arr.push(splitChart[i]);
  j.set("charts", arr);
  settings.j.set("split", j);
  settings.save();
}

void App::splitLoad() {
  const Json& j = settings.j["split"];
  if (j.t != Json::Obj) return;
  splitMode = clampv(int(j["mode"].num(0)), 0, 3);
  const Json& arr = j["charts"];
  if (arr.t == Json::Arr) for (size_t i = 0; i < arr.a.size() && i < 4; i++) { string id = arr.a[i].str(); if (!id.empty()) splitChart[i] = id; }
}

// ---- drawing: the chart panes (the map pane is drawn by the ordinary canvas code inside canvasR)
void App::drawSplitPanes() {
  int n = splitPanes();
  float s = ui.s;
  for (int i = 0; i < n; i++) {
    Rect r = splitRect(i);
    if (r.w < 2 || r.h < 2) continue;
    if (splitChart[i] == "map") {
      // the map pane keeps the map's own chrome; its pane controls take the place of the view chip (top right):
      // [ network · 312 terms ▾ ] [ expand ]   — the picker's label is the chip text, so nothing is lost
      const Network& N = P->net;
      string chip = string(viewLabel(view)) + "  \xC2\xB7  " + plural(N.n(), N.unitNoun);
      float cw = ui.textW(chip, 11.5f * s, 600) + 44 * s;
      float bx = r.r() - 14 * s;
      Rect ex{bx - 28 * s, r.y + 14 * s, 28 * s, 28 * s};
      bx = ex.x - 6 * s;
      Rect pick{bx - cw, r.y + 14 * s, cw, 28 * s};
      if (pick.x < r.x + 8 * s) { pick.x = r.x + 8 * s; pick.w = std::max(60 * s, ex.x - 6 * s - pick.x); }
      addChrome({pick.x, pick.y, ex.r() - pick.x, 28 * s});
      ui.fill(ex, ui.c.card, 14 * s);
      ui.stroke(ex, ui.c.border, 14 * s);
      if (splitMax == i) { if (ui.iconButton(ex, "win-restore", "Back to the split view")) { splitMax = -1; needFrame = true; } }
      else if (ui.iconButton(ex, "expand", "The map alone for a moment (Esc restores the panes)")) { splitMax = i; needFrame = true; }
      string key = "pane" + std::to_string(i);
      if (ui.button(pick, chip, BTN_NORMAL, "chev-down")) ui.openPopup(key);
      if (ui.isPopupOpen(key)) panePicker(*this, pick, i);
      continue;
    }
    if (liveId(splitChart[i])) { drawLivePane(i, r); continue; }
    drawSplitPane(i, r);
  }
  // hairlines between the panes
  if (splitMax < 0) {
    Color ln = ui.c.border;
    if (splitMode == 1 || splitMode == 3) { Rect a = splitRect(0), b = splitRect(1); ui.fill({a.r(), mainR.y, b.x - a.r(), mainR.h}, ln); }
    if (splitMode == 2) { Rect a = splitRect(0), b = splitRect(1); ui.fill({mainR.x, a.b(), mainR.w, b.y - a.b()}, ln); }
    if (splitMode == 3) { Rect a = splitRect(0), c = splitRect(2); ui.fill({mainR.x, a.b(), mainR.w, c.y - a.b()}, ln); }
  }
}

void App::drawSplitPane(int i, const Rect& r) {
  float s = ui.s;
  const string& id = splitChart[i];
  PaneState& ps = paneState_[i];
  if (ps.id != id || !ps.ready) {
    ps = PaneState();
    ps.id = id;
    Json args = Json::object();
    args.set("chart", id);
    string summary;
    if (!makeChartDef(id, args, ps.def, ps.err, &summary)) ps.def = ChartDef();
    ps.ready = true;
  }
  ui.fill(r, ui.c.bg);
  // header: title, picker, export, send to the writer, maximise / restore
  Rect hdr{r.x, r.y, r.w, 30 * s};
  float bx = hdr.r() - 6 * s;
  auto ib = [&](const char* icon, const char* tip, bool en = true) { bx -= 26 * s; return ui.iconButton({bx, hdr.y + 3 * s, 24 * s, 24 * s}, icon, tip, false, en); };
  if (splitMax == i) { if (ib("win-restore", "Back to the split view")) { splitMax = -1; needFrame = true; } }
  else if (ib("expand", "This pane alone for a moment")) { splitMax = i; needFrame = true; }
  if (ib("writer", "Insert this chart into the document (Writer)", ps.def.valid())) { writerInsertChart(id); ui.toast("Figure added", string(paneLabel(id)) + " is in the document at the caret.", 1, 3); }
  if (ib("download", "Save as PNG", ps.def.valid())) cmdExportChart(ps.def, "png");
  Rect pick{bx - 30 * s, hdr.y + 3 * s, 26 * s, 24 * s};
  string key = "pane" + std::to_string(i);
  if (ui.iconButton(pick, "chev-down", "Choose what this pane shows")) ui.openPopup(key);
  bx = pick.x;
  ui.text({hdr.x + 12 * s, hdr.y, bx - hdr.x - 16 * s, hdr.h}, ps.def.valid() ? ps.def.title : string(paneLabel(id)), 13 * s, ui.c.text, AL_LEFT, 650);
  if (ui.isPopupOpen(key)) panePicker(*this, pick, i);
  Rect card{r.x + 8 * s, hdr.b(), r.w - 16 * s, r.h - hdr.h - 8 * s};
  if (card.w < 40 * s || card.h < 40 * s) return;
  ui.fill(card, ui.c.card, 8 * s);
  ui.stroke(card, ui.c.border, 8 * s);
  if (!ps.def.valid()) {
    ui.textWrap({card.x + 16 * s, card.y + 16 * s, card.w - 32 * s, card.h - 32 * s}, ps.err.empty() ? "Choose a chart for this pane." : ps.err, 12.5f * s, ui.c.textDim);
    return;
  }
  ChartTheme t = chartTheme(false);
  double sw = (card.w - 20 * s) / s, sh = (card.h - 20 * s) / s;
  string ck = "#pane" + std::to_string(i) + "|" + id;
  ChartCacheEntry& ce = chartCache[ck];
  uint64_t sig = chartSig();
  if (chartFresh > 0 || ce.frame == 0 || ce.sig != sig || ce.w != float(sw) || ce.h != float(sh) || ce.dark != ui.dark) {
    ce.sc = ps.def.make(sw, sh, t);
    ce.sig = sig; ce.w = float(sw); ce.h = float(sh); ce.dark = ui.dark; ce.gen++;
  }
  ce.frame = std::max(1, frameNo);
  const Scene& sc = ce.sc;
  float k = 1;
  if (sc.W > 0 && sc.H > 0 && (std::fabs(sc.W - sw) > 0.5 || std::fabs(sc.H - sh) > 0.5)) k = float(std::min(sw / sc.W, sh / sc.H));
  float ox = card.x + 10 * s + float((sw - sc.W * k) * s) / 2, oy = card.y + 10 * s + float((sh - sc.H * k) * s) / 2;
  if (k == 1) { ox = card.x + 10 * s; oy = card.y + 10 * s; }
  const float ks = s * k;
  drawChartCached(ck, ce.gen, sc, ox, oy, ks);
  addChartHoverRegion(sc, card, ox, oy, ks, ui.id("chart-hover:split:" + std::to_string(i) + ":" + id));
  if (card.has(ui.in.mx, ui.in.my) && !ui.anyPopup() && !ui.anyModal()) chartHover(sc, ox, oy, ks);
  if (card.has(ui.in.mx, ui.in.my) && ps.def.onClick) {
    int tag = hitTag(sc, (ui.in.mx - ox) / ks, (ui.in.my - oy) / ks);
    if (tag >= 0) { ui.cursor = "hand"; if (ui.in.released[0] && ui.active == 0) ps.def.onClick(tag); }
  }
}

// ------------------------------------------------------------------ linked views (live panes)
bool App::liveId(const string& id, ViewKind* kind) {
  if (!startsWith(id, "live:")) return false;
  string k = id.substr(5);
  ViewKind vk = ViewKind::Network;
  if (k == "overlay") vk = ViewKind::Overlay;
  else if (k == "density") vk = ViewKind::Density;
  else if (k == "timeline") vk = ViewKind::Timeline;
  else if (k == "geo") vk = ViewKind::Geo;
  else if (k == "3d") vk = ViewKind::ThreeD;
  else if (k != "network") return false;
  if (kind) *kind = vk;
  return true;
}

bool App::liveAny() const {
  for (auto& L : livePane_) if (L.ready && L.vp.w > 0) return true;
  return false;
}

bool App::liveClean() const {
  const Input& in = ui.in;
  for (auto& L : livePane_) {
    if (!L.ready || L.vp.w <= 0) continue;
    if (L.nv.nodesDirty || L.nv.linksDirty || L.drag) return false;
    if (L.vp.has(in.mx, in.my) && (in.down[0] || in.down[1] || in.down[2] || in.wheel != 0)) return false;
  }
  return true;
}

int App::liveHover() const {
  for (auto& L : livePane_) if (L.ready && L.vp.w > 0 && L.hover >= 0) return L.hover;
  return -1;
}

uint64_t App::liveSignature() const {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  auto w = [&](uint64_t x) { h = (h ^ x) * 0x9E3779B97F4A7C15ull; h ^= h >> 31; };
  auto d = [&](double x) { uint64_t u; memcpy(&u, &x, sizeof u); w(u); };
  for (auto& L : livePane_) {
    const bool on = L.ready && L.vp.w > 0;
    w(on ? 1 : 0);
    if (!on) continue;
    w(uint64_t(L.kind)); d(L.vp.x); d(L.vp.y); d(L.vp.w); d(L.vp.h);
    d(L.nv.cam.x); d(L.nv.cam.y); d(L.nv.cam.zoom); d(L.nv.cam.yaw); d(L.nv.cam.pitch); d(L.nv.cam.dist3); d(L.nv.fitZoom());
    w(uint64_t(int64_t(L.hover) + 1)); w(L.flagsSig); w(L.posSig); w(uint64_t(L.nv.geoTint)); w(L.nv.hideMarks ? 1 : 0);
  }
  return h;
}

// Before the GPU pass: which panes are linked views this frame, their views created on first use, data, positions,
// flags and framing kept in step with the main map.
void App::liveSync() {
  const bool active = splitActive() && hasMap() && !readerOpen;  // the reader covers the main area (drawCanvas returns before the panes)
  const int n = active ? P->net.n() : 0;
  for (int i = 0; i < 4; i++) {
    LivePane& L = livePane_[i];
    ViewKind k = ViewKind::Network;
    const bool on = active && i < splitPanes() && liveId(splitChart[i], &k);
    Rect r = on ? splitRect(i) : Rect{0, 0, 0, 0};
    if (!on || r.w < 8 || r.h < 8) {
      L.vp = {0, 0, 0, 0};
      L.chrome = {0, 0, 0, 0};
      L.hover = -1;
      L.drag = 0;
      if (!on) L.fitted = false;
      continue;
    }
    if (!L.ready && !L.failed) {
      string err;
      L.ready = L.nv.init(g, &err);
      if (!L.ready) { L.failed = true; L.nv.lastError = err; }
      L.dataDirty = L.posDirty = true;
      L.fitted = false;
    }
    if (!L.ready) { L.vp = {0, 0, 0, 0}; continue; }
    if (k == ViewKind::Geo && !geoAvailable()) k = ViewKind::Network;
    if (k != L.kind) { L.kind = k; L.posDirty = true; L.fitted = false; L.asked3d = false; L.nv.visibilityChanged(); }
    if (k == ViewKind::ThreeD && !pos3dValid && !L.asked3d) {  // asked once per pane; the 2D positions show until the layout is ready
      L.asked3d = true;
      if (!busy()) start3DLayout();
    }
    // data: the same network, style and bundles as the main map; bound again when any of them moved or was rebuilt
    // (a new project, a new map) so that the view never keeps pointers into freed storage
    uint64_t ds = 0x2545F4914F6CDD1Dull;
    {
      auto w = [&](uint64_t x) { ds = (ds ^ x) * 0x9E3779B97F4A7C15ull; ds ^= ds >> 31; };
      w(reinterpret_cast<uintptr_t>(&P->net)); w(reinterpret_cast<uintptr_t>(P->net.nodes.data())); w(uint64_t(n)); w(uint64_t(P->net.links.size()));
      w(reinterpret_cast<uintptr_t>(&P->style)); w(reinterpret_cast<uintptr_t>(&P->bundles)); w(uint64_t(P->bundles.pts.size()));
    }
    if (L.dataDirty || ds != L.dataSig) {
      L.nv.setData(&P->net, &P->style, &P->bundles);
      L.dataDirty = false;
      L.dataSig = ds;
      L.nv.nodesDirty = L.nv.linksDirty = true;
    }
    // positions: the map, the kind, the 3D layout or the z scale changed
    uint64_t ps = 0x51ED270B27C4B7ull;
    {
      auto w = [&](uint64_t x) { ps = (ps ^ x) * 0x9E3779B97F4A7C15ull; ps ^= ps >> 31; };
      w(uint64_t(k)); w(uint64_t(n)); w(reinterpret_cast<uintptr_t>(P->net.nodes.data())); w(uint64_t(P->corpusVersion));
      if (k == ViewKind::Timeline) {
        int si = P->net.scoreIndex("Avg. pub. year");
        if (si < 0) si = P->net.scoreIdx;
        w(uint64_t(si)); w(uint64_t(P->net.scoreNames.size()));
      }
      w(pos3dValid ? 1 : 0);
      double zs = P->style.zScale;
      uint64_t u;
      memcpy(&u, &zs, sizeof u);
      w(u);
      w(uint64_t(geoNodeCountry.size()));
    }
    if (L.posDirty || ps != L.posSig || L.nv.pos.size() != size_t(n)) {
      L.nv.pos = targetPositions(k);
      L.nv.positionsChanged();
      L.posSig = ps;
      L.posDirty = false;
      L.nv.nodesDirty = L.nv.linksDirty = true;
    }
    L.nv.uiScale = ui.s;
    L.nv.kind = k;
    geoFitBounds(k, L.nv);
    // framing: fitted on first show and when the kind changes; kept when the pane is resized
    if (!L.fitted) { L.nv.vp = r; L.nv.fit(); L.fitted = true; }
    else if (k != ViewKind::ThreeD && L.nv.vp.w > 50 && L.nv.vp.h > 50 && (std::fabs(L.nv.vp.w - r.w) > 0.5f || std::fabs(L.nv.vp.h - r.h) > 0.5f)) {
      Camera keep = L.nv.cam;
      L.nv.fit();
      double f0 = L.nv.cam.zoom;
      L.nv.vp = r;
      L.nv.fit();
      double f1 = L.nv.cam.zoom;
      L.nv.cam = keep;
      if (f0 > 0) L.nv.cam.zoom *= f1 / f0;
    }
    L.nv.vp = r;
    L.vp = r;
    L.chrome = livePaneChromeRect(i, r);
    {  // Geo layers as in the main view
      bool g1 = k == ViewKind::Geo && geoMode() == 1;
      ViewKind tint = g1 && geoLayerKind == 1 ? ViewKind::Overlay : ViewKind::Geo;
      bool hide = g1 && geoLayerKind == 2;
      if (tint != L.nv.geoTint || hide != L.nv.hideMarks) { L.nv.geoTint = tint; L.nv.hideMarks = hide; L.nv.nodesDirty = L.nv.linksDirty = true; }
    }
    // Resolve each linked pane's hover before building flags and the canvas cache signature; the input frame then
    // paints the new target directly instead of scheduling a redundant follow-up frame.
    const bool overChrome = L.chrome.has(ui.in.mx, ui.in.my);
    const bool inside = r.has(ui.in.mx, ui.in.my) && !overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen && (ui.active == 0 || L.drag);
    L.hover = inside && !L.drag ? L.nv.hitNode(ui.in.mx, ui.in.my) : -1;
    // flags: shared selection, filters, hits and hover; what is hidden depends on the kind
    const int hv = hover >= 0 ? hover : liveHover();
    uint64_t fs = flagsSignature(k, hv);
    if (fs != L.flagsSig || L.nv.flags.size() != size_t(n)) {
      L.flagsSig = fs;
      vector<uint32_t> f;
      computeFlags(k, hv, f);
      if (f != L.nv.flags) {
        if (k == ViewKind::Geo || k == ViewKind::Timeline) L.nv.visibilityChanged();
        L.nv.flags.swap(f); L.nv.nodesDirty = L.nv.linksDirty = true;
      }
    }
  }
}

void App::liveBackground() {
  for (auto& L : livePane_) {
    if (!L.ready || L.vp.w <= 0) continue;
    if (L.kind == ViewKind::Geo && geoMode() > 0) {
      NetView* v = &L.nv;
      L.nv.backgroundPainter = [this, v](ID2D1DeviceContext* dc) { geoPaint(dc, *v); };
      L.nv.drawBackgroundLayer();
    } else L.nv.backgroundPainter = nullptr;
  }
}

void App::liveRender(const Theme& th) {
  for (auto& L : livePane_) if (L.ready && L.vp.w > 0) L.nv.renderGpu(th.bg);
}

void App::liveOverlay(const Theme& th) {
  for (auto& L : livePane_) if (L.ready && L.vp.w > 0) L.nv.drawOverlay(g.dc.get(), th, showLabels, P->style.maxLabels);
}

Rect App::livePaneChromeRect(int i, const Rect& r) {
  const float s = ui.s;
  const string chip = string(viewLabel(livePane_[i].kind)) + "  \xC2\xB7  linked";
  const float cw = ui.textW(chip, 11.5f * s, 600) + 44 * s;
  float bx = r.r() - 14 * s;
  Rect ex{bx - 28 * s, r.y + 14 * s, 28 * s, 28 * s};
  bx = ex.x - 6 * s;
  const int mapPane = splitMapPane();
  Rect sw{bx - 28 * s, r.y + 14 * s, 28 * s, 28 * s};
  if (mapPane >= 0) bx = sw.x - 6 * s;
  Rect pick{bx - cw, r.y + 14 * s, cw, 28 * s};
  if (pick.x < r.x + 8 * s) { pick.x = r.x + 8 * s; pick.w = std::max(60 * s, bx - pick.x); }
  return {pick.x, pick.y, ex.r() - pick.x, 28 * s};
}

// The pane's chrome and interaction (drawn after the canvas layer, like the map's own chrome).
void App::drawLivePane(int i, const Rect& r) {
  LivePane& L = livePane_[i];
  const float s = ui.s;
  const Input& in = ui.in;
  const Network& N = P->net;
  const bool shown = L.ready && L.vp.w > 0;
  if (!shown) {
    ui.fill(r, ui.c.bg);
    string msg = L.failed ? "This pane could not create its view: " + (L.nv.lastError.empty() ? string("graphics error") : L.nv.lastError) : string();
    if (!msg.empty()) ui.textWrap({r.x + 16 * s, r.y + 44 * s, r.w - 32 * s, 80 * s}, msg, 12.5f * s, ui.c.textDim);
  }
  Theme th = canvasTheme(P->style);
  // ---- chrome (top right): [ overlay · linked ▾ ] [ swap ] [ expand ]
  Rect chrome = L.chrome;
  string chip = string(viewLabel(L.kind)) + "  \xC2\xB7  linked";
  float cw = ui.textW(chip, 11.5f * s, 600) + 44 * s;
  float bx = r.r() - 14 * s;
  Rect ex{bx - 28 * s, r.y + 14 * s, 28 * s, 28 * s};
  bx = ex.x - 6 * s;
  const int mapPane = splitMapPane();
  Rect sw{bx - 28 * s, r.y + 14 * s, 28 * s, 28 * s};
  if (mapPane >= 0) bx = sw.x - 6 * s;
  Rect pick{bx - cw, r.y + 14 * s, cw, 28 * s};
  if (pick.x < r.x + 8 * s) { pick.x = r.x + 8 * s; pick.w = std::max(60 * s, bx - pick.x); }
  ui.fill(ex, ui.c.card, 14 * s);
  ui.stroke(ex, ui.c.border, 14 * s);
  if (splitMax == i) { if (ui.iconButton(ex, "win-restore", "Back to the split view")) { splitMax = -1; needFrame = true; } }
  else if (ui.iconButton(ex, "expand", "This view alone for a moment (Esc restores the panes)")) { splitMax = i; needFrame = true; }
  if (mapPane >= 0) {
    ui.fill(sw, ui.c.card, 14 * s);
    ui.stroke(sw, ui.c.border, 14 * s);
    if (ui.iconButton(sw, "shuffle", "Swap with the main map: this view becomes the main one (with all its tools), the main view comes here")) {
      ViewKind mainK = view, paneK = L.kind;
      if (paneK != ViewKind::ThreeD || pos3dValid) {
        setView(paneK, true);
        splitChart[i] = string("live:") + liveSuffix(mainK);
        L.fitted = false;
        splitSave();
        needFrame = true;
      } else ui.toast("3D layout", "The 3D layout is still being computed.", 2, 3);
    }
  }
  string key = "pane" + std::to_string(i);
  if (ui.button(pick, chip, BTN_NORMAL, "chev-down")) ui.openPopup(key);
  if (ui.isPopupOpen(key)) panePicker(*this, pick, i);
  if (!shown) return;
  // ---- timeline axis
  double tS0, tS1, tX0, tX1;
  timelineRange(&tS0, &tS1, &tX0, &tX1);
  if (L.kind == ViewKind::Timeline && tS1 > tS0) {
    Color dim = th.muted;
    float y = r.b() - 26 * s;
    ui.fill({r.x, y - 8 * s, r.w, 34 * s}, th.bg.withA(0.85f));
    ui.line(r.x + 16 * s, y, r.r() - 16 * s, y, dim.withA(0.5f));
    int y0 = int(std::floor(tS0)), y1 = int(std::ceil(tS1));
    int step = std::max(1, (y1 - y0) / std::max(2, int(r.w / (70 * s))));
    for (int yr = y0; yr <= y1; yr += step) {
      double wx = tX0 + (yr - tS0) / (tS1 - tS0) * (tX1 - tX0);
      float sx, sy;
      L.nv.worldToScreen(wx, 0, 0, sx, sy);
      if (sx < r.x + 10 * s || sx > r.r() - 10 * s) continue;
      ui.line(sx, y - 4 * s, sx, y + 4 * s, dim);
      ui.text({sx - 30 * s, y + 5 * s, 60 * s, 16 * s}, std::to_string(yr), 10.5f * s, dim, AL_CENTER, 600);
    }
  }
  if (L.kind == ViewKind::ThreeD && !pos3dValid) ui.text({r.x, r.b() - 26 * s, r.w, 18 * s}, "Computing the 3D layout\xE2\x80\xA6", 11 * s, th.muted, AL_CENTER);
  // ---- interaction
  bool overChrome = chrome.has(in.mx, in.my);
  if (mapPane >= 0 && sw.has(in.mx, in.my)) overChrome = true;
  const bool inside = r.has(in.mx, in.my) && !overChrome && !ui.anyPopup() && !ui.anyModal() && !paletteOpen && (ui.active == 0 || L.drag);
  const bool is3D = L.kind == ViewKind::ThreeD;
  int h = inside && !L.drag ? L.nv.hitNode(in.mx, in.my) : -1;
  if (h != L.hover) { L.hover = h; needFrame = true; }
  if (L.hover >= 0 && !L.drag) ui.cursor = "hand";
  const float zoomGesture = in.wheel + in.pinch;
  if (inside && zoomGesture != 0) {
    if (is3D) L.nv.cam.dist3 = clampv(L.nv.cam.dist3 * std::pow(0.88, double(zoomGesture)), 0.2, 6.0);
    else {
      double z0 = L.nv.cam.zoom;
      double z1 = clampv(z0 * std::pow(1.2, double(zoomGesture)), L.nv.fitZoom() * 0.15, L.nv.fitZoom() * 120);
      double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
      double mxw = L.nv.cam.x + (in.mx - cx) / z0, myw = L.nv.cam.y + (in.my - cy) / z0;
      L.nv.cam.zoom = z1;
      L.nv.cam.x = mxw - (in.mx - cx) / z1;
      L.nv.cam.y = myw - (in.my - cy) / z1;
    }
    needFrame = true;
  }
  if (inside && !L.drag && in.dbl) {
    if (L.hover >= 0) focusOn(L.hover);  // the main map goes there too
    else { L.nv.fit(); needFrame = true; }
  } else if (inside && !L.drag && (in.pressed[0] || in.pressed[1] || in.pressed[2])) {
    L.drag = 1;
    L.dragX0 = L.lastMx = in.mx;
    L.dragY0 = L.lastMy = in.my;
    L.dragMoved = false;
  }
  if (L.drag) {
    float dx = in.mx - L.lastMx, dy = in.my - L.lastMy;
    if (std::hypot(in.mx - L.dragX0, in.my - L.dragY0) > 3 * s) L.dragMoved = true;
    if (L.dragMoved) {
      if (is3D) { L.nv.cam.yaw += dx * 0.008f; L.nv.cam.pitch = clampv(L.nv.cam.pitch + dy * 0.006f, -1.45f, 1.45f); L.nv.nodesDirty = true; }
      else { L.nv.cam.x -= dx / L.nv.cam.zoom; L.nv.cam.y -= dy / L.nv.cam.zoom; }
      ui.cursor = "move";
      needFrame = true;
    }
    L.lastMx = in.mx;
    L.lastMy = in.my;
    if (!in.down[0] && !in.down[1] && !in.down[2]) {
      if (!L.dragMoved && in.released[0]) {
        if (L.hover >= 0) selectNode(L.hover, in.ctrl);
        else if (!in.ctrl) clearSelection();
        needFrame = true;
      }
      L.drag = 0;
    }
  }
  // ---- the hovered item
  if (L.hover >= 0 && L.hover < N.n() && !L.drag) {
    const Node& nd = N.nodes[size_t(L.hover)];
    string t1 = nd.label;
    string cn = N.clusterName(nd.cluster);
    string t2 = "Cluster " + std::to_string(nd.cluster + 1) + (cn.empty() ? string() : " \xC2\xB7 " + cn);
    if (N.weightIdx >= 0 && size_t(N.weightIdx) < nd.w.size() && size_t(N.weightIdx) < N.weightNames.size()) t2 += "  \xC2\xB7  " + N.weightNames[size_t(N.weightIdx)] + " " + fmtNum(nd.w[size_t(N.weightIdx)], 0);
    float w = std::max(ui.textW(t1, 12 * s, 600), ui.textW(t2, 11 * s)) + 24 * s;
    w = std::min(w, r.w - 16 * s);
    Rect card{in.mx + 14 * s, in.my + 16 * s, w, 44 * s};
    if (card.r() > r.r() - 6 * s) card.x = in.mx - w - 10 * s;
    if (card.b() > r.b() - 6 * s) card.y = in.my - card.h - 10 * s;
    ui.shadow(card, 8 * s, 10 * s);
    ui.fill(card, th.light ? Color(1, 1, 1, 0.94f) : Color(0.09f, 0.1f, 0.12f, 0.92f), 8 * s);
    ui.stroke(card, th.light ? Color(0, 0, 0, 0.1f) : Color(1, 1, 1, 0.1f), 8 * s);
    ui.text({card.x + 12 * s, card.y + 5 * s, card.w - 24 * s, 18 * s}, t1, 12 * s, th.fg, AL_LEFT, 600);
    ui.text({card.x + 12 * s, card.y + 23 * s, card.w - 24 * s, 16 * s}, t2, 11 * s, th.muted);
  }
}

}  // namespace win
}  // namespace vs
