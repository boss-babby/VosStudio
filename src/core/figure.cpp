#include "figure.h"
#include "charts.h"

#include <cstdio>

namespace vs {

const vector<FigPreset>& figPresets() {
  static const vector<FigPreset> P = {
      {"single", "Journal \xC2\xB7 single column (90 mm)", 90, 75, 6},
      {"onehalf", "Journal \xC2\xB7 1.5 column (140 mm)", 140, 105, 6.5},
      {"double", "Journal \xC2\xB7 double column (180 mm)", 180, 120, 7},
      {"a4", "A4 landscape", 297, 210, 8},
      {"slide", "Slide 16:9", 254, 143, 10},
      {"poster", "Poster panel (400 mm)", 400, 300, 14},
  };
  return P;
}

// ------------------------------------------------------------ text metrics
static const short HELV[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
                               556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667,
                               778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222,
                               222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
static const short HELVB[95] = {278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
                                556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778, 667,
                                778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611, 611, 278,
                                278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};

static vector<uint32_t> utf8cp(const string& s) {
  vector<uint32_t> o;
  for (size_t i = 0; i < s.size();) {
    uint8_t c = uint8_t(s[i]);
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = c & 31; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 15; n = 3; }
    else { cp = c & 7; n = 4; }
    for (int k = 1; k < n && i + size_t(k) < s.size(); k++) cp = (cp << 6) | (uint8_t(s[i + size_t(k)]) & 63);
    o.push_back(cp);
    i += size_t(n);
  }
  return o;
}

// Unicode -> WinAnsiEncoding byte (0 when unmappable)
static uint8_t winAnsi(uint32_t cp) {
  if (cp < 0x80) return uint8_t(cp);
  if (cp >= 0xA0 && cp <= 0xFF) return uint8_t(cp);
  switch (cp) {
    case 0x20AC: return 0x80; case 0x201A: return 0x82; case 0x0192: return 0x83; case 0x201E: return 0x84; case 0x2026: return 0x85;
    case 0x2020: return 0x86; case 0x2021: return 0x87; case 0x02C6: return 0x88; case 0x2030: return 0x89; case 0x0160: return 0x8A;
    case 0x2039: return 0x8B; case 0x0152: return 0x8C; case 0x017D: return 0x8E; case 0x2018: return 0x91; case 0x2019: return 0x92;
    case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95; case 0x2013: return 0x96; case 0x2014: return 0x97;
    case 0x02DC: return 0x98; case 0x2122: return 0x99; case 0x0161: return 0x9A; case 0x203A: return 0x9B; case 0x0153: return 0x9C;
    case 0x017E: return 0x9E; case 0x0178: return 0x9F; case 0x2212: return '-';
    case 0x2010: case 0x2011: case 0x2012: case 0x2043: return '-';  // hyphens (OpenAlex names use U+2011)
    case 0x2015: return 0x97; case 0x201B: return 0x91; case 0x201F: return 0x93; case 0x2032: return '\''; case 0x2033: return '"'; case 0x2044: return '/';
    case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F: return ' ';
  }
  return 0;
}

static double glyphW(uint32_t cp, bool bold) {
  const short* T = bold ? HELVB : HELV;
  if (cp >= 32 && cp < 127) return T[cp - 32];
  switch (cp) {
    case 0x2013: return 556; case 0x2014: return 1000; case 0x2026: return 1000; case 0xB7: return 278; case 0x2022: return 350;
    case 0x2018: case 0x2019: return bold ? 278 : 222; case 0x201C: case 0x201D: return bold ? 500 : 333;
  }
  if (cp >= 0xC0 && cp <= 0x17F) {
    string u;
    u += char(0xC0 | (cp >> 6));
    u += char(0x80 | (cp & 63));
    string f = asciiFold(u);
    if (!f.empty() && uint8_t(f[0]) >= 32 && uint8_t(f[0]) < 127) return T[uint8_t(f[0]) - 32];
  }
  if (cp >= 0x3000) return 1000;  // CJK approximation
  return 556;
}

double textWidth(const string& s, double size, bool bold) {
  double w = 0;
  for (auto cp : utf8cp(s)) w += glyphW(cp, bold);
  return w / 1000.0 * size;
}

vector<string> wrapText(const string& text, double size, double width, bool bold) {
  vector<string> lines;
  for (auto& para : split(text, '\n', true)) {
    string cur;
    for (auto& w : split(para, ' ')) {
      string t = cur.empty() ? w : cur + " " + w;
      if (!cur.empty() && textWidth(t, size, bold) > width) { lines.push_back(cur); cur = w; }
      else cur = t;
    }
    lines.push_back(cur);
  }
  return lines;
}

// ------------------------------------------------------------ density raster
// average distance between items (world units): exact up to 2000 items, sampled beyond
static double avgItemDistance(const Network& net) {
  int n = net.n();
  if (n < 2) return 0;
  double sum = 0, cnt = 0;
  if (n <= 2000) {
    for (int a = 0; a < n; a++)
      for (int b = a + 1; b < n; b++) { sum += std::hypot(net.nodes[a].x - net.nodes[b].x, net.nodes[a].y - net.nodes[b].y); cnt++; }
  } else {
    Rng rnd(12345);
    for (int k = 0; k < 400000; k++) {
      int a = rnd.below(n), b = rnd.below(n);
      if (a == b) continue;
      sum += std::hypot(net.nodes[a].x - net.nodes[b].x, net.nodes[a].y - net.nodes[b].y); cnt++;
    }
  }
  return cnt > 0 ? sum / cnt : 0;
}

vector<uint8_t> densityRaster(const Network& net, const Encoder& enc, const Theme& th, int pw, int ph, double s, double ox, double oy, double rx,
                              double ry, double kx) {
  vector<float> grid(size_t(pw) * size_t(ph), 0.f), tmp(grid.size());
  const ViewStyle& st = *enc.st;
  int K = std::max(1, net.nClusters);
  bool byCl = st.densityByCluster && st.colorBy == ColorBy::Cluster;
  vector<vector<float>> cl;
  if (byCl) cl.assign(size_t(K), vector<float>(grid.size(), 0.f));
  for (int i = 0; i < net.n(); i++) {
    double x = ((net.nodes[i].x * s + ox) - rx) * kx, y = ((net.nodes[i].y * s + oy) - ry) * kx;
    int xi = int(std::lround(x)), yi = int(std::lround(y));
    if (xi < 0 || yi < 0 || xi >= pw || yi >= ph) continue;
    float v = float(st.kernelAuto ? enc.weightT(i) : 0.25 + 0.75 * std::sqrt(enc.weightT(i)));
    grid[size_t(yi) * pw + xi] += v;
    if (byCl) cl[size_t(std::max(0, net.nodes[i].cluster) % K)][size_t(yi) * pw + xi] += v;
  }
  double sigma = std::max(1.5, 70 * st.kernel * 0.55 * s * kx);
  if (st.kernelAuto) {  // VOSviewer: Gaussian width 0.125 x average item distance (van Eck & Waltman 2010)
    double sw = 0.125 * avgItemDistance(net) / std::sqrt(2.0) * st.kernel;
    if (sw > 0) sigma = std::max(1.0, sw * s * kx);
  }
  int r = std::max(1, int(std::lround(std::sqrt(12 * sigma * sigma / 3 + 1) / 2)));
  auto blur = [&](vector<float>& g) {
    for (int pass = 0; pass < 3; pass++) {
      for (int y = 0; y < ph; y++) {
        double acc = 0;
        size_t row = size_t(y) * pw;
        for (int x = -r; x < pw + r; x++) {
          if (x + r < pw && x + r >= 0) acc += g[row + size_t(x + r)];
          if (x - r - 1 >= 0 && x - r - 1 < pw) acc -= g[row + size_t(x - r - 1)];
          if (x >= 0 && x < pw) tmp[row + size_t(x)] = float(acc / (2 * r + 1));
        }
      }
      for (int x = 0; x < pw; x++) {
        double acc = 0;
        for (int y = -r; y < ph + r; y++) {
          if (y + r < ph && y + r >= 0) acc += tmp[size_t(y + r) * pw + size_t(x)];
          if (y - r - 1 >= 0 && y - r - 1 < ph) acc -= tmp[size_t(y - r - 1) * pw + size_t(x)];
          if (y >= 0 && y < ph) g[size_t(y) * pw + size_t(x)] = float(acc / (2 * r + 1));
        }
      }
    }
  };
  blur(grid);
  if (byCl) for (auto& g : cl) blur(g);
  float mx = 0;
  for (float v : grid) mx = std::max(mx, v);
  double gamma = 1.0 / std::max(0.2, double(st.densityAlpha)) * 0.55;
  vector<uint8_t> px(grid.size() * 4);
  for (size_t i = 0; i < grid.size(); i++) {
    double t = mx > 0 ? std::pow(grid[i] / mx, gamma) : 0;
    Color c;
    if (byCl) {
      double tot = 0; float R = 0, G = 0, B = 0;
      for (int k = 0; k < K; k++) { double v = cl[size_t(k)][i]; if (v <= 0) continue; Color cc = enc.clusterColor(k); R += float(cc.r * v); G += float(cc.g * v); B += float(cc.b * v); tot += v; }
      c = tot > 0 ? Color(float(R / tot), float(G / tot), float(B / tot)) : th.bg;
    } else c = simulateCvd(cmapAt(st.densityScheme, t), st.cvd);
    double fade = st.densityFull ? 1.0 : clampv(t / (byCl ? 0.5 : 0.12), 0.0, 1.0);
    if (byCl) fade = std::min(1.0, t * 1.1);
    px[i * 4] = uint8_t(clampv(th.bg.r + (c.r - th.bg.r) * fade, 0.0, 1.0) * 255 + 0.5);
    px[i * 4 + 1] = uint8_t(clampv(th.bg.g + (c.g - th.bg.g) * fade, 0.0, 1.0) * 255 + 0.5);
    px[i * 4 + 2] = uint8_t(clampv(th.bg.b + (c.b - th.bg.b) * fade, 0.0, 1.0) * 255 + 0.5);
    px[i * 4 + 3] = 255;
  }
  return px;
}

// ------------------------------------------------------------ figure builder
namespace {
struct FigTh { Color bg, fg, muted, halo, stroke; float linkK = 1; bool light = true; };
FigTh figTheme(FigTheme t, const ViewStyle& st) {
  FigTh o;
  if (t == FigTheme::Slide) { o.bg = Color(0.06f, 0.07f, 0.09f); o.fg = Color(0.95f, 0.96f, 0.98f); o.muted = Color(0.64f, 0.67f, 0.73f); o.light = false; }
  else if (t == FigTheme::Screen) { Theme th = canvasTheme(st); o.bg = th.bg; o.fg = th.fg; o.muted = th.muted; o.light = th.light; }
  else { o.bg = Color(1, 1, 1); o.fg = Color(0.1f, 0.1f, 0.11f); o.muted = Color(0.36f, 0.37f, 0.4f); o.linkK = 1.15f; o.light = true; }
  o.halo = o.bg;
  o.stroke = o.bg;
  return o;
}
struct Rc { double x, y, w, h; };
Prim textP(double x, double y, const string& s, double size, const Color& c, int anchor = 0, bool bold = false, const char* g = "text") {
  Prim p; p.type = Prim::Text; p.x = float(x); p.y = float(y); p.text = s; p.size = float(size); p.fill = true; p.fillC = c; p.anchor = anchor; p.bold = bold; p.group = g;
  return p;
}
Prim rectP(double x, double y, double w, double h, const Color& c, const char* g) {
  Prim p; p.type = Prim::Rect; p.x = float(x); p.y = float(y); p.w = float(w); p.h = float(h); p.fill = true; p.fillC = c; p.group = g;
  return p;
}
Prim circleP(double x, double y, double r, const Color& c, const char* g) {
  Prim p; p.type = Prim::Circle; p.x = float(x); p.y = float(y); p.r = float(r); p.fill = true; p.fillC = c; p.group = g;
  return p;
}
}  // namespace

vector<ViewKind> figurePanels(const FigureSpec& spec) {
  vector<ViewKind> v;
  if (spec.panelNetwork) v.push_back(ViewKind::Network);
  if (spec.panelOverlay) v.push_back(ViewKind::Overlay);
  if (spec.panelDensity) v.push_back(ViewKind::Density);
  if (spec.panelTimeline) v.push_back(ViewKind::Timeline);
  if (spec.panelGeo) v.push_back(ViewKind::Geo);
  if (spec.panel3D) v.push_back(ViewKind::ThreeD);
  if (spec.panelMatrix) v.push_back(ViewKind::Matrix);
  if (v.empty()) v.push_back(ViewKind::Network);
  return v;
}

namespace {
// Timeline panel: x = average publication year (as in the Timeline view), y = layout; a year axis underneath
FigPanelDef timelinePanel(const Network& net, double fs, const FigTh& th) {
  FigPanelDef d;
  d.kind = ViewKind::Timeline;
  int si = net.scoreIndex("Avg. pub. year");
  if (si < 0) si = net.scoreIdx;
  double s0 = 1e18, s1 = -1e18, x0 = 1e18, x1 = -1e18;
  for (int i = 0; i < net.n(); i++) {
    x0 = std::min(x0, net.nodes[size_t(i)].x); x1 = std::max(x1, net.nodes[size_t(i)].x);
    double v = si >= 0 && size_t(si) < net.nodes[size_t(i)].sc.size() ? net.nodes[size_t(i)].sc[size_t(si)] : NAN;
    if (std::isfinite(v)) { s0 = std::min(s0, v); s1 = std::max(s1, v); }
  }
  if (!(s1 > s0)) { d.note = "The Timeline panel needs a year score (maps built from bibliographic records have one)"; return d; }
  double span = std::max(200.0, (x1 - x0) * 1.25), X0 = -span / 2, X1 = span / 2;
  d.pos.resize(size_t(net.n()));
  for (int i = 0; i < net.n(); i++) {
    double v = si >= 0 && size_t(si) < net.nodes[size_t(i)].sc.size() ? net.nodes[size_t(i)].sc[size_t(si)] : NAN;
    d.pos[size_t(i)] = {std::isfinite(v) ? X0 + (v - s0) / (s1 - s0) * span : net.nodes[size_t(i)].x, net.nodes[size_t(i)].y};
  }
  d.bandBottom = fs * 2.6;
  d.over = [s0, s1, X0, X1](Scene& sc, const FigPanelCtx& c) {
    double y = c.ry + c.rh - c.fs * 2.0;
    Prim ax; ax.type = Prim::Path; ax.group = "axis"; ax.stroke = true; ax.strokeC = c.muted.withA(0.6f); ax.sw = 0.5f;
    ax.d = {{'M', float(c.rx + 2), float(y), 0, 0}, {'L', float(c.rx + c.rw - 2), float(y), 0, 0}};
    sc.items.push_back(ax);
    int y0 = int(std::floor(s0)), y1 = int(std::ceil(s1));
    int step = std::max(1, int(std::ceil((y1 - y0) / std::max(1.0, c.rw / (c.fs * 4.5)))));
    for (int yr = y0; yr <= y1; yr += step) {
      double wx = X0 + (yr - s0) / (s1 - s0) * (X1 - X0), x = wx * c.s + c.ox;
      if (x < c.rx + 4 || x > c.rx + c.rw - 4) continue;
      Prim t = ax; t.d = {{'M', float(x), float(y - 2), 0, 0}, {'L', float(x), float(y + 2), 0, 0}};
      sc.items.push_back(t);
      Prim gl = ax; gl.strokeC = c.muted.withA(0.15f); gl.sw = 0.35f; gl.dash = {1.5f, 2.f};
      gl.d = {{'M', float(x), float(c.ry), 0, 0}, {'L', float(x), float(y - 3), 0, 0}};
      sc.items.push_back(gl);
      sc.items.push_back(textP(x, y + c.fs * 1.25, std::to_string(yr), c.fs * 0.9, c.muted, 1, false, "axis"));
    }
    sc.items.push_back(textP(c.rx + c.rw - 2, y - 3, "Avg. publication year \xE2\x86\x92", c.fs * 0.85, c.muted, 2, false, "axis"));
  };
  (void)th;
  return d;
}
}  // namespace

Scene buildFigure(const Network& net, const ViewStyle& style, const FigureSpec& spec, const Bundles* bundles, const string& methodsShort, const vector<FigPanelDef>* panelDefs) {
  Scene sc;
  sc.W = spec.wmm * MM2PT;
  sc.H = spec.hmm * MM2PT;
  FigTh th = figTheme(spec.theme, style);
  ViewStyle st = style;
  Encoder enc;
  enc.prepare(net, st);
  const double pad = 6, fs = spec.labelPt;
  if (!spec.transparent) sc.items.push_back(rectP(0, 0, sc.W, sc.H, th.bg, "background"));
  double top = pad, bottom = sc.H - pad;
  if (!spec.title.empty()) { sc.items.push_back(textP(pad, top + fs + 2.5, spec.title, fs + 3, th.fg, 0, true)); top += fs + 3 + 7; }
  double capW = sc.W - 2 * pad;
  auto capLines = spec.caption.empty() ? vector<string>{} : wrapText(spec.caption, fs, capW);
  double footFs = std::max(5.0, fs - 1.5);
  auto footLines = (spec.footer && !methodsShort.empty()) ? wrapText(methodsShort, footFs, capW) : vector<string>{};
  bottom -= double(capLines.size()) * fs * 1.3 + double(footLines.size()) * footFs * 1.3 + (capLines.empty() && footLines.empty() ? 0 : 4);

  vector<ViewKind> panels = figurePanels(spec);
  size_t np = panels.size();
  // per-panel geometry: supplied by the caller (Geo, 3D) or built here (Timeline); Network/Overlay/Density use the layout
  vector<FigPanelDef> defs(np);
  for (size_t k = 0; k < np; k++) {
    bool found = false;
    if (panelDefs)
      for (auto& d : *panelDefs) if (d.kind == panels[k]) { defs[k] = d; found = true; break; }
    if (!found && panels[k] == ViewKind::Timeline) defs[k] = timelinePanel(net, fs, th);
    if (!found && (panels[k] == ViewKind::Geo || panels[k] == ViewKind::ThreeD)) defs[k].note = panels[k] == ViewKind::Geo ? "No country data for a Geo panel" : "";
    defs[k].kind = panels[k];
  }
  auto colourByScore = [&](ViewKind k) { return k == ViewKind::Overlay || k == ViewKind::Timeline; };
  bool clusterLegend = spec.legend && st.colorBy == ColorBy::Cluster && net.nClusters > 0 && [&] {
    for (size_t k = 0; k < np; k++) if (defs[k].clusterLegend && !colourByScore(panels[k])) return true;
    return false;
  }();
  bool hasBar = spec.colorbar && [&] {
    for (size_t k = 0; k < np; k++) if (defs[k].nodes && panels[k] != ViewKind::Matrix && (colourByScore(panels[k]) || st.colorBy == ColorBy::Score)) return true;
    return false;
  }();
  bool hasDenBar = spec.colorbar && std::count(panels.begin(), panels.end(), ViewKind::Density) > 0 && !st.densityByCluster;
  // cluster legend layout
  struct LegE { string s; int c; };
  vector<LegE> leg;
  double legRowH = fs * 1.5, legEntryW = 0;
  int legCols = 1, legRows = 0;
  if (clusterLegend) {
    vector<int> sizes(size_t(net.nClusters), 0);
    for (auto& nd : net.nodes) if (nd.cluster >= 0 && nd.cluster < net.nClusters) sizes[nd.cluster]++;
    for (int c = 0; c < net.nClusters; c++) {
      if (!sizes[c]) continue;
      string nm = net.clusterName(c);
      leg.push_back({truncate(nm, 40) + "  (" + std::to_string(sizes[c]) + ")", c});
    }
    for (auto& e : leg) legEntryW = std::max(legEntryW, textWidth(e.s, fs, false) + fs * 1.6 + 10);
    legCols = std::max(1, int(capW / std::max(1.0, legEntryW)));
    legCols = std::min(legCols, int(leg.size()));
    legRows = int((leg.size() + size_t(legCols) - 1) / size_t(legCols));
  }
  // per-panel networks (positions of the view) and world bounds
  vector<Network> own(np);
  vector<const Network*> nets(np, &net);
  for (size_t k = 0; k < np; k++)
    if (defs[k].pos.size() == size_t(net.n()) && net.n() > 0) {
      own[k] = net;
      for (int i = 0; i < net.n(); i++) { own[k].nodes[size_t(i)].x = defs[k].pos[size_t(i)][0]; own[k].nodes[size_t(i)].y = defs[k].pos[size_t(i)][1]; }
      nets[k] = &own[k];
    }
  struct Bx { double x0, y0, x1, y1; bool fixed; };
  vector<Bx> bxs(np);
  for (size_t k = 0; k < np; k++) {
    const FigPanelDef& D = defs[k];
    const Network& N = *nets[k];
    if (D.bounds) { bxs[k] = {D.bx0, D.by0, D.bx1, D.by1, true}; continue; }
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (int i = 0; i < N.n(); i++) {
      if (!D.hidden.empty() && D.hidden[size_t(i)]) continue;
      double r = enc.vos() ? 0 : enc.radius(i) * (D.rMul.empty() ? 1.0 : double(D.rMul[size_t(i)]));
      x0 = std::min(x0, N.nodes[i].x - r); x1 = std::max(x1, N.nodes[i].x + r);
      y0 = std::min(y0, N.nodes[i].y - r); y1 = std::max(y1, N.nodes[i].y + r);
    }
    if (x0 > x1) { x0 = y0 = 0; x1 = y1 = 1; }
    bxs[k] = {x0, y0, x1, y1, false};
  }
  // grid of panels: side by side on landscape figures, stacked on portrait ones, two columns beyond three panels
  int ncol = np <= 1 ? 1 : (sc.W >= sc.H * 1.15 ? (np <= 3 ? int(np) : int((np + 1) / 2)) : (np <= 3 ? 1 : 2));
  int nrow = int((np + size_t(ncol) - 1) / size_t(ncol));
  auto panelRegions = [&](double bot) {
    vector<Rc> R;
    double gap = 6, w = (capW - gap * (ncol - 1)) / ncol, h = (bot - top - gap * (nrow - 1)) / nrow;
    for (size_t k = 0; k < np; k++) R.push_back({pad + double(k % size_t(ncol)) * (w + gap), top + double(k / size_t(ncol)) * (h + gap), w, h});
    return R;
  };
  auto scaleFor = [&](const Rc& r, const Bx& b) {
    double px = r.w * (b.fixed ? 0.01 : 0.07), py = r.h * (b.fixed ? 0.01 : 0.05);
    return std::min((r.w - 2 * px) / std::max(1e-9, b.x1 - b.x0), (r.h - 2 * py) / std::max(1e-9, b.y1 - b.y0));
  };
  double legH = 0;
  if (clusterLegend) legH += legRows * legRowH + 4;
  Rc r0 = panelRegions(bottom - legH)[0];
  r0.h -= defs[0].bandBottom;
  double sEst = scaleFor(r0, bxs[0]);
  // VOSviewer sizing: screen pixels map to points with pxK = label pt / 9 (the smallest VOS label is 9 px)
  const bool vosF = enc.vos();
  const double pxK = fs / 9.0;
  double vosMaxR = 0;
  if (vosF) for (int i = 0; i < net.n(); i++) vosMaxR = std::max(vosMaxR, enc.vosRadiusPx(i) * pxK);
  double sizeR = vosF ? vosMaxR : st.maxSize * st.scale * sEst;
  bool sizeLeg = spec.sizeLegend && net.n() > 0 && std::any_of(defs.begin(), defs.end(), [](const FigPanelDef& d) { return d.nodes && d.kind != ViewKind::Matrix && d.kind != ViewKind::Density; });
  double band = 0;
  if (hasBar || hasDenBar || sizeLeg) band = std::max(fs * 3.4, sizeLeg ? std::min(2 * sizeR, 36.0) + fs * 1.8 : 0.0) + 4;
  legH += band;
  auto regions = panelRegions(bottom - legH);

  // ---- panels
  double lastScale = 1;
  const char* letters = "abcdefghij";
  for (size_t k = 0; k < np; k++) {
    ViewKind kind = panels[k];
    const FigPanelDef& D = defs[k];
    const Network& N = *nets[k];
    const bool moved = nets[k] != &net;  // positions differ from the layout
    Rc cell = regions[k];
    Rc region{cell.x, cell.y, cell.w, cell.h - D.bandBottom};
    const Bx& bb = bxs[k];
    double s = scaleFor(region, bb);
    if (k == 0) lastScale = s;
    double ox = region.x + region.w / 2 - (bb.x0 + bb.x1) / 2 * s, oy = region.y + region.h / 2 - (bb.y0 + bb.y1) / 2 * s;
    auto P = [&](double x, double y) { return std::make_pair(x * s + ox, y * s + oy); };
    auto hid = [&](int i) { return !D.hidden.empty() && D.hidden[size_t(i)]; };
    const ViewKind ck = D.colorAs >= 0 ? ViewKind(D.colorAs) : kind;
    auto rm = [&](int i) { return D.rMul.empty() ? 1.0 : double(D.rMul[size_t(i)]); };
    FigPanelCtx ctx;
    ctx.s = s; ctx.ox = ox; ctx.oy = oy; ctx.rx = cell.x; ctx.ry = cell.y; ctx.rw = cell.w; ctx.rh = cell.h;
    ctx.bg = th.bg; ctx.fg = th.fg; ctx.muted = th.muted; ctx.light = th.light; ctx.fs = fs;
    if (D.under) D.under(sc, ctx);
    vector<Rc> rects;
    vector<uint8_t> denPx;
    int denW = 0, denH = 0;
    double denKx = 1, denRx = 0, denRy = 0;
    auto nodeR = [&](int i) { return vosF ? std::max(0.4, enc.vosRadiusPx(i) * pxK * rm(i)) : std::max(0.6, enc.radius(i) * s * rm(i)); };
    if (kind == ViewKind::Matrix) {  // the density matrix chart, fitted into the panel
      vector<Color> cc;
      for (int c = 0; c < N.nClusters; c++) cc.push_back(enc.clusterColor(c));
      ChartTheme ct = ChartTheme::make(th.light);
      ct.bg = th.bg; ct.fg = th.fg; ct.muted = th.muted; ct.font = float(std::max(5.0, fs * 0.9)); ct.transparent = true;
      double top = spec.letters && np > 1 ? fs + 6 : 0;  // keep the panel letter clear of the chart title
      Scene m = chartDensityMatrix(N, cc, cell.w, cell.h - top, ct);
      float ox = float(cell.x), oy = float(cell.y + top);
      for (auto& q : m.items) {
        if (q.group == "background") continue;
        q.x += ox; q.y += oy;
        for (auto& d : q.d) { d.x1 += ox; d.y1 += oy; d.x2 += ox; d.y2 += oy; }
        sc.items.push_back(std::move(q));
      }
      if (spec.letters && np > 1) sc.items.push_back(textP(cell.x + 1, cell.y + fs + 2, string(1, letters[k]), fs + 2.5, th.fg, 0, true, "text"));
      continue;
    }
    if (kind == ViewKind::Density && !moved) {
      int pw = std::max(16, int(region.w / 72 * std::min(spec.dpi, 200.0))), ph = std::max(16, int(region.h / 72 * std::min(spec.dpi, 200.0)));
      double kx = pw / region.w;
      Theme tt; tt.bg = th.bg;
      Prim im; im.type = Prim::Image; im.x = float(region.x); im.y = float(region.y); im.w = float(region.w); im.h = float(region.h);
      im.imgW = pw; im.imgH = ph; im.rgba = densityRaster(net, enc, tt, pw, ph, s, ox, oy, region.x, region.y, kx); im.group = "density";
      denPx = im.rgba; denW = pw; denH = ph; denKx = kx; denRx = region.x; denRy = region.y;
      sc.items.push_back(std::move(im));
    } else {
      if (st.hulls && kind == ViewKind::Network && !moved) {
        for (int c = 0; c < N.nClusters; c++) {
          vector<P2> pts;
          for (int i = 0; i < N.n(); i++) if (N.nodes[i].cluster == c) { double r = vosF ? (nodeR(i) + 4) / std::max(1e-9, s) : enc.radius(i) + 14; for (int a = 0; a < 8; a++) pts.push_back({N.nodes[i].x + r * std::cos(a * M_PI / 4), N.nodes[i].y + r * std::sin(a * M_PI / 4)}); }
          if (pts.size() < 24) continue;
          auto hull = convexHull(pts);
          if (hull.size() < 3) continue;
          Color col = enc.clusterColor(c);
          Prim p; p.type = Prim::Path; p.group = "hulls";
          size_t n = hull.size();
          auto mid = [&](size_t a, size_t b) { return P((hull[a].x + hull[b].x) / 2, (hull[a].y + hull[b].y) / 2); };
          auto m0 = mid(n - 1, 0);
          p.d.push_back({'M', float(m0.first), float(m0.second), 0, 0});
          for (size_t i = 0; i < n; i++) { auto q = P(hull[i].x, hull[i].y); auto m = mid(i, (i + 1) % n); p.d.push_back({'Q', float(q.first), float(q.second), float(m.first), float(m.second)}); }
          p.d.push_back({'Z', 0, 0, 0, 0});
          p.fill = true; p.fillC = col.withA(0.08f); p.stroke = true; p.strokeC = col.withA(0.35f); p.sw = 0.5f; p.dash = {2, 2};
          sc.items.push_back(std::move(p));
        }
      }
      if (st.linksVisible && D.links) {
        bool bundled = !moved && st.bundle && bundles && bundles->valid() && bundles->pts.size() == size_t(N.m()) * size_t((bundles->P + 2) * 2);
        for (int li : enc.linkOrder) {
          const Link& l = N.links[li];
          if (hid(l.a) || hid(l.b)) continue;
          float t = enc.linkWidthT(li);
          double sw = vosF ? enc.vosLinkPx(li) * pxK * 0.6 : 0.25 + t * std::max(0.0, spec.linkMaxPt - 0.25);
          if (vosF && sw < 0.25) sw = 0.25;
          Prim p; p.type = Prim::Path; p.group = "links"; p.stroke = true; p.roundCap = true; p.sw = float(sw);
          float la = clampv(enc.linkAlpha(li) * th.linkK, 0.06f, 1.f);
          if (vosF) { double raw = enc.vosLinkPx(li) * pxK * 0.6; if (raw < 0.25) la *= float(std::max(0.15, raw / 0.25)); }  // hairlines fade instead of vanishing
          p.strokeC = enc.linkColorOf(li, ck).withA(la);
          const bool vosGrad = vosF && !bundled && st.linkColor == LinkColor::Cluster && ck != ViewKind::Overlay;
          auto A = P(N.nodes[l.a].x, N.nodes[l.a].y), B = P(N.nodes[l.b].x, N.nodes[l.b].y);
          if (bundled) {
            int stride = (bundles->P + 2) * 2, cnt = bundles->P + 2;
            const float* q = &bundles->pts[size_t(li) * stride];
            vector<std::pair<double, double>> pp;
            for (int j = 0; j < cnt; j++) pp.push_back(P(q[j * 2], q[j * 2 + 1]));
            p.d.push_back({'M', float(pp[0].first), float(pp[0].second), 0, 0});
            for (int j = 1; j < cnt - 1; j++) {
              bool last = j == cnt - 2;
              double mx = last ? pp[j + 1].first : (pp[j].first + pp[j + 1].first) / 2, my = last ? pp[j + 1].second : (pp[j].second + pp[j + 1].second) / 2;
              p.d.push_back({'Q', float(pp[j].first), float(pp[j].second), float(mx), float(my)});
            }
          } else {
            double bend = st.linkGeom == LinkGeom::Straight ? 0 : (st.linkGeom == LinkGeom::Arc ? 1 : st.curvature);
            p.d.push_back({'M', float(A.first), float(A.second), 0, 0});
            double mx = (A.first + B.first) / 2, my = (A.second + B.second) / 2;
            if (bend > 0.001 && size_t(li) < D.linkCtrl.size()) {  // 3D panel: the projected world-space control point
              auto C = P(D.linkCtrl[size_t(li)][0], D.linkCtrl[size_t(li)][1]);
              mx = C.first; my = C.second;
            } else if (bend > 0.001) {
              double dx = B.first - A.first, dy = B.second - A.second, len = std::max(1e-9, std::hypot(dx, dy)), off = bend * len * 0.22;
              mx += dy / len * off * 2; my -= dx / len * off * 2;
            }
            if (vosGrad) {
              // VOSviewer link colour: gradient between the two (lightened) endpoint colours, as 4 butt-capped pieces
              Color ea = enc.linkEnd(li, ck, false), eb = enc.linkEnd(li, ck, true);
              auto qp = [&](double t) { double u = 1 - t; return std::make_pair(u * u * A.first + 2 * u * t * mx + t * t * B.first, u * u * A.second + 2 * u * t * my + t * t * B.second); };
              const int pieces = 4, sub = 4;
              for (int k2 = 0; k2 < pieces; k2++) {
                Prim q = p;
                q.d.clear(); q.roundCap = false;
                q.strokeC = ea.mix(eb, (k2 + 0.5f) / pieces).withA(la);
                for (int j = 0; j <= sub; j++) {
                  auto pt = qp((k2 + double(j) / sub) / pieces);
                  q.d.push_back({j ? 'L' : 'M', float(pt.first), float(pt.second), 0, 0});
                }
                sc.items.push_back(std::move(q));
              }
              sc.minLine = std::min(sc.minLine, sw);
              continue;
            }
            if (bend > 0.001) p.d.push_back({'Q', float(mx), float(my), float(B.first), float(B.second)});
            else p.d.push_back({'L', float(B.first), float(B.second), 0, 0});
          }
          sc.minLine = std::min(sc.minLine, sw);
          sc.items.push_back(std::move(p));
        }
      }
      vector<int> ord = D.order;
      if (ord.size() != size_t(N.n())) {
        ord.assign(static_cast<size_t>(N.n()), 0);
        for (int i = 0; i < N.n(); i++) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](int a, int b) { return enc.radius(a) > enc.radius(b); });
      }
      for (int i : ord) {
        if (!D.nodes || hid(i)) continue;
        auto q = P(N.nodes[i].x, N.nodes[i].y);
        double r = nodeR(i);
        Color c = enc.nodeColor(i, ck);
        if (!D.fade.empty() && D.fade[size_t(i)] > 0) c = c.mix(th.bg, D.fade[size_t(i)]);
        Prim p = circleP(q.first, q.second, r, c.withA(st.flat ? st.nodeOpacity : std::max(0.85f, st.nodeOpacity)), "nodes");
        p.sphere = spec.shading == 2 || (spec.shading == 0 && !st.flat);
        if (st.borderCol == BorderCol::None || st.border <= 0) {
          if (!st.flat) { p.stroke = true; p.strokeC = th.stroke.withA(0.9f); p.sw = float(std::min(0.4, r * 0.18)); }
        } else {
          p.stroke = true;
          p.strokeC = st.borderCol == BorderCol::Cluster ? c.mix(Color(0, 0, 0), 0.35f) : (st.borderCol == BorderCol::Custom ? st.borderCustom : th.stroke);
          p.sw = float(std::min(0.5, r * 0.2) * std::max(0.4f, st.border));
        }
        sc.items.push_back(std::move(p));
      }
    }
    // cluster names (decoration)
    if (st.clusterNames && D.clusterNames && D.nodes && !colourByScore(kind) && N.nClusters > 0) {
      vector<double> cx(size_t(N.nClusters), 0), cy(size_t(N.nClusters), 0), cw(size_t(N.nClusters), 0);
      vector<int> cn(size_t(N.nClusters), 0);
      for (int i = 0; i < N.n(); i++) { int c = N.nodes[i].cluster; if (c < 0 || hid(i)) continue; double w = 1 + std::sqrt(N.weight(i)); cx[c] += N.nodes[i].x * w; cy[c] += N.nodes[i].y * w; cw[c] += w; cn[c]++; }
      int maxN = std::max(1, *std::max_element(cn.begin(), cn.end()));
      for (int c = 0; c < std::min(12, N.nClusters); c++) {
        if (cn[c] < 3) continue;
        auto q = P(cx[c] / cw[c], cy[c] / cw[c]);
        double size = fs * (1.25 + 0.5 * std::sqrt(double(cn[c]) / maxN));
        Color col = enc.clusterColor(c);
        col = th.light ? col.mix(Color(0, 0, 0), 0.4f) : col.mix(Color(1, 1, 1), 0.45f);
        string s2 = truncate(N.clusterName(c), 36);
        double w = textWidth(s2, size, true);
        if (w > region.w * 0.9) { size *= region.w * 0.9 / w; w = textWidth(s2, size, true); }
        q.first = clampv(q.first, region.x + w / 2 + 2, region.x + region.w - w / 2 - 2);
        q.second = clampv(q.second, region.y + size, region.y + region.h - size);
        Prim p = textP(q.first, q.second + fs * 0.5, s2, size, col.withA(0.9f), 1, true, "clusterNames");
        p.halo = true; p.haloC = th.halo; p.haloW = float(fs * 0.35);
        rects.push_back({q.first - w / 2, q.second + fs * 0.5 - size, w, size * 1.2});
        sc.items.push_back(std::move(p));
      }
    }
    // labels: collision placement in point space, heaviest first
    vector<int> order(static_cast<size_t>(N.n()));
    for (int i = 0; i < N.n(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return N.weight(a) > N.weight(b); });
    int placed = 0;
    for (int i : order) {
      if (!D.labels) break;  // labels do not need circles: the Geo density layer labels the countries under the density
      if (hid(i)) continue;
      if (st.maxLabels > 0 && placed >= st.maxLabels) break;
      auto q = P(N.nodes[i].x, N.nodes[i].y);
      double x = q.first, y = q.second;
      double r = kind == ViewKind::Density || !D.nodes ? 0 : nodeR(i);
      double size = vosF ? enc.vosLabelPx(i) * pxK : fs * enc.labelScale(i) * st.labelSize;
      string text = enc.label(i);
      double tw = textWidth(text, size, st.labelWeight >= 600), thh = size * 1.18, gap = r + size * 0.3;
      struct Cand { double x, y; int a; double rx, ry; };
      vector<Cand> cands;
      bool centreL = st.labelPlace == LabelPlace::Centre || kind == ViewKind::Density || !D.nodes;
      if (centreL) cands.push_back({x, y + size * 0.35, 1, x - tw / 2, y - thh / 2});
      if (!(centreL && vosF)) {  // VOSviewer: labels sit on their circle or are left out
        cands.push_back({x + gap, y + size * 0.35, 0, x + gap, y - thh / 2});
        cands.push_back({x - gap, y + size * 0.35, 2, x - gap - tw, y - thh / 2});
        cands.push_back({x, y - gap - size * 0.25, 1, x - tw / 2, y - gap - thh});
        cands.push_back({x, y + gap + size * 0.85, 1, x - tw / 2, y + gap});
      }
      const Cand* pick = nullptr;
      Rc pr{};
      for (auto& c : cands) {
        Rc R{c.rx - 0.6, c.ry, tw + 1.2, thh};
        if (R.x < region.x || R.y < region.y || R.x + R.w > region.x + region.w || R.y + R.h > region.y + region.h) continue;
        bool hit = false;
        for (auto& o : rects) if (R.x < o.x + o.w && R.x + R.w > o.x && R.y < o.y + o.h && R.y + R.h > o.y) { hit = true; break; }
        if (!hit) { pick = &c; pr = R; break; }
      }
      sc.candidates++;
      if (!pick) continue;
      rects.push_back(pr);
      placed++;
      Color col = th.fg;
      if (st.labelByCluster) { Color cc = enc.clusterColor(N.nodes[i].cluster); col = th.light ? cc.mix(Color(0, 0, 0), 0.3f) : cc.mix(Color(1, 1, 1), 0.15f); }
      if (vosF && !st.labelByCluster) col = th.light ? Color(0, 0, 0, 0.8f) : Color(1, 1, 1, 0.8f);
      if (kind == ViewKind::Density && th.light) col = Color(0.08f, 0.08f, 0.1f);
      if (kind == ViewKind::Density && !denPx.empty()) {  // black or white text depending on the density colour below
        int px = clampv(int((x - denRx) * denKx), 0, denW - 1), py = clampv(int((y - denRy) * denKx), 0, denH - 1);
        size_t o = (size_t(py) * size_t(denW) + size_t(px)) * 4;
        float lum = (0.2126f * denPx[o] + 0.7152f * denPx[o + 1] + 0.0722f * denPx[o + 2]) / 255.f;
        col = lum > 0.45f ? Color(0.05f, 0.05f, 0.07f) : Color(0.97f, 0.98f, 1.f);
      }
      Prim p = textP(pick->x, pick->y, text, size, col, pick->a, st.labelWeight >= 600, "labels");
      if (st.labelHalo && !(kind == ViewKind::Density && st.densityFull)) { p.halo = true; p.haloC = kind == ViewKind::Density ? Color(1, 1, 1, 0.55f) : th.halo; p.haloW = float(size * 0.22); }
      sc.minLabel = std::min(sc.minLabel, size);
      sc.items.push_back(std::move(p));
    }
    sc.labels += placed;
    if (D.over) D.over(sc, ctx);
    if (!D.note.empty()) sc.items.push_back(textP(cell.x + cell.w / 2, cell.y + cell.h / 2, D.note, fs, th.muted, 1, false, "text"));
    if (spec.letters && np > 1) sc.items.push_back(textP(cell.x + 1, cell.y + fs + 2, string(1, letters[k]), fs + 2.5, th.fg, 0, true, "text"));
  }

  // ---- legends
  double ly = bottom - legH + 2;
  if (clusterLegend) {
    double colW = capW / legCols;
    for (size_t e = 0; e < leg.size(); e++) {
      int col = int(e) / legRows, row = int(e) % legRows;
      double x = pad + col * colW, y = ly + row * legRowH + fs * 0.75;
      sc.items.push_back(circleP(x + fs * 0.45, y, fs * 0.42, enc.clusterColor(leg[e].c), "legend"));
      sc.items.push_back(textP(x + fs * 1.3, y + fs * 0.35, leg[e].s, fs, th.fg, 0, false, "legend"));
    }
    ly += legRows * legRowH + 4;
  }
  double bx = pad;
  auto colorbar = [&](const string& title, const string& lo, const string& hi, bool density) {
    double bw = std::min(140.0, capW * 0.32), bh = fs * 0.8, by = ly + fs * 1.1;
    sc.items.push_back(textP(bx, ly + fs * 0.8, title, fs * 0.95, th.fg, 0, true, "legend"));
    int steps = 48;
    for (int k = 0; k < steps; k++) {
      double t = (k + 0.5) / steps;
      Color c = density ? simulateCvd(cmapAt(st.densityScheme, t), st.cvd) : simulateCvd(cmapAt(st.scheme, t), st.cvd);
      sc.items.push_back(rectP(bx + bw * k / steps, by, bw / steps + 0.3, bh, c, "legend"));
    }
    sc.items.push_back(textP(bx, by + bh + fs * 1.05, lo, fs * 0.9, th.muted, 0, false, "legend"));
    sc.items.push_back(textP(bx + bw, by + bh + fs * 1.05, hi, fs * 0.9, th.muted, 2, false, "legend"));
    bx += bw + 18;
  };
  if (band > 0) {
    if (hasBar) {
      string nm = net.scoreIdx >= 0 && net.scoreIdx < int(net.scoreNames.size()) ? net.scoreNames[net.scoreIdx] : "Score";
      bool yr = icontains(nm, "year");
      colorbar(nm, yr ? fmtFixed(enc.sMin, 1) : fmtNum(enc.sMin, 2), yr ? fmtFixed(enc.sMax, 1) : fmtNum(enc.sMax, 2), false);
    }
    if (hasDenBar) colorbar("Item density", "low", "high", true);
    if (sizeLeg) {
      string wn = net.weightIdx >= 0 && net.weightIdx < int(net.weightNames.size()) ? net.weightNames[net.weightIdx] : "Weight";
      sc.items.push_back(textP(bx, ly + fs * 0.8, "Circle size: " + lower(wn), fs * 0.95, th.fg, 0, true, "legend"));
      // legend values: the maximum, a round mid value and the (round) minimum actually present in the map
      double wMin = 1e18;
      for (int i = 0; i < net.n(); i++) wMin = std::min(wMin, net.weight(i));
      if (!(wMin < 1e17)) wMin = enc.maxW / 16;
      auto nice = [](double v) {
        if (v <= 0) return v;
        double e = std::pow(10.0, std::floor(std::log10(v)));
        double m = v / e;
        double r = m < 1.5 ? 1 : (m < 3.5 ? 2 : (m < 7.5 ? 5 : 10));
        return r * e;
      };
      vector<double> fr;
      fr.push_back(1.0);
      double mid = nice(std::sqrt(std::max(1e-9, wMin) * enc.maxW));
      double lo = wMin;
      if (mid < enc.maxW * 0.8 && mid > lo * 1.3) fr.push_back(mid / enc.maxW);
      if (lo < enc.maxW * 0.8) fr.push_back(lo / enc.maxW);
      double cxp = bx;
      double baseY = ly + fs * 1.3 + std::min(2 * sizeR, 36.0);
      double sScale = std::min(1.0, 18.0 / std::max(1e-9, sizeR));
      for (double f : fr) {
        double w = enc.maxW * f;
        double t = f;
        double rr = (st.baseSize + (st.maxSize - st.baseSize) * std::pow(t, std::max(0.05, double(st.labelVar)))) * st.scale * lastScale * sScale;
        if (vosF) rr = st.scale * std::max(16 * std::pow(w / std::max(1e-9, enc.meanW), double(st.sizeVar)), 5.0) / 2 * pxK * sScale;
        rr = std::max(0.8, rr);
        Prim c = circleP(cxp + rr, baseY - rr, rr, Color(0, 0, 0, 0), "legend");
        c.fill = false; c.stroke = true; c.strokeC = th.muted; c.sw = 0.5f;
        sc.items.push_back(c);
        string ws = (w >= 100 || std::fabs(w - std::round(w)) < 1e-6) ? fmtInt((long long)std::lround(w)) : fmtNum(w, w < 10 ? 1 : 0);
        sc.items.push_back(textP(cxp + rr, baseY + fs * 0.95, ws, fs * 0.85, th.muted, 1, false, "legend"));
        cxp += 2 * rr + std::max(10.0, textWidth(ws, fs * 0.85, false)) + 4;
      }
      if (sScale < 0.999) sc.warnings.push_back("Size legend circles are drawn at reduced scale.");
    }
  }
  double cy = sc.H - pad - double(footLines.size()) * footFs * 1.3 - double(capLines.size()) * fs * 1.3;
  for (auto& l : capLines) { cy += fs * 1.3; sc.items.push_back(textP(pad, cy - fs * 0.3, l, fs, th.fg, 0, false, "caption")); }
  for (auto& l : footLines) { cy += footFs * 1.3; sc.items.push_back(textP(pad, cy - footFs * 0.3, l, footFs, th.muted, 0, false, "caption")); }
  if (sc.minLabel < 5) sc.warnings.push_back("Some labels are smaller than 5 pt. Most journals require at least 5\xE2\x80\x93" "6 pt.");
  if (sc.minLine < 0.25) sc.warnings.push_back("Some lines are thinner than 0.25 pt and may disappear in print.");
  if (sc.labels < std::min(10, net.n())) sc.warnings.push_back("Only " + std::to_string(sc.labels) + " labels fit; enlarge the figure or reduce the label size.");
  if (!paletteList().empty()) {
    bool safe = false;
    for (auto& p : paletteList()) if (st.palette == p.id) safe = p.cvdSafe;
    if (!safe && clusterLegend) sc.warnings.push_back("The palette is not colour-vision-deficiency safe (try Okabe\xE2\x80\x93Ito or Tol).");
  }
  return sc;
}

Scene chartScene(double wpt, double hpt, bool light) {
  Scene s;
  s.W = wpt; s.H = hpt;
  s.items.push_back(rectP(0, 0, wpt, hpt, light ? Color(1, 1, 1) : Color(0.08f, 0.09f, 0.11f), "background"));
  return s;
}

// ------------------------------------------------------------ SVG writer
static string f2(double v) {
  char b[32];
  snprintf(b, sizeof b, "%.2f", v);
  string s = b;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s == "-0") s = "0";
  return s;
}
static string hex6(const Color& c) {
  char b[8];
  snprintf(b, sizeof b, "#%02x%02x%02x", int(clampv(c.r, 0.f, 1.f) * 255 + .5f), int(clampv(c.g, 0.f, 1.f) * 255 + .5f), int(clampv(c.b, 0.f, 1.f) * 255 + .5f));
  return b;
}
static string pathD(const vector<PathCmd>& d) {
  string o;
  for (auto& c : d) {
    if (c.op == 'M' || c.op == 'L') o += string(1, c.op) + f2(c.x1) + " " + f2(c.y1);
    else if (c.op == 'Q') o += "Q" + f2(c.x1) + " " + f2(c.y1) + " " + f2(c.x2) + " " + f2(c.y2);
    else if (c.op == 'Z') o += "Z";
  }
  return o;
}

vector<SphereStop> sphereStops(const Color& base) {
  // Sample the on-screen shader along the ray from the highlight through the centre to the far rim.
  const double lx0 = -0.45, ly0 = -0.6, lz0 = 0.75;  // light, screen coordinates (y down)
  const double ll = std::sqrt(lx0 * lx0 + ly0 * ly0 + lz0 * lz0);
  const double Lx = lx0 / ll, Ly = ly0 / ll, Lz = lz0 / ll;
  const double fx = SPHERE_FX, fy = SPHERE_FY, fl = std::sqrt(fx * fx + fy * fy);
  const double dx = -fx / fl, dy = -fy / fl, total = fl + 1.0;
  vector<SphereStop> out;
  const int K = 14;
  for (int k = 0; k <= K; k++) {
    double t = double(k) / K;
    t = t * t * 0.35 + t * 0.65;  // denser near the highlight
    double qx = fx + dx * total * t, qy = fy + dy * total * t;
    double r2 = std::min(1.0, qx * qx + qy * qy);
    double nz = std::sqrt(std::max(0.0, 1 - r2));
    double ndl = std::max(0.0, qx * Lx + qy * Ly + nz * Lz);
    double rz = 2 * ndl * nz - Lz;  // reflect(-L, N).z
    double spec = std::pow(std::max(0.0, rz), 28.0);
    double m = 0.66 + 0.44 * ndl;
    Color c(float(clampv(base.r * m + spec * 0.22, 0.0, 1.0)), float(clampv(base.g * m + spec * 0.22, 0.0, 1.0)), float(clampv(base.b * m + spec * 0.22, 0.0, 1.0)), 1.f);
    out.push_back({float(t), c});
  }
  return out;
}

string toSVG(const Scene& sc, bool serif) {
  string fam = serif ? "'Times New Roman', Times, serif" : "Helvetica, Arial, 'Liberation Sans', sans-serif";
  string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  o += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" + f2(sc.W / MM2PT) + "mm\" height=\"" + f2(sc.H / MM2PT) +
       "mm\" viewBox=\"0 0 " + f2(sc.W) + " " + f2(sc.H) + "\" font-family=\"" + fam + "\">\n";
  o += "<!-- Generated by VOSStudio Native. 1 user unit = 1 pt. -->\n";
  string cur;
  bool groupOpen = false;
  std::set<string> sphereDefs;
  for (auto& p : sc.items) {
    if (!p.fill && !p.stroke && p.type != Prim::Text && p.type != Prim::Image) continue;  // hover targets only
    string gname = startsWith(p.group, "hit:") ? string("items") : p.group;
    if (gname != cur || !groupOpen) {
      if (groupOpen) o += "</g>\n";
      cur = gname;
      groupOpen = true;
      o += "<g id=\"" + (cur.empty() ? string("layer") : cur) + "\">\n";
    }
    auto fillAttr = [&]() {
      string a;
      if (p.fill) { a += " fill=\"" + hex6(p.fillC) + "\""; if (p.fillC.a < 0.999f) a += " fill-opacity=\"" + f2(p.fillC.a) + "\""; if (p.evenOdd) a += " fill-rule=\"evenodd\""; }
      else a += " fill=\"none\"";
      if (p.stroke) {
        a += " stroke=\"" + hex6(p.strokeC) + "\" stroke-width=\"" + f2(p.sw) + "\"";
        if (p.strokeC.a < 0.999f) a += " stroke-opacity=\"" + f2(p.strokeC.a) + "\"";
        if (p.roundCap) a += " stroke-linecap=\"round\"";
        if (!p.dash.empty()) { a += " stroke-dasharray=\""; for (size_t k = 0; k < p.dash.size(); k++) a += (k ? " " : "") + f2(p.dash[k]); a += "\""; }
      }
      return a;
    };
    switch (p.type) {
      case Prim::Rect: o += "<rect x=\"" + f2(p.x) + "\" y=\"" + f2(p.y) + "\" width=\"" + f2(p.w) + "\" height=\"" + f2(p.h) + "\"" + fillAttr() + "/>\n"; break;
      case Prim::Circle: {
        if (p.sphere && p.fill) {
          string gid = "sph" + hex6(p.fillC).substr(1);
          if (!sphereDefs.count(gid)) {
            sphereDefs.insert(gid);
            o += "<defs><radialGradient id=\"" + gid + "\" cx=\"0.5\" cy=\"0.5\" r=\"0.5\" fx=\"" + f2(0.5 + SPHERE_FX * 0.5) + "\" fy=\"" + f2(0.5 + SPHERE_FY * 0.5) + "\">";
            for (auto& st : sphereStops(p.fillC)) o += "<stop offset=\"" + f2(st.t) + "\" stop-color=\"" + hex6(st.c) + "\"/>";
            o += "</radialGradient></defs>\n";
          }
          string a = " fill=\"url(#" + gid + ")\"";
          if (p.fillC.a < 0.999f) a += " fill-opacity=\"" + f2(p.fillC.a) + "\"";
          if (p.stroke) { a += " stroke=\"" + hex6(p.strokeC) + "\" stroke-width=\"" + f2(p.sw) + "\""; if (p.strokeC.a < 0.999f) a += " stroke-opacity=\"" + f2(p.strokeC.a) + "\""; }
          o += "<circle cx=\"" + f2(p.x) + "\" cy=\"" + f2(p.y) + "\" r=\"" + f2(p.r) + "\"" + a + "/>\n";
        } else o += "<circle cx=\"" + f2(p.x) + "\" cy=\"" + f2(p.y) + "\" r=\"" + f2(p.r) + "\"" + fillAttr() + "/>\n";
        break;
      }
      case Prim::Path: o += "<path d=\"" + pathD(p.d) + "\"" + fillAttr() + "/>\n"; break;
      case Prim::Text: {
        o += "<text x=\"" + f2(p.x) + "\" y=\"" + f2(p.y) + "\" font-size=\"" + f2(p.size) + "\"";
        if (p.bold) o += " font-weight=\"bold\"";
        if (p.italic) o += " font-style=\"italic\"";
        if (p.mono) o += " font-family=\"Consolas, 'Courier New', monospace\"";
        else if (!p.family.empty()) o += " font-family=\"" + xmlEscape(p.family) + (p.face == 2 || (p.face == 0 && sc.serif) ? ", 'Times New Roman', serif\"" : ", Helvetica, Arial, sans-serif\"");
        else if (p.face == 2 || (p.face == 0 && sc.serif)) o += " font-family=\"'Times New Roman', Times, serif\"";
        else if (p.face == 1) o += " font-family=\"Helvetica, Arial, 'Liberation Sans', sans-serif\"";
        if (p.underline) o += " text-decoration=\"underline\"";
        if (p.anchor == 1) o += " text-anchor=\"middle\"";
        else if (p.anchor == 2) o += " text-anchor=\"end\"";
        o += " fill=\"" + hex6(p.fillC) + "\"";
        if (p.fillC.a < 0.999f) o += " fill-opacity=\"" + f2(p.fillC.a) + "\"";
        if (p.halo) {
          o += " stroke=\"" + hex6(p.haloC) + "\" stroke-width=\"" + f2(p.haloW * 2) + "\" stroke-linejoin=\"round\" paint-order=\"stroke\"";
          if (p.haloC.a < 0.999f) o += " stroke-opacity=\"" + f2(p.haloC.a) + "\"";
        }
        o += ">" + xmlEscape(p.text) + "</text>\n";
        if (!p.href.empty()) { size_t at = o.rfind("<text "); o.insert(at, "<a href=\"" + xmlEscape(p.href) + "\">"); o += "</a>\n"; }
        break;
      }
      case Prim::Image: {
        bool alpha = false;
        for (size_t i = 3; i < p.rgba.size(); i += 4) if (p.rgba[i] != 255) { alpha = true; break; }
        string png = pngEncode(p.imgW, p.imgH, p.rgba.data(), alpha);
        o += "<image x=\"" + f2(p.x) + "\" y=\"" + f2(p.y) + "\" width=\"" + f2(p.w) + "\" height=\"" + f2(p.h) +
             "\" preserveAspectRatio=\"none\" xlink:href=\"data:image/png;base64," + base64(png) + "\"/>\n";
        break;
      }
    }
  }
  if (groupOpen) o += "</g>\n";
  o += "</svg>\n";
  return o;
}

// ------------------------------------------------------------ PDF writer
static string pdfStr(const string& utf8) {
  string o = "(";
  for (auto cp : utf8cp(utf8)) {
    uint8_t b = winAnsi(cp);
    if (!b) {
      string u;
      if (cp < 0x800) { u += char(0xC0 | (cp >> 6)); u += char(0x80 | (cp & 63)); }
      string f = asciiFold(u);
      b = (!f.empty() && uint8_t(f[0]) < 128) ? uint8_t(f[0]) : uint8_t('?');
    }
    if (b == '(' || b == ')' || b == '\\') { o += '\\'; o += char(b); }
    else if (b < 32 || b > 126) { char e[8]; snprintf(e, sizeof e, "\\%03o", b); o += e; }
    else o += char(b);
  }
  return o + ")";
}

string toPDF(const vector<Scene>& scenes, const string& title) {
  std::map<int, int> alphaGs;  // alpha*1000 -> index
  auto gsFor = [&](float a) { int k = int(std::lround(clampv(a, 0.f, 1.f) * 1000)); auto it = alphaGs.find(k); if (it != alphaGs.end()) return it->second; int id = int(alphaGs.size()); alphaGs[k] = id; return id; };
  auto num = [](double v) { return f2(v); };
  vector<const Prim*> images;
  std::map<string, int> shadingIds;
  vector<Color> shadingCols;
  auto shadingFor = [&](const Color& col) {
    string k = hex6(col);
    auto it = shadingIds.find(k);
    if (it != shadingIds.end()) return it->second;
    int id = int(shadingCols.size());
    shadingIds[k] = id;
    shadingCols.push_back(col);
    return id;
  };
  vector<string> contents;  // one content stream per page; resources are shared
  vector<vector<string>> pageAnnots;  // link annotations per page
  bool serif = false;
  for (const Scene& sc : scenes) {
  if (sc.serif) serif = true;
  vector<string> annots;
  string c;  // content stream
  auto rgb = [&](const Color& col, bool stroke) { return num(col.r) + " " + num(col.g) + " " + num(col.b) + (stroke ? " RG\n" : " rg\n"); };
  c += "1 0 0 -1 0 " + num(sc.H) + " cm\n1 j\n";
  auto circlePath = [&](double x, double y, double r) {
    const double k = 0.5522847498 * r;
    c += num(x + r) + " " + num(y) + " m\n";
    c += num(x + r) + " " + num(y + k) + " " + num(x + k) + " " + num(y + r) + " " + num(x) + " " + num(y + r) + " c\n";
    c += num(x - k) + " " + num(y + r) + " " + num(x - r) + " " + num(y + k) + " " + num(x - r) + " " + num(y) + " c\n";
    c += num(x - r) + " " + num(y - k) + " " + num(x - k) + " " + num(y - r) + " " + num(x) + " " + num(y - r) + " c\n";
    c += num(x + k) + " " + num(y - r) + " " + num(x + r) + " " + num(y - k) + " " + num(x + r) + " " + num(y) + " c\n";
  };
  for (auto& p : sc.items) {
    if (!p.fill && !p.stroke && p.type != Prim::Text && p.type != Prim::Image) continue;  // hover targets only
    c += "q\n";
    float fa = p.fill ? p.fillC.a : 1, sa = p.stroke ? p.strokeC.a : 1;
    if (p.type != Prim::Text && p.type != Prim::Image) {
      if (p.fill && fa < 0.999f) c += "/GF" + std::to_string(gsFor(fa)) + " gs\n";
      if (p.stroke && sa < 0.999f) c += "/GS" + std::to_string(gsFor(sa)) + " gs\n";
      if (p.fill) c += rgb(p.fillC, false);
      if (p.stroke) {
        c += rgb(p.strokeC, true) + num(p.sw) + " w\n";
        if (p.roundCap) c += "1 J\n";
        if (!p.dash.empty()) { c += "["; for (auto d : p.dash) c += num(d) + " "; c += "] 0 d\n"; }
      }
      string op = p.fill && p.stroke ? (p.evenOdd ? "B*\n" : "B\n") : (p.fill ? (p.evenOdd ? "f*\n" : "f\n") : "S\n");
      if (p.type == Prim::Rect) c += num(p.x) + " " + num(p.y) + " " + num(p.w) + " " + num(p.h) + " re\n" + op;
      else if (p.type == Prim::Circle && p.sphere && p.fill) {
        // clip to the circle, map the unit shading onto it, then stroke on top
        int sh = shadingFor(p.fillC);
        circlePath(p.x, p.y, p.r);
        c += "W n\n" + num(p.r) + " 0 0 " + num(p.r) + " " + num(p.x) + " " + num(p.y) + " cm\n/Sh" + std::to_string(sh) + " sh\n";
        if (p.stroke) {
          c += "Q\nq\n";
          if (sa < 0.999f) c += "/GS" + std::to_string(gsFor(sa)) + " gs\n";
          c += rgb(p.strokeC, true) + num(p.sw) + " w\n";
          circlePath(p.x, p.y, p.r);
          c += "S\n";
        }
      }
      else if (p.type == Prim::Circle) { circlePath(p.x, p.y, p.r); c += op; }
      else if (p.type == Prim::Path) {
        double lx = 0, ly = 0, sx = 0, sy = 0;
        for (auto& d : p.d) {
          if (d.op == 'M') { c += num(d.x1) + " " + num(d.y1) + " m\n"; lx = sx = d.x1; ly = sy = d.y1; }
          else if (d.op == 'L') { c += num(d.x1) + " " + num(d.y1) + " l\n"; lx = d.x1; ly = d.y1; }
          else if (d.op == 'Q') {
            double c1x = lx + 2.0 / 3 * (d.x1 - lx), c1y = ly + 2.0 / 3 * (d.y1 - ly);
            double c2x = d.x2 + 2.0 / 3 * (d.x1 - d.x2), c2y = d.y2 + 2.0 / 3 * (d.y1 - d.y2);
            c += num(c1x) + " " + num(c1y) + " " + num(c2x) + " " + num(c2y) + " " + num(d.x2) + " " + num(d.y2) + " c\n";
            lx = d.x2; ly = d.y2;
          } else if (d.op == 'Z') { c += "h\n"; lx = sx; ly = sy; }
        }
        c += op;
      }
    } else if (p.type == Prim::Image) {
      images.push_back(&p);
      c += num(p.w) + " 0 0 " + num(-p.h) + " " + num(p.x) + " " + num(p.y + p.h) + " cm\n/Im" + std::to_string(images.size()) + " Do\n";
    } else {
      double w = textWidth(p.text, p.size, p.bold);
      if (p.mono) w = p.size * 0.6 * double(utf8Len(p.text));
      double x = p.anchor == 1 ? p.x - w / 2 : (p.anchor == 2 ? p.x - w : p.x);
      bool ser = p.face == 2 || (p.face == 0 && sc.serif);
      int fb = ser ? 7 : 1;  // F1–F4 Helvetica, F7–F10 Times, F5/F6 Courier
      string font = p.mono ? (p.bold ? "/F6 " : "/F5 ") : "/F" + std::to_string(fb + (p.italic ? 2 : 0) + (p.bold ? 1 : 0)) + " ";
      string tm = "1 0 0 -1 " + num(x) + " " + num(p.y) + " Tm\n";
      string s = pdfStr(p.text);
      if (p.underline) c += rgb(p.fillC, true) + num(std::max(0.4, p.size * 0.06)) + " w " + num(x) + " " + num(p.y + p.size * 0.12) + " m " + num(x + w) + " " + num(p.y + p.size * 0.12) + " l S\n";
      if (!p.href.empty()) {
        string u;
        for (unsigned char ch : p.href) { if (ch == '(' || ch == ')' || ch == '\\') u += '\\'; if (ch < 0x20) continue; u += char(ch); }
        annots.push_back("<< /Type /Annot /Subtype /Link /Rect [" + num(x) + " " + num(sc.H - (p.y + p.size * 0.25)) + " " + num(x + w) + " " + num(sc.H - (p.y - p.size * 0.85)) +
                         "] /Border [0 0 0] /A << /S /URI /URI (" + u + ") >> >>");
      }
      if (p.halo && p.haloW > 0) {
        c += "q\n";
        if (p.haloC.a < 0.999f) c += "/GS" + std::to_string(gsFor(p.haloC.a)) + " gs\n";
        c += rgb(p.haloC, true) + num(p.haloW * 2) + " w 1 j\nBT " + font + num(p.size) + " Tf 1 Tr\n" + tm + s + " Tj\nET\nQ\n";
      }
      if (fa < 0.999f) c += "/GF" + std::to_string(gsFor(fa)) + " gs\n";
      c += rgb(p.fillC, false) + "BT " + font + num(p.size) + " Tf 0 Tr\n" + tm + s + " Tj\nET\n";
    }
    c += "Q\n";
  }
  contents.push_back(std::move(c));
  pageAnnots.push_back(std::move(annots));
  }
  // --- objects
  vector<string> objs;  // 1-based numbering
  auto add = [&](const string& s) { objs.push_back(s); return int(objs.size()); };
  int catalog = add(""), pages = add(""), resources = add("");
  (void)serif;
  const char* fam[10] = {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique", "Courier", "Courier-Bold",
                         "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic"};
  int fontIds[10];
  for (int k = 0; k < 10; k++) fontIds[k] = add(string("<< /Type /Font /Subtype /Type1 /BaseFont /") + fam[k] + " /Encoding /WinAnsiEncoding >>");
  string fontDict;
  for (int k = 0; k < 10; k++) fontDict += "/F" + std::to_string(k + 1) + " " + std::to_string(fontIds[k]) + " 0 R ";
  string gsDict;
  for (auto& kv : alphaGs) {
    string a = f2(kv.first / 1000.0);
    int g1 = add("<< /Type /ExtGState /ca " + a + " >>");
    int g2 = add("<< /Type /ExtGState /CA " + a + " >>");
    gsDict += "/GF" + std::to_string(kv.second) + " " + std::to_string(g1) + " 0 R /GS" + std::to_string(kv.second) + " " + std::to_string(g2) + " 0 R ";
  }
  string shDict;
  for (size_t k = 0; k < shadingCols.size(); k++) {
    auto stops = sphereStops(shadingCols[k]);
    string fns, bounds, enc;
    for (size_t i = 0; i + 1 < stops.size(); i++) {
      auto& a = stops[i];
      auto& b2 = stops[i + 1];
      fns += "<< /FunctionType 2 /Domain [0 1] /C0 [" + f2(a.c.r) + " " + f2(a.c.g) + " " + f2(a.c.b) + "] /C1 [" + f2(b2.c.r) + " " + f2(b2.c.g) + " " + f2(b2.c.b) + "] /N 1 >> ";
      if (i + 2 < stops.size()) bounds += f2(b2.t) + " ";
      enc += "0 1 ";
    }
    int id = add("<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [" + f2(SPHERE_FX) + " " + f2(SPHERE_FY) + " 0 0 0 1] /Function << /FunctionType 3 /Domain [0 1] /Functions [" +
                 fns + "] /Bounds [" + bounds + "] /Encode [" + enc + "] >> /Extend [true true] >>");
    shDict += "/Sh" + std::to_string(k) + " " + std::to_string(id) + " 0 R ";
  }
  string xo;
  for (size_t k = 0; k < images.size(); k++) {
    const Prim& im = *images[k];
    string raw;
    raw.reserve(size_t(im.imgW) * size_t(im.imgH) * 3);
    for (size_t i = 0; i < size_t(im.imgW) * size_t(im.imgH); i++) { raw += char(im.rgba[i * 4]); raw += char(im.rgba[i * 4 + 1]); raw += char(im.rgba[i * 4 + 2]); }
    string z = zlibCompress(raw);
    // translucent rasters (Geo density over the basemap) carry their alpha as a soft mask
    string smask;
    bool hasAlpha = false;
    for (size_t i = 0; i < size_t(im.imgW) * size_t(im.imgH); i++) if (im.rgba[i * 4 + 3] != 255) { hasAlpha = true; break; }
    if (hasAlpha) {
      string a;
      a.reserve(size_t(im.imgW) * size_t(im.imgH));
      for (size_t i = 0; i < size_t(im.imgW) * size_t(im.imgH); i++) a += char(im.rgba[i * 4 + 3]);
      string za = zlibCompress(a);
      int sid = add("<< /Type /XObject /Subtype /Image /Width " + std::to_string(im.imgW) + " /Height " + std::to_string(im.imgH) +
                    " /ColorSpace /DeviceGray /BitsPerComponent 8 /Interpolate true /Filter /FlateDecode /Length " + std::to_string(za.size()) + " >>\nstream\n" + za + "\nendstream");
      smask = " /SMask " + std::to_string(sid) + " 0 R";
    }
    int id = add("<< /Type /XObject /Subtype /Image /Width " + std::to_string(im.imgW) + " /Height " + std::to_string(im.imgH) +
                 " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Interpolate true" + smask + " /Filter /FlateDecode /Length " + std::to_string(z.size()) + " >>\nstream\n" + z + "\nendstream");
    xo += "/Im" + std::to_string(k + 1) + " " + std::to_string(id) + " 0 R ";
  }
  string t = pdfStr(title);
  int info = add("<< /Title " + t + " /Producer (VOSStudio Native) /Creator (VOSStudio Native) >>");
  objs[resources - 1] = "<< /Font << " + fontDict + ">> /ExtGState << " + gsDict + ">> /Shading << " + shDict +
                        ">> /XObject << " + xo + ">> /ProcSet [/PDF /Text /ImageC] >>";
  string kids;
  for (size_t k = 0; k < scenes.size(); k++) {
    string cz = zlibCompress(contents[k]);
    int content = add("<< /Length " + std::to_string(cz.size()) + " /Filter /FlateDecode >>\nstream\n" + cz + "\nendstream");
    string annots;
    for (auto& a : pageAnnots[k]) annots += std::to_string(add(a)) + " 0 R ";
    int page = add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 " + f2(scenes[k].W) + " " + f2(scenes[k].H) + "] /Resources " +
                   std::to_string(resources) + " 0 R /Contents " + std::to_string(content) + " 0 R /Group << /S /Transparency /CS /DeviceRGB >> >>" +
                   (annots.empty() ? string() : " /Annots [" + annots + "]") + " >>");
    kids += std::to_string(page) + " 0 R ";
  }
  objs[catalog - 1] = "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>";
  objs[pages - 1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(scenes.size()) + " >>";
  string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  vector<size_t> off;
  for (size_t i = 0; i < objs.size(); i++) {
    off.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
  }
  size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
  for (auto o : off) { char b[24]; snprintf(b, sizeof b, "%010zu 00000 n \n", o); out += b; }
  out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " + std::to_string(info) + " 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return out;
}

string toPDF(const Scene& sc, const string& title) { return toPDF(vector<Scene>{sc}, title); }

vector<Scene> buildFigurePages(const Network& net, const ViewStyle& style, const FigureSpec& spec, const Bundles* bundles,
                               const string& methodsShort, const vector<FigPanelDef>* panelDefs) {
  vector<Scene> out;
  for (ViewKind k : figurePanels(spec)) {
    FigureSpec one = spec;
    one.panelNetwork = k == ViewKind::Network; one.panelOverlay = k == ViewKind::Overlay; one.panelDensity = k == ViewKind::Density;
    one.panelTimeline = k == ViewKind::Timeline; one.panelGeo = k == ViewKind::Geo; one.panel3D = k == ViewKind::ThreeD; one.panelMatrix = k == ViewKind::Matrix;
    one.letters = false;
    out.push_back(buildFigure(net, style, one, bundles, methodsShort, panelDefs));
  }
  return out;
}

}  // namespace vs
