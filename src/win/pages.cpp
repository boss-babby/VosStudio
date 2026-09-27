// VOSStudio Native — left panel pages: Data, Build, Look, Analyse, Trends, Actors, Publish
#include "../core/records.h"
#include "app.h"

namespace vs {
namespace win {

namespace {
// label on the left, control on the right
Rect field(Ui& ui, Lay& L, const string& label, float h = 30, float labelFrac = 0.42f) {
  float s = ui.s;
  Rect r = L.row(h * s);
  float lw = r.w * labelFrac;
  ui.text({r.x, r.y, lw - 6 * s, r.h}, label, 12.5f * s, ui.c.textDim);
  if (const string* h = ui.helpOf(label)) ui.help(std::min(r.x + ui.textW(label, 12.5f * s) + 5 * s, r.x + lw - 22 * s), r.y + r.h / 2, *h);
  return {r.x + lw, r.y, r.w - lw, r.h};
}
int indexOf(const vector<string>& v, const string& x) {
  for (size_t i = 0; i < v.size(); i++) if (v[i] == x) return int(i);
  return 0;
}
const vector<std::pair<Unit, const char*>>& corpusUnits() {
  static vector<std::pair<Unit, const char*>> u = {{Unit::AllKeywords, "All keywords"}, {Unit::Keywords, "Author keywords"}, {Unit::IndexTerms, "Index terms"},
                                                   {Unit::Terms, "Title/abstract terms"}, {Unit::Authors, "Authors"}, {Unit::Orgs, "Organisations"},
                                                   {Unit::Countries, "Countries"}, {Unit::Sources, "Sources"}, {Unit::Refs, "Cited references"}};
  return u;
}
vector<string> corpusUnitLabels() { vector<string> v; for (auto& u : corpusUnits()) v.push_back(u.second); return v; }
}  // namespace

// A chart card inside a panel: title row with expand + export menu, then the chart
float App::chartBox(Lay& L, const string& key, const ChartDef& def, float h) {
  float s = ui.s;
  frameCharts.push_back(def);
  Rect card = L.row(h * s + 40 * s);
  ui.fill(card, ui.c.card, 10 * s);
  ui.stroke(card, ui.c.border, 10 * s);
  ui.text({card.x + 12 * s, card.y + 6 * s, card.w - 110 * s, 24 * s}, def.title, 12.5f * s, ui.c.text, AL_LEFT, 650);
  float bx = card.r() - 30 * s;
  ui.pushId(key);
  if (ui.iconButton({bx, card.y + 5 * s, 26 * s, 26 * s}, "download", "Export chart (SVG / PDF / PNG)")) ui.openPopup("exp");
  if (ui.isPopupOpen("exp")) {
    Rect anchor{bx, card.y + 32 * s, 26 * s, 0};
    ChartDef d = def;
    ui.overlay([this, anchor, d, s]() {
      bool many = pageCharts.size() > 1;
      Rect pr{anchor.r() - 230 * s, anchor.y, 230 * s, 3 * 32 * s + 8 * s + (many ? 32 * s + 9 * s : 0)};
      ui.shadow(pr, 8 * s);
      ui.fill(pr, ui.c.panel2, 8 * s);
      ui.stroke(pr, ui.c.border, 8 * s);
      ui.popupRect(pr);
      const char* f[3] = {"svg", "pdf", "png"};
      const char* lbl[3] = {"SVG (vector)", "PDF (vector)", "PNG (300 dpi)"};
      for (int i = 0; i < 3; i++) {
        Rect rr{pr.x + 4 * s, pr.y + 4 * s + i * 32 * s, pr.w - 8 * s, 30 * s};
        if (ui.listRow(rr, string("ef") + f[i], false)) { ui.closePopup(); cmdExportChart(d, f[i]); }
        ui.text({rr.x + 10 * s, rr.y, rr.w, rr.h}, lbl[i], 12.5f * s, ui.c.text);
      }
      if (many) {
        float ly = pr.y + 4 * s + 3 * 32 * s + 4 * s;
        ui.line(pr.x + 10 * s, ly, pr.r() - 10 * s, ly, ui.c.border);
        Rect rr{pr.x + 4 * s, ly + 5 * s, pr.w - 8 * s, 30 * s};
        if (ui.listRow(rr, "efall", false)) { ui.closePopup(); cmdExportChartsPdf(pageCharts); }
        ui.text({rr.x + 10 * s, rr.y, rr.w, rr.h}, "All " + std::to_string(pageCharts.size()) + " charts, one per page (PDF)", 12.5f * s, ui.c.text);
      }
    });
  }
  bx -= 30 * s;
  if (ui.iconButton({bx, card.y + 5 * s, 26 * s, 26 * s}, "fit", "Expand into the main area")) { mainChart = def; mainChartOpen = true; }
  ui.popId();
  Rect cr{card.x + 8 * s, card.y + 34 * s, card.w - 16 * s, h * s};
  ChartTheme t = chartTheme(false);
  string ck = key + "|" + def.title;
  ChartCacheEntry& ce = chartCache[ck];
  uint64_t sig = chartSig();
  float cw = cr.w / s, chh = cr.h / s;
  if (chartFresh > 0 || ce.frame == 0 || ce.sig != sig || ce.w != cw || ce.h != chh || ce.dark != ui.dark) {
    ce.sc = def.make(cw, chh, t);
    ce.sig = sig; ce.w = cw; ce.h = chh; ce.dark = ui.dark;
  }
  ce.frame = std::max(1, frameNo);
  const Scene& sc = ce.sc;
  drawScene(g.dc.get(), g.d2f.get(), g.dw.get(), g, sc, cr.x, cr.y, s);
  if (ui.mouseIn(cr) && !ui.anyPopup() && !ui.anyModal()) chartHover(sc, cr.x, cr.y, s);
  if (def.onClick && ui.mouseIn(cr) && !ui.anyPopup() && !ui.anyModal()) {
    int tag = hitTag(sc, (ui.in.mx - cr.x) / s, (ui.in.my - cr.y) / s);
    if (tag >= 0) { ui.cursor = "hand"; if (ui.in.released[0] && ui.active == 0) def.onClick(tag); }
  }
  return card.h;
}


// ------------------------------------------------------------------ interactive charts: hover highlight + value card
void App::chartHover(const Scene& sc, float ox, float oy, float scale) {
  double x = (ui.in.mx - ox) / scale, y = (ui.in.my - oy) / scale;
  const Prim* p = tipAt(sc, x, y);
  if (!p) return;
  Color hl = ui.c.text.withA(0.9f);
  if (p->type == Prim::Rect) {
    ui.stroke({ox + p->x * scale - 1, oy + p->y * scale - 1, p->w * scale + 2, p->h * scale + 2}, hl, 2 * scale, 1.6f * scale);
  } else if (p->type == Prim::Circle) {
    float r = std::max(p->r, 2.5f) * scale;
    ui.circle(ox + p->x * scale, oy + p->y * scale, r + 2.5f * scale, hl, false, 1.6f * scale);
    if (!p->fill) ui.circle(ox + p->x * scale, oy + p->y * scale, 3 * scale, ui.c.accent);
  } else if (p->type == Prim::Path && p->fill) {
    // outline the hovered flow
    Com<ID2D1PathGeometry> geo;
    if (SUCCEEDED(g.d2f->CreatePathGeometry(geo.put()))) {
      Com<ID2D1GeometrySink> sk;
      geo->Open(sk.put());
      bool open = false;
      for (auto& d : p->d) {
        if (d.op == 'M') { if (open) sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->BeginFigure({ox + d.x1 * scale, oy + d.y1 * scale}, D2D1_FIGURE_BEGIN_FILLED); open = true; }
        else if (d.op == 'L') sk->AddLine({ox + d.x1 * scale, oy + d.y1 * scale});
        else if (d.op == 'Q') sk->AddQuadraticBezier({{ox + d.x1 * scale, oy + d.y1 * scale}, {ox + d.x2 * scale, oy + d.y2 * scale}});
        else if (d.op == 'Z' && open) { sk->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; }
      }
      if (open) sk->EndFigure(D2D1_FIGURE_END_OPEN);
      sk->Close();
      Com<ID2D1SolidColorBrush> br;
      g.dc->CreateSolidColorBrush(D2D1::ColorF(p->fillC.r, p->fillC.g, p->fillC.b, std::min(1.f, p->fillC.a * 2.2f)), br.put());
      g.dc->FillGeometry(geo.get(), br.get());
    }
  }
  ui.richTip(p->tip);
}

// ------------------------------------------------------------------ publish: full-screen figure preview (zoom & pan)
// ---- cached scene drawing: a figure (with every view as a panel) is thousands of paths plus raster
// panels; redrawing it vector by vector on every frame made scrolling the Publish panel stutter.
// The scene is rendered once into an offscreen bitmap and only re-rendered when its content key or
// on-screen pixel size changes; scrolling and panning just blit the bitmap.
namespace {
struct SceneCache {
  Com<ID2D1BitmapRenderTarget> rt;
  Com<ID2D1Bitmap> bmp;
  string key;
  int w = 0, h = 0;
};
SceneCache s_pubCache, s_zoomCache;
unsigned s_figGen = 0;  // bumped whenever figPreview is rebuilt (the cache key)

void drawSceneCached(Gfx& g, SceneCache& c, const Scene& sc, float ox, float oy, float k, bool serif, const string& key) {
  int w = int(std::ceil(sc.W * k)), h = int(std::ceil(sc.H * k));
  auto direct = [&] { drawScene(g.dc.get(), g.d2f.get(), g.dw.get(), g, sc, ox, oy, k, serif); };
  if (w < 1 || h < 1 || w > 6000 || h > 6000 || double(w) * h > 12e6) { direct(); return; }  // huge zoom: draw directly
  ox = std::round(ox); oy = std::round(oy);
  string full = key + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" + fmtNum(k, 5) + (serif ? "s" : "");
  if (full != c.key || !c.bmp) {
    if (!c.rt || c.w != w || c.h != h) {
      c.rt.reset(); c.bmp.reset(); c.key.clear();
      D2D1_SIZE_F sz = D2D1::SizeF(float(w), float(h));
      D2D1_SIZE_U px = D2D1::SizeU(UINT32(w), UINT32(h));
      D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
      if (FAILED(g.dc->CreateCompatibleRenderTarget(&sz, &px, &pf, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, c.rt.put()))) { c.rt.reset(); direct(); return; }
      c.rt->SetDpi(96, 96);
      c.w = w; c.h = h;
    }
    c.rt->BeginDraw();
    c.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    c.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    drawScene(c.rt.get(), g.d2f.get(), g.dw.get(), g, sc, 0, 0, k, serif);
    c.bmp.reset();
    if (FAILED(c.rt->EndDraw()) || FAILED(c.rt->GetBitmap(c.bmp.put()))) { c.rt.reset(); c.bmp.reset(); c.key.clear(); direct(); return; }
    c.key = full;
  }
  g.dc->DrawBitmap(c.bmp.get(), D2D1::RectF(ox, oy, ox + float(w), oy + float(h)), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}
}  // namespace

void App::drawFigureZoom() {
  float s = ui.s;
  static float lastX = 0, lastY = 0;
  static bool dragging = false;
  Rect W{0, 0, float(g.W), float(g.H)};
  // opening animation: the backdrop fades in while the figure grows out of its preview (~220 ms)
  float e = 1;
  if (figZoomT0 >= 0) {
    double k = scriptMode ? 1.0 : (ui.time - figZoomT0) / 0.22;
    if (k >= 1) figZoomT0 = -1;
    else { float t = float(std::max(0.0, k)); e = 1 - (1 - t) * (1 - t) * (1 - t); ui.animating = true; }
  }
  // neutral viewing backdrop (like a document viewer), the app's own bar on top
  Color backdrop = ui.dark ? Color::hex(0x161618) : Color::hex(0xdedee2);
  ui.fill(W, backdrop.withA(e));
  const Scene& sc = figPreview;
  if (sc.W <= 0) { figZoomOpen = false; return; }
  Rect hdr{0, 0, W.w, 52 * s};
  Rect area{32 * s, hdr.b() + 24 * s, W.w - 64 * s, W.h - hdr.h - 24 * s - 44 * s};
  double kFit = std::min(area.w / sc.W, area.h / sc.H);
  double k = kFit * figZoomK;
  // wheel zoom about the cursor
  if (area.has(ui.in.mx, ui.in.my) && ui.in.wheel != 0) {
    double nk = clampv(figZoomK * std::pow(1.2, ui.in.wheel > 0 ? 1 : -1), 0.25, 40.0);
    double cx = area.x + area.w / 2 + figZoomX, cy = area.y + area.h / 2 + figZoomY;  // figure centre on screen
    double f = nk / figZoomK;
    figZoomX += (ui.in.mx - cx) * (1 - f);
    figZoomY += (ui.in.my - cy) * (1 - f);
    figZoomK = nk;
    k = kFit * figZoomK;
  }
  // drag to pan
  if (ui.in.pressed[0] && area.has(ui.in.mx, ui.in.my) && ui.active == 0) { dragging = true; lastX = ui.in.mx; lastY = ui.in.my; }
  if (dragging) {
    if (!ui.in.down[0]) dragging = false;
    else { figZoomX += ui.in.mx - lastX; figZoomY += ui.in.my - lastY; lastX = ui.in.mx; lastY = ui.in.my; ui.cursor = "move"; }
  } else if (area.has(ui.in.mx, ui.in.my)) ui.cursor = "move";
  float fw = float(sc.W * k), fh = float(sc.H * k);
  float ox = float(area.x + area.w / 2 + figZoomX - fw / 2), oy = float(area.y + area.h / 2 + figZoomY - fh / 2);
  if (e < 1 && figZoomFrom.w > 1 && sc.W > 0) {
    double k0 = std::min(figZoomFrom.w / sc.W, figZoomFrom.h / sc.H);
    double kk = k0 + (k - k0) * e;
    float fw0 = float(sc.W * k0), fh0 = float(sc.H * k0);
    float ox0 = figZoomFrom.x + (figZoomFrom.w - fw0) / 2, oy0 = figZoomFrom.y + (figZoomFrom.h - fh0) / 2;
    k = kk;
    fw = float(sc.W * kk); fh = float(sc.H * kk);
    ox = ox0 + (ox - ox0) * e; oy = oy0 + (oy - oy0) * e;
  }
  Rect view{0, hdr.b(), W.w, W.h - hdr.h};
  ui.pushClip(view);
  ui.shadow({ox, oy, fw, fh}, 2 * s, 22 * s);
  ui.fill({ox, oy, fw, fh}, Color(1, 1, 1));
  ui.stroke({ox - 0.5f, oy - 0.5f, fw + 1, fh + 1}, Color(0, 0, 0, ui.dark ? 0.5f : 0.12f), 0);
  if (P->fig.transparent) {  // checkerboard behind transparent figures
    float c = 12 * s;
    ui.fill({ox, oy, fw, fh}, Color(1, 1, 1));
    for (float yy = std::max(oy, area.y); yy < std::min(oy + fh, area.b()); yy += c)
      for (float xx = std::max(ox, area.x); xx < std::min(ox + fw, area.r()); xx += c)
        if ((int((xx - ox) / c) + int((yy - oy) / c)) % 2) ui.fill({xx, yy, std::min(c, ox + fw - xx), std::min(c, oy + fh - yy)}, Color(0.9f, 0.9f, 0.92f));
  }
  drawSceneCached(g, s_zoomCache, sc, ox, oy, float(k), P->fig.serif, std::to_string(s_figGen));  // panning reuses the bitmap
  ui.popClip();
  // ---- toolbar: same bar as the main window (the window buttons sit at its right end)
  ui.fill(hdr, ui.c.rail.withA(e));
  ui.line(0, hdr.b() - 0.5f, hdr.w, hdr.b() - 0.5f, ui.c.border.withA(e));
  double actual = s * 96.0 / 72.0;  // 1 pt at 100 % (96 dpi logical)
  float by = 11 * s, bh = 30 * s, cy = by + bh / 2;
  ui.pushId("figzoom");
  // flat button inside a pill group (the view switcher's look)
  auto flat = [&](const Rect& r, const string& key, const string& label, const string& ic, const string& tip, bool tab = false) {
    uint64_t idv = ui.id(key);
    bool hov = false, held = false;
    bool c = ui.behave(idv, r, &hov, &held);
    if (hov) ui.fill(r, held ? ui.c.active : ui.c.hover, 5.5f * s);
    Color fg = ui.c.text.withA(0.85f);
    if (label.empty()) ui.icon(ic, r.x + r.w / 2, r.y + r.h / 2, 15 * s, fg, 1.7f);
    else if (!ic.empty()) {
      float tw = ui.textW(label, 12 * s, 550), x0 = r.x + (r.w - tw - 20 * s) / 2;
      ui.icon(ic, x0 + 7 * s, r.y + r.h / 2, 13 * s, fg, 1.7f);
      ui.text({x0 + 20 * s, r.y, tw + 4 * s, r.h}, label, 12 * s, fg, AL_LEFT, 550);
    } else ui.text(r, label, 12 * s, fg, AL_CENTER, 550, tab);
    if (!tip.empty()) ui.tipFor(idv, tip);
    return c;
  };
  Color pillBg = ui.dark ? Color(1, 1, 1, 0.07f) : Color(0, 0, 0, 0.055f);
  auto divider = [&](float x) { ui.line(x, by + 8 * s, x, by + bh - 8 * s, ui.c.border); };
  // centre: zoom controls, centred on the window like the view switcher
  float zw = 30 * s, pw = 56 * s, fitW = 44 * s, actW = 84 * s;
  float groupW = (zw * 2 + pw + 4 * s) + 8 * s + (fitW + actW + 4 * s);
  float gx = std::round(W.w / 2 - groupW / 2);
  Rect zp{gx, by, zw * 2 + pw + 4 * s, bh};
  ui.fill(zp, pillBg, 7 * s);
  Rect r1{zp.x + 2 * s, by + 2 * s, zw, bh - 4 * s};
  if (flat(r1, "zout", "", "minus", "Zoom out  (-)")) figZoomK = clampv(figZoomK / 1.25, 0.25, 40.0);
  Rect r2{r1.r(), r1.y, pw, r1.h};
  if (flat(r2, "zpct", fmtNum(k / actual * 100, 0) + "%", "", "Actual size", true)) { figZoomK = actual / kFit; figZoomX = figZoomY = 0; }
  Rect r3{r2.r(), r1.y, zw, r1.h};
  if (flat(r3, "zin", "", "plus", "Zoom in  (+)")) figZoomK = clampv(figZoomK * 1.25, 0.25, 40.0);
  Rect fp{zp.r() + 8 * s, by, fitW + actW + 4 * s, bh};
  ui.fill(fp, pillBg, 7 * s);
  bool isFit = std::fabs(figZoomK - 1) < 1e-6 && figZoomX == 0 && figZoomY == 0;
  bool isAct = std::fabs(k - actual) < 1e-3 * actual;
  Rect r4{fp.x + 2 * s, by + 2 * s, fitW, bh - 4 * s}, r5{r4.r(), r4.y, actW, r4.h};
  auto selBg = [&](const Rect& r) {
    ui.fill({r.x, r.y + 0.8f * s, r.w, r.h}, Color(0, 0, 0, ui.dark ? 0.3f : 0.07f), 5.5f * s);
    ui.fill(r, ui.dark ? Color::hex(0x636366) : Color::hex(0xffffff), 5.5f * s);
  };
  if (isFit) selBg(r4);
  if (isAct) selBg(r5);
  if (flat(r4, "zfit", "Fit", "", "Fit to window  (0)")) { figZoomK = 1; figZoomX = figZoomY = 0; }
  if (flat(r5, "zact", "Actual Size", "", "Print size at 100 %  (1)")) { figZoomK = actual / kFit; figZoomX = figZoomY = 0; }
  // right: export + done, kept clear of the window buttons
  float rx = W.w - captionW() - 12 * s;
  rx -= 72 * s;
  if (ui.button({rx, by, 72 * s, bh}, "Done", BTN_PRIMARY)) figZoomOpen = false;
  ui.tip("Close the preview  (Esc)");
  rx -= 12 * s;
  divider(rx);
  rx -= 12 * s;
  const char* fmts[3] = {"SVG", "PDF", "PNG"};
  const char* ftips[3] = {"Export as SVG (vector)", "Export as PDF (vector)", "Export as PNG (image)"};
  float ew = 52 * s, epw = ew * 3 + 4 * s;
  Rect ep{rx - epw, by, epw, bh};
  ui.fill(ep, pillBg, 7 * s);
  for (int i = 0; i < 3; i++)
    if (flat({ep.x + 2 * s + i * ew, by + 2 * s, ew, bh - 4 * s}, string("ex") + fmts[i], fmts[i], "", ftips[i])) cmdExportFigure(string(fmts[i]) == "SVG" ? "svg" : string(fmts[i]) == "PDF" ? "pdf" : "png");
  float ex = ep.x - 8 * s;
  float lw = ui.textW("Export", 12 * s, 400);
  if (ex - lw > gx + groupW + 16 * s) { ui.icon("download", ex - lw - 12 * s, cy, 13 * s, ui.c.textDim, 1.7f); ui.text({ex - lw, by, lw, bh}, "Export", 12 * s, ui.c.textDim, AL_LEFT); }
  ui.popId();
  // left: title and print size (truncated before the zoom controls)
  float lx = 16 * s, lMax = gx - 20 * s - lx;
  string title = P->fig.title.empty() ? string("Figure Preview") : P->fig.title;
  string sub = fmtNum(P->fig.wmm, 0) + " \xC3\x97 " + fmtNum(P->fig.hmm, 0) + " mm" + (P->fig.transparent ? string("  \xC2\xB7  Transparent") : string());
  if (lMax > 60 * s) {
    ui.text({lx, 8 * s, lMax, 20 * s}, title, 13 * s, ui.c.text, AL_LEFT, 650);
    ui.text({lx, 26 * s, lMax, 18 * s}, sub, 11 * s, ui.c.textDim, AL_LEFT);
  }
  // keyboard: 1 = actual size (0 and +/- are handled with the other shortcuts)
  if (!ui.editing() && !paletteOpen)
    for (int kk : ui.in.keys)
      if (kk == '1' || kk == VK_NUMPAD1) { figZoomK = actual / kFit; figZoomX = figZoomY = 0; }
  // hint pill at the bottom
  string hint = "Scroll to zoom  \xC2\xB7  Drag to pan";
  float hw = ui.textW(hint, 11 * s) + 24 * s;
  Rect hr{std::round(W.w / 2 - hw / 2), W.h - 34 * s, hw, 22 * s};
  ui.fill(hr, (ui.dark ? Color(0, 0, 0, 0.35f) : Color(1, 1, 1, 0.6f)).withA(e * (ui.dark ? 0.35f : 0.6f)), 11 * s);
  ui.text(hr, hint, 11 * s, ui.c.textDim.withA(e), AL_CENTER);
}

// ------------------------------------------------------------------ "?" help texts for option labels
void App::registerHelp() {
  auto& h = ui.helpTexts;
  // export
  h["Transparent background"] = "Leaves the background empty in PNG, SVG and PDF. Applies to the figure, single views and charts.";
  h["PDF: one page per plot"] = "Each selected view goes on its own page at the figure size, instead of all views on one page.";
  // build
  h["Min. citations per document"] = "Only documents cited at least this often contribute to the map.";
  h["Max. items to map"] = "Upper limit on the number of items; the strongest items are kept when more pass the threshold.";
  h["Only the largest connected set"] = "Drop items that are not connected to the main component, as VOSviewer does by default.";
  h["Min. link strength"] = "Links weaker than this are removed before layout and clustering.";
  h["Ignore docs with more authors than"] = "Documents with very many authors (consortia) create huge cliques; they are skipped above this number.";
  h["Years (0 = open)"] = "Only documents published in this range are used. 0 leaves that end open.";
  h["Keep most relevant"] = "For text terms: keep this share of the terms, ranked by relevance (how specific a term is to some clusters rather than all).";
  h["Normalisation"] = "How raw co-occurrence counts become similarities. Association strength (the VOSviewer default) corrects for how often each item occurs.";
  h["Attraction"] = "VOS layout attraction exponent. Higher values pull linked items closer together.";
  h["Repulsion"] = "VOS layout repulsion exponent (must be below attraction). Lower values fill the map more evenly and pull sparse outliers in; higher values make dense cores tighter.";
  h["Layout spread"] = "Balanced (attraction 1, repulsion 0) separates clusters and avoids a sparse fringe that squeezes the cores - the setting CWTS uses for its larger maps. Classic VOS (2, 1) is VOSviewer's default and suits small maps. Even (1, -1) spreads items most uniformly but blurs cluster borders.";
  h["Resolution"] = "Leiden clustering resolution. Higher values give more, smaller clusters.";
  h["Clustering"] = "As VOSviewer: the same clustering quality function as VOSviewer, so maps match VOSviewer. Modularity: the method of VOSStudio 1.5 and earlier, to reproduce older results. Projects from earlier versions keep modularity until you switch.";
  h["Min. cluster size"] = "Clusters smaller than this are merged into their best-connected neighbour.";
  h["Random seed"] = "Fix the seed to reproduce the same layout and clusters exactly.";
  h["Name clusters automatically"] = "Label each cluster with its most distinctive terms.";
  h["Remove node overlap"] = "Nudge overlapping circles apart after the layout.";
  // look
  h["Size by"] = "Which weight sets the circle size (e.g. occurrences, documents, citations, total link strength).";
  h["Size range"] = "Smallest and largest circle size on screen.";
  h["Opacity"] = "Transparency of the circles.";
  h["Flat nodes (no shading)"] = "Draw circles flat instead of as shaded spheres. Exports follow this unless Publish \xE2\x86\x92 Node shading says otherwise.";
  h["Border"] = "Outline around each circle.";
  h["Colour labels by cluster"] = "Draw each label in its cluster's colour instead of the text colour.";
  h["Halo behind labels"] = "A soft outline in the background colour keeps labels readable over links.";
  h["Max. labels"] = "At most this many labels are drawn; the most important items win and overlapping labels are skipped.";
  h["Show links"] = "Draw the links between items.";
  h["Edge bundling (FDEB)"] = "Force-directed edge bundling groups links that run in parallel into smooth bundles. Compute it once with the Bundle command.";
  h["Curvature"] = "How much curved links bend.";
  h["Max. line width (pt)"] = "Width of the strongest link.";
  h["Kernel width"] = "Density view: how far each item's influence spreads. With the automatic kernel this multiplies VOSviewer's width (0.125 x the average distance between items).";
  h["Sizing"] = "VOSviewer: circles, labels and lines keep a constant size on screen, so zooming in spreads items apart (as in VOSviewer). Studio: the 1.3 behaviour, where nodes are sized in map units and grow as you zoom.";
  h["Size variation"] = "How strongly circle and label size follow the item weight (VOSviewer's size variation). 0 = all equal; 0.5 = VOSviewer default (size ~ square root of weight).";
  h["Width variation"] = "How strongly line width follows link strength (VOSviewer's line size variation).";
  h["Placement"] = "Centre puts each label on its circle and hides it when it would collide, as VOSviewer does; zoom in to reveal more. Beside tries positions around the circle.";
  h["Fill"] = "Whole canvas colours the entire background with the colour map, as VOSviewer does. Around items fades low density into the background (1.3 look).";
  h["Kernel"] = "Auto scales the kernel to the map (0.125 x average item distance, van Eck & Waltman 2010), so density looks the same for any map size. Fixed uses a constant width.";
  h["Intensity"] = "Density view: overall brightness of the density map.";
  h["Cluster density (per-cluster colours)"] = "Blend a separate density surface per cluster in the cluster colours.";
  h["Colour map"] = "Colour scale for scores (overlay), density and the Geo choropleth.";
  h["Background"] = "Canvas background; Follow theme uses the app's light or dark theme.";
  h["Cluster hulls  (H)"] = "Draw a soft outline around each cluster.";
  h["Cluster names  (N)"] = "Write each cluster's name at its centre.";
  h["Cluster legend  (G)"] = "Show the legend card in the top-left corner of the canvas.";
  h["3D depth"] = "Stretch of the third dimension in the 3D view.";
  h["Country fill"] = "Countries map: tint countries by their cluster colour, by their weight, or leave them plain.";
  h["World basemap"] = "Draw the world map (Natural Earth, Equal Earth projection) under the nodes.";
  // publish
  h["Node shading"] = "Match view follows the Look (spheres unless nodes are flat). Sphere shading is exported as real radial gradients in SVG and PDF, so it stays vector.";
  h["Cluster legend"] = "Add the cluster legend to the figure.";
  h["Size legend"] = "Add a key for circle sizes.";
  h["Colour bar (overlay)"] = "Add the colour scale when the figure shows scores.";
  h["Panel letters (a, b, c)"] = "Label multi-panel figures a, b, c.";
  h["Methods footer"] = "Print the methods summary (data, thresholds, algorithms) under the figure.";
  h["Transparent background"] = "No background fill (PNG with alpha; SVG/PDF without the background rectangle).";
  h["Serif font"] = "Use a Times-like serif font instead of Helvetica/Arial.";
  h["PNG resolution (dpi)"] = "Pixels per inch for PNG export; most journals ask for 300\xE2\x80\x93" "600 dpi.";
  // data
  h["Append to the current records"] = "Add the fetched works to the loaded records instead of replacing them (duplicates are removed).";
  h["Strategy"] = "Direct: each query returns the 50 works OpenAlex finds closest in meaning, and the results are merged.\n\n"
                  "Expand: those works are the seeds. Their references and the works citing them are added, and every candidate is kept or dropped by how closely its title, abstract and keywords match the seeds. This builds a larger, focused data set for mapping.";
  h["Keep"] = "How closely an added work must match the seeds to be kept. Closest gives a small, tight data set; More keeps broader neighbours. The search results themselves are always kept.";
  h["Search in"] = "Where OpenAlex looks for your terms: title, abstract and full text; title and abstract only; or titles only (most precise).";
  h["Match"] = "Any term: one search per term, results merged and de-duplicated. All terms: works must contain every term.";
}

// =====================================================================================
// DATA
// =====================================================================================
void App::pageData(Lay& L) {
  float s = ui.s;
  // grouped list (label left, value right, hairline separators)
  auto group = [&](const vector<std::pair<string, string>>& rows) {
    float rh = 28 * s;
    Rect box = L.row(rh * float(rows.size()));
    ui.fill(box, ui.c.card, 8 * s);
    ui.stroke(box, ui.c.border, 8 * s);
    for (size_t i = 0; i < rows.size(); i++) {
      Rect r{box.x + 12 * s, box.y + rh * float(i), box.w - 24 * s, rh};
      if (i) ui.line(r.x, r.y + 0.5f, box.r(), r.y + 0.5f, ui.c.border);
      ui.text({r.x, r.y, r.w * 0.55f, r.h}, rows[i].first, 12.5f * s, ui.c.text);
      ui.text({r.x + r.w * 0.4f, r.y, r.w * 0.6f, r.h}, rows[i].second, 12.5f * s, ui.c.textDim, AL_RIGHT);
    }
  };
  if (!hasCorpus()) {
    sectionTitle(L, "Import");
    Rect drop = L.row(104 * s);
    bool hov = false;
    uint64_t idv = ui.id("dropzone");
    if (ui.behave(idv, drop, &hov)) cmdOpenFiles();
    ui.fill(drop, hov ? ui.c.hover : ui.c.card, 8 * s);
    ui.stroke(drop, hov ? ui.c.accent : ui.c.border, 8 * s);
    ui.icon("download", drop.x + drop.w / 2, drop.y + 30 * s, 20 * s, hov ? ui.c.accent : ui.c.textFaint, 1.5f);
    ui.text({drop.x, drop.y + 48 * s, drop.w, 20 * s}, "Drop files or click to browse", 13 * s, ui.c.text, AL_CENTER, 500);
    ui.text({drop.x, drop.y + 70 * s, drop.w, 16 * s}, "WoS, Scopus, RIS, BibTeX, OpenAlex, VOSviewer", 11 * s, ui.c.textFaint, AL_CENTER);
    ui.tipFor(idv, "Web of Science plain text or tab-delimited, Scopus CSV, RIS, BibTeX, OpenAlex JSON, VOSviewer map and network files, or a VOSStudio project");
    auto c2 = cols(L.row(30 * s), 2, 8 * s);
    if (ui.button(c2[0], "WoS Sample", BTN_NORMAL)) cmdSample(false);
    if (ui.button(c2[1], "Scopus Sample", BTN_NORMAL)) cmdSample(true);
  } else {
    const QualityReport& q = statQuality();
    sectionTitle(L, "Summary");
    string years = q.yearMin ? std::to_string(q.yearMin) + "\xE2\x80\x93" + std::to_string(q.yearMax) : "n/a";
    group({{"Records", fmtInt(q.records)}, {"Years", years}, {"Sources", fmtInt(q.sources)}, {"Authors", fmtInt(q.authors)},
           {"Cited references", fmtInt(q.totalRefs)}, {"Citations per document", fmtNum(q.citesPerDoc, 1)}});
    // coverage
    sectionTitle(L, "Field coverage", "Share of records with each field. Low coverage limits the analyses that use it.");
    struct Cov { const char* l; double v; const char* need; };
    Cov cv[] = {{"Author keywords", q.kwCov, "keyword maps"}, {"Index terms", q.idCov, "index term maps"}, {"Abstracts", q.absCov, "term maps"},
                {"Cited references", q.refCov, "coupling and co-citation"}, {"Affiliations", q.affCov, "organisation and country maps"}, {"DOI", q.doiCov, "linking and duplicate detection"},
                {"Publication year", q.yearCov, "trends and overlays"}};
    for (auto& c : cv) {
      Rect r = L.row(22 * s);
      ui.text({r.x, r.y, r.w * 0.45f, r.h}, c.l, 12.5f * s, ui.c.text);
      Rect bar{r.x + r.w * 0.47f, r.y + 9 * s, r.w * 0.38f, 4 * s};
      ui.fill(bar, ui.c.active, 2 * s);
      Color col = c.v >= 0.8 ? ui.c.textFaint : c.v >= 0.3 ? ui.c.warn : ui.c.danger;
      ui.fill({bar.x, bar.y, float(bar.w * c.v), bar.h}, col, 2 * s);
      ui.text({bar.r(), r.y, r.r() - bar.r(), r.h}, fmtNum(c.v * 100, 0) + "%", 12 * s, ui.c.textDim, AL_RIGHT);
      uint64_t tid = ui.id(string("cov") + c.l);
      ui.behave(tid, r);
      ui.tipFor(tid, string("Needed for ") + c.need);
    }
    sectionTitle(L, "Files");
    vector<std::pair<string, string>> fr;
    for (auto& f : P->corpus.files) fr.push_back({truncate(f.name, 36), string(formatLabel(f.format)) + "  \xC2\xB7  " + fmtInt(f.records)});
    if (P->corpus.duplicatesRemoved) fr.push_back({"Duplicates merged", fmtInt(P->corpus.duplicatesRemoved)});
    if (!fr.empty()) group(fr);
    auto c3 = cols(L.row(30 * s), 2, 8 * s);
    if (ui.button(c3[0], "Add Files\xE2\x80\xA6", BTN_NORMAL, "", !busy())) cmdOpenFiles();
    if (ui.button(c3[1], "Remove All", BTN_DANGER, "", !busy())) { P->clearCorpus(); pageStateReset(); }
    dataLiving(L);
    sectionTitle(L, "Records flow", "How the records move from import to the map: duplicates, filters and thresholds, in the style of a PRISMA diagram. Expand it or export it for the methods section.");
    {
      ChartDef d;
      d.title = "Records flow";
      d.make = [this](double w, double h, const ChartTheme& t) { return chartFlow(recordsFlow(), w, h, t); };
      chartBox(L, "flow", d, 300);
    }
  }
  // OpenAlex
  sectionTitle(L, "Search OpenAlex",
               "Fetch works directly from the free OpenAlex API.\n\nSeveral keywords: separate them with a semicolon ( ; ) or put each on its own line, e.g.\n   bibliometrics; science mapping; \"citation analysis\"\n\n"
               "Any term: each term is searched separately and the results are merged; works found by several terms are kept once.\nAll terms: works must match every term (combined with AND).\n\n"
               "Inside one term you can use quotes for exact phrases and AND / OR / NOT in capitals. Pasting DOIs fetches exactly those works.");
  if (ui.button(L.row(30 * s), "Plan the Search with AI\xE2\x80\xA6", BTN_NORMAL, "sparkle")) {
    if (!trim(oaQuery).empty() && aiInput.empty()) aiInput = trim(oaQuery);
    aiRun(ai::Task::Search, "");
  }
  {
    Rect kr = L.row(30 * s);
    ui.segmented(kr, {"Keywords", "Semantic"}, oaKind, "oakind");
  }
  if (oaKind == 1) {
    // one query per row; each row is one semantic search (at most 50 works)
    int remove = -1;
    for (size_t i = 0; i < oaSemQueries.size(); i++) {
      Rect qr = L.row(32 * s);
      bool submitted = false;
      ui.pushId("sq" + std::to_string(i));
      Rect ir = oaSemQueries.size() > 1 ? Rect{qr.x, qr.y, qr.w - 34 * s, qr.h} : qr;
      ui.textInput(ir, "q", oaSemQueries[i], i == 0 ? "Describe the topic" : "Another angle on the topic", &submitted, "search");
      if (oaSemQueries.size() > 1 && ui.iconButton({qr.r() - 30 * s, qr.y + 2 * s, 28 * s, 28 * s}, "x", "Remove this query")) remove = int(i);
      ui.popId();
      if (submitted && !busy()) cmdOpenAlex();
    }
    if (remove >= 0) oaSemQueries.erase(oaSemQueries.begin() + remove);
    if (oaSemQueries.size() < 8) {
      Rect ar = L.row(24 * s);
      string l = "Add query";
      float lw = ui.textW(l, 12 * s, 550) + 22 * s;
      Rect lr{ar.x, ar.y, lw, ar.h};
      uint64_t aid = ui.id("sqadd");
      bool ah = false;
      if (ui.behave(aid, lr, &ah)) oaSemQueries.push_back("");
      ui.icon("plus", lr.x + 6 * s, lr.y + lr.h / 2, 12 * s, ui.c.accent, 1.8f);
      ui.text({lr.x + 18 * s, lr.y, lw, lr.h}, l, 12 * s, ah ? ui.c.accent.withA(0.8f) : ui.c.accent, AL_LEFT, 550);
    }
    Rect f = field(ui, L, "Strategy", 30, 0.3f);
    int st = oaSemStrategy == 1 ? 0 : 1;
    if (ui.segmented(f, {"Expand", "Direct"}, st, "oasem")) oaSemStrategy = st == 0 ? 1 : 0;
    if (oaSemStrategy == 1) {
      Rect f2 = field(ui, L, "Keep", 30, 0.3f);
      ui.segmented(f2, {"More", "Balanced", "Closest"}, oaStrict, "oastrict");
    }
    if (openAlexKey().empty()) {
      Rect hr = L.row(24 * s);
      ui.text({hr.x, hr.y, hr.w - 90 * s, hr.h}, "No API key", 12 * s, ui.c.textDim);
      string kl = "Add Key\xE2\x80\xA6";
      float kw = ui.textW(kl, 12 * s, 500);
      Rect kr{hr.r() - kw, hr.y, kw, hr.h};
      uint64_t kid = ui.id("oaaddkey");
      bool kh = false;
      if (ui.behave(kid, kr, &kh)) settingsWanted = true;
      ui.text(kr, kl, 12 * s, kh ? ui.c.accentHover : ui.c.accent, AL_RIGHT, 500);
      ui.tipFor(kid, "A free key raises the daily OpenAlex allowance. Recommended for regular use.");
    }
  } else {
    Rect qr = L.row(32 * s);
    bool submitted = false;
    ui.textInput(qr, "oaq", oaQuery, "keyword one; keyword two; \"exact phrase\"", &submitted, "search");
    if (submitted && !busy()) cmdOpenAlex();
    // chips: one per term (x removes it); shows per-term totals after a fetch
    vector<string> terms = openAlexTerms(oaQuery);
    if (terms.size() > 1 || (!oaCounts.empty() && !terms.empty())) {
      float x = L.x, y = L.y, ch = 24 * s;
      string removeTerm;
      for (auto& t : terms) {
        string cnt;
        for (auto& c : oaCounts) if (c.term == t) cnt = "  " + fmtInt(c.total);
        string lbl = truncate(t, 34);
        float w = ui.textW(lbl, 11.5f * s, 550) + ui.textW(cnt, 11 * s) + 34 * s;
        if (x + w > L.x + L.w && x > L.x) { x = L.x; y += ch + 5 * s; }
        Rect cr{x, y, std::min(w, L.w), ch};
        ui.fill(cr, ui.c.active, 6 * s);
        ui.text({cr.x + 10 * s, cr.y, cr.w - 30 * s, ch}, lbl, 11.5f * s, ui.c.text, AL_LEFT, 500);
        if (!cnt.empty()) ui.text({cr.x + 10 * s, cr.y, cr.w - 28 * s, ch}, cnt, 11 * s, ui.c.textDim, AL_RIGHT);
        ui.pushId("chip" + t);
        if (ui.iconButton({cr.r() - 22 * s, cr.y + 2 * s, 20 * s, 20 * s}, "x", "Remove this term")) removeTerm = t;
        ui.popId();
        x += cr.w + 5 * s;
      }
      L.y = y + ch + 8 * s;
      if (!removeTerm.empty()) {
        string rebuilt;
        for (auto& t : terms) if (t != removeTerm) rebuilt += (rebuilt.empty() ? "" : "; ") + t;
        oaQuery = rebuilt;
      }
    }
    if (terms.size() > 1) {
      Rect f = field(ui, L, "Match", 30, 0.3f);
      ui.segmented(f, {"Any term", "All terms"}, oaMode, "oamode");
    }
    Rect f2 = field(ui, L, "Search in", 30, 0.3f);
    ui.combo(f2, "oafield", {"Title, abstract & full text", "Title & abstract", "Title only"}, oaField);
  }
  {
    auto c3 = cols(L.row(30 * s), 3, 6 * s);
    ui.textInput(c3[0], "oafrom", oaFrom, "From year");
    ui.textInput(c3[1], "oato", oaTo, "To year");
    if (oaKind == 1 && oaSemStrategy == 0) ui.text(c3[2], "50 per query", 12 * s, ui.c.textDim, AL_CENTER);
    else {
      string ml = oaKind == 0 && openAlexTerms(oaQuery).size() > 1 && oaMode == 0 ? "Max/term" : "Max";
      float mw = ui.textW(ml, 12 * s) + 8 * s;
      ui.text({c3[2].x, c3[2].y, mw, c3[2].h}, ml, 12 * s, ui.c.textDim);
      ui.textInput({c3[2].x + mw, c3[2].y, c3[2].w - mw, c3[2].h}, "oamax", oaMax, "500");
    }
  }
  if (hasCorpus()) ui.toggle(L.row(26 * s), "Append to the current records", oaAppend);
  if (ui.button(L.row(34 * s), busy() && jobLabel.find("OpenAlex") != string::npos ? "Fetching\xE2\x80\xA6" : "Fetch Works", BTN_PRIMARY, "", !busy())) cmdOpenAlex();
  if (!oaLastRecs.empty()) {
    Rect hr = L.row(18 * s);
    ui.text(hr, "Export the last " + plural(long(oaLastRecs.size()), "work"), 11.5f * s, ui.c.textDim);
    auto c4 = cols(L.row(28 * s), 4, 5 * s);
    ui.pushId("oaexp");
    if (ui.button(c4[0], "WoS", BTN_NORMAL, "download")) cmdExportRecords(true, "wos");
    ui.tip("Web of Science tagged text, for VOSviewer, bibliometrix and CiteSpace");
    if (ui.button(c4[1], "RIS", BTN_NORMAL, "download")) cmdExportRecords(true, "ris");
    ui.tip("RIS, for Zotero, EndNote and Mendeley");
    if (ui.button(c4[2], "CSV", BTN_NORMAL, "download")) cmdExportRecords(true, "csv");
    ui.tip("CSV with Scopus column names (Excel, R, Python)");
    if (ui.button(c4[3], "JSON", BTN_NORMAL, "download", bool(oaLastRaw))) cmdExportRecords(true, "json");
    ui.tip("Raw OpenAlex works, with the terms that matched each work");
    ui.popId();
  }
  if (!hasCorpus()) return;

  // Cleaning: rule-based variants and AI proposals
  sectionTitle(L, "Clean terms", "Scan finds spelling variants, plurals, hyphenation, acronyms and typos. AI also proposes synonyms, abbreviations and generic terms. Nothing changes until you merge, and every merge can be undone.");
  {
    const auto& cu = cleanUnits();
    vector<string> lbl;
    for (auto& u : cu) lbl.push_back(u.second);
    Rect r = L.row(32 * s);
    float bw = 64 * s;
    Rect cb{r.x, r.y, r.w - 2 * bw - 12 * s, r.h};
    if (ui.combo(cb, "cleanunit", lbl, cleanUnit)) { variantsScanned = false; variants.clear(); cleanNote.clear(); cleanEdit = -1; }
    if (ui.button({cb.r() + 6 * s, r.y, bw, r.h}, "Scan", BTN_NORMAL, "", !busy())) cleanScan();
    ui.tip("Find spelling, plural, hyphen and acronym variants by rules");
    bool aiBusy = aiLive && aiLiveTask == ai::Task::Clean;
    if (ui.button({r.r() - bw, r.y, bw, r.h}, aiBusy ? "\xE2\x80\xA6" : "AI", BTN_NORMAL, "", !aiLive && !agent.active)) aiCleanTerms(cleanUnit);
    ui.tip("Ask the AI for synonyms, abbreviations and generic terms (uses the provider in Settings)");
    if (variantsScanned) {
      if (variants.empty()) ui.text(L.row(22 * s), cleanNote.empty() ? string("No variants found.") : cleanNote, 12 * s, ui.c.ok);
      else {
        int nsel = 0, nai = 0;
        for (auto& v : variants) { if (v.apply) nsel++; if (v.ai) nai++; }
        Rect hr = L.row(22 * s);
        ui.text(hr, plural(long(variants.size()), "group") + (nai ? " \xC2\xB7 " + std::to_string(nai) + " from AI" : string()) + " \xC2\xB7 " + std::to_string(nsel) + " selected", 12 * s, ui.c.textDim);
        for (size_t i = 0; i < std::min<size_t>(60, variants.size()); i++) {
          auto& v = variants[i];
          string sub = v.ignore ? "Ignore" + (v.note.empty() ? string() : " \xC2\xB7 " + v.note) : "\xE2\x86\x90 " + join(v.members, ", ");
          float th = ui.textWrap({0, 0, L.w - 44 * s, 1000}, sub, 11.5f * s, ui.c.textDim, 400, false);
          Rect rr = L.row(30 * s + th);
          ui.pushId("var" + std::to_string(i));
          ui.fill(rr, ui.c.card, 6 * s);
          ui.checkbox({rr.x + 6 * s, rr.y + 5 * s, 22 * s, 22 * s}, "", v.apply);
          string tagText = v.ai ? "AI \xC2\xB7 " + v.reason : v.reason;
          float tw = ui.textW(tagText, 10.5f * s, 600) + 16 * s;
          Rect tag{rr.r() - tw - 8 * s, rr.y + 7 * s, tw, 18 * s};
          Color tc = v.ai ? ui.c.accent : (v.safe ? ui.c.ok : ui.c.warn);
          ui.fill(tag, tc.withA(0.15f), 9 * s);
          ui.text(tag, tagText, 10.5f * s, tc, AL_CENTER, 600);
          float titleW = tag.x - (rr.x + 32 * s) - (v.ignore ? 6 * s : 30 * s);
          if (cleanEdit == int(i)) {
            Rect ti{rr.x + 30 * s, rr.y + 3 * s, titleW + 24 * s, 24 * s};
            bool enter = false;
            ui.textInput(ti, "cedit", cleanEditText, "Preferred label", &enter);
            if (enter || ui.in.key(VK_RETURN)) {
              string nl = bibClean(cleanEditText);
              if (!nl.empty() && nl != v.target) {
                bool inMembers = false;
                for (auto& m : v.members) if (lower(m) == lower(v.target)) inMembers = true;
                if (!inMembers && v.targetCount > 0) { v.members.insert(v.members.begin(), v.target); v.counts.insert(v.counts.begin(), v.targetCount); }
                v.target = nl;
                v.targetCount = 0;
                for (size_t k = 0; k < v.members.size(); k++) if (lower(v.members[k]) == lower(nl)) { v.targetCount = k < v.counts.size() ? v.counts[k] : 0; v.members.erase(v.members.begin() + long(k)); if (k < v.counts.size()) v.counts.erase(v.counts.begin() + long(k)); break; }
              }
              cleanEdit = -1;
            } else if (ui.in.key(VK_ESCAPE)) cleanEdit = -1;
          } else {
            string title = v.target + (v.ignore ? (v.targetCount > 0 ? " (" + std::to_string(v.targetCount) + ")" : string()) : v.targetCount > 0 ? " (" + std::to_string(v.targetCount) + ")" : string(" (new)"));
            ui.text({rr.x + 32 * s, rr.y + 4 * s, titleW, 22 * s}, title, 12.5f * s, v.ignore ? ui.c.danger : ui.c.text, AL_LEFT, 600);
            if (!v.ignore && ui.iconButton({tag.x - 28 * s, rr.y + 5 * s, 22 * s, 22 * s}, "text", "Edit the preferred label")) { cleanEdit = int(i); cleanEditText = v.target; ui.focus = ui.id("ti:cedit"); }
          }
          ui.textWrap({rr.x + 32 * s, rr.y + 26 * s, rr.w - 44 * s, th + 4}, sub, 11.5f * s, ui.c.textDim);
          ui.popId();
        }
        auto c2 = cols(L.row(32 * s), 2, 8 * s);
        if (ui.button(c2[0], "Merge selected", BTN_PRIMARY, "check", nsel > 0)) cleanMergeSelected();
        if (ui.button(c2[1], nsel ? "Select none" : "Select all", BTN_NORMAL)) { bool to = nsel == 0; for (auto& v : variants) v.apply = to; }
        if (!cleanNote.empty()) {
          float th = ui.textWrap({0, 0, L.w, 400 * s}, cleanNote, 11.5f * s, ui.c.textDim, 400, false);
          ui.textWrap(L.row(th + 4 * s), cleanNote, 11.5f * s, ui.c.textDim);
        }
      }
    }
    if (!thUndo.empty()) {
      if (ui.button(L.row(30 * s), "Undo: " + thUndo.back().what, BTN_NORMAL, "undo")) cleanUndo();
    }
  }
  // Thesaurus
  sectionTitle(L, "Thesaurus (" + std::to_string(P->engine.thesaurus.replace.size()) + ")", "Replace or ignore terms everywhere. Leave \xE2\x80\x9Creplace with\xE2\x80\x9D empty to ignore a term.");
  {
    auto c2 = cols(L.row(30 * s), 2, 6 * s);
    ui.textInput(c2[0], "thfrom", thesFrom, "Term");
    ui.textInput(c2[1], "thto", thesTo, "Replace with (or empty)");
    auto c3 = cols(L.row(30 * s), 3, 6 * s);
    if (ui.button(c3[0], "Add", BTN_NORMAL, "plus", !trim(thesFrom).empty())) {
      P->engine.thesaurus.replace[bibKey(thesFrom)] = bibClean(thesTo);
      P->corpusChanged(); P->dirty = true; sensSig.clear();
      thesFrom.clear(); thesTo.clear();
    }
    if (ui.button(c3[1], "Import\xE2\x80\xA6", BTN_NORMAL, "folder")) {
      auto f = openFileDialog(hwnd, "Import VOSviewer thesaurus", {{"Thesaurus", "*.txt"}, {"All files", "*.*"}}, false);
      if (!f.empty()) {
        string err;
        Thesaurus t;
        if (t.load(readFileU(f[0]), &err)) { for (auto& kv : t.replace) P->engine.thesaurus.replace[kv.first] = kv.second; P->corpusChanged(); sensSig.clear(); ui.toast("Thesaurus imported", fmtInt(long(t.replace.size())) + " entries", 1); }
        else ui.toast("Import failed", err, 3);
      }
    }
    if (ui.button(c3[2], "Export\xE2\x80\xA6", BTN_NORMAL, "download", !P->engine.thesaurus.empty())) {
      string p = saveFileDialog(hwnd, "Export thesaurus", {{"VOSviewer thesaurus", "*.txt"}}, "thesaurus.txt", "txt");
      if (!p.empty() && writeFileU(p, P->engine.thesaurus.save())) ui.toast("Thesaurus exported", fileName(p), 1);
    }
    if (!P->engine.thesaurus.empty()) {
      ui.textInput(L.row(28 * s), "thfilter", thesFilter, "Filter entries", nullptr, "filter");
      string del;
      int shown = 0;
      for (auto& kv : P->engine.thesaurus.replace) {
        if (!thesFilter.empty() && !icontains(kv.first, thesFilter) && !icontains(kv.second, thesFilter)) continue;
        if (++shown > 60) break;
        Rect r = L.row(24 * s);
        ui.text({r.x, r.y, r.w * 0.45f, r.h}, kv.first, 12 * s, ui.c.text);
        ui.text({r.x + r.w * 0.45f, r.y, r.w * 0.45f, r.h}, kv.second.empty() ? "(ignored)" : "\xE2\x86\x92 " + kv.second, 12 * s, kv.second.empty() ? ui.c.danger : ui.c.textDim);
        if (ui.iconButton({r.r() - 22 * s, r.y + 1 * s, 22 * s, 22 * s}, "x", "Remove")) del = kv.first;
      }
      if (!del.empty()) { P->engine.thesaurus.replace.erase(del); P->corpusChanged(); sensSig.clear(); }
      if (ui.button(L.row(30 * s), "Clear thesaurus", BTN_DANGER, "trash")) { P->engine.thesaurus.replace.clear(); P->corpusChanged(); sensSig.clear(); }
    }
  }
}

// =====================================================================================
// BUILD — the analysis builder
// =====================================================================================
void App::pageBuild(Lay& L) {
  float s = ui.s;
  if (!hasCorpus()) {
    emptyHint(L, "data", "No Records", "Import data to build a map.");
    if (ui.button(L.row(30 * s), "Go to Data", BTN_PRIMARY)) page = PG_DATA;
    if (ui.button(L.row(30 * s), "Import VOSviewer Map\xE2\x80\xA6", BTN_NORMAL)) cmdLoadVosviewer();
    return;
  }
  AnaSpec& sp = P->spec;
  bool locked = busy();
  // 1. type
  sectionTitle(L, "Type of analysis", "What the links in the map mean. Hover the ? on each type for details.");
  {
    float rh = 30 * s;
    Rect box{L.x, L.y, L.w, rh * float(anaTypes().size())};
    ui.fill(box, ui.c.card, 8 * s);
    ui.stroke(box, ui.c.border, 8 * s);
  }
  float typeY0 = L.y;
  for (auto& ti : anaTypes()) {
    bool sel = sp.type == ti.type;
    Rect r{L.x, L.y, L.w, 30 * s};
    L.y += 30 * s;
    uint64_t idv = ui.id(string("type:") + ti.id);
    bool hov = false;
    bool overHelp = ui.help(r.r() - 26 * s, r.y + r.h / 2, ti.hint);
    if (ui.behave(idv, r, &hov) && !overHelp && !locked && !sel) { sp.type = ti.type; sp.unit = ti.units[0].first; sp.setDefaults(); sensSig.clear(); }
    if (r.y > typeY0) ui.line(r.x + 34 * s, r.y + 0.5f, r.r(), r.y + 0.5f, ui.c.border);
    if (hov && !sel) ui.fill(inset(r, 1 * s, 1 * s), ui.c.hover, 7 * s);
    float cy = r.y + r.h / 2;
    ui.circle(r.x + 17 * s, cy, 7 * s, sel ? ui.c.accent : ui.c.input, true);
    if (!sel) ui.circle(r.x + 17 * s, cy, 7 * s, ui.c.borderStrong, false, 1 * s);
    if (sel) ui.circle(r.x + 17 * s, cy, 2.6f * s, ui.c.accentText);
    ui.text({r.x + 34 * s, r.y, r.w - 64 * s, r.h}, ti.label, 12.5f * s, ui.c.text, AL_LEFT, sel ? 600 : 400);
    ui.help(r.r() - 26 * s, cy, ti.hint);
  }
  L.y += L.gap;
  // 2. unit
  sectionTitle(L, "Unit of analysis", "Which items become the nodes of the map for the chosen type.");
  {
    const TypeInfo& ti = typeInfo(sp.type);
    vector<string> lbl;
    int sel = 0;
    for (size_t i = 0; i < ti.units.size(); i++) { lbl.push_back(ti.units[i].second); if (ti.units[i].first == sp.unit) sel = int(i); }
    // pills
    float x = L.x, y = L.y;
    for (size_t i = 0; i < lbl.size(); i++) {
      float w = ui.textW(lbl[i], 12.5f * s, 600) + 22 * s;
      if (x + w > L.x + L.w) { x = L.x; y += 32 * s; }
      Rect r{x, y, w, 26 * s};
      uint64_t idv = ui.id("unit:" + lbl[i]);
      bool hov = false;
      if (ui.behave(idv, r, &hov) && !locked && int(i) != sel) { sp.unit = ti.units[i].first; sp.setDefaults(); sensSig.clear(); }
      bool on = int(i) == sel;
      ui.fill(r, on ? ui.c.accent.withA(ui.dark ? 0.28f : 0.12f) : (hov ? ui.c.hover : ui.c.card), 6 * s);
      ui.stroke(r, on ? ui.c.accent.withA(0.6f) : ui.c.border, 6 * s);
      ui.text(r, lbl[i], 12.5f * s, on ? (ui.dark ? ui.c.text : ui.c.accentHover) : ui.c.text, AL_CENTER, on ? 600 : 400);
      x += w + 6 * s;
    }
    L.y = y + 34 * s;
  }
  // 3. counting
  sectionTitle(L, "Counting method", "Full counting: every co-occurrence in a document counts as one link.\n\nFractional counting: each document has the same total weight (1), split across its links, so documents with many authors or keywords do not dominate.");
  {
    int c = sp.fractional ? 1 : 0;
    if (ui.segmented(L.row(32 * s), {"Full counting", "Fractional counting"}, c, "counting") && !locked) { sp.fractional = c == 1; }
    L.space(4 * s);
  }
  // 4. threshold with sensitivity
  sectionTitle(L, "Threshold", "Items must reach this minimum to be mapped. The bar chart shows how many items meet each threshold. Click a bar to choose it. Aim for roughly 50\xE2\x80\x93" "300 items for a readable map.");
  if (!locked) {
    string sig = string(typeInfo(sp.type).id) + unitId(sp.unit) + std::to_string(sp.fractional) + std::to_string(sp.minCites) + std::to_string(sp.y0) + std::to_string(sp.y1) + P->engine.thesaurus.sig();
    if (sig != sensSig) {
      try { sens = P->engine.sensitivity(sp, 20); } catch (...) { sens.clear(); }
      sensSig = sig;
    }
  }
  {
    Rect f = field(ui, L, thresholdLabel(sp), 32, 0.58f);
    if (ui.numberInput(f, "thr", sp.min, 1, 100000) && !locked) {}
    int meet = 0, total = 0;
    for (auto& p : sens) { if (p.first == 1) total = p.second; if (p.first == std::min(sp.min, 20)) meet = p.second; }
    string msg = sens.empty() ? "" : (sp.min <= 20 ? fmtInt(meet) + " of " + fmtInt(total) + " " + unitNounPlural(sp.unit) + " meet the threshold" : "threshold above 20");
    Color mc = meet > 1500 ? ui.c.warn : ui.c.textDim;
    ui.text(L.row(18 * s), msg + (meet > 1500 ? ". Consider a higher threshold." : ""), 11.5f * s, mc);
    if (!sens.empty()) {
      ChartDef d;
      d.title = "Items meeting each threshold";
      auto sv = sens;
      int cur = sp.min;
      d.make = [sv, cur](double w, double h, const ChartTheme& t) { return chartSensitivity(sv, cur, "", w, h, t); };
      d.onClick = [this](int tag) { if (!busy()) P->spec.min = tag; };
      Rect cr = L.row(84 * s);
      ui.fill(cr, ui.c.card, 8 * s);
      ChartTheme t = chartTheme(false);
      Scene sc = d.make(cr.w / s - 12, cr.h / s - 8, t);
      drawScene(g.dc.get(), g.d2f.get(), g.dw.get(), g, sc, cr.x + 6 * s, cr.y + 4 * s, s);
      if (ui.mouseIn(cr)) {
        chartHover(sc, cr.x + 6 * s, cr.y + 4 * s, s);
        int tag = hitTag(sc, (ui.in.mx - cr.x - 6 * s) / s, (ui.in.my - cr.y - 4 * s) / s);
        if (tag >= 0) { ui.cursor = "hand"; if (ui.in.released[0] && ui.active == 0 && !locked) sp.min = tag; }
      }
    }
    Measure M = anaMeasure(sp);
    if (M.secondary) { Rect f2 = field(ui, L, "Min. citations per document", 30, 0.58f); ui.numberInput(f2, "mincites", sp.minCites, 0, 100000); }
    {
      Rect f3 = field(ui, L, "Max. items to map", 30, 0.58f);
      ui.numberInput(f3, "maxitems", sp.maxItems, 10, 20000, 50);
    }
    if (ui.button(L.row(28 * s), "Suggest a threshold for ~120 items", BTN_GHOST, "sparkle", !locked)) { sp.min = P->engine.suggest(sp, 120); ui.toast("Threshold", thresholdLabel(sp) + " \xE2\x86\x92 " + std::to_string(sp.min), 0, 2); }
  }
  // advanced
  L.space(4 * s);
  ui.header(L.row(28 * s), "Advanced filters", advOpen);
  if (advOpen) {
    ui.pushId("adv");
    ui.toggle(L.row(28 * s), "Only the largest connected set", sp.largest);
    Rect f = field(ui, L, "Min. link strength", 30, 0.58f);
    ui.numberInputD(f, "minlink", sp.minLink, 0, 1e6, 1, 1);
    if (sp.type == AnaType::Coauth || sp.unit == Unit::Authors) { Rect f2 = field(ui, L, "Ignore docs with more authors than", 30, 0.58f); ui.numberInput(f2, "maxauth", sp.maxAuth, 1, 1000); }
    Rect fy = field(ui, L, "Years (0 = open)", 30, 0.42f);
    auto c2 = cols(fy, 2, 6 * s);
    ui.numberInput(c2[0], "y0", sp.y0, 0, 2100);
    ui.numberInput(c2[1], "y1", sp.y1, 0, 2100);
    if (sp.unit == Unit::Terms) { float k = float(sp.termKeep); if (ui.slider(L.row(28 * s), "Keep most relevant", k, 0.2f, 1.0f, "%.0f%%", 0.05f)) sp.termKeep = k; }
    ui.popId();
  }
  // method
  ui.header(L.row(28 * s), "Normalisation, layout and clustering", methodOpen);
  if (methodOpen) {
    ui.pushId("meth");
    vector<string> norms = {"Association strength", "Fractionalization", "LinLog / modularity", "No normalisation"};
    int ni = int(P->params.norm);
    Rect f = field(ui, L, "Normalisation", 32);
    if (ui.combo(f, "norm", norms, ni)) P->params.norm = Norm(ni);
    float a = float(P->params.layout.attraction), r = float(P->params.layout.repulsion), res = float(P->params.cluster.resolution);
    {
      // layout spread presets (attraction / repulsion exponents of the VOS quality function)
      static const double pre[3][2] = {{1, 0}, {2, 1}, {1, -1}};
      vector<string> spreads = {"Balanced (VOSviewer large maps)", "Classic VOS (compact)", "Even (maximum spread)", "Custom"};
      int si = 3;
      for (int k = 0; k < 3; k++) if (a == float(pre[k][0]) && r == float(pre[k][1])) si = k;
      Rect fl = field(ui, L, "Layout spread", 32);
      if (ui.combo(fl, "spread", spreads, si) && si < 3) { P->params.layout.attraction = a = float(pre[si][0]); P->params.layout.repulsion = r = float(pre[si][1]); }
    }
    if (ui.slider(L.row(28 * s), "Attraction", a, 1, 10, "%.0f", 1)) { P->params.layout.attraction = a; if (P->params.layout.repulsion >= a) P->params.layout.repulsion = a - 1; }
    if (ui.slider(L.row(28 * s), "Repulsion", r, -1, 5, "%.0f", 1)) { P->params.layout.repulsion = r; if (r >= P->params.layout.attraction) P->params.layout.attraction = r + 1; }
    {
      vector<string> meths = {"As VOSviewer", "Modularity (1.5 and earlier)"};
      Rect fc = field(ui, L, "Clustering", 32);
      int cm = P->params.clusterMethod;
      if (ui.combo(fc, "cmethod", meths, cm)) P->params.clusterMethod = cm;
    }
    if (ui.slider(L.row(28 * s), "Resolution", res, 0.1f, 3.0f, "%.2f", 0.05f)) P->params.cluster.resolution = res;
    Rect fm = field(ui, L, "Min. cluster size", 30, 0.58f);
    ui.numberInput(fm, "minsize", P->params.cluster.minSize, 1, 1000);
    Rect fs = field(ui, L, "Random seed", 30, 0.58f);
    ui.numberInput(fs, "seed", P->params.layout.seed, 0, 99999);
    ui.toggle(L.row(28 * s), "Name clusters automatically", P->params.autoName);
    ui.toggle(L.row(28 * s), "Remove node overlap", P->params.overlap);
    ui.popId();
  }
  // excluded / candidates
  if (!locked) {
    const Selection* sel = nullptr;
    try { sel = &P->engine.select(sp); } catch (...) { sel = nullptr; }
    auto& ex = P->engine.exclSet(sp);
    sectionTitle(L, "Candidate items", "Items that pass the threshold. Untick an item to exclude it from the map; excluded items are listed at the end and can be ticked again.",
                 sel ? fmtInt(long(sel->sel.size())) + " mapped \xC2\xB7 " + std::to_string(ex.size()) + " excluded" : "");
    if (sel) {
      ui.textInput(L.row(28 * s), "candf", candFilter, "Filter candidates", nullptr, "filter");
      const RawNet& R = P->engine.raw(sp);
      if (R.pairsLimited > 0) {  // memory guard of very dense analyses
        string note = "Links were counted among the " + fmtInt(R.pairsLimited) + " strongest items to stay within memory. Raise the threshold to map fewer items with every link counted.";
        float h = ui.textWrap({L.x, L.y, L.w, 200 * s}, note, 11.5f * s, ui.c.warn, 400, false);
        ui.textWrap({L.x, L.y, L.w, h + 4}, note, 11.5f * s, ui.c.warn);
        L.y += h + 8 * s;
      }
      // show the passing items plus the excluded ones
      int shown = 0;
      float rowH = 24 * s;
      Rect clip = ui.clipRect();
      for (size_t k = 0; k < sel->sel.size() && shown < 300; k++) {
        const RawItem& it = R.items[size_t(sel->sel[k])];
        if (!candFilter.empty() && !icontains(it.label, candFilter)) continue;
        shown++;
        Rect r = L.row(rowH);
        if (r.b() < clip.y || r.y > clip.b()) continue;
        bool on = true;
        ui.pushId("c" + it.key);
        if (ui.checkbox({r.x, r.y + 1 * s, r.w - 60 * s, r.h - 2 * s}, it.label, on) && !on) { ex.insert(it.key); sensSig.clear(); }
        ui.text({r.r() - 56 * s, r.y, 56 * s, r.h}, fmtNum(anaMeasure(sp).cites ? it.cites : it.occ, 0), 11 * s, ui.c.textDim, AL_RIGHT);
        ui.popId();
      }
      if (!ex.empty()) {
        ui.text(L.row(20 * s), "Excluded", 12 * s, ui.c.textDim, AL_LEFT, 600);
        string back;
        for (auto& k : ex) {
          Rect r = L.row(rowH);
          bool on = false;
          string lbl = k;
          for (auto& it : R.items) if (it.key == k) { lbl = it.label; break; }
          ui.pushId("x" + k);
          if (ui.checkbox({r.x, r.y + 1 * s, r.w, r.h - 2 * s}, lbl, on) && on) back = k;
          ui.popId();
        }
        if (!back.empty()) { ex.erase(back); sensSig.clear(); }
      }
    }
  } else {
    ui.text(L.row(22 * s), "Building\xE2\x80\xA6 settings are locked until the map is ready.", 12 * s, ui.c.textDim);
  }
  L.space(10 * s);
  if (ui.button(L.row(30 * s), "Import VOSviewer map\xE2\x80\xA6", BTN_GHOST, "network")) cmdLoadVosviewer();
}

// =====================================================================================
// LOOK
// =====================================================================================
void App::pageLook(Lay& L) {
  float s = ui.s;
  ViewStyle& S = P->style;
  bool ch = false;
  sectionTitle(L, "Looks", "One-click styles; everything below stays editable.");
  {
    const auto& looks = lookPresets();
    int per = 2;
    for (size_t i = 0; i < looks.size(); i += size_t(per)) {
      auto c2 = cols(L.row(62 * s), per, 8 * s);
      for (int k = 0; k < per && i + size_t(k) < looks.size(); k++) {
        auto& lk = looks[i + size_t(k)];
        Rect r = c2[size_t(k)];
        uint64_t idv = ui.id(string("look:") + lk.id);
        bool hov = false;
        if (ui.behave(idv, r, &hov)) {
          int was = S.sizing;
          applyLook(S, lk.id); S.darkTheme = ui.dark; ch = true;
          if (S.sizing != was && hasMap()) { nv.fit(); camTargetZoom = -1; }
        }
        bool sel = S.look == lk.id;
        ui.fill(r, hov && !sel ? ui.c.hover : ui.c.card, 8 * s);
        ui.stroke(r, sel ? ui.c.accent : ui.c.border, 8 * s, sel ? 2.f * s : 1.f);
        // tiny preview: three nodes and two links in the look's palette
        ViewStyle tmp = S;
        applyLook(tmp, lk.id);
        const auto& pal = palette(tmp.palette);
        float px = r.x + 12 * s, py = r.y + 14 * s;
        ui.line(px + 4 * s, py + 22 * s, px + 20 * s, py + 6 * s, pal[0].withA(0.6f), 1.5f * s);
        ui.line(px + 20 * s, py + 6 * s, px + 34 * s, py + 26 * s, pal[1 % pal.size()].withA(0.6f), 1.5f * s);
        ui.circle(px + 4 * s, py + 22 * s, 5 * s, pal[0]);
        ui.circle(px + 20 * s, py + 6 * s, 6.5f * s, pal[1 % pal.size()]);
        ui.circle(px + 34 * s, py + 26 * s, 4 * s, pal[2 % pal.size()]);
        ui.text({r.x + 54 * s, r.y + 10 * s, r.w - 60 * s, 20 * s}, lk.label, 12.5f * s, ui.c.text, AL_LEFT, 650);
        ui.text({r.x + 54 * s, r.y + 30 * s, r.w - 60 * s, 18 * s}, lk.sub, 10.5f * s, ui.c.textDim);
      }
    }
  }
  // nodes
  ui.pushId("nodes");
  sectionTitle(L, "Nodes");
  {
    int cb = int(S.colorBy);
    if (ui.segmented(L.row(30 * s), {"Cluster", "Score", "Single"}, cb, "colorby")) { S.colorBy = ColorBy(cb); ch = true; }
    if (S.colorBy == ColorBy::Cluster) {
      vector<string> pl, pid;
      for (auto& p : paletteList()) { pl.push_back(string(p.label) + (p.cvdSafe ? "  \xE2\x9C\x93 CVD-safe" : "")); pid.push_back(p.id); }
      int pi = indexOf(pid, S.palette);
      Rect f = field(ui, L, "Palette", 32);
      if (ui.combo(f, "palette", pl, pi)) { S.palette = pid[size_t(pi)]; ch = true; }
      // palette strip
      Rect strip = L.row(12 * s);
      const auto& pal = palette(S.palette);
      int n = std::min<int>(12, int(pal.size()));
      for (int k = 0; k < n; k++) ui.fill({strip.x + strip.w * k / n, strip.y, strip.w / n - 2 * s, strip.h}, pal[size_t(k)], 3 * s);
    } else if (S.colorBy == ColorBy::Score) {
      const auto& cm = colormapList();
      int ci = indexOf(cm, S.scheme);
      Rect f = field(ui, L, "Colour map", 32);
      if (ui.combo(f, "scheme", cm, ci)) { S.scheme = cm[size_t(ci)]; ch = true; }
    } else {
      Rect f = field(ui, L, "Colour", 28);
      if (ui.colorSwatch({f.x, f.y + 2 * s, 48 * s, f.h - 4 * s}, "single", S.single)) ch = true;
    }
    if (hasMap()) {
      int wi = std::max(0, P->net.weightIdx);
      Rect f = field(ui, L, "Size by", 32);
      if (ui.combo(f, "weight", P->net.weightNames, wi)) { P->net.weightIdx = wi; ch = true; }
      if (!P->net.scoreNames.empty()) {
        int si = std::max(0, P->net.scoreIdx);
        Rect f2 = field(ui, L, "Score (overlay)", 32);
        if (ui.combo(f2, "score", P->net.scoreNames, si)) { P->net.scoreIdx = si; ch = true; }
      }
    }
    {
      int sz = S.sizing == 1 ? 0 : 1;
      Rect f = field(ui, L, "Sizing", 30, 0.3f);
      if (ui.segmented(f, {"VOSviewer", "Studio"}, sz, "sizing")) { S.sizing = sz == 0 ? 1 : 0; ch = true; if (hasMap()) { nv.fit(); camTargetZoom = -1; } }
    }
    ch |= ui.slider(L.row(28 * s), "Size", S.scale, 0.3f, 3.0f, "%.2f");
    if (S.sizing == 1) ch |= ui.slider(L.row(28 * s), "Size variation", S.sizeVar, 0, 1, "%.2f");
    else ch |= ui.slider(L.row(28 * s), "Size range", S.maxSize, 4, 40, "%.0f", 1);
    ch |= ui.slider(L.row(28 * s), "Opacity", S.nodeOpacity, 0.2f, 1.0f, "%.2f");
    ch |= ui.slider(L.row(28 * s), "Border", S.border, 0, 3, "%.1f", 0.1f);
    ch |= ui.toggle(L.row(28 * s), "Flat nodes (no shading)", S.flat);
  }
  ui.popId();
  ui.pushId("labels");
  sectionTitle(L, "Labels");
  {
    ch |= ui.slider(L.row(28 * s), "Size", S.labelSize, 0.5f, 2.5f, "%.2f");
    if (S.sizing != 1) ch |= ui.slider(L.row(28 * s), "Variation", S.labelVar, 0, 1, "%.2f");
    {
      int lp = S.labelPlace == LabelPlace::Centre ? 1 : 0;
      Rect f = field(ui, L, "Placement", 30, 0.3f);
      if (ui.segmented(f, {"Beside", "Centre"}, lp, "lplace")) { S.labelPlace = lp ? LabelPlace::Centre : LabelPlace::Side; ch = true; }
    }
    float ml = float(S.maxLen);
    if (ui.slider(L.row(28 * s), "Max. length", ml, 8, 80, "%.0f", 1)) { S.maxLen = int(ml); ch = true; }
    float mx = float(S.maxLabels);
    if (ui.slider(L.row(28 * s), "Max. labels", mx, 0, 500, mx == 0 ? "auto" : "%.0f", 5)) { S.maxLabels = int(mx); ch = true; }
    int w = S.labelWeight >= 600 ? 2 : S.labelWeight >= 450 ? 1 : 0;
    if (ui.segmented(L.row(30 * s), {"Regular", "Medium", "Bold"}, w, "lw")) { S.labelWeight = w == 2 ? 650 : w == 1 ? 500 : 400; ch = true; }
    ch |= ui.toggle(L.row(28 * s), "Halo behind labels", S.labelHalo);
    ch |= ui.toggle(L.row(28 * s), "Colour labels by cluster", S.labelByCluster);
    ch |= ui.toggle(L.row(28 * s), "Show labels  (L)", showLabels);
  }
  ui.popId();
  ui.pushId("links");
  sectionTitle(L, "Links");
  {
    ch |= ui.toggle(L.row(28 * s), "Show links", S.linksVisible);
    int lc = int(S.linkColor);
    Rect f = field(ui, L, "Colour", 32);
    if (ui.combo(f, "lc", {"By cluster", "Gradient", "Grey", "Single colour", "By weight"}, lc)) { S.linkColor = LinkColor(lc); ch = true; }
    int lg = int(S.linkGeom);
    if (ui.segmented(L.row(30 * s), {"Straight", "Curved", "Arc"}, lg, "geom")) { S.linkGeom = LinkGeom(lg); ch = true; }
    ch |= ui.slider(L.row(28 * s), "Opacity", S.linkOpacity, 0.05f, 1, "%.2f");
    ch |= ui.slider(L.row(28 * s), "Width", S.linkWidth, 0.2f, 4, "%.1f");
    if (S.sizing == 1) ch |= ui.slider(L.row(28 * s), "Width variation", S.linkVar, 0, 1, "%.2f");
    ch |= ui.slider(L.row(28 * s), "Curvature", S.curvature, 0, 1.0f, "%.2f");
    float ml = float(S.maxLines);
    if (ui.slider(L.row(28 * s), "Max. lines", ml, 50, 20000, "%.0f", 50)) { S.maxLines = int(ml); ch = true; }
    bool b = S.bundle;
    if (ui.toggle(L.row(28 * s), "Edge bundling (FDEB)", b)) {
      S.bundle = b;
      if (b && !P->bundles.valid()) cmdBundles();
      ch = true;
    }
  }
  ui.popId();
  ui.pushId("density");
  sectionTitle(L, "Density");
  {
    {
      int fl = S.densityFull ? 0 : 1;
      Rect f = field(ui, L, "Fill", 30, 0.3f);
      if (ui.segmented(f, {"Whole canvas", "Around items"}, fl, "dfill")) { S.densityFull = fl == 0; ch = true; }
      int ka = S.kernelAuto ? 0 : 1;
      Rect f2 = field(ui, L, "Kernel", 30, 0.3f);
      if (ui.segmented(f2, {"Auto", "Fixed"}, ka, "kauto")) { S.kernelAuto = ka == 0; ch = true; }
    }
    ch |= ui.slider(L.row(28 * s), "Kernel width", S.kernel, 0.3f, 3, "%.2f");
    ch |= ui.slider(L.row(28 * s), "Intensity", S.densityAlpha, 0.1f, 1, "%.2f");
    const auto& cm = colormapList();
    int ci = indexOf(cm, S.densityScheme);
    Rect f = field(ui, L, "Colour map", 32);
    if (ui.combo(f, "dscheme", cm, ci)) { S.densityScheme = cm[size_t(ci)]; ch = true; }
    ch |= ui.toggle(L.row(28 * s), "Cluster density (per-cluster colours)", S.densityByCluster);
  }
  ui.popId();
  ui.pushId("canvas");
  sectionTitle(L, "Canvas & accessibility");
  {
    int bd = int(S.backdrop);
    Rect f = field(ui, L, "Background", 32);
    if (ui.combo(f, "bd", {"Follow theme", "White", "Light grey", "Dark", "Black"}, bd)) { S.backdrop = Backdrop(bd); ch = true; }
    ch |= ui.toggle(L.row(28 * s), "Cluster hulls  (H)", S.hulls);
    ch |= ui.toggle(L.row(28 * s), "Cluster names  (N)", S.clusterNames);
    ui.toggle(L.row(28 * s), "Cluster legend  (G)", showLegend);
    int cvd = S.cvd;
    ui.text(L.row(18 * s), "Colour-vision simulation", 12 * s, ui.c.textDim);
    if (ui.segmented(L.row(30 * s), {"Off", "Protan", "Deutan", "Tritan"}, cvd, "cvd")) { S.cvd = cvd; ch = true; }
    ch |= ui.slider(L.row(28 * s), "3D depth", S.zScale, 0.2f, 3, "%.2f");
  }
  ui.popId();
  // geo
  if (geoAvailable()) {
    ui.pushId("geo");
    sectionTitle(L, "Geo view", "The Geo view (key 6) places countries on a world map (Natural Earth, Equal Earth projection). Country names are recognised in any common spelling (USA, Peoples R China, England, Turkiye, ISO codes\xE2\x80\xA6).\n\nWith a map of countries the nodes sit on their countries; with any other map the canvas shows the loaded records per country and their strongest collaborations.");
    geoUpdate();
    {
      Rect f = field(ui, L, "Layer", 30, 0.36f);
      ui.segmented(f, {geoModeA ? "Network" : "Countries", "Overlay", "Density"}, geoLayerKind, "geolayer");
    }
    if (geoModeA) {
      Rect f = field(ui, L, "Country fill", 30, 0.36f);
      ui.segmented(f, {"Cluster", "Weight", "None"}, geoFill, "geofill");
      ui.toggle(L.row(28 * s), "World basemap", geoBasemap);
    } else {
      ui.toggle(L.row(28 * s), "Collaboration arcs (top 150)", geoArcs);
      if (ui.button(L.row(30 * s), "Map co-authorship of countries", BTN_NORMAL, "globe", !busy())) {
        P->spec.type = AnaType::Coauth; P->spec.unit = Unit::Countries; P->spec.setDefaults(); P->spec.min = 1; geoAfterBuild = true; cmdBuild();
      }
    }
    if (view != ViewKind::Geo && ui.button(L.row(30 * s), "Open the Geo view", BTN_GHOST, "globe")) setView(ViewKind::Geo);
    ui.popId();
  }
  if (ch) { styleDirty = true; if (view == ViewKind::ThreeD) posDirty = true; }
}

// =====================================================================================
// ANALYSE
// =====================================================================================
void App::pageAnalyse(Lay& L) {
  float s = ui.s;
  if (!hasMap()) { emptyHint(L, "analyse", "No Map", "Build a map to see clusters and item statistics."); return; }
  Network& N = P->net;
  auto tabs = cols(L.row(34 * s), 4, 2 * s);
  const char* tn[4] = {"Clusters", "Items", "Network", "Stability"};
  for (int i = 0; i < 4; i++) if (ui.tab(tabs[size_t(i)], tn[i], anaTab == i)) anaTab = i;
  L.space(4 * s);
  auto ccols = clusterColors();
  if (!cinfoValid) { cinfo = clusterInfo(N, hasCorpus() ? &P->corpus : nullptr); cinfoValid = true; }
  if (anaTab == 0) {
    ChartDef d;
    d.title = "Strategic diagram (Callon)";
    d.make = [this](double w, double h, const ChartTheme& t) { if (!cinfoValid) { cinfo = clusterInfo(P->net, &P->corpus); cinfoValid = true; } return chartStrategic(cinfo, clusterColors(), w, h, t); };
    d.onClick = [this](int c) { focusCluster(c); };
    chartBox(L, "strat", d, 230);
    auto c2 = cols(L.row(30 * s), 2, 8 * s);
    if (ui.button(c2[0], "Auto-name all", BTN_NORMAL, "sparkle")) { pushUndo("Names"); applyAutoNames(N, hasCorpus() ? &P->corpus : nullptr); cinfoValid = false; P->dirty = true; }
    if (ui.button(c2[1], "Re-cluster", BTN_NORMAL, "shuffle", !busy())) cmdRecluster();
    {
      float res = float(P->params.cluster.resolution);
      if (ui.slider(L.row(28 * s), "Resolution", res, 0.1f, 3.0f, "%.2f", 0.05f)) P->params.cluster.resolution = res;
    }
    analyseClusterYears(L);
    sectionTitle(L, "Clusters", "Names can be typed in each card, or written by the AI assistant from the items and most cited records of every cluster (undo with Ctrl+Z).");
    if (ui.button(L.row(30 * s), aiLive && aiLiveTask == ai::Task::NameClusters ? "Naming Clusters\xE2\x80\xA6" : "Name Clusters with AI", BTN_NORMAL, "sparkle", !aiLive)) {
      aiSend(ai::Task::NameClusters, "");
    }
    for (auto& ci : cinfo) {
      int c = ci.c;
      Rect card = L.row(118 * s);
      ui.pushId("cl" + std::to_string(c));
      ui.fill(card, clusterFilter == c ? ui.c.accent.withA(0.08f) : ui.c.card, 8 * s);
      ui.stroke(card, clusterFilter == c ? ui.c.accent : ui.c.border, 8 * s);
      Color col = ccols[size_t(c) % ccols.size()];
      if (ui.colorSwatch({card.x + 10 * s, card.y + 12 * s, 20 * s, 20 * s}, "sw", col)) { pushUndo("Cluster colour"); P->style.clusterOverride[c] = col; styleDirty = true; }
      if (N.clusterNames.size() < size_t(N.nClusters)) N.clusterNames.resize(size_t(N.nClusters));
      string nm = N.clusterNames[size_t(c)].empty() ? ci.autoName : N.clusterNames[size_t(c)];
      string before = nm;
      ui.textInput({card.x + 38 * s, card.y + 8 * s, card.w - 84 * s, 28 * s}, "name", nm, "Cluster name");
      if (nm != before) { N.clusterNames[size_t(c)] = nm; P->dirty = true; }
      if (ui.iconButton({card.r() - 38 * s, card.y + 9 * s, 28 * s, 26 * s}, "target", "Focus on the canvas")) focusCluster(c);
      string meta = plural(ci.size, "item") + " \xC2\xB7 " + ci.quadrant;
      if (std::isfinite(ci.avgYear)) meta += " \xC2\xB7 avg. year " + fmtNum(ci.avgYear, 1);
      ui.text({card.x + 12 * s, card.y + 42 * s, card.w - 24 * s, 18 * s}, meta, 11.5f * s, ui.c.textDim);
      string top;
      for (size_t k = 0; k < std::min<size_t>(6, ci.items.size()); k++) top += (k ? ", " : "") + N.nodes[size_t(ci.items[k])].label;
      ui.textWrap({card.x + 12 * s, card.y + 62 * s, card.w - 24 * s, 34 * s}, top, 11.5f * s, ui.c.text);
      ui.text({card.x + 12 * s, card.b() - 22 * s, card.w - 24 * s, 18 * s}, "centrality " + fmtNum(ci.centrality, 2) + " \xC2\xB7 density " + fmtNum(ci.density, 2), 11 * s, ui.c.textFaint);
      ui.popId();
    }
  } else if (anaTab == 1) {
    const ItemMetrics& M = P->itemMetricsCached();
    Rect fr = L.row(30 * s);
    ui.textInput({fr.x, fr.y, fr.w - 38 * s, fr.h}, "itf", itemFilter, "Filter items", nullptr, "filter");
    if (ui.iconButton({fr.r() - 32 * s, fr.y + 1 * s, 30 * s, 28 * s}, "download", "Export all items (CSV)")) cmdExportItemsCsv();
    // header
    struct Col { string name; float w; std::function<double(int)> v; };
    vector<Col> colsDef = {{"Item", 0.44f, nullptr}, {"Cl.", 0.1f, [&](int i) { return double(N.nodes[size_t(i)].cluster); }},
                           {N.weightNames.size() > 2 ? N.weightNames[2] : "Weight", 0.16f, [&](int i) { return N.nodes[size_t(i)].w.size() > 2 ? N.nodes[size_t(i)].w[2] : N.weight(i); }},
                           {"Links", 0.14f, [&](int i) { return M.degree[size_t(i)]; }}, {"Betw.", 0.16f, [&](int i) { return M.betweenness[size_t(i)]; }}};
    Rect hr = L.row(24 * s);
    float x = hr.x;
    for (size_t k = 0; k < colsDef.size(); k++) {
      Rect c{x, hr.y, hr.w * colsDef[k].w, hr.h};
      uint64_t idv = ui.id("ih" + std::to_string(k));
      bool hov = false;
      if (ui.behave(idv, c, &hov)) { if (itemSortCol == int(k)) itemSortDesc = !itemSortDesc; else { itemSortCol = int(k); itemSortDesc = k != 0; } }
      string t = colsDef[k].name + (itemSortCol == int(k) ? (itemSortDesc ? " \xE2\x86\x93" : " \xE2\x86\x91") : "");
      ui.text({c.x + 4 * s, c.y, c.w - 8 * s, c.h}, t, 11 * s, hov ? ui.c.text : ui.c.textDim, k ? AL_RIGHT : AL_LEFT, 700);
      x += c.w;
    }
    ui.line(hr.x, hr.b(), hr.r(), hr.b(), ui.c.border);
    vector<int> idx;
    for (int i = 0; i < N.n(); i++) if (itemFilter.empty() || icontains(N.nodes[size_t(i)].label, itemFilter)) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
      if (itemSortCol == 0) return itemSortDesc ? N.nodes[size_t(a)].label > N.nodes[size_t(b)].label : lower(N.nodes[size_t(a)].label) < lower(N.nodes[size_t(b)].label);
      double va = colsDef[size_t(itemSortCol)].v(a), vb = colsDef[size_t(itemSortCol)].v(b);
      return itemSortDesc ? va > vb : va < vb;
    });
    Rect clip = ui.clipRect();
    for (size_t r = 0; r < idx.size(); r++) {
      int i = idx[r];
      Rect row = L.row(24 * s);
      if (row.b() < clip.y || row.y > clip.b()) continue;
      bool sel = std::find(selection.begin(), selection.end(), i) != selection.end();
      if (ui.listRow(row, "it" + std::to_string(i), sel)) focusOn(i);
      float xx = row.x;
      for (size_t k = 0; k < colsDef.size(); k++) {
        Rect c{xx, row.y, row.w * colsDef[k].w, row.h};
        if (k == 0) {
          ui.circle(c.x + 7 * s, c.y + c.h / 2, 4 * s, ccols[size_t(std::max(0, N.nodes[size_t(i)].cluster)) % ccols.size()]);
          ui.text({c.x + 16 * s, c.y, c.w - 18 * s, c.h}, N.nodes[size_t(i)].label, 12 * s, ui.c.text);
        } else {
          double v = colsDef[k].v(i);
          string t = k == 1 ? std::to_string(int(v) + 1) : (k == 4 ? fmtNum(v, 3) : fmtNum(v, v == std::floor(v) ? 0 : 1));
          ui.text({c.x, c.y, c.w - 4 * s, c.h}, t, 11.5f * s, ui.c.textDim, AL_RIGHT);
        }
        xx += c.w;
      }
    }
  } else if (anaTab == 2) {
    NetSummary ns = netSummary(N);
    auto kv = [&](const string& k, const string& v) { Rect r = L.row(22 * s); ui.text(r, k, 12.5f * s, ui.c.textDim); ui.text(r, v, 12.5f * s, ui.c.text, AL_RIGHT, 600); };
    sectionTitle(L, "Network");
    kv("Items", fmtInt(ns.n)); kv("Links", fmtInt(ns.m)); kv("Density", fmtNum(ns.density, 4)); kv("Average degree", fmtNum(ns.avgDegree, 2));
    kv("Total link strength", fmtNum(ns.totalStrength, 0)); kv("Components", fmtInt(ns.components)); kv("Isolated items", fmtInt(ns.isolated));
    kv("Clusters", fmtInt(ns.clusters)); kv("Modularity Q", fmtFixed(P->last.Q, 3));
    if (P->last.layoutMs > 0) { kv("Layout time", fmtNum(P->last.layoutMs, 0) + " ms"); kv("Clustering time", fmtNum(P->last.clusterMs, 0) + " ms"); }
    const ItemMetrics& M = P->itemMetricsCached();
    std::map<string, int> roles;
    for (auto& r : M.role) roles[r]++;
    sectionTitle(L, "Item roles", "Guimer\xC3\xA0 & Amaral: within-cluster degree z and participation P");
    for (auto& r : roles) kv(r.first, fmtInt(r.second));
    sectionTitle(L, "Methods paragraph", "Paste into your paper; includes all parameters for reproducibility.");
    string m = P->methods();
    float h = ui.textWrap({L.x, L.y, L.w, 10000}, m, 12 * s, ui.c.text);
    L.y += h + 8 * s;
    {
      auto mc = cols(L.row(30 * s), 2, 6 * s);
      if (ui.button(mc[0], "Copy Paragraph", BTN_NORMAL, "copy")) { setClipboardText(hwnd, m); ui.toast("Copied", "", 1, 1.5); }
      if (ui.button(mc[1], "Rewrite with AI", BTN_NORMAL, "sparkle", !aiLive)) aiRun(ai::Task::Methods, m);
    }
  } else {
    sectionTitle(L, "Cluster stability", "Re-runs Leiden with different seeds and compares partitions (ARI / NMI). Values near 1 mean robust clusters.");
    float runs = float(stabRuns);
    if (ui.slider(L.row(28 * s), "Runs", runs, 3, 50, "%.0f", 1)) stabRuns = int(runs);
    if (ui.button(L.row(34 * s), "Run stability check", BTN_PRIMARY, "play", !busy())) {
      auto copy = std::make_shared<Network>(N);
      auto out = std::make_shared<Stability>();
      ClusterOpts co = P->params.clusterOpts();
      int rr = stabRuns;
      startJob("Stability check", [copy, out, co, rr](Job&) { *out = clusterStability(*copy, co, rr); }, [this, out] { stab = *out; stabValid = true; });
    }
    if (stabValid) {
      auto c3 = cols(L.row(64 * s), 3, 6 * s);
      auto st = [&](const Rect& r, const string& v, const string& l, double q) {
        ui.fill(r, ui.c.card, 8 * s);
        ui.stroke(r, ui.c.border, 8 * s);
        ui.text({r.x + 10 * s, r.y + 8 * s, r.w, 26 * s}, v, 19 * s, q > 0.8 ? ui.c.ok : q > 0.5 ? ui.c.warn : ui.c.danger, AL_LEFT, 700);
        ui.text({r.x + 10 * s, r.y + 38 * s, r.w, 16 * s}, l, 11 * s, ui.c.textDim);
      };
      st(c3[0], fmtFixed(stab.meanAri, 2), "mean ARI", stab.meanAri);
      st(c3[1], fmtFixed(stab.minAri, 2), "min ARI", stab.minAri);
      st(c3[2], fmtFixed(stab.meanNmi, 2), "mean NMI", stab.meanNmi);
      if (stab.freshReference) {
        string note = "The map\xE2\x80\x99s clusters were made with other settings, so the runs were compared with a new clustering using the current settings. Re-cluster to update the map.";
        float th = ui.textWrap({0, 0, L.w, 400 * s}, note, 11.5f * s, ui.c.textDim, 400, false);
        ui.textWrap(L.row(th + 4 * s), note, 11.5f * s, ui.c.textDim);
      }
      if (!stab.itemStability.empty() && stab.itemStability.size() == size_t(N.n())) {
        sectionTitle(L, "Least stable items", "Items that switch cluster most often between runs");
        vector<int> idx(static_cast<size_t>(N.n()));
        for (int i = 0; i < N.n(); i++) idx[size_t(i)] = i;
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return stab.itemStability[size_t(a)] < stab.itemStability[size_t(b)]; });
        for (size_t k = 0; k < std::min<size_t>(12, idx.size()); k++) {
          Rect r = L.row(22 * s);
          if (ui.listRow(r, "st" + std::to_string(idx[k]), false)) focusOn(idx[k]);
          ui.text({r.x + 6 * s, r.y, r.w - 60 * s, r.h}, N.nodes[size_t(idx[k])].label, 12 * s, ui.c.text);
          ui.text(r, fmtNum(stab.itemStability[size_t(idx[k])] * 100, 0) + "%", 11.5f * s, ui.c.textDim, AL_RIGHT);
        }
      }
    }
    analyseSweep(L);
  }
}

// =====================================================================================
// TRENDS
// =====================================================================================
void App::pageTrends(Lay& L) {
  float s = ui.s;
  if (!hasCorpus()) { emptyHint(L, "trends", "No Records", "Import data to see trends."); return; }
  tabRow(L, {"Growth", "Topics", "Bursts", "Themes", "Compare", "Main path", "3-field", "RPYS"}, {0, 5, 1, 2, 3, 7, 4, 6}, trTab);
  L.space(4 * s);
  if (trTab == 5) { trendsTopics(L); return; }
  if (trTab == 6) { trendsRpys(L); return; }
  if (trTab == 7) { trendsMainPath(L); return; }
  auto ul = corpusUnitLabels();
  const Growth& gr = statGrowth();
  if (trTab == 0) {
    ChartDef d;
    d.title = "Annual scientific production";
    d.make = [gr](double w, double h, const ChartTheme& t) { return chartGrowth(gr, w, h, t); };
    chartBox(L, "growth", d, 220);
    sectionTitle(L, "Per year");
    for (size_t i = 0; i < gr.perYear.size(); i++) {
      Rect r = L.row(20 * s);
      ui.text(r, std::to_string(gr.perYear[i].first), 12 * s, ui.c.textDim);
      ui.text({r.x + 60 * s, r.y, 80 * s, r.h}, fmtInt(gr.perYear[i].second) + " docs", 12 * s, ui.c.text);
      if (i < gr.citesPerYear.size()) ui.text(r, fmtNum(gr.citesPerYear[i].second, 1) + " cites/doc", 11.5f * s, ui.c.textDim, AL_RIGHT);
    }
  } else if (trTab == 1) {
    sectionTitle(L, "Burst detection", "Kleinberg's two-state automaton over yearly document frequencies (as in CiteSpace).");
    Rect f = field(ui, L, "Unit", 32);
    if (ui.combo(f, "bu", ul, trUnit)) burstsValid = false;
    Rect f2 = field(ui, L, "Min. documents", 30, 0.58f);
    if (ui.numberInput(f2, "bmin", burstMin, 2, 1000)) burstsValid = false;
    if (ui.slider(L.row(28 * s), "Rate ratio s", burstS, 1.1f, 5, "%.1f", 0.1f)) burstsValid = false;
    if (ui.slider(L.row(28 * s), "\xCE\xB3 (transition cost)", burstG, 0, 2, "%.2f", 0.05f)) burstsValid = false;
    if (!burstsValid) { bursts = detectBursts(P->corpus, corpusUnits()[size_t(trUnit)].first, burstMin, burstS, burstG); burstsValid = true; }
    ChartDef d;
    d.title = "Top bursts \xC2\xB7 " + ul[size_t(trUnit)];
    auto b = bursts;
    int y0 = gr.y0, y1 = gr.y1;
    d.make = [b, y0, y1](double w, double h, const ChartTheme& t) { return chartBursts(b, y0, y1, 15, w, h, t); };
    d.onClick = [this](int k) { if (hasMap() && k < int(bursts.size())) { search = bursts[size_t(k)].label; runSearch(); if (!searchHits.empty()) focusOn(searchHits[0]); } };
    chartBox(L, "bursts", d, std::max(140.f, 26.f * std::min<int>(15, int(bursts.size())) + 30));
    ui.text(L.row(18 * s), plural(long(bursts.size()), "burst") + " detected" + (bursts.size() < 5 ? ". Lower \xCE\xB3 or the minimum to see more." : ""), 11.5f * s, ui.c.textDim);
  } else if (trTab == 2) {
    sectionTitle(L, "Thematic evolution", "Themes per period (clusters of keywords) and how they flow into each other.");
    Rect f = field(ui, L, "Unit", 32);
    if (ui.combo(f, "eu", ul, trUnit)) evoValid = false;
    Rect f2 = field(ui, L, "Cut years", 30);
    string before = evoCuts;
    ui.textInput(f2, "cuts", evoCuts, "auto (e.g. 2019, 2022)");
    if (before != evoCuts) evoValid = false;
    if (!evoValid) {
      vector<int> cuts;
      for (auto& t : splitAny(evoCuts, ", ;")) if (isDigits(t)) cuts.push_back(toInt(t));
      evo = thematicEvolution(P->corpus, corpusUnits()[size_t(trUnit)].first, cuts, 2, &P->engine.thesaurus);
      evoValid = true;
    }
    ChartDef d;
    d.title = "Thematic evolution \xC2\xB7 " + ul[size_t(trUnit)];
    Evolution e = evo;
    d.make = [e](double w, double h, const ChartTheme& t) { return chartSankey(e, w, h, t); };
    chartBox(L, "evo", d, 300);
    for (size_t p = 0; p < evo.periods.size(); p++) {
      sectionTitle(L, std::to_string(evo.periods[p].first) + "\xE2\x80\x93" + std::to_string(evo.periods[p].second));
      for (auto& th : evo.themes) if (th.period == int(p)) {
        float h = ui.textWrap({0, 0, L.w - 12 * s, 1000}, join(th.keywords, ", "), 11.5f * s, ui.c.textDim, 400, false);
        Rect r = L.row(24 * s + h);
        ui.text({r.x, r.y, r.w, 20 * s}, th.name + "  (" + std::to_string(th.size) + ")", 12.5f * s, ui.c.text, AL_LEFT, 600);
        ui.textWrap({r.x, r.y + 20 * s, r.w, h + 4}, join(th.keywords, ", "), 11.5f * s, ui.c.textDim);
      }
    }
  } else if (trTab == 3) {
    sectionTitle(L, "Compare two periods", "Which terms grew or declined in relative frequency (log ratio of shares).");
    if (!cmpA0) { int mid = (gr.y0 + gr.y1) / 2; cmpA0 = gr.y0; cmpA1 = mid; cmpB0 = mid + 1; cmpB1 = gr.y1; }
    Rect f = field(ui, L, "Unit", 32);
    if (ui.combo(f, "cu", ul, trUnit)) cmpValid = false;
    Rect fa = field(ui, L, "Period A", 30, 0.3f);
    auto ca = cols(fa, 2, 6 * s);
    if (ui.numberInput(ca[0], "a0", cmpA0, 1900, 2100)) cmpValid = false;
    if (ui.numberInput(ca[1], "a1", cmpA1, 1900, 2100)) cmpValid = false;
    Rect fb = field(ui, L, "Period B", 30, 0.3f);
    auto cb = cols(fb, 2, 6 * s);
    if (ui.numberInput(cb[0], "b0", cmpB0, 1900, 2100)) cmpValid = false;
    if (ui.numberInput(cb[1], "b1", cmpB1, 1900, 2100)) cmpValid = false;
    if (!cmpValid) { cmp = comparePeriods(P->corpus, corpusUnits()[size_t(trUnit)].first, cmpA0, cmpA1, cmpB0, cmpB1); cmpValid = true; }
    ChartDef d;
    d.title = "Emerging vs declining \xC2\xB7 " + ul[size_t(trUnit)];
    Comparison c = cmp;
    d.make = [c](double w, double h, const ChartTheme& t) { return chartCompare(c, 10, w, h, t); };
    chartBox(L, "cmp", d, 380);
    ui.text(L.row(18 * s), "A: " + fmtInt(cmp.nA) + " docs \xC2\xB7 B: " + fmtInt(cmp.nB) + " docs \xC2\xB7 " + std::to_string(cmp.stable.size()) + " stable terms", 11.5f * s, ui.c.textDim);
    trendsDifference(L);
  } else {
    sectionTitle(L, "Three-field plot", "How the top items of three fields connect through shared documents.");
    Rect f1 = field(ui, L, "Left", 32);
    if (ui.combo(f1, "tl", ul, tfL)) tfValid = false;
    Rect f2 = field(ui, L, "Middle", 32);
    if (ui.combo(f2, "tm", ul, tfM)) tfValid = false;
    Rect f3 = field(ui, L, "Right", 32);
    if (ui.combo(f3, "tr", ul, tfR)) tfValid = false;
    Rect f4 = field(ui, L, "Items per field", 30, 0.58f);
    if (ui.numberInput(f4, "tn", tfTop, 3, 30)) tfValid = false;
    if (!tfValid) { tf = threeField(P->corpus, corpusUnits()[size_t(tfL)].first, corpusUnits()[size_t(tfM)].first, corpusUnits()[size_t(tfR)].first, tfTop); tfValid = true; }
    ChartDef d;
    d.title = ul[size_t(tfL)] + " \xE2\x86\x92 " + ul[size_t(tfM)] + " \xE2\x86\x92 " + ul[size_t(tfR)];
    ThreeField t3 = tf;
    string a = ul[size_t(tfL)], b = ul[size_t(tfM)], c = ul[size_t(tfR)];
    d.make = [t3, a, b, c](double w, double h, const ChartTheme& t) { return chartThreeField(t3, a, b, c, w, h, t); };
    chartBox(L, "tf", d, 360);
  }
}

// =====================================================================================
// ACTORS
// =====================================================================================
void App::pageActors(Lay& L) {
  float s = ui.s;
  if (!hasCorpus()) { emptyHint(L, "actors", "No Records", "Import data to see authors, sources and countries."); return; }
  const char* tn[5] = {"Authors", "Sources", "Countries", "Orgs", "Laws"};
  tabRow(L, {"Authors", "Sources", "Countries", "Orgs", "Documents", "Laws"}, {0, 1, 2, 3, 5, 4}, acTab);
  L.space(4 * s);
  if (acTab == 5) { actorsDocs(L); return; }
  if (acTab == 4) {
    const Bradford& b = statBradford();
    ChartDef d;
    d.title = "Bradford's law: source concentration";
    d.make = [b](double w, double h, const ChartTheme& t) { return chartBradford(b, w, h, t); };
    chartBox(L, "brad", d, 200);
    ui.textWrap({L.x, L.y, L.w, 60 * s}, "Core zone: " + std::to_string(b.zone1) + " sources publish a third of the documents; zone 2: " + std::to_string(b.zone2 - b.zone1) + " sources; the rest form zone 3.", 12 * s, ui.c.textDim);
    L.y += 40 * s;
    for (size_t k = 0; k < std::min<size_t>(size_t(b.zone1), b.sources.size()); k++) {
      Rect r = L.row(20 * s);
      ui.text({r.x, r.y, r.w - 50 * s, r.h}, b.sources[k].first, 12 * s, ui.c.text);
      ui.text(r, fmtInt(b.sources[k].second), 11.5f * s, ui.c.textDim, AL_RIGHT);
    }
    const Lotka& lk = statLotka();
    ChartDef d2;
    d2.title = "Lotka's law: author productivity";
    d2.make = [lk](double w, double h, const ChartTheme& t) { return chartLotka(lk, w, h, t); };
    L.space(8 * s);
    chartBox(L, "lotka", d2, 200);
    ui.textWrap({L.x, L.y, L.w, 60 * s}, "Fitted exponent n = " + fmtNum(lk.exponent, 2) + " (Lotka's classic value is 2). R\xC2\xB2 = " + fmtNum(lk.r2, 2) + ".", 12 * s, ui.c.textDim);
    L.y += 36 * s;
    return;
  }
  static const Unit units[4] = {Unit::Authors, Unit::Sources, Unit::Countries, Unit::Orgs};
  if (actorsUnit != acTab) { actors = topActors(P->corpus, units[acTab], 200); actorsUnit = acTab; }
  {
    vector<std::pair<string, double>> rows;
    for (size_t k = 0; k < std::min<size_t>(10, actors.size()); k++) rows.push_back({actors[k].label, double(actors[k].docs)});
    ChartDef d;
    d.title = string("Most productive ") + lower(tn[acTab]);
    d.make = [rows](double w, double h, const ChartTheme& t) { return chartBars(rows, "", w, h, t, ""); };
    d.onClick = [this](int k) { if (hasMap() && k < int(actors.size())) { search = actors[size_t(k)].label; runSearch(); if (!searchHits.empty()) focusOn(searchHits[0]); } };
    chartBox(L, "bars", d, 230);
  }
  actorsTimeline(L, units[acTab]);
  if (acTab == 2) {  // countries: single- vs multiple-country publications
    if (!collabValid) { collab = countryCollaboration(P->corpus, 12); collabValid = true; }
    if (!collab.empty()) {
      sectionTitle(L, "International collaboration", "SCP: documents whose authors are all from one country. MCP: documents with co-authors from several countries. The label shows the MCP share. Documents are credited to every country in their affiliations.");
      ChartDef d;
      d.title = "SCP vs MCP";
      d.make = [this](double w, double h, const ChartTheme& t) { return chartCollab(collab, w, h, t); };
      chartBox(L, "scp", d, float(std::max<size_t>(6, collab.size())) * 21 + 40);
    }
  }
  Rect er = L.row(28 * s);
  if (ui.button({er.r() - 120 * s, er.y, 120 * s, er.h}, "Export CSV", BTN_GHOST, "download")) {
    string p = saveFileDialog(hwnd, "Export table", {{"CSV (UTF-8)", "*.csv"}}, lower(tn[acTab]) + ".csv", "csv");
    if (!p.empty()) {
      string o = "\xEF\xBB\xBFlabel,documents,citations,h_index,citations_per_doc,first_year,last_year\n";
      for (auto& a : actors) o += "\"" + replaceAll(a.label, "\"", "\"\"") + "\"," + std::to_string(a.docs) + "," + std::to_string(a.cites) + "," + std::to_string(a.h) + "," + fmtNum(a.citesPerDoc, 2) + "," + std::to_string(a.firstYear) + "," + std::to_string(a.lastYear) + "\n";
      if (writeFileU(p, o)) ui.toast("Exported", fileName(p), 1);
    }
  }
  const char* hn[5] = {"Name", "Docs", "Cites", "h", "Years"};
  float cw[5] = {0.44f, 0.12f, 0.14f, 0.1f, 0.2f};
  Rect hr = L.row(24 * s);
  float x = hr.x;
  for (int k = 0; k < 5; k++) {
    Rect c{x, hr.y, hr.w * cw[k], hr.h};
    uint64_t idv = ui.id("ah" + std::to_string(k));
    bool hov = false;
    if (k < 4 && ui.behave(idv, c, &hov)) { if (acSortCol == k) acSortDesc = !acSortDesc; else { acSortCol = k; acSortDesc = k != 0; } }
    string t = string(hn[k]) + (acSortCol == k ? (acSortDesc ? " \xE2\x86\x93" : " \xE2\x86\x91") : "");
    ui.text({c.x + 4 * s, c.y, c.w - 8 * s, c.h}, t, 11 * s, hov ? ui.c.text : ui.c.textDim, k ? AL_RIGHT : AL_LEFT, 700);
    x += c.w;
  }
  ui.line(hr.x, hr.b(), hr.r(), hr.b(), ui.c.border);
  vector<Actor> v = actors;
  std::sort(v.begin(), v.end(), [&](const Actor& a, const Actor& b) {
    switch (acSortCol) {
      case 0: return acSortDesc ? a.label > b.label : a.label < b.label;
      case 2: return acSortDesc ? a.cites > b.cites : a.cites < b.cites;
      case 3: return acSortDesc ? a.h > b.h : a.h < b.h;
      default: return acSortDesc ? a.docs > b.docs : a.docs < b.docs;
    }
  });
  Rect clip = ui.clipRect();
  for (size_t i = 0; i < v.size(); i++) {
    Rect r = L.row(22 * s);
    if (r.b() < clip.y || r.y > clip.b()) continue;
    if (ui.listRow(r, "ac" + std::to_string(i), false) && hasMap()) { search = v[i].label; runSearch(); if (!searchHits.empty()) focusOn(searchHits[0]); else ui.toast("Not on the map", v[i].label, 0, 2); }
    float xx = r.x;
    string cells[5] = {v[i].label, fmtInt(v[i].docs), fmtInt(v[i].cites), std::to_string(v[i].h), v[i].firstYear ? std::to_string(v[i].firstYear) + "\xE2\x80\x93" + std::to_string(v[i].lastYear % 100 < 10 ? v[i].lastYear : v[i].lastYear) : ""};
    for (int k = 0; k < 5; k++) {
      Rect c{xx, r.y, r.w * cw[k], r.h};
      ui.text({c.x + 4 * s, c.y, c.w - 8 * s, c.h}, cells[k], k ? 11.5f * s : 12 * s, k ? ui.c.textDim : ui.c.text, k ? AL_RIGHT : AL_LEFT);
      xx += c.w;
    }
  }
}

// =====================================================================================
// PUBLISH
// =====================================================================================
void App::pagePublish(Lay& L) {
  float s = ui.s;
  if (!hasMap()) { emptyHint(L, "publish", "No Map", "Build a map to create figures and exports."); return; }
  FigureSpec& F = P->fig;
  bool ch = false;
  // live preview
  string sig = styleToJson(P->style).dump() + fmtNum(F.wmm, 1) + fmtNum(F.hmm, 1) + fmtNum(F.labelPt, 2) + fmtNum(F.linkMaxPt, 2) + std::to_string(int(F.theme)) +
               std::to_string(F.panelNetwork) + std::to_string(F.panelOverlay) + std::to_string(F.panelDensity) + std::to_string(F.panelTimeline) + std::to_string(F.panelGeo) +
               std::to_string(F.panel3D) + std::to_string(F.panelMatrix) + figurePanelSig() + std::to_string(F.legend) + std::to_string(F.sizeLegend) +
               std::to_string(F.colorbar) + std::to_string(F.letters) + std::to_string(F.footer) + std::to_string(F.transparent) + std::to_string(F.serif) + std::to_string(F.shading) + F.title + F.caption +
               std::to_string(P->net.n()) + std::to_string(P->bundles.valid()) + std::to_string(frameNo / 1000000);
  if (sig != figSig) { auto defs = figurePanelDefs(); figPreview = buildFigure(P->net, P->style, F, &P->bundles, P->methodsShort(), &defs); figSig = sig; s_figGen++; }
  {
    float pw = L.w, ph = float(pw * F.hmm / F.wmm);
    ph = std::min(ph, 320 * s);
    float k = std::min(pw / float(figPreview.W), ph / float(figPreview.H));
    Rect r = L.row(ph + 8 * s);
    ui.fill(r, ui.dark ? Color::hex(0x0e1013) : Color::hex(0xe4e7ec), 8 * s);
    float ox = r.x + (r.w - float(figPreview.W) * k) / 2, oy = r.y + 4 * s + (ph - float(figPreview.H) * k) / 2;
    ui.shadow({ox, oy, float(figPreview.W) * k, float(figPreview.H) * k}, 0, 8 * s);
    drawSceneCached(g, s_pubCache, figPreview, ox, oy, k, F.serif, std::to_string(s_figGen));
    {
      Rect fr{ox, oy, float(figPreview.W) * k, float(figPreview.H) * k};
      uint64_t idv = ui.id("figpreview");
      bool hov = false;
      if (ui.behave(idv, fr, &hov)) { figZoomOpen = true; figZoomK = 1; figZoomX = figZoomY = 0; figZoomT0 = ui.time; figZoomFrom = fr; }
      if (hov) {
        ui.cursor = "hand";
        Rect bz{fr.r() - 34 * s, fr.y + 6 * s, 28 * s, 28 * s};
        ui.fill(bz, Color(0, 0, 0, 0.55f), 6 * s);
        ui.icon("fit", bz.x + bz.w / 2, bz.y + bz.h / 2, 15 * s, Color(1, 1, 1));
      }
      ui.tipFor(idv, "Click to open the figure full screen (wheel to zoom, drag to pan, Esc to close)");
    }
    string info = fmtNum(F.wmm, 0) + " \xC3\x97 " + fmtNum(F.hmm, 0) + " mm \xC2\xB7 " + std::to_string(figPreview.labels) + " labels";
    if (figPreview.minLabel < 1e8) info += " \xC2\xB7 smallest label " + fmtNum(figPreview.minLabel, 1) + " pt";
    ui.text(L.row(18 * s), info, 11.5f * s, ui.c.textDim);
    for (auto& w : figPreview.warnings) {
      float h = ui.textWrap({0, 0, L.w - 24 * s, 1000}, w, 11.5f * s, ui.c.warn, 400, false);
      Rect wr = L.row(h + 10 * s);
      ui.fill(wr, ui.c.warn.withA(0.12f), 6 * s);
      ui.icon("warn", wr.x + 11 * s, wr.y + 12 * s, 13 * s, ui.c.warn);
      ui.textWrap({wr.x + 22 * s, wr.y + 5 * s, wr.w - 28 * s, h + 4}, w, 11.5f * s, ui.c.warn);
    }
  }
  // which views go into the figure — every canvas view can be a panel
  {
    Rect hr = L.row(20 * s);
    ui.text(hr, "Views in this figure", 12 * s, ui.c.textDim, AL_LEFT, 600);
    ui.help(hr.x + ui.textW("Views in this figure", 10.5f * s, 700) + 6 * s, hr.y + hr.h / 2,
            "Each selected view becomes a lettered panel (a, b, c\xE2\x80\xA6) of one print-ready figure. Geo uses the Geo view's fill mode "
            "(documents per country, or countries as nodes); 3D uses the 3D view's camera angle; Timeline and Matrix are laid out for print.");
    bool geoOk = geoAvailable();
    if (!geoOk) F.panelGeo = false;
    struct Chip { const char* label; const char* icon; bool* v; bool en; const char* tip; };
    Chip chips[7] = {{"Network", "network", &F.panelNetwork, true, "Clusters, as in the Network view"},
                     {"Overlay", "clock", &F.panelOverlay, true, "Colour by average year (or score), with the colour bar"},
                     {"Density", "density", &F.panelDensity, true, "Item density heat map"},
                     {"Timeline", "trends", &F.panelTimeline, true, "Items placed by average year, with a year axis"},
                     {"Geo", "globe", &F.panelGeo, geoOk, geoOk ? "World map, following the Geo view's mode" : "Needs country data (affiliations with countries)"},
                     {"3D", "cube", &F.panel3D, true, "The 3D layout seen from the 3D view's current camera angle"},
                     {"Matrix", "grid", &F.panelMatrix, true, "Link strength between the top items"}};
    ui.pushId("figviews");
    for (int row = 0; row < 2; row++) {
      int n0 = row == 0 ? 0 : 4, n = row == 0 ? 4 : 3;
      auto cc = cols(L.row(30 * s), 4, 5 * s);
      for (int i = 0; i < n; i++) {
        Chip& c = chips[n0 + i];
        Rect r = cc[size_t(i)];
        uint64_t idv = ui.id(c.label);
        bool hov = false;
        bool clicked = ui.behave(idv, r, &hov) && c.en;
        if (clicked) { *c.v = !*c.v; ch = true; }
        bool on = *c.v;
        ui.fill(r, hov && c.en && !on ? ui.c.hover : ui.c.input, 7 * s);
        ui.stroke(r, on ? ui.c.accent : ui.c.border, 7 * s, on ? 2.f * s : 1.f);
        Color fg = !c.en ? ui.c.textFaint : on ? ui.c.text : ui.c.textDim;
        float fsz = 11.5f * s;
        float tw = ui.textW(c.label, fsz, 500);
        bool withIcon = tw + 25 * s <= r.w - 6 * s;  // drop the icon rather than crowd the label in a narrow panel
        float cx = r.x + (r.w - (tw + (withIcon ? 19 * s : 0))) / 2;
        if (withIcon) ui.icon(c.icon, cx + 7 * s, r.y + r.h / 2, 14 * s, !c.en ? ui.c.textFaint : on ? ui.c.accent : ui.c.textDim, 1.6f);
        ui.text({cx + (withIcon ? 19 * s : 0), r.y, tw + 4 * s, r.h}, c.label, fsz, fg, AL_LEFT, 500);
        ui.tipFor(idv, c.tip);
        if (hov && c.en) ui.cursor = "hand";
      }
    }
    ui.popId();
    if (!F.panelNetwork && !F.panelOverlay && !F.panelDensity && !F.panelTimeline && !F.panelGeo && !F.panel3D && !F.panelMatrix) F.panelNetwork = true;
  }
  // export options + buttons
  {
    ch |= ui.toggle(L.row(26 * s), "Transparent background", F.transparent);
    ch |= ui.toggle(L.row(26 * s), "PDF: one page per plot", F.pdfPages);
    auto c3 = cols(L.row(40 * s), 3, 6 * s);
    if (ui.button(c3[0], "SVG", BTN_PRIMARY, "download")) cmdExportFigure("svg");
    if (ui.button(c3[1], "PDF", BTN_PRIMARY, "download")) cmdExportFigure("pdf");
    if (ui.button(c3[2], "PNG", BTN_PRIMARY, "download")) cmdExportFigure("png");
    if (ui.button(L.row(30 * s), "Copy figure to clipboard", BTN_NORMAL, "copy")) cmdCopyFigure();
  }
  // one view, exactly as shown on the canvas
  {
    L.row(4 * s);
    Rect hr = L.row(20 * s);
    ui.text(hr, "Export one view", 12 * s, ui.c.textDim, AL_LEFT, 600);
    ui.help(hr.x + ui.textW("Export one view", 12 * s, 600) + 6 * s, hr.y + hr.h / 2,
            "Exports exactly what the canvas shows, at the on-screen size. Also available from the canvas download button and Ctrl+Shift+E.");
    static const ViewKind vk[7] = {ViewKind::Network, ViewKind::Overlay, ViewKind::Density, ViewKind::Timeline, ViewKind::Geo, ViewKind::ThreeD, ViewKind::Matrix};
    static const char* vic[7] = {"network", "clock", "density", "trends", "globe", "cube", "grid"};
    Rect vr = L.row(32 * s);
    ui.fill(vr, ui.c.input, 7 * s);
    ui.stroke(vr, ui.c.border, 7 * s);
    float bw = (vr.w - 4 * s) / 7;
    ui.pushId("expview");
    for (int i = 0; i < 7; i++) {
      Rect br{vr.x + 2 * s + i * bw, vr.y + 2 * s, bw, vr.h - 4 * s};
      if (ui.iconButton(br, vic[i], "Show the " + viewName(vk[i]) + " view", view == vk[i])) setView(vk[i]);
    }
    string vn = viewName(view);
    auto c4 = cols(L.row(30 * s), 3, 6 * s);
    if (ui.button(c4[0], vn + " SVG", BTN_NORMAL, "download")) cmdExportCurrentView("svg");
    if (ui.button(c4[1], vn + " PDF", BTN_NORMAL, "download")) cmdExportCurrentView("pdf");
    if (ui.button(c4[2], vn + " PNG", BTN_NORMAL, "download")) cmdExportCurrentView("png");
    ui.popId();
  }
  ui.pushId("fig");
  sectionTitle(L, "Size & journal presets");
  {
    const auto& pr = figPresets();
    vector<string> lbl;
    for (auto& p : pr) lbl.push_back(string(p.label) + "  (" + fmtNum(p.wmm, 0) + "\xC3\x97" + fmtNum(p.hmm, 0) + " mm)");
    Rect f = field(ui, L, "Preset", 32, 0.3f);
    if (ui.combo(f, "preset", lbl, figPreset)) { F.wmm = pr[size_t(figPreset)].wmm; F.hmm = pr[size_t(figPreset)].hmm; F.labelPt = pr[size_t(figPreset)].labelPt; ch = true; }
    Rect fw = field(ui, L, "Width \xC3\x97 height (mm)", 30);
    auto c2 = cols(fw, 2, 6 * s);
    ch |= ui.numberInputD(c2[0], "w", F.wmm, 40, 600, 5, 0);
    ch |= ui.numberInputD(c2[1], "h", F.hmm, 30, 600, 5, 0);
    Rect fl = field(ui, L, "Label size (pt)", 30, 0.58f);
    ch |= ui.numberInputD(fl, "lp", F.labelPt, 4, 24, 0.5, 1);
    Rect fk = field(ui, L, "Max. line width (pt)", 30, 0.58f);
    ch |= ui.numberInputD(fk, "lk", F.linkMaxPt, 0.2, 6, 0.1, 1);
    Rect fd = field(ui, L, "PNG resolution (dpi)", 30, 0.58f);
    ch |= ui.numberInputD(fd, "dpi", F.dpi, 72, 1200, 50, 0);
    int th = int(F.theme);
    if (ui.segmented(L.row(30 * s), {"Print", "Screen", "Slide"}, th, "ftheme")) { F.theme = FigTheme(th); ch = true; }
  }
  sectionTitle(L, "Elements");
  {
    Rect f = field(ui, L, "Node shading", 30, 0.36f);
    ch |= ui.segmented(f, {"Match view", "Flat", "Sphere"}, F.shading, "shading");
  }
  ch |= ui.toggle(L.row(26 * s), "Cluster legend", F.legend);
  ch |= ui.toggle(L.row(26 * s), "Size legend", F.sizeLegend);
  ch |= ui.toggle(L.row(26 * s), "Colour bar (overlay)", F.colorbar);
  ch |= ui.toggle(L.row(26 * s), "Panel letters (a, b, c)", F.letters);
  ch |= ui.toggle(L.row(26 * s), "Methods footer", F.footer);
  ch |= ui.toggle(L.row(26 * s), "Serif font", F.serif);
  ui.textInput(L.row(30 * s), "title", F.title, "Figure title (optional)");
  ui.textInput(L.row(30 * s), "caption", F.caption, "Caption (optional)");
  if (ui.button(L.row(30 * s), "Write Caption and Results with AI", BTN_GHOST, "sparkle", !aiLive)) aiRun(ai::Task::Caption);
  ui.popId();
  sectionTitle(L, "Data & project", "Projects are self-contained .vosproj bundles (records, thesaurus, map, look and figure settings) with SHA-256 checksums for reproducibility. Record exports can be opened in VOSviewer, bibliometrix, CiteSpace or a reference manager.");
  {
    auto c2 = cols(L.row(30 * s), 2, 6 * s);
    if (ui.button(c2[0], "VOSviewer files", BTN_NORMAL, "network")) cmdExportVosviewer();
    if (ui.button(c2[1], "Items CSV", BTN_NORMAL, "table")) cmdExportItemsCsv();
    auto c3 = cols(L.row(30 * s), 2, 6 * s);
    if (ui.button(c3[0], "Save project", BTN_NORMAL, "save")) cmdSaveProject(false);
    if (ui.button(c3[1], "Methods text\xE2\x80\xA6", BTN_NORMAL, "file")) {
      string p = saveFileDialog(hwnd, "Save methods paragraph", {{"Text", "*.txt"}}, "methods.txt", "txt");
      if (!p.empty() && writeFileU(p, P->methods() + "\n")) ui.toast("Saved", fileName(p), 1);
    }
    auto c5 = cols(L.row(30 * s), 3, 6 * s);
    ui.pushId("recexp");
    if (ui.button(c5[0], "Records WoS", BTN_NORMAL, "download")) cmdExportRecords(false, "wos");
    ui.tip("All loaded records as Web of Science tagged text (VOSviewer, bibliometrix, CiteSpace)");
    if (ui.button(c5[1], "RIS", BTN_NORMAL, "download")) cmdExportRecords(false, "ris");
    ui.tip("All loaded records as RIS");
    if (ui.button(c5[2], "CSV", BTN_NORMAL, "download")) cmdExportRecords(false, "csv");
    ui.tip("All loaded records as CSV (Scopus column names)");
    ui.popId();
  }
  if (ch) figSig.clear();
}

// =====================================================================================
// 1.2 analyses: trend topics, RPYS, production over time, documents, clusters over time
// =====================================================================================
// Tabs sized to their labels; wraps onto a second row in narrow panels.
void App::tabRow(Lay& L, const vector<string>& names, const vector<int>& ids, int& sel) {
  float s = ui.s, h = 34 * s;
  vector<float> w;
  for (auto& n : names) w.push_back(ui.textW(n, 13 * s, 600) + 18 * s);
  size_t i = 0;
  while (i < names.size()) {
    size_t j = i;
    float sum = 0;
    while (j < names.size() && (j == i || sum + w[j] <= L.w)) sum += w[j++];
    Rect r = L.row(h);
    float k = r.w / std::max(1.f, sum), x = r.x;
    for (size_t t = i; t < j; t++) {
      float ww = (j == names.size() && i > 0) ? w[t] : w[t] * k;  // last wrapped row keeps natural widths
      if (ui.tab({x, r.y, ww, h}, names[t], sel == ids[t])) sel = ids[t];
      x += ww;
    }
    i = j;
  }
}

void App::trendsTopics(Lay& L) {
  float s = ui.s;
  sectionTitle(L, "Trend topics", "For each frequent term: the dot marks its median publication year (size = frequency) and the line spans the middle half of its years (Q1\xE2\x80\x93Q3), as in Bibliometrix. Terms are grouped by median year, keeping the most frequent of each year. Click a term to find it on the map.");
  auto ul = corpusUnitLabels();
  Rect f = field(ui, L, "Terms from", 32);
  if (ui.combo(f, "ttu", ul, trUnit)) ttValid = false;
  Rect f2 = field(ui, L, "Min. frequency", 30, 0.58f);
  if (ui.numberInput(f2, "ttmin", ttMin, 2, 500, 1)) ttValid = false;
  Rect f3 = field(ui, L, "Terms per year", 30, 0.58f);
  if (ui.numberInput(f3, "ttper", ttPerYear, 1, 10, 1)) ttValid = false;
  if (!ttValid) {
    ttopics = trendTopics(P->corpus, corpusUnits()[size_t(clampv(trUnit, 0, int(corpusUnits().size()) - 1))].first, ttMin, ttPerYear, &P->engine.thesaurus);
    ttValid = true;
  }
  if (ttopics.empty()) {
    float h = ui.textWrap({L.x, L.y, L.w, 200}, "No term reaches the minimum frequency with dated documents. Lower the minimum frequency or choose another unit.", 12 * s, ui.c.textDim);
    L.y += h + 8 * s;
    return;
  }
  ChartDef d;
  d.title = "Trend topics \xC2\xB7 " + ul[size_t(clampv(trUnit, 0, int(ul.size()) - 1))];
  d.make = [this](double w, double h, const ChartTheme& t) { return chartTrendTopics(ttopics, w, h, t); };
  d.onClick = [this](int k) { if (k >= 0 && k < int(ttopics.size())) findInMap(ttopics[size_t(k)].label); };
  chartBox(L, "tt", d, std::max(160.f, float(ttopics.size()) * 17 + 44));
  float h = ui.textWrap({L.x, L.y, L.w, 200}, std::to_string(ttopics.size()) + " terms. Recent medians (right) are emerging topics; long lines mark terms used over many years.", 11.5f * s, ui.c.textDim);
  L.y += h + 6 * s;
}

void App::trendsRpys(Lay& L) {
  float s = ui.s;
  sectionTitle(L, "Reference publication year spectroscopy", "Counts the cited references of your corpus by the year the cited work was published. Peaks above the 5-year median (orange) reveal the historical roots of the field: the older works it builds on (Marx, Bornmann, Barth & Leydesdorff, 2014). From/To = 0 chooses the range automatically.");
  bool anyRefs = false;
  for (auto& r : P->corpus.recs) if (!r.refs.empty()) { anyRefs = true; break; }
  if (!anyRefs) {
    float h = ui.textWrap({L.x, L.y, L.w, 200}, "RPYS needs cited references. Export your records from Web of Science or Scopus including cited references.", 12 * s, ui.c.textDim);
    L.y += h + 8 * s;
    return;
  }
  auto c2 = cols(L.row(30 * s), 2, 8 * s);
  ui.text({c2[0].x, c2[0].y, 44 * s, c2[0].h}, "From", 12 * s, ui.c.textDim);
  if (ui.numberInput({c2[0].x + 44 * s, c2[0].y, c2[0].w - 44 * s, c2[0].h}, "rpf", rpFrom, 0, 2100, 1)) rpValid = false;
  ui.text({c2[1].x, c2[1].y, 30 * s, c2[1].h}, "To", 12 * s, ui.c.textDim);
  if (ui.numberInput({c2[1].x + 30 * s, c2[1].y, c2[1].w - 30 * s, c2[1].h}, "rpt", rpTo, 0, 2100, 1)) rpValid = false;
  if (!rpValid) { rpy = rpys(P->corpus, rpFrom, rpTo); rpValid = true; }
  ChartDef d;
  d.title = "References by year";
  d.make = [this](double w, double h, const ChartTheme& t) { return chartRpys(rpy, w, h, t); };
  chartBox(L, "rpys", d, 220);
  string cov = fmtInt(long(rpy.dated)) + " of " + fmtInt(long(rpy.refs)) + " references dated (" + fmtNum(100.0 * double(rpy.dated) / double(std::max<long long>(1, rpy.refs)), 0) + "%) \xC2\xB7 " + std::to_string(rpy.y0) + "\xE2\x80\x93" + std::to_string(rpy.y1);
  ui.text(L.row(18 * s), cov, 11.5f * s, ui.c.textDim);
  if (rpy.peaks.empty()) return;
  sectionTitle(L, "Peak years", "Years whose reference count stands out from the 5-year median, with the most cited work of that year.");
  for (auto& pk : rpy.peaks) {
    float th = ui.textWrap({0, 0, L.w - 16 * s, 1000}, pk.topRef, 11.5f * s, ui.c.text, 400, false);
    Rect card = L.row(th + 34 * s);
    ui.fill(card, ui.c.card, 8 * s);
    ui.stroke(card, ui.c.border, 8 * s);
    ui.text({card.x + 10 * s, card.y + 6 * s, 60 * s, 18 * s}, std::to_string(pk.year), 14 * s, ui.c.text, AL_LEFT, 700);
    ui.text({card.x + 60 * s, card.y + 6 * s, card.w - 70 * s, 18 * s}, plural(pk.count, "reference") + " \xC2\xB7 +" + fmtNum(pk.dev, 0) + " above median", 11 * s, ui.c.warn, AL_RIGHT, 600);
    ui.textWrap({card.x + 8 * s, card.y + 27 * s, card.w - 16 * s, th + 4}, pk.topRef + "  (cited " + std::to_string(pk.topRefCount) + "\xC3\x97)", 11.5f * s, ui.c.text);
  }
}

void App::actorsTimeline(Lay& L, Unit u) {
  if (prodUnit != int(u)) { prod = productionOverTime(P->corpus, u, 10); prodUnit = int(u); }
  if (prod.labels.empty() || prod.y1 <= prod.y0) return;
  sectionTitle(L, "Production over time", "Top 10 by documents. Circle size = documents in that year; colour intensity = citations per year received by those documents (as in Bibliometrix).");
  ChartDef d;
  d.title = "Top " + std::to_string(prod.labels.size()) + " over time";
  d.make = [this](double w, double h, const ChartTheme& t) { return chartProduction(prod, w, h, t); };
  d.onClick = [this](int k) { if (k >= 0 && k < int(prod.labels.size())) findInMap(prod.labels[size_t(k)]); };
  chartBox(L, "prod", d, float(prod.labels.size()) * 24 + 46);
}

void App::actorsDocs(Lay& L) {
  float s = ui.s;
  const Corpus& C = P->corpus;
  const CitationSummary& cs = statCiteSummary();
  const CitationIndex& X = citations();
  sectionTitle(L, "Citation overview", "Global citations are the counts reported by the database (Web of Science, Scopus, OpenAlex). Local citations count only citations from other documents of this data set.");
  {
    vector<std::pair<string, string>> kv = {
        {"Documents", fmtInt(cs.docs)}, {"Citations", fmtInt(long(cs.total))}, {"h-index", std::to_string(cs.h)}, {"g-index", std::to_string(cs.g)},
        {"Mean citations", fmtNum(cs.mean, 1)}, {"Median citations", fmtNum(cs.median, 1)},
        {"Uncited", fmtNum(100.0 * cs.uncited / std::max(1, cs.docs), 0) + "%"}, {"Local citation links", fmtInt(X.links)}};
    float rh = 27 * s;
    Rect box = L.row(rh * float(kv.size()));
    ui.fill(box, ui.c.card, 8 * s);
    ui.stroke(box, ui.c.border, 8 * s);
    for (size_t i = 0; i < kv.size(); i++) {
      Rect rr{box.x, box.y + rh * float(i), box.w, rh};
      if (i) ui.line(rr.x + 10 * s, rr.y, rr.r(), rr.y, ui.c.border);
      ui.text({rr.x + 10 * s, rr.y, rr.w * 0.6f, rr.h}, kv[i].first, 12.5f * s, ui.c.text);
      ui.text({rr.x, rr.y, rr.w - 10 * s, rr.h}, kv[i].second, 12.5f * s, ui.c.textDim, AL_RIGHT);
    }
  }
  // most cited
  sectionTitle(L, "Most cited documents", "Click a bar for a preview of the document.");
  ui.segmented(L.row(30 * s), {"Global", "Per year", "Local"}, docRank, "docrank");
  SYSTEMTIME st;
  GetLocalTime(&st);
  int nowY = st.wYear;
  static vector<DocBar> rowsCache;
  static uint64_t rowsKey = 0;
  uint64_t rk = P->corpusVersion * 31 + uint64_t(docRank) * 7 + uint64_t(X.links) * 131 + uint64_t(reinterpret_cast<uintptr_t>(P.get()));
  if (rk != rowsKey || rowsCache.empty()) {
    rowsKey = rk;
    rowsCache.clear();
    vector<DocBar>& rows = rowsCache;
    vector<std::pair<double, int>> v;
    for (size_t i = 0; i < C.recs.size(); i++) {
      const Record& r = C.recs[i];
      double val = docRank == 0 ? r.cites : docRank == 1 ? (r.year ? double(r.cites) / std::max(1, nowY - r.year + 1) : 0.0) : double(i < X.citedBy.size() ? X.citedBy[i].size() : 0);
      if (val > 0) v.push_back({val, int(i)});
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    for (size_t k = 0; k < std::min<size_t>(15, v.size()); k++) {
      const Record& r = C.recs[size_t(v[k].second)];
      rows.push_back({v[k].second, shortCite(r), v[k].first, truncate(r.title, 110) + "\n" + truncate(titleCase(lower(r.source)), 60)});
    }
  }
  const vector<DocBar>& rows = rowsCache;
  if (rows.empty()) ui.text(L.row(22 * s), docRank == 2 ? "No document of this set cites another one." : "No citation counts in these records.", 12 * s, ui.c.textDim);
  else {
    ChartDef d;
    d.title = docRank == 0 ? "Global citations" : docRank == 1 ? "Citations per year" : "Local citations";
    string suffix = docRank == 1 ? "/yr" : "";
    d.make = [rows, suffix](double w, double h, const ChartTheme& t) { return chartDocBars(rows, w, h, t, suffix); };
    d.onClick = [this](int rec) { openDoc(rec); };
    chartBox(L, "topdocs", d, float(rows.size()) * 22 + 34);
  }
  // distribution
  if (!cs.bins.empty()) {
    vector<std::pair<string, double>> b;
    for (auto& x : cs.bins) b.push_back({x.first, double(x.second)});
    ChartDef d;
    d.title = "Citation classes";
    d.make = [b](double w, double h, const ChartTheme& t) { return chartBars(b, "", w, h, t, ""); };
    L.space(4 * s);
    chartBox(L, "citdist", d, float(b.size()) * 22 + 30);
  }
  // historiograph
  sectionTitle(L, "Historiograph", "Direct citations among the most locally cited documents (Garfield's HistCite): documents are placed by publication year, arrows run from the citing to the cited paper, circle size = local citations. Documents are matched by DOI, WoS reference key or title. Click a circle for a preview.");
  if (X.links == 0) {
    float h = ui.textWrap({L.x, L.y, L.w, 200}, "No citations between documents of this data set were found (needs cited references in the records).", 12 * s, ui.c.textDim);
    L.y += h + 8 * s;
    return;
  }
  Rect f = field(ui, L, "Documents shown", 30, 0.58f);
  ui.numberInput(f, "histtop", histTop, 10, 80, 5);
  ChartDef d;
  d.title = "Historiograph \xC2\xB7 top " + std::to_string(histTop);
  d.make = [this](double w, double h, const ChartTheme& t) { return chartHistoriograph(P->corpus, citations(), histTop, w, h, t); };
  d.onClick = [this](int rec) { openDoc(rec); };
  chartBox(L, "hist", d, 360);
  ui.text(L.row(18 * s), fmtInt(X.links) + " citation links inside the data set", 11.5f * s, ui.c.textDim);
}

void App::analyseClusterYears(Lay& L) {
  float s = ui.s;
  if (!hasCorpus() || P->mapSource != "analysis") return;
  if (!cyValid) { cyears = clusterYears(P->net, P->corpus); cyValid = true; }
  if (cyears.docs.empty() || cyears.y1 <= cyears.y0) return;
  sectionTitle(L, "Clusters over time", "Documents per year that contain items of each cluster. Share shows each cluster's part of the yearly activity. Widening bands are growing topics, narrowing bands are fading ones.");
  ui.segmented(L.row(28 * s), {"Documents", "Share"}, cyMode, "cymode");
  ChartDef d;
  d.title = cyMode ? "Cluster share per year" : "Documents per cluster";
  d.make = [this](double w, double h, const ChartTheme& t) {
    if (!cyValid) { cyears = clusterYears(P->net, P->corpus); cyValid = true; }
    vector<string> names;
    for (int c = 0; c < P->net.nClusters; c++) names.push_back(P->net.clusterName(c));
    return chartClusterYears(cyears, names, clusterColors(), cyMode == 1, w, h, t);
  };
  d.onClick = [this](int c) { if (c >= 0 && c < P->net.nClusters) focusCluster(c); };
  chartBox(L, "cy", d, 210);
}

// ---------------------------------------------------------------- cleaning studio
const vector<std::pair<Unit, const char*>>& cleanUnits() {
  static const vector<std::pair<Unit, const char*>> cu = {{Unit::Keywords, "Author keywords"}, {Unit::AllKeywords, "All keywords"}, {Unit::IndexTerms, "Index terms"},
                                                          {Unit::Authors, "Authors"}, {Unit::Sources, "Sources"}, {Unit::Orgs, "Organisations"}};
  return cu;
}

void App::cleanScan() {
  if (!hasCorpus()) return;
  const auto& cu = cleanUnits();
  cleanUnit = clampv(cleanUnit, 0, int(cu.size()) - 1);
  variants = findVariants(P->corpus, cu[size_t(cleanUnit)].first, P->engine.thesaurus);
  variantsScanned = true;
  cleanAiUnit = -1;
  cleanEdit = -1;
  cleanNote.clear();
}

void App::cleanMergeSelected() {
  if (!hasCorpus()) return;
  const auto& cu = cleanUnits();
  Unit u = cu[size_t(clampv(cleanUnit, 0, int(cu.size()) - 1))].first;
  int merged = 0, ignored = 0;
  ThUndo undo{"", P->engine.thesaurus.replace};
  for (auto& v : variants) {
    if (!v.apply) continue;
    if (v.ignore) { P->engine.thesaurus.replace[thesaurusKey(u, v.target)] = ""; ignored++; continue; }
    for (auto& m : v.members) {
      if (thesaurusKey(u, m) == thesaurusKey(u, v.target)) continue;
      P->engine.thesaurus.replace[thesaurusKey(u, m)] = v.target;
      merged++;
    }
    // an edited preferred label also renames the existing term of that spelling
    if (v.targetCount > 0) {
      string k = thesaurusKey(u, v.target);
      auto it = P->engine.thesaurus.replace.find(k);
      if (it != P->engine.thesaurus.replace.end() && it->second.empty()) P->engine.thesaurus.replace.erase(it);
    }
  }
  if (!merged && !ignored) return;
  undo.what = (merged ? "merge of " + plural(merged, "variant") : string()) + (merged && ignored ? ", " : "") + (ignored ? plural(ignored, "ignored term") : string());
  if (undo.what.compare(0, 5, "merge") != 0) undo.what[0] = char(toupper((unsigned char)undo.what[0]));
  thUndo.push_back(std::move(undo));
  if (thUndo.size() > 20) thUndo.erase(thUndo.begin());
  P->corpusChanged();
  P->dirty = true;
  sensSig.clear();
  variants = findVariants(P->corpus, u, P->engine.thesaurus);
  cleanAiUnit = -1;
  cleanEdit = -1;
  cleanNote.clear();
  ui.toast("Thesaurus updated", (merged ? plural(merged, "variant") + " merged" : string()) + (merged && ignored ? ", " : "") + (ignored ? plural(ignored, "term") + " ignored" : string()) + ". Rebuild the map to apply.", 1, 4);
}

void App::cleanUndo() {
  if (thUndo.empty()) return;
  ThUndo u = std::move(thUndo.back());
  thUndo.pop_back();
  P->engine.thesaurus.replace = std::move(u.replace);
  P->corpusChanged();
  P->dirty = true;
  sensSig.clear();
  if (variantsScanned) cleanScan();
  ui.toast("Undone", "Thesaurus restored before the " + u.what + ". Rebuild the map to apply.", 1, 4);
}

const RecordsFlow& App::recordsFlow() {
  if (!hasCorpus()) { flowCache = RecordsFlow(); flowSig.clear(); return flowCache; }
  if (busy()) return flowCache;  // the engine caches belong to the running job
  string spec = P->specSig();
  bool built = !P->builtSig.empty() && P->builtSig == spec;
  bool withItems = built || P->corpus.recs.size() <= 5000 || P->spec.unit != Unit::Terms;
  string sig = std::to_string(P->corpusVersion) + "|" + spec + "|" + P->builtSig + "|" + std::to_string(P->net.n()) + "|" + (withItems ? "1" : "0");
  if (sig != flowSig) { flowCache = P->recordsFlow(withItems); flowSig = sig; }
  return flowCache;
}

}  // namespace win
}  // namespace vs
