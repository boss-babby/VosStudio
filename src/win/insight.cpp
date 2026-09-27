// VOSStudio Native 1.2 — top-left toolbar and map history, window title, panel animation,
// document preview and country profile in the inspector.
#include "../core/world.h"
#include "app.h"

namespace vs {
namespace win {

namespace {
float ease(float t) { t = clampv(t, 0.f, 1.f); return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }
string urlEncode(const string& s) {
  static const char* hx = "0123456789ABCDEF";
  string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
    else if (c == ' ') o += '+';
    else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
  }
  return o;
}
string clockNow() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[16];
  snprintf(b, sizeof b, "%02d:%02d", st.wHour, st.wMinute);
  return b;
}
}  // namespace

// =====================================================================================
// window title: the OS title bar carries the product and project name
// =====================================================================================
void App::updateWindowTitle() {
  string t;
  if (!P->path.empty()) t = fileName(P->path) + (P->dirty ? " \xE2\x80\xA2" : "") + " - VOSStudio";
  else if (hasCorpus()) t = string("Untitled project") + (P->dirty ? " \xE2\x80\xA2" : "") + " - VOSStudio";
  else t = "VOSStudio";
  if (t == windowTitle_) return;
  windowTitle_ = t;
  SetWindowTextW(hwnd, widen(t).c_str());
}

// =====================================================================================
// panel slide animation (≈200 ms, only renders while moving; off when Windows animations are off)
// =====================================================================================
bool App::canInspect() const {
  if (page == PG_AI) return docPreview >= 0 && hasCorpus();  // the assistant gets the full width; an open document preview still shows
  return hasMap() || (docPreview >= 0 && hasCorpus());
}

void App::stepPanelAnim() {
  if (page >= 0) leftShownPage = page;
  float tl = page >= 0 ? 1.f : 0.f, tr = inspectorOpen && canInspect() ? 1.f : 0.f;
  bool moving = leftT != tl || rightT != tr;
  float step = (reduceMotion || scriptMode) ? 1.f : float((panelMoving_ ? lastDtSec() : 1.0 / 60.0) / 0.2);
  auto adv = [&](float& v, float target) { v = v < target ? std::min(target, v + step) : std::max(target, v - step); };
  adv(leftT, tl);
  adv(rightT, tr);
  panelMoving_ = leftT != tl || rightT != tr;
  if (moving) ui.animating = true;
}
float App::leftEase() const { return ease(leftT); }
float App::rightEase() const { return ease(rightT); }

// =====================================================================================
// map history (maps built or opened in this session)
// =====================================================================================
string App::mapTitle() const {
  if (!hasMap()) return "No map";
  if (P->mapSource == "analysis") return string(typeInfo(P->spec.type).label) + " \xC2\xB7 " + unitLabel(P->spec.type, P->spec.unit);
  const string& d = P->net.description;
  if (!d.empty()) return titleCase(d.substr(0, 1)) + d.substr(1);
  return P->mapSource == "vosviewer" ? "VOSviewer map" : "Map";
}

void App::rememberMap() {
  if (!hasMap() || restoringMap) return;
  MapSnap m;
  m.net = P->net;
  m.spec = P->spec;
  m.params = P->params;
  m.bundles = P->bundles;
  m.last = P->last;
  m.mapSource = P->mapSource;
  m.title = mapTitle();
  m.when = clockNow();
  // the same map rebuilt (same kind and size) replaces its entry, anything else is a new entry
  if (mapCur >= 0 && mapCur < int(mapHistory.size())) {
    const MapSnap& c = mapHistory[size_t(mapCur)];
    if (c.title == m.title && c.net.n() == m.net.n() && c.net.m() == m.net.m()) { mapHistory[size_t(mapCur)] = std::move(m); return; }
  }
  mapHistory.push_back(std::move(m));
  if (mapHistory.size() > 8) mapHistory.erase(mapHistory.begin());
  mapCur = int(mapHistory.size()) - 1;
}

void App::restoreMap(int k) {
  if (k < 0 || k >= int(mapHistory.size()) || busy()) return;
  ui.closePopup();
  if (mapCur >= 0 && mapCur < int(mapHistory.size())) {  // keep edits (pins, layout, names) of the map we leave
    mapHistory[size_t(mapCur)].net = P->net;
    mapHistory[size_t(mapCur)].bundles = P->bundles;
  }
  const MapSnap& m = mapHistory[size_t(k)];
  P->net = m.net;
  P->spec = m.spec;
  P->params = m.params;
  P->bundles = m.bundles;
  P->last = m.last;
  P->mapSource = m.mapSource;
  P->builtSig = m.mapSource == "analysis" ? P->specSig() : string();
  P->metricsValid = false;
  P->dirty = true;
  undoStack.clear();
  redoStack.clear();
  restoringMap = true;
  afterMapChanged(true);
  restoringMap = false;
  mapCur = k;
  styleDirty = true;
  ui.toast("Switched map", m.title + " \xC2\xB7 " + plural(m.net.n(), m.net.unitNoun), 0, 2.5);
}

void App::mapsToProject() {
  P->maps.clear();
  P->mapCur = -1;
  if (mapCur >= 0 && mapCur < int(mapHistory.size())) {  // the current map carries the latest edits
    mapHistory[size_t(mapCur)].net = P->net;
    mapHistory[size_t(mapCur)].bundles = P->bundles;
  }
  for (size_t k = 0; k < mapHistory.size(); k++) {
    const MapSnap& m = mapHistory[k];
    SavedMap e;
    if (int(k) != mapCur) { e.net = m.net; e.spec = m.spec; e.params = m.params; e.mapSource = m.mapSource; }
    e.title = m.title; e.when = m.when;
    P->maps.push_back(std::move(e));
  }
  P->mapCur = mapCur;
}

void App::mapsFromProject() {
  if (P->maps.empty() || P->mapCur < 0) return;
  mapHistory.clear();
  for (size_t k = 0; k < P->maps.size(); k++) {
    SavedMap& e = P->maps[k];
    MapSnap m;
    if (int(k) == P->mapCur) { m.net = P->net; m.spec = P->spec; m.params = P->params; m.mapSource = P->mapSource; m.last = P->last; }
    else {
      m.net = std::move(e.net); m.spec = e.spec; m.params = e.params; m.mapSource = e.mapSource;
      m.last.n = m.net.n(); m.last.m = m.net.m(); m.last.clusters = m.net.nClusters; m.last.Q = m.net.quality;
    }
    m.title = e.title; m.when = e.when;
    mapHistory.push_back(std::move(m));
  }
  mapCur = P->mapCur;
  P->maps.clear();  // the App owns the history from here; it is written back on save
}

// =====================================================================================
// top bar, left block: file menu, save, undo/redo, map switcher
// =====================================================================================
void App::drawTopLeft(float& xEnd) {
  float s = ui.s;
  float x = 10 * s, y = topR.y + 10 * s, bh = 32 * s;
  // ---- file menu
  Rect fb{x, y, 46 * s, bh};
  uint64_t fid = ui.id("tl:file");
  bool hov = false;
  if (ui.behave(fid, fb, &hov)) { if (ui.isPopupOpen("filemenu")) ui.closePopup(); else ui.openPopup("filemenu"); }
  bool fopen = ui.isPopupOpen("filemenu");
  if (hov || fopen) ui.fill(fb, ui.c.hover, 7 * s);
  ui.icon("folder", fb.x + 16 * s, fb.y + bh / 2, 17 * s, hov || fopen ? ui.c.text : ui.c.textDim, 1.7f);
  ui.icon("chev-down", fb.x + 34 * s, fb.y + bh / 2 + 1 * s, 11 * s, ui.c.textFaint, 2.f);
  ui.tipFor(fid, "Open data or a project, new project, export, recent files");
  if (fopen) {
    ui.overlay([this, fb, s]() {
      struct It { const char* icon; const char* label; const char* key; int cmd; };
      static const It items[] = {{"folder", "Open data\xE2\x80\xA6", "Ctrl+O", 0}, {"file", "Open project\xE2\x80\xA6", "Ctrl+Shift+O", 1}, {"plus", "New project", "Ctrl+N", 2},
                                 {"save", "Save project as\xE2\x80\xA6", "Ctrl+Shift+S", 3}, {"sparkle", "Load the sample data", "", 4},
                                 {"download", "", "Ctrl+Shift+E", 5}, {"publish", "Publication figure\xE2\x80\xA6", "Ctrl+E", 6},
                                 {"settings", "Settings\xE2\x80\xA6", "Ctrl+,", 7}};
      const int NI = 8;
      auto rec = settings.recent();
      size_t nr = std::min<size_t>(6, rec.size());
      float rowH = 30 * s;
      Rect pr{fb.x, fb.b() + 4 * s, 290 * s, 8 * s + NI * rowH + 18 * s + (nr ? 30 * s + nr * rowH : 0) + 6 * s};
      ui.shadow(pr, 8 * s);
      ui.fill(pr, ui.c.panel2, 8 * s);
      ui.stroke(pr, ui.c.border, 8 * s);
      ui.popupRect(pr);
      float yy = pr.y + 4 * s;
      for (int i = 0; i < NI; i++) {
        if (i == 5 || i == 7) { ui.line(pr.x + 10 * s, yy + 4 * s, pr.r() - 10 * s, yy + 4 * s, ui.c.border); yy += 9 * s; }
        Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, rowH - 2 * s};
        bool en = i == 3 ? hasCorpus() : (i == 5 || i == 6) ? hasMap() : true;
        string label = i == 5 ? "Export the " + viewName(view) + " view\xE2\x80\xA6" : string(items[i].label);
        if (ui.listRow(rr, string("fm") + std::to_string(i), false) && en) {
          ui.closePopup();
          switch (items[i].cmd) {
            case 0: cmdOpenFiles(); break;
            case 1: cmdOpenProject(); break;
            case 2: cmdNewProject(); break;
            case 3: cmdSaveProject(true); break;
            case 4: cmdSample(false); break;
            case 5: cmdExportCurrentView(""); break;
            case 6: page = PG_PUBLISH; break;
            case 7: settingsWanted = true; break;
          }
        }
        Color fg = en ? ui.c.text : ui.c.textFaint;
        ui.icon(items[i].icon, rr.x + 14 * s, rr.y + rr.h / 2, 15 * s, en ? ui.c.textDim : ui.c.textFaint, 1.6f);
        ui.text({rr.x + 32 * s, rr.y, rr.w - 120 * s, rr.h}, label, 12.5f * s, fg);
        ui.text({rr.r() - 100 * s, rr.y, 92 * s, rr.h}, items[i].key, 11 * s, ui.c.textFaint, AL_RIGHT);
        yy += rowH;
      }
      if (nr) {
        ui.line(pr.x + 10 * s, yy + 6 * s, pr.r() - 10 * s, yy + 6 * s, ui.c.border);
        ui.text({pr.x + 14 * s, yy + 10 * s, pr.w, 18 * s}, "Recent projects", 12 * s, ui.c.textFaint, AL_LEFT, 600);
        yy += 30 * s;
        for (size_t i = 0; i < nr; i++) {
          Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, rowH - 2 * s};
          if (ui.listRow(rr, "fr" + std::to_string(i), false)) { ui.closePopup(); cmdOpenProject(rec[i]); return; }
          ui.icon("clock", rr.x + 14 * s, rr.y + rr.h / 2, 14 * s, ui.c.textFaint, 1.6f);
          ui.text({rr.x + 32 * s, rr.y, rr.w - 40 * s, rr.h}, fileName(rec[i]), 12.5f * s, ui.c.text);
          ui.tip(rec[i]);
          yy += rowH;
        }
      }
    });
  }
  x = fb.r() + 2 * s;
  // ---- save, undo, redo
  if (ui.iconButton({x, y, 32 * s, bh}, "save", P->dirty ? "Save project  (Ctrl+S)\nUnsaved changes" : "Save project  (Ctrl+S)", false, hasCorpus())) cmdSaveProject(false);
  if (P->dirty && hasCorpus()) ui.circle(x + 25 * s, y + 8 * s, 3 * s, ui.c.accent);
  x += 34 * s;
  if (ui.iconButton({x, y, 32 * s, bh}, "undo", "Undo  (Ctrl+Z)" + (undoStack.empty() ? string() : ": " + undoStack.back().what), false, !undoStack.empty())) undo();
  x += 34 * s;
  if (ui.iconButton({x, y, 32 * s, bh}, "redo", "Redo  (Ctrl+Y)" + (redoStack.empty() ? string() : ": " + redoStack.back().what), false, !redoStack.empty())) redo();
  x += 34 * s + 8 * s;
  ui.line(x, y + 6 * s, x, y + bh - 6 * s, ui.c.border);
  x += 9 * s;
  // ---- map switcher
  float cw = clampv(topR.w * 0.17f, 150 * s, 270 * s);
  Rect chip{x, y, cw, bh};
  uint64_t mid = ui.id("tl:maps");
  bool mh = false;
  if (ui.behave(mid, chip, &mh)) {
    if (!hasMap()) setPage(hasCorpus() ? PG_BUILD : PG_DATA);
    else if (ui.isPopupOpen("maphist")) ui.closePopup();
    else ui.openPopup("maphist");
  }
  bool mopen = ui.isPopupOpen("maphist");
  ui.fill(chip, mh || mopen ? ui.c.hover : ui.c.input, 8 * s);
  ui.stroke(chip, mopen ? ui.c.accent : ui.c.border, 8 * s);
  if (hasMap()) {
    ui.icon("map", chip.x + 15 * s, chip.y + bh / 2, 15 * s, ui.c.accent, 1.6f);
    string cnt = fmtInt(P->net.n());
    float cntW = ui.textW(cnt, 11 * s);
    ui.text({chip.x + 30 * s, chip.y, chip.w - 58 * s - cntW, bh}, mapTitle(), 12.5f * s, ui.c.text, AL_LEFT, 600);
    ui.text({chip.r() - 26 * s - cntW, chip.y, cntW, bh}, cnt, 11 * s, ui.c.textFaint, AL_RIGHT);
    ui.icon("chev-down", chip.r() - 13 * s, chip.y + bh / 2 + 1 * s, 11 * s, ui.c.textDim, 2.f);
    if (mapHistory.size() > 1) {  // badge: number of maps you can switch between
      string b = std::to_string(mapHistory.size());
      ui.circle(chip.r() - 3 * s, chip.y + 3 * s, 7 * s, ui.c.accent);
      ui.text({chip.r() - 10 * s, chip.y - 4 * s, 14 * s, 14 * s}, b, 9.5f * s, ui.c.accentText, AL_CENTER, 700);
    }
    ui.tipFor(mid, mapTitle() + " \xC2\xB7 " + plural(P->net.n(), P->net.unitNoun) + "\nClick to switch maps");
  } else {
    ui.icon("map", chip.x + 15 * s, chip.y + bh / 2, 15 * s, ui.c.textFaint, 1.6f);
    ui.text({chip.x + 30 * s, chip.y, chip.w - 36 * s, bh}, hasCorpus() ? "No map yet" : "No data loaded", 12 * s, ui.c.textDim);
    ui.tipFor(mid, hasCorpus() ? "Open the Build panel" : "Open the Data panel");
  }
  if (mopen && hasMap()) {
    ui.overlay([this, chip, s]() {
      int n = int(mapHistory.size());
      float rowH = 46 * s;
      Rect pr{chip.x, chip.b() + 4 * s, std::max(chip.w, 320 * s), 32 * s + n * rowH + 46 * s};
      ui.shadow(pr, 8 * s);
      ui.fill(pr, ui.c.panel2, 8 * s);
      ui.stroke(pr, ui.c.border, 8 * s);
      ui.popupRect(pr);
      ui.text({pr.x + 14 * s, pr.y + 8 * s, pr.w - 28 * s, 18 * s}, "Maps in this session", 12 * s, ui.c.textFaint, AL_LEFT, 600);
      ui.help(pr.x + 14 * s + ui.textW("Maps in this session", 10.5f * s, 700) + 6 * s, pr.y + 17 * s,
              "Every map you build or open is kept here (up to 8), with its layout, pins and cluster names. Click one to switch back to it instantly, without rebuilding.");
      float yy = pr.y + 32 * s;
      for (int k = n - 1; k >= 0; k--) {
        const MapSnap& m = mapHistory[size_t(k)];
        Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, rowH - 4 * s};
        bool cur = k == mapCur;
        if (ui.listRow(rr, "mh" + std::to_string(k), cur)) { ui.closePopup(); if (!cur) restoreMap(k); return; }
        ui.icon(cur ? "check" : "map", rr.x + 15 * s, rr.y + rr.h / 2, 15 * s, cur ? ui.c.accent : ui.c.textFaint, 1.7f);
        ui.text({rr.x + 34 * s, rr.y + 4 * s, rr.w - 90 * s, 18 * s}, m.title, 12.5f * s, ui.c.text, AL_LEFT, 600);
        ui.text({rr.r() - 60 * s, rr.y + 4 * s, 52 * s, 18 * s}, m.when, 11 * s, ui.c.textFaint, AL_RIGHT);
        string sub = plural(m.net.n(), m.net.unitNoun) + " \xC2\xB7 " + plural(m.net.m(), "link") + " \xC2\xB7 " + plural(m.net.nClusters, "cluster");
        if (m.mapSource == "analysis") sub += " \xC2\xB7 min " + std::to_string(m.spec.min);
        ui.text({rr.x + 34 * s, rr.y + 22 * s, rr.w - 40 * s, 16 * s}, sub, 11 * s, ui.c.textDim);
        yy += rowH;
      }
      if (ui.button({pr.x + 8 * s, pr.b() - 40 * s, pr.w - 16 * s, 32 * s}, "Build another map\xE2\x80\xA6", BTN_GHOST, "plus", hasCorpus())) { ui.closePopup(); page = PG_BUILD; }
    });
  }
  xEnd = chip.r() + 12 * s;
}

// =====================================================================================
// local citations (cached per corpus)
// =====================================================================================
const CitationIndex& App::citations() {
  if (citeVer != P->corpusVersion || citeIdx.cites.size() != P->corpus.recs.size()) { citeIdx = localCitations(P->corpus); citeVer = P->corpusVersion; }
  return citeIdx;
}

// =====================================================================================
// document preview (inspector)
// =====================================================================================
void App::openDoc(int rec) {
  if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) return;
  if (docPreview >= 0 && docPreview != rec) docBackStack.push_back(docPreview);
  if (docBackStack.size() > 30) docBackStack.erase(docBackStack.begin());
  docPreview = rec;
  docAbsOpen = false;
  inspectorOpen = true;
  ui.scrollTo("inspector", 0);
}

void App::closeDoc() { docPreview = -1; docBackStack.clear(); }

void App::findInMap(const string& label) {
  if (!hasMap()) { ui.toast("Not on the map", "Build a map to locate \xE2\x80\x9C" + label + "\xE2\x80\x9D.", 0, 2.5); return; }
  search = label;
  runSearch();
  if (!searchHits.empty()) focusOn(searchHits[0]);
  else ui.toast("Not on the map", "\xE2\x80\x9C" + label + "\xE2\x80\x9D is not an item of the current map.", 0, 2.5);
}

float App::docRow(Lay& L, int rec, const string& key) {
  float s = ui.s;
  if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) return 0;
  const Record& r = P->corpus.recs[size_t(rec)];
  float th = ui.textWrap({0, 0, L.w - 16 * s, 1000}, r.title, 12 * s, ui.c.text, 550, false);
  Rect rr = L.row(th + 30 * s);
  if (ui.listRow(rr, key + std::to_string(rec), docPreview == rec)) openDoc(rec);
  ui.textWrap({rr.x + 8 * s, rr.y + 5 * s, rr.w - 16 * s, th + 4}, r.title, 12 * s, ui.c.text, 550);
  string meta = shortCite(r) + " \xC2\xB7 " + truncate(titleCase(lower(r.source)), 30) + " \xC2\xB7 " + std::to_string(r.cites) + " cites";
  ui.text({rr.x + 8 * s, rr.y + th + 8 * s, rr.w - 16 * s, 16 * s}, meta, 11 * s, ui.c.textDim);
  return rr.h;
}

void App::chipFlow(Lay& L, const vector<string>& v, const string& key, int maxN) {
  float s = ui.s;
  float x = L.x, y = L.y, h = 24 * s, gap = 6 * s;
  int n = std::min<int>(maxN, int(v.size()));
  for (int i = 0; i < n; i++) {
    string t = truncate(v[size_t(i)], 42);
    float w = std::min(L.w, ui.textW(t, 11.5f * s, 500) + 20 * s);
    if (x + w > L.x + L.w + 0.5f) { x = L.x; y += h + gap; }
    Rect r{x, y, w, h};
    uint64_t idv = ui.id(key + std::to_string(i));
    bool hov = false;
    bool clicked = ui.behave(idv, r, &hov);
    ui.fill(r, hov ? ui.c.hover : ui.c.input, h / 2);
    ui.stroke(r, hov ? ui.c.accent.withA(0.6f) : ui.c.border, h / 2);
    ui.text(r, t, 11.5f * s, ui.c.text, AL_CENTER, 500);
    ui.tipFor(idv, "Find \xE2\x80\x9C" + v[size_t(i)] + "\xE2\x80\x9D on the map");
    if (clicked) findInMap(v[size_t(i)]);
    x += w + gap;
  }
  if (n) L.y = y + h + L.gap;
}

void App::drawDocPreview(Lay& L) {
  float s = ui.s;
  const Record& r = P->corpus.recs[size_t(docPreview)];
  const int me = docPreview;
  // header
  Rect top = L.row(24 * s);
  if (ui.iconButton({top.x - 4 * s, top.y, 26 * s, 24 * s}, "arrow-left", docBackStack.empty() ? "Back  (Esc)" : "Back to the previous document", false, true)) {
    if (!docBackStack.empty()) { docPreview = docBackStack.back(); docBackStack.pop_back(); docAbsOpen = false; }
    else docPreview = -1;
    return;
  }
  ui.text({top.x + 26 * s, top.y, top.w - 60 * s, top.h}, "Document", 12 * s, ui.c.textDim, AL_LEFT, 600);
  if (ui.iconButton({top.r() - 22 * s, top.y + 1 * s, 22 * s, 22 * s}, "x", "Close preview  (Esc)")) { closeDoc(); return; }
  L.space(2 * s);
  float h = ui.textWrap({L.x, L.y, L.w, 1000}, r.title.empty() ? "(untitled)" : r.title, 15.5f * s, ui.c.text, 700);
  L.y += h + 6 * s;
  // authors
  if (!r.authors.empty()) {
    string au;
    size_t na = std::min<size_t>(12, r.authors.size());
    for (size_t i = 0; i < na; i++) au += (i ? "; " : "") + r.authors[i];
    if (r.authors.size() > na) au += "; +" + std::to_string(r.authors.size() - na) + " more";
    float ah = ui.textWrap({L.x, L.y, L.w, 1000}, au, 12 * s, ui.c.text, 500);
    L.y += ah + 4 * s;
  }
  // source line
  string src = titleCase(lower(r.source));
  string meta = (r.year ? std::to_string(r.year) : string("n.d.")) + (src.empty() ? "" : " \xC2\xB7 " + src) + (r.volume.empty() ? "" : " \xC2\xB7 Vol. " + r.volume) + (r.pages.empty() ? "" : " \xC2\xB7 pp. " + r.pages);
  float mh = ui.textWrap({L.x, L.y, L.w, 1000}, meta, 12 * s, ui.c.textDim);
  L.y += mh + 8 * s;
  // pills
  {
    const CitationIndex& X = citations();
    int lcs = size_t(me) < X.citedBy.size() ? int(X.citedBy[size_t(me)].size()) : 0;
    struct Pill { string t; Color c; bool plain; };
    vector<Pill> pills;
    pills.push_back({fmtInt(r.cites) + (r.cites == 1 ? " citation" : " citations"), ui.c.accent, false});
    if (lcs) pills.push_back({"cited by " + std::to_string(lcs) + " here", ui.c.ok, false});
    if (!r.docType.empty()) pills.push_back({r.docType, ui.c.textDim, true});
    if (!r.language.empty() && lower(r.language) != "english") pills.push_back({r.language, ui.c.textDim, true});
    float x = L.x, y = L.y;
    for (auto& p : pills) {
      float w = ui.textW(p.t, 11 * s, 600) + 18 * s;
      if (x + w > L.x + L.w) { x = L.x; y += 26 * s; }
      Rect pr{x, y, w, 22 * s};
      ui.fill(pr, p.c.withA(0.14f), 11 * s);
      ui.text(pr, p.t, 11 * s, p.plain ? ui.c.text : p.c, AL_CENTER, 600);
      x += w + 6 * s;
    }
    L.y = y + 22 * s + 10 * s;
  }
  // actions
  auto a1 = cols(L.row(32 * s), 2, 6 * s);
  bool hasDoi = !r.doi.empty();
  if (ui.button(a1[0], hasDoi ? "Open in browser" : "Search Scholar", BTN_PRIMARY, "external")) {
    if (hasDoi) openUrl("https://doi.org/" + r.doi);
    else openUrl("https://scholar.google.com/scholar?q=" + urlEncode(r.title));
  }
  ui.tip(hasDoi ? "https://doi.org/" + r.doi : "No DOI. Search the title on Google Scholar.");
  if (ui.button(a1[1], "Copy citation", BTN_NORMAL, "quote")) { setClipboardText(hwnd, apaCitation(r)); ui.toast("Citation copied", "APA style", 1, 2); }
  auto a2 = cols(L.row(30 * s), 2, 6 * s);
  if (ui.button(a2[0], "Copy DOI", BTN_GHOST, "copy", hasDoi)) { setClipboardText(hwnd, "https://doi.org/" + r.doi); ui.toast("Copied", r.doi, 1, 1.6); }
  if (ui.button(a2[1], "Copy title", BTN_GHOST, "copy", !r.title.empty())) { setClipboardText(hwnd, r.title); ui.toast("Copied", truncate(r.title, 60), 1, 1.6); }
  if (ui.button(L.row(30 * s), "Summarise with AI", BTN_NORMAL, "sparkle", !aiLive)) { aiDoc = docPreview; aiRun(ai::Task::Document); }
  // abstract (collapsible)
  if (!r.abstract_.empty()) {
    sectionTitle(L, "Abstract");
    float full = ui.textWrap({0, 0, L.w, 100000}, r.abstract_, 12.5f * s, ui.c.text, 400, false);
    float cap = 8 * 18.5f * s;
    bool longAbs = full > cap + 20 * s;
    float shown = (longAbs && !docAbsOpen) ? cap : full;
    ui.pushClip({L.x, L.y, L.w, shown});
    ui.textWrap({L.x, L.y, L.w, full + 4}, r.abstract_, 12.5f * s, ui.c.text, 400);
    ui.popClip();
    if (longAbs && !docAbsOpen) ui.fill({L.x, L.y + shown - 22 * s, L.w, 22 * s}, ui.c.panel.withA(0.6f));
    L.y += shown + 4 * s;
    if (longAbs) {
      Rect mr = L.row(22 * s);
      uint64_t idv = ui.id("absmore");
      bool hv = false;
      if (ui.behave(idv, {mr.x, mr.y, 110 * s, mr.h}, &hv)) docAbsOpen = !docAbsOpen;
      ui.text(mr, docAbsOpen ? "Show less" : "Show the full abstract", 12 * s, hv ? ui.c.text : ui.c.accent, AL_LEFT, 600);
    }
  }
  // keywords
  if (!r.keywords.empty()) { sectionTitle(L, "Author keywords", "Click a keyword to find it on the map"); chipFlow(L, r.keywords, "kw", 30); }
  if (!r.indexTerms.empty()) { sectionTitle(L, "Index terms"); chipFlow(L, r.indexTerms, "it", 20); }
  // affiliations
  if (!r.affiliations.empty() || !r.countries.empty()) {
    sectionTitle(L, "Affiliations");
    for (size_t i = 0; i < std::min<size_t>(8, r.affiliations.size()); i++) {
      float ah = ui.textWrap({L.x, L.y, L.w, 1000}, r.affiliations[i], 12 * s, ui.c.text);
      L.y += ah + 3 * s;
    }
    if (!r.countries.empty()) {
      std::set<string> cs(r.countries.begin(), r.countries.end());
      string c;
      for (auto& x : cs) c += (c.empty() ? "" : " \xC2\xB7 ") + x;
      float ch = ui.textWrap({L.x, L.y, L.w, 1000}, c, 11.5f * s, ui.c.textDim);
      L.y += ch + 4 * s;
    }
  }
  // map items built from this document
  if (hasMap()) {
    const Network& N = P->net;
    vector<int> items;
    for (int i = 0; i < N.n(); i++) {
      auto& rv = N.nodes[size_t(i)].recs;
      if (std::find(rv.begin(), rv.end(), me) != rv.end()) items.push_back(i);
    }
    if (!items.empty()) {
      sectionTitle(L, "On this map (" + std::to_string(items.size()) + ")", "Items of the current map that come from this document \xC2\xB7 click to focus");
      auto cc = clusterColors();
      std::sort(items.begin(), items.end(), [&](int a, int b) { return N.weight(a) > N.weight(b); });
      for (size_t k = 0; k < std::min<size_t>(14, items.size()); k++) {
        int i = items[k];
        Rect rr = L.row(24 * s);
        if (ui.listRow(rr, "mi" + std::to_string(i), false)) { focusOn(i); return; }
        ui.circle(rr.x + 9 * s, rr.y + rr.h / 2, 4.5f * s, cc.empty() ? ui.c.accent : cc[size_t(std::max(0, N.nodes[size_t(i)].cluster)) % cc.size()]);
        ui.text({rr.x + 20 * s, rr.y, rr.w - 70 * s, rr.h}, N.nodes[size_t(i)].label, 12 * s, ui.c.text);
        ui.text({rr.r() - 50 * s, rr.y, 44 * s, rr.h}, fmtNum(N.weight(i), 0), 11 * s, ui.c.textDim, AL_RIGHT);
      }
    }
  }
  // local citation context
  const CitationIndex& X = citations();
  if (size_t(me) < X.citedBy.size()) {
    vector<int> by = X.citedBy[size_t(me)], to = X.cites[size_t(me)];
    auto byCites = [&](int a, int b) { return P->corpus.recs[size_t(a)].cites > P->corpus.recs[size_t(b)].cites; };
    std::sort(by.begin(), by.end(), byCites);
    std::sort(to.begin(), to.end(), byCites);
    if (!by.empty()) {
      sectionTitle(L, "Cited by in this corpus (" + std::to_string(by.size()) + ")", "Documents of your data set that cite this one");
      for (size_t k = 0; k < std::min<size_t>(10, by.size()); k++) docRow(L, by[k], "cb");
    }
    if (!to.empty()) {
      sectionTitle(L, "Cites in this corpus (" + std::to_string(to.size()) + ")", "Documents of your data set that this one cites");
      for (size_t k = 0; k < std::min<size_t>(10, to.size()); k++) docRow(L, to[k], "ct");
    }
  }
  // cited references
  if (!r.refs.empty()) {
    sectionTitle(L, "References", "", fmtInt(long(r.refs.size())) + " cited");
    size_t nref = std::min<size_t>(docRefsAll ? 200 : 6, r.refs.size());
    for (size_t k = 0; k < nref; k++) {
      float rh = ui.textWrap({L.x, L.y, L.w, 1000}, r.refs[k], 11 * s, ui.c.textDim);
      L.y += rh + 4 * s;
    }
    if (r.refs.size() > 6) {
      Rect mr = L.row(22 * s);
      uint64_t idv = ui.id("refmore");
      bool hv = false;
      if (ui.behave(idv, {mr.x, mr.y, 160 * s, mr.h}, &hv)) docRefsAll = !docRefsAll;
      ui.text(mr, docRefsAll ? "Show fewer" : "Show all " + fmtInt(long(r.refs.size())) + " references", 12 * s, hv ? ui.c.text : ui.c.accent, AL_LEFT, 600);
    }
  }
}

// =====================================================================================
// country profile (Geo view selection, or a country node)
// =====================================================================================
void App::buildCountryProfile(int country) {
  geoUpdate();
  string sig = geoSig_ + "|" + std::to_string(country);
  if (cprof.country == country && cprof.sig == sig) return;
  CountryProfile C;
  C.country = country;
  C.sig = sig;
  const auto& recs = P->corpus.recs;
  std::map<int, std::pair<int, long long>> years;
  std::unordered_map<string, std::pair<string, int>> kw, orgs, au, src;
  vector<int> cites;
  auto add = [](std::unordered_map<string, std::pair<string, int>>& m, const string& lab0, Unit u) {
    string lab = bibClean(lab0), k = unitKey(lab, u);
    if (k.empty()) return;
    auto& e = m[k];
    if (e.first.empty()) e.first = lab;
    e.second++;
  };
  for (size_t i = 0; i < recs.size(); i++) {
    const Record& r = recs[i];
    bool mine = false;
    std::set<int> cs;
    for (auto& c : r.countries) { int k = findCountry(c); if (k >= 0) cs.insert(k); if (k == country) mine = true; }
    if (!mine) continue;
    C.recs.push_back(int(i));
    if (cs.size() > 1) C.intl++;
    C.cites += r.cites;
    cites.push_back(r.cites);
    if (r.year) { years[r.year].first++; years[r.year].second += r.cites; }
    std::set<string> seenK;
    for (auto& k : r.keywords) if (seenK.insert(bibKey(k)).second) add(kw, k, Unit::Keywords);
    std::set<string> seenO;
    for (size_t a = 0; a < r.affiliations.size(); a++) {
      // organisations of this country only when the record has a single country (affiliation-country pairs are not kept)
      if (cs.size() == 1 && seenO.insert(bibKey(r.affiliations[a])).second) add(orgs, r.affiliations[a], Unit::Orgs);
    }
    for (auto& a : r.authors) add(au, a, Unit::Authors);
    if (!r.source.empty()) add(src, r.source, Unit::Sources);
  }
  std::sort(cites.begin(), cites.end(), std::greater<int>());
  for (size_t i = 0; i < cites.size(); i++) if (cites[i] >= int(i + 1)) C.h = int(i + 1);
  if (!years.empty()) {
    C.gr.y0 = years.begin()->first;
    C.gr.y1 = years.rbegin()->first;
    for (int y = C.gr.y0; y <= C.gr.y1; y++) {
      auto it = years.find(y);
      int n = it == years.end() ? 0 : it->second.first;
      C.gr.perYear.push_back({y, n});
      C.gr.citesPerYear.push_back({y, n ? double(it->second.second) / n : 0.0});
    }
    int n0 = C.gr.perYear.front().second, n1 = C.gr.perYear.back().second, span = C.gr.y1 - C.gr.y0;
    C.gr.cagr = (n0 > 0 && n1 > 0 && span > 0) ? std::pow(double(n1) / n0, 1.0 / span) - 1 : 0;
  }
  auto top = [](std::unordered_map<string, std::pair<string, int>>& m, size_t n) {
    vector<std::pair<string, int>> v;
    for (auto& kv : m) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    if (v.size() > n) v.resize(n);
    return v;
  };
  C.kw = top(kw, 12);
  C.orgs = top(orgs, 6);
  C.authors = top(au, 6);
  C.sources = top(src, 5);
  std::sort(C.recs.begin(), C.recs.end(), [&](int a, int b) { return recs[size_t(a)].cites > recs[size_t(b)].cites; });
  cprof = std::move(C);
}

void App::drawCountryProfile(Lay& L, int country, bool compact) {
  float s = ui.s;
  if (country < 0 || country >= worldCountryCount()) return;
  buildCountryProfile(country);
  const CountryProfile& C = cprof;
  const WorldCountry& wc = worldCountry(country);
  int n = int(C.recs.size());
  if (!compact) {
    Rect top = L.row(18 * s);
    ui.text(top, "Country", 12 * s, ui.c.textDim, AL_LEFT, 600);
    if (ui.iconButton({top.r() - 22 * s, top.y - 3 * s, 22 * s, 22 * s}, "x", "Clear selection  (Esc)")) { geoSel = -1; return; }
    ui.text(L.row(26 * s), wc.name, 19 * s, ui.c.text, AL_LEFT, 700);
    ui.text(L.row(16 * s), string(wc.continent) + (geoDocsTotal ? " \xC2\xB7 " + fmtNum(100.0 * n / std::max(1, geoDocsTotal), 1) + "% of documents with a country" : ""), 11.5f * s, ui.c.textDim);
    L.space(4 * s);
    auto grid = [&](const string& a, const string& av, const string& b, const string& bv) {
      auto c2 = cols(L.row(52 * s), 2, 8 * s);
      for (int k = 0; k < 2; k++) {
        ui.text({c2[size_t(k)].x + 0 * s, c2[size_t(k)].y + 6 * s, c2[size_t(k)].w - 20 * s, 22 * s}, k ? bv : av, 18 * s, ui.c.text, AL_LEFT, 500);
        ui.text({c2[size_t(k)].x + 0 * s, c2[size_t(k)].y + 29 * s, c2[size_t(k)].w - 20 * s, 16 * s}, k ? b : a, 11 * s, ui.c.textDim);
      }
    };
    grid("Documents", fmtInt(n), "Citations", fmtInt(C.cites));
    grid("Citations / doc", n ? fmtNum(double(C.cites) / n, 1) : "n/a", "h-index", std::to_string(C.h));
    Rect ir = L.row(20 * s);
    ui.text(ir, "International co-authorship", 12 * s, ui.c.textDim);
    ui.text(ir, fmtInt(C.intl) + "  (" + fmtNum(100.0 * C.intl / std::max(1, n), 0) + "%)", 12 * s, ui.c.text, AL_RIGHT, 600);
    Rect bar = L.row(5 * s);
    ui.fill(bar, ui.c.input, 2.5f * s);
    ui.fill({bar.x, bar.y, bar.w * float(C.intl) / std::max(1, n), bar.h}, ui.c.warn, 2.5f * s);
  }
  if (!C.gr.perYear.empty()) {
    ChartDef d;
    d.title = "Documents per year";
    Growth gr = C.gr;
    d.make = [gr](double w, double h, const ChartTheme& t) { return chartGrowth(gr, w, h, t); };
    L.space(4 * s);
    chartBox(L, compact ? "cpy" : "gpy", d, 150);
  }
  // collaborating countries
  geoUpdate();
  if (size_t(country) < geoPartners.size() && !geoPartners[size_t(country)].empty()) {
    auto& pp = geoPartners[size_t(country)];
    sectionTitle(L, "Collaborates with", compact ? "" : "Co-authored documents with each country \xC2\xB7 click to select it", std::to_string(pp.size()) + " countries");
    int mx = pp[0].second;
    for (size_t k = 0; k < std::min<size_t>(8, pp.size()); k++) {
      Rect r = L.row(24 * s);
      bool click = ui.listRow(r, "pp" + std::to_string(pp[k].first), false);
      ui.fill({r.x, r.b() - 3 * s, float(r.w * pp[k].second / std::max(1, mx)), 2 * s}, ui.c.warn.withA(0.55f), 1 * s);
      ui.text({r.x + 6 * s, r.y, r.w - 60 * s, r.h - 2 * s}, worldCountry(pp[k].first).name, 12 * s, ui.c.text);
      ui.text({r.r() - 50 * s, r.y, 46 * s, r.h - 2 * s}, fmtInt(pp[k].second), 11 * s, ui.c.textDim, AL_RIGHT);
      if (click) {
        if (geoMode() == 2) { geoSel = pp[k].first; closeDoc(); ui.scrollTo("inspector", 0); }
        else {  // countries map: select the partner's node
          for (int i = 0; i < int(geoNodeCountry.size()); i++) if (geoNodeCountry[size_t(i)] == pp[k].first) { focusOn(i); break; }
        }
        return;
      }
    }
  }
  auto list = [&](const string& title, const vector<std::pair<string, int>>& v, const string& key, bool findable) {
    if (v.empty()) return;
    sectionTitle(L, title);
    int mx = v[0].second;
    for (size_t k = 0; k < v.size(); k++) {
      Rect r = L.row(22 * s);
      bool click = findable && ui.listRow(r, key + std::to_string(k), false);
      if (!findable) ui.fill({r.x, r.b() - 2 * s, float(r.w * v[k].second / std::max(1, mx)), 2 * s}, ui.c.accent.withA(0.3f), 1 * s);
      else ui.fill({r.x, r.b() - 2 * s, float(r.w * v[k].second / std::max(1, mx)), 2 * s}, ui.c.accent.withA(0.45f), 1 * s);
      ui.text({r.x + 6 * s, r.y, r.w - 56 * s, r.h - 2 * s}, v[k].first, 12 * s, ui.c.text);
      ui.text({r.r() - 46 * s, r.y, 42 * s, r.h - 2 * s}, fmtInt(v[k].second), 11 * s, ui.c.textDim, AL_RIGHT);
      if (click) { findInMap(v[k].first); return; }
    }
  };
  if (!C.kw.empty()) {
    sectionTitle(L, "Top keywords", "Most frequent author keywords of this country's documents \xC2\xB7 click to find on the map");
    // chips show "keyword  n"; searching uses the keyword only
    float xx = L.x, yy = L.y, hh = 24 * s, gap = 6 * s;
    for (size_t i = 0; i < C.kw.size(); i++) {
      string t = truncate(C.kw[i].first, 34);
      string cnt = std::to_string(C.kw[i].second);
      float w = std::min(L.w, ui.textW(t, 11.5f * s, 500) + ui.textW(cnt, 10.5f * s) + 26 * s);
      if (xx + w > L.x + L.w + 0.5f) { xx = L.x; yy += hh + gap; }
      Rect r{xx, yy, w, hh};
      uint64_t idv = ui.id("ckw" + std::to_string(i));
      bool hv = false;
      bool click = ui.behave(idv, r, &hv);
      ui.fill(r, hv ? ui.c.hover : ui.c.input, hh / 2);
      ui.stroke(r, hv ? ui.c.accent.withA(0.6f) : ui.c.border, hh / 2);
      ui.text({r.x + 10 * s, r.y, r.w, r.h}, t, 11.5f * s, ui.c.text, AL_LEFT, 500);
      ui.text({r.x, r.y, r.w - 10 * s, r.h}, cnt, 10.5f * s, ui.c.textFaint, AL_RIGHT);
      if (click) { findInMap(C.kw[i].first); return; }
      xx += w + gap;
    }
    L.y = yy + hh + L.gap;
  }
  if (!compact) {
    list("Top organisations", C.orgs, "co", true);
    list("Top authors", C.authors, "ca", true);
    list("Top sources", C.sources, "cs", false);
    if (!C.recs.empty()) {
      sectionTitle(L, "Documents (" + fmtInt(n) + ")", "Most cited first \xC2\xB7 click for a preview");
      for (size_t k = 0; k < std::min<size_t>(25, C.recs.size()); k++) docRow(L, C.recs[k], "cdoc");
    }
  }
}

// =====================================================================================
// publication figure: 3D panel and the per-view panel data
// =====================================================================================
// The 3D layout seen with the 3D view's camera angles: perspective projection, far nodes smaller and
// faded toward the background, painted back to front.
FigPanelDef App::threeDFigurePanel() {
  FigPanelDef d;
  d.kind = ViewKind::ThreeD;
  const Network& N = P->net;
  int n = N.n();
  if (!pos3dValid || pos3d.size() != size_t(n)) {
    start3DLayout();
    d.note = "Computing the 3D layout\xE2\x80\xA6";
    d.nodes = d.links = d.labels = d.clusterNames = false;
    return d;
  }
  double cx = 0, cy = 0, cz = 0, zs = P->style.zScale;
  for (auto& p : pos3d) { cx += p[0]; cy += p[1]; cz += p[2] * zs; }
  cx /= std::max(1, n); cy /= std::max(1, n); cz /= std::max(1, n);
  double ext = 1;
  for (auto& p : pos3d) ext = std::max(ext, std::sqrt((p[0] - cx) * (p[0] - cx) + (p[1] - cy) * (p[1] - cy) + (p[2] * zs - cz) * (p[2] * zs - cz)));
  double yaw = nv.cam.yaw, pitch = nv.cam.pitch;
  double sy = std::sin(yaw), cyw = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
  double D = ext * 3.2;
  d.pos.resize(size_t(n));
  d.rMul.resize(size_t(n));
  d.fade.resize(size_t(n));
  vector<std::pair<double, int>> depth(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    double x = pos3d[size_t(i)][0] - cx, y = pos3d[size_t(i)][1] - cy, z = pos3d[size_t(i)][2] * zs - cz;
    double x1 = x * cyw + z * sy, z1 = -x * sy + z * cyw;   // yaw about the vertical axis
    double y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;    // pitch about the horizontal axis
    double f = D / std::max(1e-6, D + z2);
    d.pos[size_t(i)] = {x1 * f, y2 * f};
    d.rMul[size_t(i)] = float(f);
    d.fade[size_t(i)] = float(clampv(0.38 * (z2 + ext) / (2 * ext), 0.0, 0.38));
    depth[size_t(i)] = {z2, i};
  }
  std::sort(depth.begin(), depth.end(), [](const std::pair<double, int>& a, const std::pair<double, int>& b) { return a.first > b.first; });
  for (auto& p : depth) d.order.push_back(p.second);
  // curved links: the same world-space curve as the 3D view, its control point projected like the nodes
  const ViewStyle& S = P->style;
  double bend = S.linkGeom == LinkGeom::Straight ? 0 : (S.linkGeom == LinkGeom::Arc ? 1 : S.curvature);
  if (bend > 0.001) {
    auto proj = [&](double x, double y, double z) {
      double x1 = x * cyw + z * sy, z1 = -x * sy + z * cyw;
      double y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
      double f = D / std::max(1e-6, D + z2);
      return std::array<double, 2>{x1 * f, y2 * f};
    };
    d.linkCtrl.resize(size_t(N.m()));
    for (int li = 0; li < N.m(); li++) {
      const Link& l = N.links[size_t(li)];
      const auto &pa = pos3d[size_t(l.a)], &pb = pos3d[size_t(l.b)];
      double ax = pa[0] - cx, ay = pa[1] - cy, az = pa[2] * zs - cz, bx = pb[0] - cx, by = pb[1] - cy, bz = pb[2] * zs - cz;
      double dx = bx - ax, dy = by - ay, dz = bz - az, len = std::max(1e-9, std::sqrt(dx * dx + dy * dy + dz * dz));
      double k = bend * len * 0.22 * 2 / len;
      d.linkCtrl[size_t(li)] = proj((ax + bx) / 2 + dy * k, (ay + by) / 2 - dx * k, (az + bz) / 2);
    }
  }
  return d;
}

vector<FigPanelDef> App::figurePanelDefs() {
  vector<FigPanelDef> v;
  if (!hasMap()) return v;
  if (P->fig.panelGeo) v.push_back(geoFigurePanel());
  if (P->fig.panel3D) v.push_back(threeDFigurePanel());
  return v;
}

// anything outside the figure spec and style that changes the Geo/3D panels
string App::figurePanelSig() const {
  string s;
  if (P->fig.panelGeo) s += "g" + geoSig_ + std::to_string(geoFill) + std::to_string(geoBasemap) + std::to_string(geoArcs) + std::to_string(geoLayerKind) + std::to_string(geoDenStyle) + std::to_string(geoDenMeasure) + fmtNum(P->style.kernel, 2);
  if (P->fig.panel3D) s += "3" + std::to_string(pos3dValid) + fmtNum(nv.cam.yaw, 2) + fmtNum(nv.cam.pitch, 2);
  return s;
}

}  // namespace win
}  // namespace vs
