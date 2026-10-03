// VOSStudio Native: the menu bar at the top left — File, Edit, View, Map, Analyse, Write, AI, Help — with cascading
// submenus. The menus are built from the same command registry as the palette (buildCommands), so every entry has the
// same label, shortcut and behaviour everywhere; a few menu-only entries (Exit, recent projects, view toggles) are
// handled by menuRun. Hovering another title while a menu is open switches menus, as in every desktop application.
// When the window is too narrow for the titles, one "Menu" button opens the same menus as pages of a single list.
#include "../core/style.h"
#include "../core/ai.h"
#include "app.h"
#include "reader.h"
#include "pdfload.h"
#include "version.h"

#include <algorithm>

namespace vs {
namespace win {

namespace {

struct MI {
  string label, id, sc, icon;
  bool enabled = true, checked = false, sep = false;
  vector<MI> sub;
};
struct Menu { string title; vector<MI> items; };

MI item(const string& label, const string& id, bool enabled = true, bool checked = false, const string& icon = "") { MI m; m.label = label; m.id = id; m.enabled = enabled; m.checked = checked; m.icon = icon; return m; }
MI sep() { MI m; m.sep = true; return m; }
MI submenu(const string& label, vector<MI> items, const string& icon = "") { MI m; m.label = label; m.sub = std::move(items); m.icon = icon; return m; }

}  // namespace

// the model: rebuilt every frame from the application's state (cheap: a few dozen small strings)
static vector<Menu> menuModel(App& a) {
  auto cmd = [&](const string& id) -> const Command* { for (auto& c : a.commands) if (c.id == id) return &c; return nullptr; };
  auto sc = [&](const string& id) { const Command* c = cmd(id); return c ? c->shortcut : string(); };
  auto fill = [&](MI m) { if (m.sc.empty()) m.sc = sc(m.id); if (m.icon.empty()) { const Command* c = cmd(m.id); if (c) m.icon = c->icon; } return m; };
  const bool corpus = a.hasCorpus(), map = a.hasMap();
  const bool saveable = corpus || map || !a.wdoc.empty() || a.P->library.hasData();
  vector<Menu> menus;
  // ---- File
  {
    vector<MI> recents;
    for (auto& p : a.settings.recent()) { if (recents.size() >= 8) break; recents.push_back(item(fileName(p), "recent:" + p, true, false, "clock")); }
    if (recents.empty()) recents.push_back(item("No recent projects", "", false));
    vector<MI> v = {fill(item(a.workspace == WS_BIBLIOGRAPHY ? "Add bibliographic files\xE2\x80\xA6" : "Open data\xE2\x80\xA6", "open")), fill(item("Open project\xE2\x80\xA6", "openproj")), submenu("Recent projects", recents, "history"), sep(),
                    fill(item("New project", "new")), sep(), fill(item("Save project", "save", saveable)), fill(item("Save project as\xE2\x80\xA6", "saveas", saveable)), sep(),
                    submenu("Sample data", {fill(item("Web of Science sample (140 records)", "sample")), fill(item("Scopus sample (90 records)", "sample2"))}, "sparkle"),
                    fill(item("Import VOSviewer map / network\xE2\x80\xA6", "vosload")),
                    submenu("Export", {fill(item("Current view\xE2\x80\xA6", "viewany", map)), fill(item("Current view as SVG\xE2\x80\xA6", "viewsvg", map)), fill(item("Current view as PDF\xE2\x80\xA6", "viewpdf", map)),
                                       fill(item("Current view as PNG\xE2\x80\xA6", "viewpng", map)), sep(), item("Publication figure\xE2\x80\xA6", "page" + std::to_string(PG_PUBLISH), map, false, "publish"), sep(),
                                       fill(item("VOSviewer map + network files\xE2\x80\xA6", "vosexport", map)), fill(item("Items table (CSV)\xE2\x80\xA6", "items", map)), sep(),
                                       fill(item("Records: Web of Science format\xE2\x80\xA6", "recwos", corpus)), fill(item("Records: RIS\xE2\x80\xA6", "recris", corpus)),
                                       fill(item("Records: BibTeX\xE2\x80\xA6", "recbib", corpus)), fill(item("Records: CSV\xE2\x80\xA6", "reccsv", corpus)), sep(),
                                       fill(item("Document as PDF\xE2\x80\xA6", "writerpdf", !a.wdoc.empty())), fill(item("Document as Word\xE2\x80\xA6", "writerdocx", !a.wdoc.empty())),
                                       fill(item("Document as web page\xE2\x80\xA6", "writerhtml", !a.wdoc.empty())), fill(item("LaTeX source bundle (advanced)\xE2\x80\xA6", "writerlatex", !a.wdoc.empty()))}, "download"),
                    sep(), fill(item("Settings\xE2\x80\xA6", "settings")), sep(), item("Exit", "exit", true, false, "x")};
    if (a.workspace == WS_BIBLIOGRAPHY) {
      v.erase(std::remove_if(v.begin(), v.end(), [](const MI& m) { return m.id == "vosload"; }), v.end());
      for (MI& m : v) if (m.label == "Export") {
        m.sub.erase(std::remove_if(m.sub.begin(), m.sub.end(), [](const MI& x) {
          return x.id.rfind("view", 0) == 0 || x.id == "vosexport" || x.id == "items" || x.id == "page" + std::to_string(PG_PUBLISH);
        }), m.sub.end());
        while (!m.sub.empty() && m.sub.front().sep) m.sub.erase(m.sub.begin());
        m.sub.erase(std::unique(m.sub.begin(), m.sub.end(), [](const MI& x, const MI& y) { return x.sep && y.sep; }), m.sub.end());
        while (!m.sub.empty() && m.sub.back().sep) m.sub.pop_back();
      }
    }
    v.back().sc = "Alt+F4";
    menus.push_back({"File", v});
  }
  // ---- Edit
  {
    string un = a.undoStack.empty() ? "Undo" : "Undo " + a.undoStack.back().what, re = a.redoStack.empty() ? "Redo" : "Redo " + a.redoStack.back().what;
    if (a.writerOpen) { un = a.wed.canUndo() ? "Undo (document)" : "Undo"; re = a.wed.canRedo() ? "Redo (document)" : "Redo"; }
    bool canU = a.writerOpen ? a.wed.canUndo() : !a.undoStack.empty(), canR = a.writerOpen ? a.wed.canRedo() : !a.redoStack.empty();
    vector<MI> v = {fill(item(un, "undo", canU)), fill(item(re, "redo", canR)), sep()};
    if (a.writerOpen) {
      bool sel = a.wed.hasSelection() || a.wed.blockSelected();
      MI cut = item("Cut", "wcut", sel, false, "trash"); cut.sc = "Ctrl+X";
      MI cp = item("Copy", "wcopy", sel, false, "copy"); cp.sc = "Ctrl+C";
      MI cb = item("Paste into Word at the cursor", "wcopybib", sel, false, "external"); cb.sc = "Ctrl+Shift+C";
      MI pa = item("Paste", "wpaste", true, false, "file"); pa.sc = "Ctrl+V";
      MI pp = item("Paste as plain text", "wpasteplain"); pp.sc = "Ctrl+Shift+V";
      MI all = item("Select all", "wselall"); all.sc = "Ctrl+A";
      MI fr = item("Find and replace\xE2\x80\xA6", "wfind", true, false, "search"); fr.sc = "Ctrl+F";
      v.insert(v.end(), {cut, cp, cb, pa, pp, sep(), all, fr, sep()});
    }
    MI cf = fill(item("Copy figure to clipboard", "copyfig", map));
    MI cm = fill(item("Copy methods paragraph", "methods", map));
    MI fm = item("Find in the map\xE2\x80\xA6", "search", map, false, "search"); fm.sc = "Ctrl+/";
    MI pal = item("Command palette\xE2\x80\xA6", "palette", true, false, "command"); pal.sc = "Ctrl+K";
    if (a.workspace == WS_VISUALIZATION) v.insert(v.end(), {cf, cm, sep(), fm, pal});
    else v.push_back(pal);
    menus.push_back({"Edit", v});
  }
  if (a.workspace == WS_VISUALIZATION) {
  // ---- View
  {
    vector<MI> v;
    static const ViewKind kOrder[7] = {ViewKind::Network, ViewKind::Overlay, ViewKind::Density, ViewKind::Timeline, ViewKind::Matrix, ViewKind::Geo, ViewKind::ThreeD};  // the order of the view0..6 commands
    for (int i = 0; i < 7; i++) { const Command* c = cmd("view" + std::to_string(i)); if (!c) continue; MI m = item(c->label.substr(6), c->id, map && (kOrder[i] != ViewKind::Geo || a.geoAvailable()), a.view == kOrder[i], c->icon); m.sc = c->shortcut; v.push_back(m); }
    v.push_back(sep());
    v.push_back(fill(item("Fit map to window", "fit", map)));
    v.push_back(sep());
    v.push_back(fill(item("Labels", "labels", map, a.showLabels)));
    v.push_back(fill(item("Cluster legend", "legend", map, a.showLegend)));
    v.push_back(fill(item("Cluster hulls", "hulls", map, a.P->style.hulls)));
    v.push_back(fill(item("Cluster names", "names", map, a.P->style.clusterNames)));
    v.push_back(item("Minimap", "minimap", map, a.showMinimap, "map"));
    v.push_back(sep());
    {
      vector<MI> sp = {item("Single pane", "split0", true, a.splitMode == 0), item("Two panes side by side", "split1", map, a.splitMode == 1, "table"),
                       item("Two panes, one above the other", "split2", map, a.splitMode == 2), item("Four panes", "split3", map, a.splitMode == 3, "grid")};
      sp[1].sc = "Ctrl+Shift+2"; sp[3].sc = "Ctrl+Shift+4";
      v.push_back(submenu("Split view (linked views and charts side by side)", sp, "grid"));
    }
    v.push_back(item("Inspector", "inspector", a.canInspect(), a.inspectorOpen && a.canInspect(), "layers"));
    { MI m = fill(item("Papers table", "papers", corpus, a.papersOpen && corpus)); v.push_back(m); }
    { MI m = fill(item("Writer", "writer", true, a.writerOpen)); v.push_back(m); }
    v.push_back(sep());
    vector<MI> looks;
    for (auto& lk : lookPresets()) looks.push_back(item(lk.label, string("look:") + lk.id, map, false, "look"));
    v.push_back(submenu("Looks", looks, "look"));
    v.push_back(fill(item("Dark theme", "theme", true, a.themeTarget())));
    v.push_back(sep());
    v.push_back(fill(item("Performance overlay", "perf", true, a.perfHud)));
    menus.push_back({"View", v});
  }
  // ---- Map
  {
    vector<MI> v = {fill(item(map ? "Rebuild map" : "Build map", "build", corpus && !a.busy())), fill(item("Re-run layout", "relayout", map && !a.busy())), fill(item("Re-run clustering", "recluster", map && !a.busy())),
                    fill(item("Compute edge bundling", "bundle", map && !a.busy())), sep(), fill(item("Switch map\xE2\x80\xA6", "maphist", a.mapHistory.size() > 1)),
                    item("Build another map\xE2\x80\xA6", "page" + std::to_string(PG_BUILD), corpus, false, "network"), sep(),
                    item("Data: import, merge and clean", "page" + std::to_string(PG_DATA), true, false, "data"), item("Look: colours, sizes, labels", "page" + std::to_string(PG_LOOK), map, false, "look"), sep(),
                    fill(item("Import VOSviewer map / network\xE2\x80\xA6", "vosload")), fill(item("Export VOSviewer files\xE2\x80\xA6", "vosexport", map))};
    menus.push_back({"Map", v});
  }
  // ---- Analyse
  {
    vector<MI> v = {item("Analyse: clusters, items, statistics", "page" + std::to_string(PG_ANALYSE), map, false, "analyse"), item("Trends: growth, bursts, evolution", "page" + std::to_string(PG_TRENDS), corpus, false, "trends"),
                    item("Actors: authors, sources, countries", "page" + std::to_string(PG_ACTORS), corpus, false, "actors"), sep(),
                    fill(item("Clusters over time", "clyears", map)), fill(item("Trend topics", "trtopics", corpus)), fill(item("Reference publication year spectroscopy", "rpys", corpus)),
                    fill(item("Most cited documents & historiograph", "topdocs", corpus)), sep(), fill(item("Papers table", "papers", corpus)), fill(item("Preview the most cited document", "topdoc", corpus)), sep(),
                    item("Publication figure\xE2\x80\xA6", "page" + std::to_string(PG_PUBLISH), map, false, "publish"), fill(item("Publication figure with all views", "figall", map))};
    menus.push_back({"Analyse", v});
  }
  }
  // ---- Read
  {
    const bool lib = !a.P->library.empty();
    vector<MI> v = {item("Reading list", "page" + std::to_string(PG_READ), true, a.page == PG_READ, "book"), item("Attach PDFs\xE2\x80\xA6", "rdattach", true, false, "plus"),
                    item("Get open-access PDFs\xE2\x80\xA6", "rdfetch", corpus && !a.busy(), false, "download"), item("Match PDFs to records", "rdmatch", lib && corpus, false, "link"), sep(),
                    item("Find in this PDF\xE2\x80\xA6", "rdfind", a.readerOpen, false, "search"), item("Dark page (reader only)", "rddark", true, a.readerDark(), "moon"), item("Two pages, facing", "rdtwoup", true, a.readerTwoUp(), "twopage"),
                    item("Turn the view clockwise", "rdrotate", a.readerOpen, false, "rotate"), item("Print\xE2\x80\xA6", "rdprint", a.readerOpen && !a.busy(), false, "print"), item("Save highlights into the PDF file", "rdsavepdf", a.readerOpen), item("Back to the reading list", "rdclose", a.readerOpen), sep(),
                    item("Export all notes (Markdown)\xE2\x80\xA6", "rdnotes", lib, false, "download"), item("Export the coding matrix (CSV)\xE2\x80\xA6", "rdcoding", lib, false, "table")};
    menus.push_back({a.workspace == WS_BIBLIOGRAPHY ? "Bibliography" : "Read", v});
  }
  // ---- Write
  {
    vector<MI> ins = {item("Table\xE2\x80\xA6", "wins:table", true, false, "table"), item("Chart of the loaded data\xE2\x80\xA6", "wins:chart", corpus, false, "image"), item("The current map view", "wins:map", map, false, "map"),
                      item("Citation\xE2\x80\xA6", "wins:cite", corpus, false, "quote"), sep(), item("Horizontal rule", "wins:rule"), item("Page break", "wins:pb", true, false, "pagebreak"), item("Table of contents", "wins:toc", true, false, "toc")};
    ins[0].sc = "Ctrl+T"; ins[3].sc = "Ctrl+Q"; ins[6].sc = "Ctrl+Enter";
    vector<MI> styles;
    for (auto& st : citeStyles()) styles.push_back(item(st.name, string("wcs:") + st.id, true, a.wdoc.citeStyle == st.id));
    MI open = fill(item(a.writerOpen ? "Close the writer" : "Open the writer", "writer"));
    MI nw = item("New document\xE2\x80\xA6", "wnew", true, false, "newdoc"); nw.sc = "Ctrl+Shift+N";
    MI fr = item("Find and replace\xE2\x80\xA6", "wfind", true, false, "search"); fr.sc = "Ctrl+F";
    MI zi = item("Zoom in", "wzoom+", true, false, "plus"); zi.sc = "Ctrl +";
    MI zo = item("Zoom out", "wzoom-", true, false, "minus"); zo.sc = "Ctrl -";
    MI z0 = item("Zoom 100%", "wzoom0"); z0.sc = "Ctrl+0";
    vector<MI> v = {open, nw, sep(), submenu("Insert", ins, "plus"), sep(), item("Outline pane", "wpane1", true, a.writerOpen && a.writerPane == 1, "list"),
                    item("Properties pane", "wpane2", true, a.writerOpen && a.writerPane == 2, "sliders"), fr, sep(), zi, zo, z0, sep(), submenu("Citation style", styles, "book"), sep(),
                    fill(item("Export as PDF\xE2\x80\xA6", "writerpdf", !a.wdoc.empty())), fill(item("Export as Word document\xE2\x80\xA6", "writerdocx", !a.wdoc.empty())), item("Open in Word", "wopenword", !a.wdoc.empty(), false, "external"), item("Paste selection into Word at the cursor", "wcopybib", a.writerOpen && (a.wed.hasSelection() || a.wed.blockSelected()), false, "external"),
                    fill(item("Export as web page\xE2\x80\xA6", "writerhtml", !a.wdoc.empty())), item("Export LaTeX source bundle (advanced)\xE2\x80\xA6", "writerlatex", !a.wdoc.empty(), false, "download"), sep(), item("Ask the assistant to write the report\xE2\x80\xA6", "aiwrite", true, false, "sparkle")};
    v.back().sc = "Ctrl+J";
    menus.push_back({"Write", v});
  }
  // ---- AI
  {
    vector<MI> tasks;
    for (auto& ti : ai::tasks()) { if (ti.task == ai::Task::Chat) continue; tasks.push_back(item(ti.title, string("ai:") + ti.id, corpus, false, ti.icon)); }
    vector<MI> v = {fill(item("Ask the AI assistant", "ai:chat")), submenu("AI tasks", tasks, "sparkle"), sep(), fill(item(a.live.open ? "Close Live AI" : "Live AI", "live")),
                    fill(item("Live AI: start talking", "live:talk")), fill(item("Live AI: voice orb", "live:orb")), item("Speech detection in the app", "livevad", true, a.settings.j["liveClientVad"].boolean(true), "mic"), sep(),
                    fill(item("AI settings\xE2\x80\xA6", "settings"))};
    menus.push_back({"AI", v});
  }
  // ---- Help
  {
    MI kb = item("Keyboard shortcuts & commands", "palette", true, false, "keyboard"); kb.sc = "F1";
    vector<MI> v = {kb, item("Documentation folder", "docs", true, false, "book"), sep(), fill(item("Cite VOSStudio\xE2\x80\xA6", "cite")), item("Application data folder (logs, settings)", "appdata", true, false, "folder"), sep(),
                    item("Third-party notices (PDFium + Tectonic)\xE2\x80\xA6", "notices"), item(string("About VOSStudio ") + kAppVersion, "about", true, false, "info")};
    menus.push_back({"Help", v});
  }
  return menus;
}

void App::menuRun(const string& id) {
  if (id.empty()) return;
  if (id == "exit") { PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
  if (startsWith(id, "recent:")) { cmdOpenProject(id.substr(7)); return; }
  if (id == "palette") { paletteOpen = true; paletteQuery.clear(); paletteSel = 0; return; }
  if (id == "search") { ui.focusText(ui.id("ti:search"), search); return; }
  if (id == "minimap") { showMinimap = !showMinimap; return; }
  if (id == "inspector") { inspectorOpen = !inspectorOpen; return; }
  if (id == "aiwrite") { setPage(PG_AI); return; }
  if (id == "rdattach") { readerAttachDialog(-1); return; }
  if (id == "rdfetch") { vector<int> all; for (size_t i = 0; i < P->corpus.recs.size(); i++) all.push_back(int(i)); fetchAsk(all, true); return; }
  if (id == "rdmatch") { int n = P->library.relinkAll(P->corpus); for (auto& it : P->library.items) if (it.recordKey.empty()) readerIdentify(it.id); if (n) P->dirty = true; ui.toast(plural(n, "paper") + " linked", "By DOI, else by title.", 1, 3); return; }
  if (id == "rdfind") { if (readerOpen && rd) { rd->pane = 3; rd->findFocus = true; } return; }
  if (id == "rddark") { readerSetDark(!readerDark()); return; }
  if (id == "rdtwoup") { readerSetLayout(!readerTwoUp(), readerCoverAlone()); return; }
  if (id == "rdrotate") { if (readerOpen) readerRotate(1); return; }
  if (id == "rdprint") { if (readerOpen) readerPrintDialog(); return; }
  if (id == "rdsavepdf") { if (PdfItem* it = readerItem()) readerSaveIntoPdf(it->id); return; }
  if (id == "rdclose") { closeReader(); page = PG_READ; return; }
  if (id == "rdnotes") { readerExportNotes(""); return; }
  if (id == "rdcoding") { readerExportCoding(); return; }
  if (id == "about") { ui.toast(string("VOSStudio ") + kAppVersion, "Free software under the MIT licence. The app bundles PDFium and Tectonic; see Help > Third-party notices. Help > Cite VOSStudio copies a reference for your paper.", 0, 8); return; }
  if (id == "notices") {  // the bundled PDFium and Tectonic notices, written next to the settings and opened in the default editor
    string txt = engineThirdPartyNoticesText();
    if (txt.empty()) { ui.toast("Third-party notices", "This build carries no embedded engine notices.", 2, 6); return; }
    string pth = appDataDir() + "\\THIRD-PARTY-NOTICES.txt";
    if (writeFileU(pth, txt)) openUrl(pth); else ui.toast("Could not write", pth, 3);
    return;
  }
  if (id == "docs") { revealInExplorer(exeDir() + "\\docs"); return; }
  if (id == "appdata") { revealInExplorer(appDataDir()); return; }
  if (startsWith(id, "split") && id.size() == 6 && isdigit(uint8_t(id[5]))) { int m = id[5] - '0'; if (m && !hasMap()) { ui.toast("Split view", "Build a map first; the split view shows the map, linked views of it (overlay, density, timeline, geography, 3D) and charts side by side.", 2, 4); return; } setSplit(m); return; }
  if (id == "wcut") { writerCopy(true); return; }
  if (id == "wcopy") { writerCopy(false); return; }
  if (id == "wcopybib") { writerCopy(false, true); return; }
  if (id == "wopenword") { writerOpenInWord(); return; }
  if (id == "wpaste") { writerPaste(false); return; }
  if (id == "wpasteplain") { writerPaste(true); return; }
  if (id == "wselall") { wed.selectAll(); return; }
  if (id == "wnew") { openWriter(); writerNewConfirm_ = true; ui.openPopup("wnew"); return; }
  if (id == "wfind") { openWriter(); writerPane = 3; ui.focusText(ui.id("ti:wfind"), writerFind); return; }
  if (id == "wpane1") { openWriter(); writerPane = writerPane == 1 ? 0 : 1; return; }
  if (id == "wpane2") { openWriter(); writerPane = writerPane == 2 ? 0 : 2; return; }
  if (id == "wzoom+" || id == "wzoom-" || id == "wzoom0") { openWriter(); writerZoom = id == "wzoom0" ? 1.f : clampv(writerZoom * (id == "wzoom+" ? 1.1f : 1 / 1.1f), 0.4f, 3.f); writerZoomT_ = ui.time; writerZoomAnchorY_ = -1; return; }
  if (startsWith(id, "wcs:")) { wed.setCiteStyle(id.substr(4)); writerFollow_ = true; return; }
  if (startsWith(id, "wins:")) {
    openWriter();
    string k = id.substr(5);
    if (k == "table") ui.openPopup("wtbl");
    else if (k == "chart") ui.openPopup("wchart");
    else if (k == "map") writerInsertMap();
    else if (k == "cite") { writerCiteSel.assign(hasCorpus() ? P->corpus.recs.size() : 0, 0); writerCiteFocus_ = true; ui.openPopup("wcite"); }
    else if (k == "rule") wed.insertRule();
    else if (k == "pb") wed.insertPageBreak();
    else if (k == "toc") wed.insertToc();
    writerFollow_ = true;
    return;
  }
  if ((id == "undo" || id == "redo") && writerOpen) { if (id == "undo") wed.undo(); else wed.redo(); writerFollow_ = true; return; }
  for (auto& c : commands) if (c.id == id) { c.run(); return; }
  ui.toast("Menu", "Nothing registered for \"" + id + "\".", 2, 3);
}

static float columnHeight(const vector<MI>& items, float s);

// One clipped, scrollable column. Long menus stay inside the window rather than painting rows over the page.
static string drawMenuColumn(Ui& ui, const vector<MI>& items, const Rect& col, const string& keyPrefix, int& subIdx, bool subOpen, int* subClicked = nullptr, float* scrollOffset = nullptr) {
  const float s = ui.s;
  const float rowH = 28 * s, sepH = 9 * s;
  string clicked;
  const string scrollKey = "menucol:" + keyPrefix + ":" + (items.empty() ? string() : items.front().label) + ":" + std::to_string(items.size());
  ui.beginScroll(scrollKey, col);
  const float sy = ui.scrollY();
  if (scrollOffset) *scrollOffset = sy;
  float yy = col.y + 4 * s - sy;
  for (size_t i = 0; i < items.size(); i++) {
    const MI& it = items[i];
    if (it.sep) { ui.line(col.x + 10 * s, yy + 4 * s, col.r() - 10 * s, yy + 4 * s, ui.c.border); yy += sepH; continue; }
    Rect rr{col.x + 4 * s, yy, col.w - 8 * s, rowH - 2 * s};
    bool hov = rr.has(ui.in.mx, ui.in.my);
    bool isOpenSub = subOpen && subIdx == int(i);
    bool click = ui.listRow(rr, keyPrefix + std::to_string(i), isOpenSub);
    if (!it.sub.empty() && (hov || click)) { subIdx = int(i); if (click && subClicked) *subClicked = int(i); }
    else if (hov && it.sub.empty()) subIdx = -1;
    if (click && it.enabled && it.sub.empty()) clicked = it.id;
    Color fg = it.enabled ? ui.c.text : ui.c.textFaint;
    if (!it.icon.empty()) ui.icon(it.icon, rr.x + 15 * s, rr.y + rr.h / 2, 14 * s, it.enabled ? ui.c.textDim : ui.c.textFaint, 1.6f);
    if (it.checked) ui.icon("check", rr.x + 15 * s, rr.y + rr.h / 2, 13 * s, ui.c.accent, 2.2f);
    ui.text({rr.x + 32 * s, rr.y, rr.w - 36 * s - (it.sub.empty() ? 70 * s : 16 * s), rr.h}, it.label, 12.5f * s, fg);
    if (!it.sub.empty()) ui.icon("chev-right", rr.r() - 12 * s, rr.y + rr.h / 2, 11 * s, ui.c.textDim, 2.f);
    else if (!it.sc.empty()) ui.text({rr.r() - 100 * s, rr.y, 94 * s, rr.h}, it.sc, 11 * s, ui.c.textFaint, AL_RIGHT);
    yy += rowH;
  }
  ui.endScroll(columnHeight(items, s));
  return clicked;
}

static float columnHeight(const vector<MI>& items, float s) {
  float h = 8 * s;
  for (auto& it : items) h += it.sep ? 9 * s : 28 * s;
  return h;
}

static float columnWidth(Ui& ui, const vector<MI>& items) {
  const float s = ui.s;
  float w = 200 * s;
  for (auto& it : items) if (!it.sep) w = std::max(w, ui.textW(it.label, 12.5f * s) + 40 * s + (it.sc.empty() ? 30 * s : ui.textW(it.sc, 11 * s) + 46 * s));
  return std::min(w, 380 * s);
}

void App::drawMenuBar(float& x) {
  const float s = ui.s;
  const float y = topR.y + 10 * s, bh = 32 * s;
  vector<Menu> menus = menuModel(*this);
  // the titles as a bar when they fit in a third of the window; otherwise one "Menu" button
  float total = 0;
  vector<float> tw;
  for (auto& m : menus) { float w = ui.textW(m.title, 12.5f * s, 500) + 18 * s; tw.push_back(w); total += w; }
  const bool bar = total <= topR.w * 0.30f;  // ~1500 px at 100 %: below that the view switcher and the right-hand block need the room
  const bool open = ui.isPopupOpen("menu");
  if (!open) { menuSub_ = -1; }
  if (bar) {
    for (size_t i = 0; i < menus.size(); i++) {
      Rect tr{x, y, tw[i], bh};
      uint64_t idv = ui.id("mb:" + menus[i].title);
      bool hov = false;
      bool click = ui.behave(idv, tr, &hov);
      bool cur = open && menuCur_ == menus[i].title;
      if (click) {
        if (cur) ui.closePopup();
        else { menuCur_ = menus[i].title; menuSub_ = -1; ui.openPopup("menu"); }
      } else if (hov && open && !cur) { menuCur_ = menus[i].title; menuSub_ = -1; }
      if (cur) ui.fill(tr, ui.c.active, 7 * s);
      else if (hov) ui.fill(tr, ui.c.hover, 7 * s);
      ui.text(tr, menus[i].title, 12.5f * s, cur ? ui.c.text : hov ? ui.c.text : ui.c.textDim, AL_CENTER, cur ? 600 : 500);
      if (cur) menuAnchor_ = tr;
      x += tw[i];
    }
  } else {
    Rect tr{x, y, 78 * s, bh};
    uint64_t idv = ui.id("mb:menu");
    bool hov = false;
    if (ui.behave(idv, tr, &hov)) { if (open) ui.closePopup(); else { menuCur_.clear(); menuSub_ = -1; ui.openPopup("menu"); } }
    if (open || hov) ui.fill(tr, open ? ui.c.active : ui.c.hover, 7 * s);
    ui.icon("menu", tr.x + 16 * s, tr.y + bh / 2, 16 * s, open || hov ? ui.c.text : ui.c.textDim, 1.7f);
    ui.text({tr.x + 30 * s, tr.y, tr.w - 34 * s, bh}, "Menu", 12.5f * s, open || hov ? ui.c.text : ui.c.textDim, AL_LEFT, 500);
    ui.tipFor(idv, "File, Edit, View, Map, Analyse, Write, AI, Help");
    menuAnchor_ = tr;
    x += tr.w;
  }
  x += 6 * s;
  if (!ui.isPopupOpen("menu")) return;
  // ---- the open menu
  ui.overlay([this, menus, bar, s]() {
    const Menu* M = nullptr;
    for (auto& m : menus) if (m.title == menuCur_) M = &m;
    Rect anchor = menuAnchor_;
    if (!bar && !M) {  // the title list (narrow windows)
      vector<MI> titles;
      for (auto& m : menus) { MI t; t.label = m.title; t.sub = m.items; titles.push_back(t); }
      Rect col{anchor.x, anchor.b() + 4 * s, 220 * s, columnHeight(titles, s)};
      ui.shadow(col, 8 * s);
      ui.fill(col, ui.c.panel2, 8 * s);
      ui.stroke(col, ui.c.border, 8 * s);
      ui.popupRect(col);
      float yy = col.y + 4 * s;
      for (size_t i = 0; i < titles.size(); i++) {
        Rect rr{col.x + 4 * s, yy, col.w - 8 * s, 26 * s};
        if (ui.listRow(rr, "mbt:" + std::to_string(i), false)) { menuCur_ = titles[i].label; menuSub_ = -1; }
        ui.text({rr.x + 12 * s, rr.y, rr.w - 30 * s, rr.h}, titles[i].label, 12.5f * s, ui.c.text, AL_LEFT, 500);
        ui.icon("chev-right", rr.r() - 12 * s, rr.y + rr.h / 2, 11 * s, ui.c.textDim, 2.f);
        yy += 28 * s;
      }
      return;
    }
    if (!M) return;
    vector<MI> items = M->items;
    if (!bar) { MI back; back.label = "\xE2\x80\xB9 " + M->title; back.id = "back"; back.icon = "arrow-left"; items.insert(items.begin(), {back, sep()}); }
    // narrow mode: a submenu replaces the column (with a Back row) instead of cascading
    if (!bar && menuSub_ >= 0 && size_t(menuSub_) < items.size() && !items[size_t(menuSub_)].sub.empty()) {
      MI back; back.label = "\xE2\x80\xB9 " + items[size_t(menuSub_)].label; back.id = "subback"; back.icon = "arrow-left";
      vector<MI> subItems = items[size_t(menuSub_)].sub;
      subItems.insert(subItems.begin(), {back, sep()});
      items = subItems;
    }
    float colW = columnWidth(ui, items), colH = columnHeight(items, s);
    Rect col{anchor.x, anchor.b() + 4 * s, colW, colH};
    col.h = std::min(col.h, std::max(60 * s, float(g.H) - 16 * s));
    if (col.b() > float(g.H) - 8 * s) col.y = std::max(8 * s, float(g.H) - 8 * s - col.h);
    ui.shadow(col, 8 * s);
    ui.fill(col, ui.c.panel2, 8 * s);
    ui.stroke(col, ui.c.border, 8 * s);
    Rect owned = col;
    string clicked;
    int subNow = bar ? menuSub_ : -1, subClicked = -1;
    float parentScroll = 0;
    string c1 = drawMenuColumn(ui, items, col, "mbi:", subNow, bar && menuSub_ >= 0, &subClicked, &parentScroll);
    if (!c1.empty()) clicked = c1;
    if (bar) { if (col.has(ui.in.mx, ui.in.my) || subNow != menuSub_) menuSub_ = subNow; }
    else if (subClicked >= 0) menuSub_ = subClicked;  // narrow mode: a click on a row with a submenu opens its page

    // Draw after the parent so the submenu sits on top; account for the parent's scroll when aligning the row.
    if (bar && menuSub_ >= 0 && size_t(menuSub_) < items.size() && !items[size_t(menuSub_)].sub.empty()) {
      const vector<MI>& sub = items[size_t(menuSub_)].sub;
      float rowY = col.y + 4 * s - parentScroll;
      for (int i = 0; i < menuSub_; i++) rowY += items[size_t(i)].sep ? 9 * s : 28 * s;
      float sw = columnWidth(ui, sub), sh = std::min(columnHeight(sub, s), std::max(80 * s, float(g.H) - 16 * s));
      Rect sc{col.r() - 2 * s, rowY - 4 * s, sw, sh};
      if (sc.r() > float(g.W) - 8 * s) sc.x = col.x - sw + 2 * s;
      if (sc.x < 8 * s) sc.x = 8 * s;
      if (sc.b() > float(g.H) - 8 * s) sc.y = std::max(8 * s, float(g.H) - 8 * s - sh);
      if (sc.y < 8 * s) sc.y = 8 * s;
      ui.shadow(sc, 8 * s);
      ui.fill(sc, ui.c.panel2, 8 * s);
      ui.stroke(sc, ui.c.border, 8 * s);
      int dummy = -1;
      string c2 = drawMenuColumn(ui, sub, sc, "mbs:", dummy, false);
      if (!c2.empty()) clicked = c2;
      float ox0 = std::min(owned.x, sc.x), oy0 = std::min(owned.y, sc.y), ox1 = std::max(owned.r(), sc.r()), oy1 = std::max(owned.b(), sc.b());
      owned = {ox0, oy0, ox1 - ox0, oy1 - oy0};
    }
    ui.popupRect(owned);
    if (clicked.empty()) return;
    if (clicked == "back") { menuCur_.clear(); menuSub_ = -1; return; }
    if (clicked == "subback") { menuSub_ = -1; return; }
    ui.closePopup();
    menuSub_ = -1;
    menuRun(clicked);
  });
}

}  // namespace win
}  // namespace vs
