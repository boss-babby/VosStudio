// VOSStudio Native — Geo view: world basemap (Natural Earth, Equal Earth projection), country maps and corpus overview
//
// Mode A (the map's items are countries): nodes sit on their countries over a basemap; countries are tinted by
//   cluster colour or by weight.
// Mode B (any other map, corpus has affiliations with countries): the nodes are hidden and the canvas shows a
//   choropleth of documents per country, bubbles and the strongest international collaborations.
#include <map>
#include <set>
#include "../core/world.h"
#include "app.h"

namespace vs {
namespace win {

static const double GEO_S = 173.0;  // world units per Equal Earth unit (≈ the old 2.6 units per degree at the equator)

void geoProject(double lon, double lat, float& x, float& y) {
  double X, Y;
  equalEarth(lon, lat, X, Y);
  x = float(X * GEO_S);
  y = float(-Y * GEO_S);
}

// ------------------------------------------------------------------ data
void App::geoEnsureWorld() {
  if (!geoWorld.empty()) return;
  int n = worldCountryCount();
  geoWorld.resize(size_t(n));
  const int16_t* P = worldPts();
  for (int c = 0; c < n; c++) {
    const WorldCountry& wc = worldCountry(c);
    GeoCountryShape& sh = geoWorld[size_t(c)];
    sh.x0 = sh.y0 = 1e9f; sh.x1 = sh.y1 = -1e9f;
    for (int r = wc.ring0; r < wc.ring0 + wc.rings; r++) {
      const WorldRing& wr = worldRing(r);
      vector<std::pair<float, float>> pts;
      pts.reserve(size_t(wr.count));
      for (int k = 0; k < wr.count; k++) {
        float x, y;
        geoProject(P[(wr.start + k) * 2] / 100.0, P[(wr.start + k) * 2 + 1] / 100.0, x, y);
        pts.push_back({x, y});
        sh.x0 = std::min(sh.x0, x); sh.x1 = std::max(sh.x1, x);
        sh.y0 = std::min(sh.y0, y); sh.y1 = std::max(sh.y1, y);
      }
      sh.rings.push_back(std::move(pts));
    }
    geoProject(wc.labelLon, wc.labelLat, sh.lx, sh.ly);
  }
}

string App::geoSig() const {
  string s = std::to_string(uintptr_t(P.get())) + ":" + std::to_string(P->corpus.recs.size()) + ":" + std::to_string(P->corpus.files.size());
  if (!P->corpus.recs.empty()) s += P->corpus.recs.front().title.substr(0, 20) + P->corpus.recs.back().title.substr(0, 20);
  if (hasMap()) s += "|" + std::to_string(P->net.n()) + ":" + std::to_string(P->net.m()) + ":" + (P->net.n() ? P->net.nodes[0].label : string());
  return s;
}

void App::geoUpdate() const {
  string sig = geoSig();
  if (sig == geoSig_) return;
  geoSig_ = sig;
  int nC = worldCountryCount();
  // mode A: nodes that are countries
  geoNodeCountry.assign(hasMap() ? size_t(P->net.n()) : 0, -1);
  int ok = 0;
  if (hasMap())
    for (int i = 0; i < P->net.n(); i++) {
      int c = findCountry(P->net.nodes[size_t(i)].label);
      geoNodeCountry[size_t(i)] = c;
      if (c >= 0) ok++;
    }
  // corpus statistics (mode B)
  geoStats.assign(size_t(nC), GeoStat{});
  std::map<std::pair<int, int>, int> pairs;
  geoDocsMax = 0;
  geoDocsTotal = 0;
  for (auto& r : P->corpus.recs) {
    std::set<int> cs;
    for (auto& c : r.countries) { int k = findCountry(c); if (k >= 0) cs.insert(k); }
    if (cs.empty()) continue;
    geoDocsTotal++;
    for (int k : cs) {
      GeoStat& g = geoStats[size_t(k)];
      g.docs++;
      g.cites += r.cites;
      if (r.year > 0) { g.yearSum += r.year; g.yearN++; }
    }
    if (cs.size() > 1) {
      vector<int> v(cs.begin(), cs.end());
      for (size_t a = 0; a < v.size(); a++)
        for (size_t b = a + 1; b < v.size(); b++) {
          pairs[{v[a], v[b]}]++;
          geoStats[size_t(v[a])].intl++;
          geoStats[size_t(v[b])].intl++;
        }
    }
  }
  for (auto& s : geoStats) geoDocsMax = std::max(geoDocsMax, s.docs);
  {  // average year per country, range clipped to the 5th and 95th percentile (single-paper countries are noisy)
    vector<double> av;
    for (auto& s : geoStats) if (s.yearN > 0) av.push_back(s.yearSum / s.yearN);
    std::sort(av.begin(), av.end());
    if (!av.empty()) {
      geoYearLo = av[size_t(double(av.size() - 1) * 0.05)];
      geoYearHi = av[size_t(double(av.size() - 1) * 0.95)];
      if (geoYearHi - geoYearLo < 0.5) { geoYearLo -= 0.5; geoYearHi += 0.5; }
    } else geoYearLo = geoYearHi = 0;
  }
  geoPairs.clear();
  for (auto& p : pairs) geoPairs.push_back({p.first.first, p.first.second, p.second});
  std::sort(geoPairs.begin(), geoPairs.end(), [](auto& a, auto& b) { return std::get<2>(a) > std::get<2>(b); });
  geoPartners.assign(size_t(nC), {});
  for (auto& p : geoPairs) {
    geoPartners[size_t(std::get<0>(p))].push_back({std::get<1>(p), std::get<2>(p)});
    geoPartners[size_t(std::get<1>(p))].push_back({std::get<0>(p), std::get<2>(p)});
  }
  if (geoPairs.size() > 150) geoPairs.resize(150);
  geoPairMax = geoPairs.empty() ? 1 : std::get<2>(geoPairs[0]);
  geoModeA = hasMap() && ok >= std::max(2, P->net.n() / 3);
}

bool App::geoAvailable() const {
  if (!hasMap() && !hasCorpus()) return false;
  geoUpdate();
  return geoModeA || geoDocsMax > 0;
}

int App::geoMode() const {
  if (view != ViewKind::Geo && !geoFig_) return 0;  // geoFig_: building a Geo figure panel from another view
  geoUpdate();
  return geoModeA ? 1 : (geoDocsMax > 0 ? 2 : 0);
}

// world-to-screen for the (2D) geo camera
static inline void geoW2S(const NetView& nv, float wx, float wy, float& sx, float& sy) {
  sx = float(nv.vp.x + nv.vp.w / 2 + (wx - nv.cam.x) * nv.cam.zoom);
  sy = float(nv.vp.y + nv.vp.h / 2 + (wy - nv.cam.y) * nv.cam.zoom);
}

// ------------------------------------------------------------------ layer description (shared by screen and export)
Color App::geoLand(const Theme& th) const { return th.light ? Color::hex(0xdfe3e9) : Color::hex(0x2a303a); }

void App::geoLayer(const Theme& th, vector<GeoFill>& fills, vector<GeoBubble>& bubbles, vector<GeoArc>& arcs) const {
  int mode = geoMode();
  int nC = worldCountryCount();
  Color land = geoLand(th);
  fills.assign(size_t(nC), GeoFill{land, false});
  // figure panels ignore the on-screen selection and size bubbles/arcs in points (geoFigPx_ per screen px)
  const int sel = geoFig_ ? -1 : geoSel;
  const double px = geoFig_ ? geoFigPx_ : ui.s;
  const bool shade = geoLayerKind == 2 && geoDenStyle == 2;  // density shown as shaded countries
  vector<double> shadeT;
  if (shade) {
    shadeT = geoDenValues();
    double mx = 0;
    for (double x : shadeT) mx = std::max(mx, x);
    for (double& x : shadeT) x = mx > 0 && x > 0 ? std::sqrt(x / mx) : -1;
  }
  auto shadeCol = [&](int c, Color fallback) {
    if (!shade || c >= int(shadeT.size())) return fallback;
    if (shadeT[size_t(c)] < 0) return land.mix(th.light ? Color(1, 1, 1) : Color(0.55f, 0.6f, 0.68f), 0.25f);
    return land.mix(simulateCvd(cmapAt(P->style.densityScheme, 0.1 + 0.9 * shadeT[size_t(c)]), P->style.cvd), 0.92f);
  };
  if (mode == 1) {
    const Encoder& e = nv.enc();
    for (int i = 0; i < P->net.n() && i < int(geoNodeCountry.size()); i++) {
      int c = geoNodeCountry[size_t(i)];
      if (c < 0) continue;
      double t = std::sqrt(std::max(0.0, e.weightT(i)));
      Color col = land;
      if (geoLayerKind == 2) col = shadeCol(c, land.mix(th.light ? Color(1, 1, 1) : Color(0.55f, 0.6f, 0.68f), 0.25f));  // density: mapped countries slightly lifted
      else if (geoLayerKind == 1) col = land.mix(e.nodeColor(i, ViewKind::Overlay), float(0.35 + 0.4 * t));
      else if (geoFill == 0) col = land.mix(e.clusterColor(P->net.nodes[size_t(i)].cluster), float(0.22 + 0.38 * t));
      else if (geoFill == 1) col = land.mix(cmapAt(P->style.scheme, 0.1 + 0.9 * t), 0.75f);
      if (!geoFig_ && i < int(nv.flags.size()) && (nv.flags[size_t(i)] & NF_DIM)) col = land.mix(col, 0.35f);
      fills[size_t(c)] = {col, true};
    }
  } else if (mode == 2) {
    double lm = std::log1p(double(std::max(1, geoDocsMax)));
    for (int c = 0; c < nC; c++) {
      int d = geoStats[size_t(c)].docs;
      if (d <= 0) continue;
      double t = std::log1p(double(d)) / lm;
      Color col = land.mix(cmapAt(P->style.scheme, 0.12 + 0.88 * t), float(0.45 + 0.5 * t));
      if (geoLayerKind == 1) {  // overlay: average publication year
        const GeoStat& gs = geoStats[size_t(c)];
        if (gs.yearN <= 0) col = land.mix(Color(0.6f, 0.6f, 0.6f), 0.35f);
        else {
          double u = clampv((gs.yearSum / gs.yearN - geoYearLo) / std::max(1e-9, geoYearHi - geoYearLo), 0.0, 1.0);
          col = land.mix(simulateCvd(cmapAt(P->style.scheme, u), P->style.cvd), 0.88f);
        }
      } else if (geoLayerKind == 2) col = shadeCol(c, land.mix(th.light ? Color(1, 1, 1) : Color(0.55f, 0.6f, 0.68f), 0.25f));
      if (sel >= 0 && c != sel) {
        bool partner = false;
        for (auto& p : geoPartners[size_t(sel)]) if (p.first == c) { partner = true; break; }
        if (!partner) col = land.mix(col, 0.3f);
      }
      fills[size_t(c)] = {col, true};
    }
    double zf = geoFig_ ? 1.0 : clampv(std::sqrt(nv.cam.zoom / std::max(1e-9, nv.fitZoom())), 1.0, 3.0);
    Color bc = Color::hex(0xE8743B);
    for (int c = 0; c < nC && geoLayerKind == 0; c++) {
      int d = geoStats[size_t(c)].docs;
      if (d <= 0) continue;
      float r = float((2.5 + 16 * std::sqrt(double(d) / geoDocsMax)) * px * zf);
      float a = sel >= 0 && c != sel ? 0.25f : 0.7f;
      bubbles.push_back({geoWorld[size_t(c)].lx, geoWorld[size_t(c)].ly, r, bc.withA(a * 0.55f), (th.light ? bc.mix(Color(0, 0, 0), 0.25f) : bc.mix(Color(1, 1, 1), 0.2f)).withA(a), c});
    }
    if (geoArcs && geoLayerKind == 0)
      for (auto& p : geoPairs) {
        int a = std::get<0>(p), b = std::get<1>(p), w = std::get<2>(p);
        double t = std::sqrt(double(w) / geoPairMax);
        bool on = sel < 0 || a == sel || b == sel;
        if (sel >= 0 && !on) continue;
        Color col = (th.light ? Color::hex(0x1f3a5f) : Color::hex(0xf2c14e)).withA(float((0.18 + 0.55 * t) * (sel >= 0 ? 1.4 : 1.0)));
        arcs.push_back({geoWorld[size_t(a)].lx, geoWorld[size_t(a)].ly, geoWorld[size_t(b)].lx, geoWorld[size_t(b)].ly, float((0.6 + 3.4 * t) * px), col, w});
      }
  }
}

// quadratic control point bending the arc "upwards"
static void arcCtrl(float ax, float ay, float bx, float by, float& cx, float& cy) {
  float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
  float nx = -dy / std::max(1e-6f, len), ny = dx / std::max(1e-6f, len);
  if (ny > 0) { nx = -nx; ny = -ny; }
  cx = (ax + bx) / 2 + nx * len * 0.22f;
  cy = (ay + by) / 2 + ny * len * 0.22f;
}

// ------------------------------------------------------------------ screen painter (Direct2D, under the GPU nodes)
// ------------------------------------------------------------------ density layer
// Gaussian kernel density of the countries on the map: in a country network each node (country) weighs by its
// size weight, in the corpus overview each country by its documents. The raster covers the band shown on screen
// (83Â°N to 60Â°S) at about 720 px across, is cached, and is drawn and exported as one image.
// The measure behind the density, per world country. Country maps take it from the records behind each country
// item (or the map's weights for imported maps); the corpus overview from the records' affiliations.
// Citations per document needs at least two documents (single papers make noisy peaks).
vector<double> App::geoDenValues() const {
  int nC = worldCountryCount();
  vector<double> v(size_t(nC), 0.0);
  int mode = geoMode();
  auto per = [&](double docs, double cites, double collab) {
    switch (geoDenMeasure) {
      case 1: return cites;
      case 2: return docs >= 2 ? cites / docs : 0.0;
      case 3: return collab;
      default: return docs;
    }
  };
  if (mode == 1) {
    const Network& N = P->net;
    int wDocs = N.weightIndex("Documents"), wCit = N.weightIndex("Citations"), wTls = N.weightIndex("Total link strength");
    for (int i = 0; i < N.n() && i < int(geoNodeCountry.size()); i++) {
      int c = geoNodeCountry[size_t(i)];
      if (c < 0) continue;
      const Node& nd = N.nodes[size_t(i)];
      double docs = 0, cites = 0;
      if (!nd.recs.empty() && hasCorpus()) {
        for (int r : nd.recs) if (r >= 0 && r < int(P->corpus.recs.size())) { docs++; cites += P->corpus.recs[size_t(r)].cites; }
      } else {
        docs = wDocs >= 0 && wDocs < int(nd.w.size()) ? nd.w[size_t(wDocs)] : N.weight(i);
        cites = wCit >= 0 && wCit < int(nd.w.size()) ? nd.w[size_t(wCit)] : 0;
      }
      double collab = wTls >= 0 && wTls < int(nd.w.size()) ? nd.w[size_t(wTls)] : 0;
      v[size_t(c)] += per(docs, cites, collab);
    }
  } else if (mode == 2) {
    for (int c = 0; c < nC && c < int(geoStats.size()); c++) {
      const GeoStat& g = geoStats[size_t(c)];
      if (g.docs > 0) v[size_t(c)] = per(g.docs, double(g.cites), g.intl);
    }
  }
  return v;
}

string App::geoDenWhat() const {
  static const char* w[] = {"documents", "citations", "citations per document", "collaboration"};
  return w[clampv(geoDenMeasure, 0, 3)];
}

void App::geoDensity() const {
  int mode = geoMode();
  if (!mode) return;
  const Encoder& e = nv.enc();
  int style = mode == 1 ? geoDenStyle : (geoDenStyle == 1 ? 0 : geoDenStyle);
  string sig = geoSig_ + "|" + std::to_string(mode) + "|" + fmtNum(P->style.kernel, 2) + P->style.densityScheme + "|" + std::to_string(style) + std::to_string(geoDenMeasure) +
               std::to_string(int(P->style.cvd)) + std::to_string(P->net.nClusters);
  double wsum = 0;
  if (mode == 1) for (int i = 0; i < P->net.n(); i++) wsum += e.weightT(i) * (i % 7 + 1) + P->net.nodes[size_t(i)].cluster * 0.01;
  sig += fmtNum(wsum, 4);
  if (sig == geoDenSig_ && (!geoDenRgba.empty() || style == 2)) return;
  geoDenSig_ = sig;
  if (style == 2) { geoDenRgba.clear(); return; }  // shaded countries: no raster
  float t;
  geoProject(-180, 0, geoDenX0, t);
  geoProject(180, 0, geoDenX1, t);
  geoProject(0, 83, t, geoDenY0);
  geoProject(0, -60, t, geoDenY1);
  geoDenW = 720;
  geoDenH = std::max(8, int(std::lround(720.0 * (geoDenY1 - geoDenY0) / (geoDenX1 - geoDenX0))));
  struct Pt { double x, y, w; int cl; };
  vector<Pt> pts;
  {
    // square-root weights: one very large country must not wash out the rest
    vector<double> val = geoDenValues();
    double mx = 0;
    for (double x : val) mx = std::max(mx, x);
    vector<int> clusterOf(val.size(), -1);
    if (mode == 1)
      for (int i = 0; i < P->net.n() && i < int(geoNodeCountry.size()); i++)
        if (geoNodeCountry[size_t(i)] >= 0) clusterOf[size_t(geoNodeCountry[size_t(i)])] = P->net.nodes[size_t(i)].cluster;
    for (int k = 0; k < int(val.size()); k++) {
      if (val[size_t(k)] <= 0 || mx <= 0) continue;
      float x, y;
      geoProject(worldCountry(k).labelLon, worldCountry(k).labelLat, x, y);
      pts.push_back({x, y, std::sqrt(val[size_t(k)] / mx), clusterOf[size_t(k)]});
    }
  }
  const bool byCluster = style == 1 && mode == 1 && P->net.nClusters > 0;
  const int nCl = byCluster ? P->net.nClusters : 0;
  const double pxW = double(geoDenX1 - geoDenX0) / geoDenW;
  const double sigma = double(geoDenX1 - geoDenX0) * 0.02 * clampv(double(P->style.kernel), 0.3, 3.0);
  const double sp = sigma / pxW;
  const int R = int(std::ceil(3 * sp));
  vector<float> f(size_t(geoDenW) * size_t(geoDenH), 0.f);
  vector<float> fc(byCluster ? size_t(geoDenW) * size_t(geoDenH) * size_t(nCl) : 0, 0.f);  // per cluster, for the colour mix
  vector<float> kx(size_t(2 * R + 1));
  for (auto& q : pts) {
    double cx = (q.x - geoDenX0) / pxW - 0.5, cy = (q.y - geoDenY0) / pxW - 0.5;
    int ix = int(std::lround(cx)), iy = int(std::lround(cy));
    for (int d = -R; d <= R; d++) kx[size_t(d + R)] = float(std::exp(-0.5 * sq((ix + d - cx) / sp)));
    for (int dy = -R; dy <= R; dy++) {
      int y = iy + dy;
      if (y < 0 || y >= geoDenH) continue;
      float wy = float(q.w * std::exp(-0.5 * sq((y - cy) / sp)));
      float* row = &f[size_t(y) * size_t(geoDenW)];
      for (int dx = -R; dx <= R; dx++) {
        int x = ix + dx;
        if (x < 0 || x >= geoDenW) continue;
        float v = wy * kx[size_t(dx + R)];
        row[x] += v;
        if (byCluster && q.cl >= 0 && q.cl < nCl) fc[(size_t(y) * size_t(geoDenW) + size_t(x)) * size_t(nCl) + size_t(q.cl)] += v;
      }
    }
  }
  float mx = 1e-9f;
  for (float v : f) mx = std::max(mx, v);
  geoDenRgba.assign(f.size() * 4, 0);
  for (size_t i = 0; i < f.size(); i++) {
    double u = f[i] / mx;
    if (u < 0.01) continue;
    double a = clampv((u - 0.01) / 0.3, 0.0, 1.0);
    a = a * a * (3 - 2 * a) * 0.9;
    Color col;
    if (byCluster) {  // cluster density: the clusters' colours mixed by their share of the density here
      float r = 0, g = 0, b = 0, tot = 0;
      for (int c = 0; c < nCl; c++) {
        float v = fc[i * size_t(nCl) + size_t(c)];
        if (v <= 0) continue;
        Color cc = e.clusterColor(c);
        r += cc.r * v; g += cc.g * v; b += cc.b * v; tot += v;
      }
      col = tot > 0 ? Color(r / tot, g / tot, b / tot) : Color(0.5f, 0.5f, 0.5f);
      a = std::min(0.92, a * (0.55 + 0.45 * std::pow(u, 0.5)) / 0.9);
    } else col = simulateCvd(cmapAt(P->style.densityScheme, 0.1 + 0.9 * std::pow(u, 0.6)), P->style.cvd);
    geoDenRgba[i * 4 + 0] = uint8_t(clampv(col.r, 0.f, 1.f) * 255 + 0.5f);
    geoDenRgba[i * 4 + 1] = uint8_t(clampv(col.g, 0.f, 1.f) * 255 + 0.5f);
    geoDenRgba[i * 4 + 2] = uint8_t(clampv(col.b, 0.f, 1.f) * 255 + 0.5f);
    geoDenRgba[i * 4 + 3] = uint8_t(a * 255 + 0.5);
  }
}

void App::geoPaint(ID2D1DeviceContext* dc) {
  if (!geoBasemap && geoMode() == 1 && geoLayerKind != 2) return;
  geoEnsureWorld();
  Theme th = canvasTheme(P->style);
  vector<GeoFill> fills;
  vector<GeoBubble> bubbles;
  vector<GeoArc> arcs;
  geoLayer(th, fills, bubbles, arcs);
  auto* f = g.d2f.get();
  if (geoGeom.empty()) {
    geoGeom.resize(geoWorld.size());
    for (size_t c = 0; c < geoWorld.size(); c++) {
      if (geoWorld[c].rings.empty()) continue;
      Com<ID2D1PathGeometry> pg;
      if (FAILED(f->CreatePathGeometry(pg.put()))) continue;
      Com<ID2D1GeometrySink> sk;
      pg->Open(sk.put());
      sk->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
      for (auto& ring : geoWorld[c].rings) {
        if (ring.size() < 3) continue;
        sk->BeginFigure({ring[0].first, ring[0].second}, D2D1_FIGURE_BEGIN_FILLED);
        for (size_t k = 1; k < ring.size(); k++) sk->AddLine({ring[k].first, ring[k].second});
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
      }
      sk->Close();
      geoGeom[c] = pg;
    }
    // graticule + outline
    Com<ID2D1PathGeometry> gr;
    if (SUCCEEDED(f->CreatePathGeometry(gr.put()))) {
      Com<ID2D1GeometrySink> sk;
      gr->Open(sk.put());
      for (int lon = -180; lon <= 180; lon += 30) {
        float x, y;
        geoProject(lon, -90, x, y);
        sk->BeginFigure({x, y}, D2D1_FIGURE_BEGIN_HOLLOW);
        for (int lat = -88; lat <= 90; lat += 2) { geoProject(lon, lat, x, y); sk->AddLine({x, y}); }
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
      }
      for (int lat = -60; lat <= 80; lat += 20) {
        float x, y;
        geoProject(-180, lat, x, y);
        sk->BeginFigure({x, y}, D2D1_FIGURE_BEGIN_HOLLOW);
        for (int lon = -175; lon <= 180; lon += 5) { geoProject(lon, lat, x, y); sk->AddLine({x, y}); }
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
      }
      sk->Close();
      geoGrat = gr;
    }
    Com<ID2D1PathGeometry> ol;
    if (SUCCEEDED(f->CreatePathGeometry(ol.put()))) {
      Com<ID2D1GeometrySink> sk;
      ol->Open(sk.put());
      float x, y;
      geoProject(-180, -90, x, y);
      sk->BeginFigure({x, y}, D2D1_FIGURE_BEGIN_FILLED);
      for (int lat = -88; lat <= 90; lat += 2) { geoProject(-180, lat, x, y); sk->AddLine({x, y}); }
      for (int lat = 90; lat >= -90; lat -= 2) { geoProject(180, lat, x, y); sk->AddLine({x, y}); }
      sk->EndFigure(D2D1_FIGURE_END_CLOSED);
      sk->Close();
      geoOutline = ol;
    }
  }
  D2D1_MATRIX_3X2_F base;
  dc->GetTransform(&base);
  float z = float(nv.cam.zoom);
  D2D1_MATRIX_3X2_F W = D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(float(nv.vp.x + nv.vp.w / 2 - nv.cam.x * z), float(nv.vp.y + nv.vp.h / 2 - nv.cam.y * z));
  dc->SetTransform(W * base);
  Com<ID2D1SolidColorBrush> br;
  dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), br.put());
  auto set = [&](const Color& c) { br->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a)); };
  // sphere + graticule
  if (geoOutline) {
    set(th.light ? th.bg.mix(Color::hex(0x9fb7d0), 0.10f) : th.bg.mix(Color::hex(0x4a6a8a), 0.10f));
    dc->FillGeometry(geoOutline.get(), br.get());
  }
  if (geoGrat) { set(th.fg.withA(0.07f)); dc->DrawGeometry(geoGrat.get(), br.get(), 0.8f / z); }
  if (geoOutline) { set(th.fg.withA(0.16f)); dc->DrawGeometry(geoOutline.get(), br.get(), 1.0f / z); }
  // countries
  Color border = th.light ? Color(1, 1, 1, 0.9f) : th.bg.withA(0.9f);
  for (size_t c = 0; c < geoGeom.size(); c++) {
    if (!geoGeom[c]) continue;
    set(fills[c].col);
    dc->FillGeometry(geoGeom[c].get(), br.get());
  }
  if (geoLayerKind == 2) {
    geoDensity();
    string bs = geoDenSig_ + std::to_string(uintptr_t(dc));
    if (bs != geoDenBmpSig || !geoDenBmp) {
      geoDenBmp.reset();
      vector<uint8_t> pm(geoDenRgba.size());
      for (size_t i = 0; i + 3 < geoDenRgba.size(); i += 4) {  // D2D wants premultiplied BGRA
        unsigned a = geoDenRgba[i + 3];
        pm[i + 0] = uint8_t(geoDenRgba[i + 2] * a / 255);
        pm[i + 1] = uint8_t(geoDenRgba[i + 1] * a / 255);
        pm[i + 2] = uint8_t(geoDenRgba[i + 0] * a / 255);
        pm[i + 3] = uint8_t(a);
      }
      D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      if (!pm.empty()) dc->CreateBitmap(D2D1::SizeU(UINT32(geoDenW), UINT32(geoDenH)), pm.data(), UINT32(geoDenW * 4), bp, geoDenBmp.put());
      geoDenBmpSig = bs;
    }
    if (geoDenBmp) dc->DrawBitmap(geoDenBmp.get(), D2D1::RectF(geoDenX0, geoDenY0, geoDenX1, geoDenY1), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
  set(border);
  for (size_t c = 0; c < geoGeom.size(); c++) if (geoGeom[c]) dc->DrawGeometry(geoGeom[c].get(), br.get(), 0.7f / z);
  if (geoHover >= 0 && size_t(geoHover) < geoGeom.size() && geoGeom[size_t(geoHover)]) { set(th.fg.withA(0.85f)); dc->DrawGeometry(geoGeom[size_t(geoHover)].get(), br.get(), 1.6f / z); }
  if (geoSel >= 0 && size_t(geoSel) < geoGeom.size() && geoGeom[size_t(geoSel)]) { set(Color::hex(0x4f8ef7)); dc->DrawGeometry(geoGeom[size_t(geoSel)].get(), br.get(), 2.2f / z); }
  // screen-space overlays: arcs then bubbles
  dc->SetTransform(base);
  if (!arcs.empty()) {
    Com<ID2D1StrokeStyle> round;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
    f->CreateStrokeStyle(sp, nullptr, 0, round.put());
    for (size_t k = arcs.size(); k-- > 0;) {  // weakest first
      auto& a = arcs[k];
      float ax, ay, bx, by, cx, cy;
      geoW2S(nv, a.ax, a.ay, ax, ay);
      geoW2S(nv, a.bx, a.by, bx, by);
      arcCtrl(ax, ay, bx, by, cx, cy);
      Com<ID2D1PathGeometry> pg;
      if (FAILED(f->CreatePathGeometry(pg.put()))) continue;
      Com<ID2D1GeometrySink> sk;
      pg->Open(sk.put());
      sk->BeginFigure({ax, ay}, D2D1_FIGURE_BEGIN_HOLLOW);
      sk->AddQuadraticBezier({{cx, cy}, {bx, by}});
      sk->EndFigure(D2D1_FIGURE_END_OPEN);
      sk->Close();
      set(a.col);
      dc->DrawGeometry(pg.get(), br.get(), a.w, round.get());
    }
  }
  vector<size_t> ord(bubbles.size());
  for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
  std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return bubbles[a].r > bubbles[b].r; });
  for (size_t i : ord) {
    auto& b = bubbles[i];
    float sx, sy;
    geoW2S(nv, b.wx, b.wy, sx, sy);
    D2D1_ELLIPSE e{{sx, sy}, b.r, b.r};
    set(b.fill);
    dc->FillEllipse(e, br.get());
    set(b.stroke);
    dc->DrawEllipse(e, br.get(), 1.0f * ui.s);
  }
}

// ------------------------------------------------------------------ export (same layer as scene primitives)
void App::geoPrims(Scene& sc, double k, bool under) const {
  if (view != ViewKind::Geo) return;
  const_cast<App*>(this)->geoEnsureWorld();
  Theme th = canvasTheme(P->style);
  vector<GeoFill> fills;
  vector<GeoBubble> bubbles;
  vector<GeoArc> arcs;
  geoLayer(th, fills, bubbles, arcs);
  auto X = [&](float wx, float wy, float& x, float& y) { float sx, sy; geoW2S(nv, wx, wy, sx, sy); x = float((sx - nv.vp.x) * k); y = float((sy - nv.vp.y) * k); };
  if (under) {
    if (!geoBasemap && geoMode() == 1 && geoLayerKind != 2) return;
    // sphere
    {
      Prim p; p.type = Prim::Path; p.group = "basemap"; p.fill = true;
      p.fillC = th.light ? th.bg.mix(Color::hex(0x9fb7d0), 0.10f) : th.bg.mix(Color::hex(0x4a6a8a), 0.10f);
      p.stroke = true; p.strokeC = th.fg.withA(0.16f); p.sw = float(1.0 * k);
      float x, y, wx, wy;
      geoProject(-180, -90, wx, wy); X(wx, wy, x, y); p.d.push_back({'M', x, y, 0, 0});
      for (int lat = -88; lat <= 90; lat += 2) { geoProject(-180, lat, wx, wy); X(wx, wy, x, y); p.d.push_back({'L', x, y, 0, 0}); }
      for (int lat = 90; lat >= -90; lat -= 2) { geoProject(180, lat, wx, wy); X(wx, wy, x, y); p.d.push_back({'L', x, y, 0, 0}); }
      p.d.push_back({'Z', 0, 0, 0, 0});
      sc.items.push_back(std::move(p));
    }
    // graticule
    {
      Prim p; p.type = Prim::Path; p.group = "basemap"; p.stroke = true; p.strokeC = th.fg.withA(0.07f); p.sw = float(0.8 * k);
      float x, y, wx, wy;
      for (int lon = -180; lon <= 180; lon += 30)
        for (int lat = -90; lat <= 90; lat += 2) { geoProject(lon, lat, wx, wy); X(wx, wy, x, y); p.d.push_back({lat == -90 ? 'M' : 'L', x, y, 0, 0}); }
      for (int lat = -60; lat <= 80; lat += 20)
        for (int lon = -180; lon <= 180; lon += 5) { geoProject(lon, lat, wx, wy); X(wx, wy, x, y); p.d.push_back({lon == -180 ? 'M' : 'L', x, y, 0, 0}); }
      sc.items.push_back(std::move(p));
    }
    Color border = th.light ? Color(1, 1, 1, 0.9f) : th.bg.withA(0.9f);
    float W = float(nv.vp.w * k), H = float(nv.vp.h * k);
    for (size_t c = 0; c < geoWorld.size(); c++) {
      const auto& sh = geoWorld[c];
      if (sh.rings.empty()) continue;
      float bx0, by0, bx1, by1;
      X(sh.x0, sh.y0, bx0, by0);
      X(sh.x1, sh.y1, bx1, by1);
      if (bx1 < 0 || by1 < 0 || bx0 > W || by0 > H) continue;  // off-canvas
      Prim p; p.type = Prim::Path; p.group = "basemap"; p.fill = true; p.fillC = fills[c].col; p.stroke = true; p.strokeC = border; p.sw = float(0.7 * k);
      p.evenOdd = true;
      for (auto& ring : sh.rings) {
        float lx = -1e9f, ly = -1e9f;
        for (size_t q = 0; q < ring.size(); q++) {
          float x, y;
          X(ring[q].first, ring[q].second, x, y);
          if (q && std::fabs(x - lx) < 0.15f && std::fabs(y - ly) < 0.15f && q + 1 < ring.size()) continue;  // thin sub-pixel points
          p.d.push_back({q ? 'L' : 'M', x, y, 0, 0});
          lx = x; ly = y;
        }
        p.d.push_back({'Z', 0, 0, 0, 0});
      }
      sc.items.push_back(std::move(p));
    }
    if (geoLayerKind == 2) {
      geoDensity();
      if (!geoDenRgba.empty()) {
        Prim im; im.type = Prim::Image; im.group = "density";
        float ax, ay, bx, by;
        X(geoDenX0, geoDenY0, ax, ay);
        X(geoDenX1, geoDenY1, bx, by);
        im.x = ax; im.y = ay; im.w = bx - ax; im.h = by - ay;
        im.imgW = geoDenW; im.imgH = geoDenH; im.rgba = geoDenRgba;
        sc.items.push_back(std::move(im));
      }
    }
    for (auto& a : arcs) {
      float ax, ay, bx, by, cx, cy;
      X(a.ax, a.ay, ax, ay);
      X(a.bx, a.by, bx, by);
      arcCtrl(ax, ay, bx, by, cx, cy);
      Prim p; p.type = Prim::Path; p.group = "collaborations"; p.stroke = true; p.roundCap = true; p.strokeC = a.col; p.sw = float(a.w * k);
      p.d = {{'M', ax, ay, 0, 0}, {'Q', cx, cy, bx, by}};
      sc.items.push_back(std::move(p));
    }
    std::sort(bubbles.begin(), bubbles.end(), [](auto& a, auto& b) { return a.r > b.r; });
    for (auto& b : bubbles) {
      Prim p; p.type = Prim::Circle; p.group = "bubbles";
      X(b.wx, b.wy, p.x, p.y);
      p.r = float(b.r * k); p.fill = true; p.fillC = b.fill; p.stroke = true; p.strokeC = b.stroke; p.sw = float(1.0 * ui.s * k);
      sc.items.push_back(std::move(p));
    }
  }
}

// ------------------------------------------------------------------ hover card, colour bar, chip (drawn in drawCanvas)
int App::geoHit(float mx, float my) const {
  if (geoWorld.empty()) return -1;
  double wx, wy;
  nv.screenToWorld(mx, my, wx, wy);
  // bubbles first (small / point-only countries)
  if (geoMode() == 2) {
    double zf = clampv(std::sqrt(nv.cam.zoom / std::max(1e-9, nv.fitZoom())), 1.0, 3.0);
    int best = -1;
    float bd = 1e9f;
    for (size_t c = 0; c < geoStats.size(); c++) {
      if (geoStats[c].docs <= 0) continue;
      float sx, sy;
      geoW2S(nv, geoWorld[c].lx, geoWorld[c].ly, sx, sy);
      float r = float((2.5 + 16 * std::sqrt(double(geoStats[c].docs) / geoDocsMax)) * ui.s * zf);
      float d = std::hypot(sx - mx, sy - my);
      if (d <= std::max(r, 5 * ui.s) && d < bd && geoWorld[c].rings.empty()) { bd = d; best = int(c); }
    }
    if (best >= 0) return best;
  }
  for (size_t c = 0; c < geoWorld.size(); c++) {
    const auto& sh = geoWorld[c];
    if (sh.rings.empty() || wx < sh.x0 || wx > sh.x1 || wy < sh.y0 || wy > sh.y1) continue;
    bool in = false;
    for (auto& ring : sh.rings)
      for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        if ((ring[i].second > wy) != (ring[j].second > wy) && wx < (ring[j].first - ring[i].first) * (wy - ring[i].second) / (ring[j].second - ring[i].second) + ring[i].first) in = !in;
    if (in) return int(c);
  }
  return -1;
}

void App::geoChrome(bool mouseFree) {
  int mode = geoMode();
  if (!mode) { geoHover = -1; return; }
  geoEnsureWorld();
  int h = mouseFree && canvasR.has(ui.in.mx, ui.in.my) ? geoHit(ui.in.mx, ui.in.my) : -1;
  if (h != geoHover) { geoHover = h; }
  if (mode == 2) {
    if (geoHover >= 0) {
      const GeoStat& st = geoStats[size_t(geoHover)];
      string t = worldCountry(geoHover).name;
      if (st.docs > 0) {
        t += "\n" + plural(st.docs, "document") + " (" + fmtNum(100.0 * st.docs / std::max(1, geoDocsTotal), 1) + "%) \xC2\xB7 " + fmtInt(st.cites) + " citations";
        t += "\n" + fmtNum(double(st.cites) / st.docs, 1) + " citations per document";
        auto& pp = geoPartners[size_t(geoHover)];
        if (!pp.empty()) {
          t += "\nTop partners: ";
          for (size_t k = 0; k < std::min<size_t>(5, pp.size()); k++) t += (k ? ", " : "") + string(worldCountry(pp[k].first).name) + " (" + std::to_string(pp[k].second) + ")";
        }
        t += "\nClick for its profile, papers and collaborations";
      } else t += "\nNo documents";
      ui.richTip(t);
      ui.cursor = "hand";
    }
    if (mouseFree && ui.in.released[0] && !dragMoved && canvasR.has(ui.in.mx, ui.in.my) && ui.active == 0)
    {
      geoSel = (geoHover >= 0 && geoHover != geoSel && geoStats[size_t(geoHover)].docs > 0) ? geoHover : -1;
      if (geoSel >= 0) { closeDoc(); inspectorOpen = true; ui.scrollTo("inspector", 0); }
    }
  } else if (mode == 1 && geoHover >= 0 && hover < 0) {
    // hovering a country without a node on it
    bool mapped = false;
    for (int c : geoNodeCountry) if (c == geoHover) { mapped = true; break; }
    if (!mapped) ui.richTip(string(worldCountry(geoHover).name) + "\nNot in this map");
  }
}


void App::geoChip() {
  float s = ui.s;
  if (geoMode() != 2) return;
    // chip: turn the overview into a proper country network
    {
    string lbl = "Map co-authorship of countries";
    float w = ui.textW(lbl, 12 * s, 600) + 44 * s;
    Rect r{canvasR.x + canvasR.w / 2 - w / 2, canvasR.y + 14 * s, w, 30 * s};
    addChrome(r);
    if (ui.button(r, lbl, BTN_NORMAL, "globe", !busy())) {
      P->spec.type = AnaType::Coauth;
      P->spec.unit = Unit::Countries;
      P->spec.setDefaults();
      P->spec.min = 1;
      geoAfterBuild = true;
      cmdBuild();
    }
    ui.tip("The map shows the loaded records by country. Build a co-authorship map of countries to see them as a network on the world map.");
    }
}

// legend card for the corpus overview (colour bar + arcs switch); the key follows the layer
void App::geoLegend(float x, float y) {
  if (geoLayerKind == 2) { geoDensityLegend(x, y, geoDenWhat()); return; }
  float s = ui.s;
  Theme th = canvasTheme(P->style);
  Color fg = th.fg, dim = th.muted;
  Color cardBg = th.light ? Color(1, 1, 1, 0.92f) : Color(0.09f, 0.1f, 0.13f, 0.9f), cardBd = th.light ? Color(0, 0, 0, 0.1f) : Color(1, 1, 1, 0.1f);
  bool yearL = geoLayerKind == 1;
  Rect r{x, y, 250 * s, yearL ? 78 * s : 104 * s};
  ui.fill(r, cardBg, 10 * s);
  ui.stroke(r, cardBd, 10 * s);
  addChrome(r);
  ui.text({r.x + 12 * s, r.y + 6 * s, r.w - 24 * s, 18 * s}, yearL ? "Average publication year" : "Documents per country", 11.5f * s, fg, AL_LEFT, 650);
  Rect bar{r.x + 12 * s, r.y + 28 * s, r.w - 24 * s, 9 * s};
  Color land = geoLand(th);
  int K = 48;
  for (int k = 0; k < K; k++) {
    double t = (k + 0.5) / K;
    Color col = yearL ? land.mix(simulateCvd(cmapAt(P->style.scheme, t), P->style.cvd), 0.88f) : land.mix(cmapAt(P->style.scheme, 0.12 + 0.88 * t), float(0.45 + 0.5 * t));
    ui.fill({bar.x + bar.w * k / K, bar.y, bar.w / K + 0.8f, bar.h}, col);
  }
  int nCountries = 0;
  for (auto& st : geoStats) if (st.docs > 0) nCountries++;
  if (yearL) {
    ui.text({bar.x, bar.b() + 2 * s, 80 * s, 14 * s}, fmtFixed(geoYearLo, 1), 10.5f * s, dim);
    ui.text({bar.r() - 80 * s, bar.b() + 2 * s, 80 * s, 14 * s}, fmtFixed(geoYearHi, 1), 10.5f * s, dim, AL_RIGHT);
    ui.text({r.x + 12 * s, r.y + 56 * s, r.w - 24 * s, 16 * s}, std::to_string(nCountries) + " countries \xC2\xB7 5th to 95th percentile", 10.5f * s, dim);
    return;
  }
  ui.text({bar.x, bar.b() + 2 * s, 60 * s, 14 * s}, "1", 10.5f * s, dim);
  ui.text({bar.x + bar.w / 2 - 40 * s, bar.b() + 2 * s, 80 * s, 14 * s}, fmtInt((long long)std::lround(std::sqrt(double(std::max(1, geoDocsMax))))), 10.5f * s, dim, AL_CENTER);
  ui.text({bar.r() - 60 * s, bar.b() + 2 * s, 60 * s, 14 * s}, fmtInt(geoDocsMax), 10.5f * s, dim, AL_RIGHT);
  ui.text({r.x + 12 * s, r.y + 58 * s, r.w - 24 * s, 16 * s}, std::to_string(nCountries) + " countries \xC2\xB7 log scale", 10.5f * s, dim);
  ui.pushId("geoleg");
  Rect tr{r.x + 8 * s, r.y + 76 * s, r.w - 16 * s, 24 * s};
  if (ui.checkbox(tr, "Collaboration arcs (top 150)", geoArcs)) {}
  ui.popId();
}

void App::geoDensityLegend(float x, float y, const string& what) {
  float s = ui.s;
  Theme th = canvasTheme(P->style);
  Color cardBg = th.light ? Color(1, 1, 1, 0.92f) : Color(0.09f, 0.1f, 0.13f, 0.9f), cardBd = th.light ? Color(0, 0, 0, 0.1f) : Color(1, 1, 1, 0.1f);
  Rect r{x, y, 236 * s, 62 * s};
  ui.fill(r, cardBg, 10 * s);
  ui.stroke(r, cardBd, 10 * s);
  addChrome(r);
  (void)what;
  bool clusters = geoDenStyle == 1 && geoMode() == 1 && P->net.nClusters > 0;
  string ttl = clusters ? "Cluster density \xC2\xB7 " + geoDenWhat() : "Density of " + geoDenWhat();
  ui.text({r.x + 12 * s, r.y + 6 * s, r.w - 24 * s, 18 * s}, ttl, 11.5f * s, th.fg, AL_LEFT, 650);
  Rect bar{r.x + 12 * s, r.y + 28 * s, r.w - 24 * s, 9 * s};
  if (clusters) {
    const Encoder& e = nv.enc();
    int K = std::min(P->net.nClusters, 24);
    for (int k = 0; k < K; k++) ui.fill({bar.x + bar.w * k / K + 1, bar.y, bar.w / K - 2, bar.h}, e.clusterColor(k), 2 * s);
    ui.text({bar.x, bar.b() + 2 * s, bar.w, 14 * s}, "Colours: clusters \xC2\xB7 brightness: " + geoDenWhat(), 10.5f * s, th.muted);
    return;
  }
  int K = 48;
  for (int k = 0; k < K; k++) ui.fill({bar.x + bar.w * k / K, bar.y, bar.w / K + 0.8f, bar.h}, simulateCvd(cmapAt(P->style.densityScheme, 0.1 + 0.9 * std::pow((k + 0.5) / K, 0.6)), P->style.cvd));
  ui.text({bar.x, bar.b() + 2 * s, 80 * s, 14 * s}, "Low", 10.5f * s, th.muted);
  ui.text({bar.r() - 80 * s, bar.b() + 2 * s, 80 * s, 14 * s}, "High", 10.5f * s, th.muted, AL_RIGHT);
}

// layer switch floating at the top right of the Geo view (replaces the view chip there)
void App::geoLayerSwitch() {
  float s = ui.s;
  int mode = geoMode();
  if (!mode) return;
  vector<string> opts = {mode == 1 ? "Network" : "Countries", "Overlay", "Density"};
  float w = 0;
  for (auto& o : opts) w += ui.textW(o, 12 * s, 600) + 26 * s;
  Rect r{canvasR.r() - w - 14 * s, canvasR.y + 14 * s, w, 30 * s};
  Theme th = canvasTheme(P->style);
  Rect bg{r.x - 3 * s, r.y - 3 * s, r.w + 6 * s, r.h + 6 * s};
  ui.fill(bg, th.light ? Color(1, 1, 1, 0.92f) : Color(0.09f, 0.1f, 0.13f, 0.9f), 9 * s);
  ui.stroke(bg, th.light ? Color(0, 0, 0, 0.1f) : Color(1, 1, 1, 0.1f), 9 * s);
  addChrome(bg);
  int k = geoLayerKind;
  if (ui.segmented(r, opts, k, "geolayer")) geoLayerKind = k;
  ui.tip(mode == 1 ? "Network: cluster colours. Overlay: colours by the overlay score. Density: where the countries of the map concentrate."
                   : "Countries: documents per country with collaborations. Overlay: average publication year. Density: where the documents concentrate.");
  if (geoLayerKind != 2) return;
  // density options: how it is drawn and what it is weighted by
  vector<string> st = mode == 1 ? vector<string>{"Heat", "Clusters", "Countries"} : vector<string>{"Heat", "Countries"};
  static const vector<string> ms = {"Documents", "Citations", "Citations per document", "Collaboration"};
  float cw = std::max(w, 250 * s);
  Rect card{r.r() - cw - 3 * s, bg.b() + 6 * s, cw + 6 * s, 80 * s};
  ui.fill(card, th.light ? Color(1, 1, 1, 0.92f) : Color(0.09f, 0.1f, 0.13f, 0.9f), 9 * s);
  ui.stroke(card, th.light ? Color(0, 0, 0, 0.1f) : Color(1, 1, 1, 0.1f), 9 * s);
  addChrome(card);
  Rect r1{card.x + 6 * s, card.y + 6 * s, card.w - 12 * s, 30 * s};
  int si = mode == 1 ? geoDenStyle : (geoDenStyle == 2 ? 1 : 0);
  if (ui.segmented(r1, st, si, "geodenstyle")) geoDenStyle = mode == 1 ? si : (si == 1 ? 2 : 0);
  ui.tip(mode == 1 ? "Heat: a smooth density surface. Clusters: the surface takes the colours of the clusters (cluster density). Countries: each country shaded by its value."
                   : "Heat: a smooth density surface. Countries: each country shaded by its value.");
  Rect r2{card.x + 6 * s, r1.b() + 6 * s, card.w - 12 * s, 30 * s};
  ui.text({r2.x + 4 * s, r2.y, 90 * s, r2.h}, "Weighted by", 12 * s, th.muted);
  int mi = geoDenMeasure;
  if (ui.combo({r2.x + 94 * s, r2.y, r2.w - 94 * s, r2.h}, "geodenmeasure", ms, mi)) geoDenMeasure = mi;
  ui.tip("Documents: papers with an author in the country. Citations: citations those papers received. Citations per document: their average (countries with at least two papers). "
         "Collaboration: international co-authorship (total link strength on a country map).");
}

}  // namespace win
}  // namespace vs

namespace vs { namespace win {
void App::geoFitBounds(ViewKind v) {
  if (v == ViewKind::Geo) {
    float x0, y0, x1, y1, t;
    geoProject(-180, 0, x0, t);
    geoProject(180, 0, x1, t);
    geoProject(0, 83, t, y0);
    geoProject(0, -57, t, y1);
    nv.fitX0 = x0; nv.fitX1 = x1; nv.fitY0 = y0; nv.fitY1 = y1;
  } else nv.fitX0 = nv.fitX1 = 0;
}

// ------------------------------------------------------------------ publication figure panel (Geo)
// The world between 57°S and 83°N (as on screen, Antarctica left out), coloured exactly like the Geo view:
// a map of countries puts the nodes on their countries; any other map gives the documents-per-country
// choropleth with collaboration arcs and bubbles. Everything is vector.
FigPanelDef App::geoFigurePanel() {
  FigPanelDef d;
  d.kind = ViewKind::Geo;
  geoUpdate();
  const int mode = geoModeA ? 1 : (geoDocsMax > 0 ? 2 : 0);
  if (mode == 0) {
    d.note = "Geo: no country information in these records";
    d.nodes = d.links = d.labels = d.clusterNames = d.clusterLegend = false;
    return d;
  }
  geoEnsureWorld();
  const double LAT_N = 83, LAT_S = -57;
  float x0, y0, x1, y1, t;
  geoProject(-180, 0, x0, t);
  geoProject(180, 0, x1, t);
  geoProject(0, LAT_N, t, y0);
  geoProject(0, LAT_S, t, y1);
  d.bounds = true;
  d.bx0 = x0; d.by0 = y0; d.bx1 = x1; d.by1 = y1;
  const Network& N = P->net;
  if (mode == 1) {
    d.pos.resize(size_t(N.n()));
    d.hidden.assign(size_t(N.n()), 1);
    for (int i = 0; i < N.n(); i++) {
      int c = i < int(geoNodeCountry.size()) ? geoNodeCountry[size_t(i)] : -1;
      if (c < 0) { d.pos[size_t(i)] = {0, 0}; continue; }
      float x, y;
      geoProject(worldCountry(c).labelLon, worldCountry(c).labelLat, x, y);
      d.pos[size_t(i)] = {x, y};
      d.hidden[size_t(i)] = 0;
    }
    d.clusterNames = false;
    if (geoLayerKind == 1) { d.colorAs = int(ViewKind::Overlay); d.clusterLegend = false; }
    if (geoLayerKind == 2) { d.nodes = d.links = false; d.clusterLegend = false; }
  } else {
    d.nodes = d.links = d.labels = d.clusterNames = d.clusterLegend = false;
  }
  const double worldW = double(x1 - x0);
  d.under = [this, mode, worldW, LAT_N, LAT_S](Scene& sc, const FigPanelCtx& c) {
    Theme th = canvasTheme(P->style);
    th.bg = c.bg; th.fg = c.fg; th.muted = c.muted; th.light = c.light;
    vector<GeoFill> fills;
    vector<GeoBubble> bubbles;
    vector<GeoArc> arcs;
    geoFig_ = true;
    geoFigPx_ = worldW * c.s / 1000.0;  // on screen the world is about 1000 px wide
    geoLayer(th, fills, bubbles, arcs);
    geoFig_ = false;
    auto X = [&](double lon, double lat, float& x, float& y) { float wx, wy; geoProject(lon, lat, wx, wy); x = float(wx * c.s + c.ox); y = float(wy * c.s + c.oy); };
    auto W = [&](float wx, float wy, float& x, float& y) { x = float(wx * c.s + c.ox); y = float(wy * c.s + c.oy); };
    if (mode == 2 || geoBasemap || geoLayerKind == 2) {
      // ocean: the projected band between LAT_S and LAT_N
      Prim o; o.type = Prim::Path; o.group = "basemap"; o.fill = true;
      o.fillC = th.light ? th.bg.mix(Color::hex(0x9fb7d0), 0.12f) : th.bg.mix(Color::hex(0x4a6a8a), 0.12f);
      o.stroke = true; o.strokeC = th.fg.withA(0.18f); o.sw = 0.4f;
      float x, y;
      X(-180, LAT_S, x, y); o.d.push_back({'M', x, y, 0, 0});
      for (double lat = LAT_S + 2; lat <= LAT_N; lat += 2) { X(-180, lat, x, y); o.d.push_back({'L', x, y, 0, 0}); }
      for (int lon = -175; lon <= 180; lon += 5) { X(lon, LAT_N, x, y); o.d.push_back({'L', x, y, 0, 0}); }
      for (double lat = LAT_N - 2; lat >= LAT_S; lat -= 2) { X(180, lat, x, y); o.d.push_back({'L', x, y, 0, 0}); }
      o.d.push_back({'Z', 0, 0, 0, 0});
      sc.items.push_back(o);
      // graticule
      // one path per line: some rasterisers stroke multi-figure open paths incorrectly
      Prim g; g.type = Prim::Path; g.group = "basemap"; g.stroke = true; g.strokeC = th.fg.withA(0.08f); g.sw = 0.3f;
      for (int lon = -150; lon <= 150; lon += 30) {
        g.d.clear();
        for (double lat = LAT_S; lat <= LAT_N; lat += 2) { X(lon, lat, x, y); g.d.push_back({lat == LAT_S ? 'M' : 'L', x, y, 0, 0}); }
        sc.items.push_back(g);
      }
      for (int lat = -40; lat <= 80; lat += 20) {
        g.d.clear();
        for (int lon = -180; lon <= 180; lon += 5) { X(lon, lat, x, y); g.d.push_back({lon == -180 ? 'M' : 'L', x, y, 0, 0}); }
        sc.items.push_back(g);
      }
      // countries
      Color border = th.light ? Color(1, 1, 1, 0.95f) : th.bg.withA(0.9f);
      for (size_t k = 0; k < geoWorld.size(); k++) {
        const auto& sh = geoWorld[k];
        if (sh.rings.empty() || iequals(worldCountry(int(k)).name, "Antarctica")) continue;
        Prim p; p.type = Prim::Path; p.group = "basemap"; p.fill = true; p.fillC = fills[k].col; p.stroke = true; p.strokeC = border; p.sw = 0.3f; p.evenOdd = true;
        // rings are thinned to 0.2 pt (invisible even in print) and specks below 0.3 pt are dropped:
        // near-coincident vertices and degenerate figures upset some rasterisers and bloat exports
        vector<std::pair<float, float>> pts;
        for (auto& ring : sh.rings) {
          pts.clear();
          float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
          for (size_t q = 0; q < ring.size(); q++) {
            W(ring[q].first, ring[q].second, x, y);
            if (!pts.empty() && std::fabs(x - pts.back().first) < 0.2f && std::fabs(y - pts.back().second) < 0.2f) continue;
            pts.push_back({x, y});
            bx0 = std::min(bx0, x); by0 = std::min(by0, y); bx1 = std::max(bx1, x); by1 = std::max(by1, y);
          }
          while (pts.size() > 1 && std::fabs(pts.back().first - pts[0].first) < 0.2f && std::fabs(pts.back().second - pts[0].second) < 0.2f) pts.pop_back();
          if (pts.size() < 3 || (bx1 - bx0 < 0.3f && by1 - by0 < 0.3f)) continue;
          for (size_t q = 0; q < pts.size(); q++) p.d.push_back({q ? 'L' : 'M', pts[q].first, pts[q].second, 0, 0});
          p.d.push_back({'Z', 0, 0, 0, 0});
        }
        if (!p.d.empty()) sc.items.push_back(std::move(p));
      }
    }
    if (geoLayerKind == 2) {
      geoFig_ = true;
      geoDensity();
      geoFig_ = false;
      if (!geoDenRgba.empty()) {
        Prim im; im.type = Prim::Image; im.group = "density";
        float ax, ay, bx, by;
        W(geoDenX0, geoDenY0, ax, ay);
        W(geoDenX1, geoDenY1, bx, by);
        im.x = ax; im.y = ay; im.w = bx - ax; im.h = by - ay;
        im.imgW = geoDenW; im.imgH = geoDenH; im.rgba = geoDenRgba;
        sc.items.push_back(std::move(im));
      }
    }
    for (size_t k = arcs.size(); k-- > 0;) {  // weakest first
      const auto& a = arcs[k];
      float ax, ay, bx, by, cx, cy;
      W(a.ax, a.ay, ax, ay);
      W(a.bx, a.by, bx, by);
      if (std::hypot(bx - ax, by - ay) < 0.5f) continue;  // zero-length arc: invisible, and some rasterisers stroke it as a spike
      arcCtrl(ax, ay, bx, by, cx, cy);
      Prim p; p.type = Prim::Path; p.group = "collaborations"; p.stroke = true; p.roundCap = true; p.strokeC = a.col; p.sw = std::max(0.25f, a.w);
      p.d = {{'M', ax, ay, 0, 0}, {'Q', cx, cy, bx, by}};
      sc.items.push_back(std::move(p));
    }
    std::sort(bubbles.begin(), bubbles.end(), [](const GeoBubble& a, const GeoBubble& b) { return a.r > b.r; });
    for (const auto& b : bubbles) {
      Prim p; p.type = Prim::Circle; p.group = "bubbles";
      W(b.wx, b.wy, p.x, p.y);
      p.r = std::max(0.6f, b.r); p.fill = true; p.fillC = b.fill; p.stroke = true; p.strokeC = b.stroke; p.sw = 0.35f;
      sc.items.push_back(std::move(p));
    }
  };
  {  // colour key for the layer, bottom-left of the panel
    string ttl, lo, hi;
    vector<Color> ramp;
    Theme th0 = canvasTheme(P->style);
    Color land = geoLand(th0);
    const Encoder& e = nv.enc();
    for (int q = 0; q < 40; q++) {
      double u = (q + 0.5) / 40;
      if (geoLayerKind == 2 && geoDenStyle == 1 && mode == 1 && P->net.nClusters > 0) ramp.push_back(e.clusterColor(std::min(P->net.nClusters - 1, int(u * P->net.nClusters))));
      else if (geoLayerKind == 2) ramp.push_back(cmapAt(P->style.densityScheme, 0.1 + 0.9 * std::pow(u, 0.6)));
      else if (geoLayerKind == 1 || mode == 1) ramp.push_back(simulateCvd(cmapAt(P->style.scheme, u), P->style.cvd));
      else ramp.push_back(land.mix(cmapAt(P->style.scheme, 0.12 + 0.88 * u), float(0.45 + 0.5 * u)));
    }
    if (geoLayerKind == 2) {
      bool cl = geoDenStyle == 1 && mode == 1 && P->net.nClusters > 0;
      ttl = cl ? "Cluster density (" + geoDenWhat() + ")" : "Density of " + geoDenWhat();
      lo = cl ? "Clusters" : "Low";
      hi = cl ? "" : "High";
    }
    else if (geoLayerKind == 1 && mode == 2 && geoYearHi > 0) { ttl = "Average publication year"; lo = fmtFixed(geoYearLo, 1); hi = fmtFixed(geoYearHi, 1); }
    else if (geoLayerKind == 1 && mode == 1 && P->net.scoreIdx >= 0) {
      ttl = P->net.scoreIdx < int(P->net.scoreNames.size()) ? P->net.scoreNames[size_t(P->net.scoreIdx)] : "Score";
      bool yr = contains(lower(ttl), "year");
      lo = yr ? fmtFixed(e.sMin, 1) : fmtNum(e.sMin, 2); hi = yr ? fmtFixed(e.sMax, 1) : fmtNum(e.sMax, 2);
    } else if (mode == 2 && geoLayerKind == 0) { ttl = "Documents per country (log)"; lo = "1"; hi = fmtInt(geoDocsMax); }
    if (!ttl.empty()) {
      d.over = [ttl, lo, hi, ramp](Scene& sc, const FigPanelCtx& c) {
        double bw = std::min(110.0, c.rw * 0.3), bh = c.fs * 0.7, x = c.rx + 3, y = c.ry + c.rh - c.fs * 2.4;
        Prim tt; tt.type = Prim::Text; tt.group = "legend"; tt.x = float(x); tt.y = float(y - 2); tt.text = ttl; tt.size = float(c.fs * 0.9); tt.fill = true; tt.fillC = c.fg; tt.bold = true;
        sc.items.push_back(tt);
        int K = int(ramp.size());
        for (int q = 0; q < K; q++) {
          Prim r; r.type = Prim::Rect; r.group = "legend"; r.x = float(x + bw * q / K); r.y = float(y + 1); r.w = float(bw / K + 0.3); r.h = float(bh); r.fill = true;
          r.fillC = ramp[size_t(q)];
          sc.items.push_back(r);
        }
        auto lab = [&](double lx, const string& s, int anchor) {
          Prim p; p.type = Prim::Text; p.group = "legend"; p.x = float(lx); p.y = float(y + bh + c.fs * 1.05); p.text = s; p.size = float(c.fs * 0.8); p.fill = true; p.fillC = c.muted; p.anchor = anchor;
          sc.items.push_back(p);
        };
        lab(x, lo, 0);
        lab(x + bw, hi, 2);
      };
    }
  }
  return d;
}
}}
