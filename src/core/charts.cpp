#include "charts.h"

namespace vs {

ChartTheme ChartTheme::make(bool light) {
  ChartTheme t;
  t.light = light;
  if (light) { t.bg = Color(1, 1, 1); t.fg = Color::hex(0x1b1e24); t.muted = Color::hex(0x6b717c); t.grid = Color(0, 0, 0, 0.08f); t.accent = Color::hex(0x2f6fe4); t.accent2 = Color::hex(0xe69f00); }
  else { t.bg = Color::hex(0x15181d); t.fg = Color::hex(0xe6e9ef); t.muted = Color::hex(0x9aa1ad); t.grid = Color(1, 1, 1, 0.08f); t.accent = Color::hex(0x4f8ef7); t.accent2 = Color::hex(0xf2a33c); }
  t.series = palette("okabe");
  return t;
}

namespace {
Prim T(double x, double y, const string& s, double size, const Color& c, int anchor = 0, bool bold = false) {
  Prim p; p.type = Prim::Text; p.x = float(x); p.y = float(y); p.text = s; p.size = float(size); p.fill = true; p.fillC = c; p.anchor = anchor; p.bold = bold; p.group = "text";
  return p;
}
Prim R(double x, double y, double w, double h, const Color& c, const string& g = "shapes") {
  Prim p; p.type = Prim::Rect; p.x = float(x); p.y = float(y); p.w = float(w); p.h = float(h); p.fill = true; p.fillC = c; p.group = g;
  return p;
}
Prim L(double x0, double y0, double x1, double y1, const Color& c, double w, bool dash = false) {
  Prim p; p.type = Prim::Path; p.d = {{'M', float(x0), float(y0), 0, 0}, {'L', float(x1), float(y1), 0, 0}}; p.stroke = true; p.strokeC = c; p.sw = float(w); p.group = "axes";
  if (dash) p.dash = {2.5f, 2.5f};
  return p;
}
Prim C(double x, double y, double r, const Color& c, const string& g = "shapes") {
  Prim p; p.type = Prim::Circle; p.x = float(x); p.y = float(y); p.r = float(r); p.fill = true; p.fillC = c; p.group = g;
  return p;
}
Scene base(double w, double h, const ChartTheme& t) {
  Scene s;
  s.W = w; s.H = h;
  if (!t.transparent) s.items.push_back(R(0, 0, w, h, t.bg, "background"));
  return s;
}
double niceStep(double range, int ticks) {
  double raw = range / std::max(1, ticks);
  double mag = std::pow(10, std::floor(std::log10(std::max(1e-12, raw))));
  double n = raw / mag;
  return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}
string tickLabel(double v) {
  if (std::fabs(v) >= 1000) return fmtInt((long long)std::lround(v));
  if (std::fabs(v - std::round(v)) < 1e-9) return std::to_string((long long)std::lround(v));
  return fmtNum(v, std::fabs(v) < 1 ? 2 : 1);
}
struct Axes {
  double x0, y0, x1, y1;  // plot rect in pt
  double vx0, vx1, vy0, vy1;
  double X(double v) const { return x0 + (v - vx0) / std::max(1e-12, vx1 - vx0) * (x1 - x0); }
  double Y(double v) const { return y1 - (v - vy0) / std::max(1e-12, vy1 - vy0) * (y1 - y0); }
};
void yAxis(Scene& s, const Axes& a, const ChartTheme& t, const string& title, bool right = false) {
  double st = niceStep(a.vy1 - a.vy0, 5);
  for (double v = std::ceil(a.vy0 / st) * st; v <= a.vy1 + 1e-9; v += st) {
    double y = a.Y(v);
    if (!right) s.items.push_back(L(a.x0, y, a.x1, y, t.grid, 0.5));
    s.items.push_back(T(right ? a.x1 + 4 : a.x0 - 4, y + t.font * 0.33, tickLabel(v), t.font * 0.85, t.muted, right ? 0 : 2));
  }
  if (!title.empty()) s.items.push_back(T(right ? a.x1 : a.x0, a.y0 - t.font * 0.9, title, t.font * 0.9, t.muted, right ? 2 : 0));
}
}  // namespace

Scene chartGrowth(const Growth& g, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (g.perYear.empty()) { s.items.push_back(T(w / 2, h / 2, "No publication years in the data", t.font, t.muted, 1)); return s; }
  double maxN = 1, maxC = 1e-9;
  for (auto& p : g.perYear) maxN = std::max(maxN, double(p.second));
  for (auto& p : g.citesPerYear) maxC = std::max(maxC, p.second);
  Axes a{36, 22, w - 36, h - 24, g.y0 - 0.6, g.y1 + 0.6, 0, maxN * 1.12};
  yAxis(s, a, t, "Documents");
  Axes b = a;
  b.vy1 = maxC * 1.12;
  yAxis(s, b, t, "Citations / doc", true);
  double bw = (a.x1 - a.x0) / (g.perYear.size() + 0.2) * 0.62;
  int k = 0;
  for (auto& p : g.perYear) {
    double x = a.X(p.first), y = a.Y(p.second);
    Prim r = R(x - bw / 2, y, bw, a.y1 - y, t.accent.withA(0.85f));
    r.group = "hit:" + std::to_string(k++);
    r.tip = std::to_string(p.first) + "\n" + fmtInt(p.second) + (p.second == 1 ? " document" : " documents");
    s.items.push_back(r);
    if (g.perYear.size() <= 14 || p.first % 2 == 0) s.items.push_back(T(x, a.y1 + t.font * 1.1, std::to_string(p.first), t.font * 0.85, t.muted, 1));
  }
  Prim line; line.type = Prim::Path; line.stroke = true; line.strokeC = t.accent2; line.sw = 1.6f; line.group = "series"; line.roundCap = true;
  for (size_t i = 0; i < g.citesPerYear.size(); i++) line.d.push_back({i ? 'L' : 'M', float(b.X(g.citesPerYear[i].first)), float(b.Y(g.citesPerYear[i].second)), 0, 0});
  s.items.push_back(line);
  for (auto& p : g.citesPerYear) { Prim c = C(b.X(p.first), b.Y(p.second), 2.2, t.accent2); c.tip = std::to_string(p.first) + "\n" + fmtNum(p.second, 1) + " citations per document"; s.items.push_back(c); }
  string cg = std::isfinite(g.cagr) && g.cagr != 0 ? "CAGR " + fmtNum(g.cagr * 100, 1) + "%" : "";
  s.items.push_back(T(w / 2, 12, cg, t.font * 0.9, t.fg, 1, true));
  return s;
}

Scene chartStrategic(const vector<ClusterInfo>& Cl, const vector<Color>& colors, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (Cl.empty()) { s.items.push_back(T(w / 2, h / 2, "Build a clustered map first", t.font, t.muted, 1)); return s; }
  vector<double> cx, dy;
  for (auto& c : Cl) { cx.push_back(c.centrality); dy.push_back(c.density); }
  auto med = [](vector<double> v) { std::sort(v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2; };
  double mx = med(cx), my = med(dy);
  double x0 = *std::min_element(cx.begin(), cx.end()), x1 = *std::max_element(cx.begin(), cx.end());
  double y0 = *std::min_element(dy.begin(), dy.end()), y1 = *std::max_element(dy.begin(), dy.end());
  double px = std::max(1e-9, (x1 - x0) * 0.3), py = std::max(1e-9, (y1 - y0) * 0.3);
  Axes a{12, 22, w - 12, h - 26, x0 - px, x1 + px, y0 - py, y1 + py};
  s.items.push_back(R(a.x0, a.y0, a.x1 - a.x0, a.y1 - a.y0, t.grid.withA(t.grid.a * 0.5f)));
  s.items.push_back(L(a.X(mx), a.y0, a.X(mx), a.y1, t.muted.withA(0.6f), 0.7, true));
  s.items.push_back(L(a.x0, a.Y(my), a.x1, a.Y(my), t.muted.withA(0.6f), 0.7, true));
  double f = t.font * 0.85;
  s.items.push_back(T(a.x1 - 4, a.y0 + f + 2, "Motor themes", f, t.muted.withA(0.8f), 2, true));
  s.items.push_back(T(a.x0 + 4, a.y0 + f + 2, "Niche themes", f, t.muted.withA(0.8f), 0, true));
  s.items.push_back(T(a.x0 + 4, a.y1 - 4, "Emerging or declining", f, t.muted, 0, true));
  s.items.push_back(T(a.x1 - 4, a.y1 - 4, "Basic themes", f, t.muted, 2, true));
  s.items.push_back(T((a.x0 + a.x1) / 2, h - 6, "Centrality (external link strength) \xE2\x86\x92", f, t.muted, 1));
  s.items.push_back(T(a.x0, a.y0 - 6, "\xE2\x86\x91 Density (internal link strength)", f, t.muted, 0));
  double maxW = 1e-9;
  for (auto& c : Cl) maxW = std::max(maxW, c.weight);
  for (size_t i = 0; i < Cl.size(); i++) {
    auto& c = Cl[i];
    double r = 5 + 16 * std::sqrt(c.weight / maxW);
    Color col = colors.empty() ? t.accent : colors[size_t(c.c) % colors.size()];
    Prim cp = C(a.X(c.centrality), a.Y(c.density), r, col.withA(0.55f));
    cp.stroke = true; cp.strokeC = col; cp.sw = 1.1f; cp.group = "hit:" + std::to_string(c.c);
    cp.tip = c.autoName + "\n" + std::to_string(c.size) + " items \xC2\xB7 " + c.quadrant + "\nCentrality " + fmtNum(c.centrality, 1) + " \xC2\xB7 density " + fmtNum(c.density, 2) +
             (std::isfinite(c.avgYear) ? "\nAvg. year " + fmtNum(c.avgYear, 1) : "");
    s.items.push_back(cp);
    string nm = truncate(c.autoName, 30);
    double tw = textWidth(nm, f, true), lx = std::clamp(a.X(c.centrality), a.x0 + tw / 2 + 2, a.x1 - tw / 2 - 2);
    double ly = a.Y(c.density) - r - 3;
    if (ly < a.y0 + f * 2.2) ly = a.Y(c.density) + r + f + 1;
    Prim tl = T(lx, ly, nm, f, t.fg, 1, true);
    tl.halo = true; tl.haloC = t.bg; tl.haloW = 1.6f;
    s.items.push_back(tl);
  }
  return s;
}

Scene chartBursts(const vector<Burst>& b, int y0, int y1, int top, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  int n = std::min(int(b.size()), top);
  if (!n || y1 <= y0) { s.items.push_back(T(w / 2, h / 2, "No bursts detected (try lowering the minimum occurrences)", t.font, t.muted, 1)); return s; }
  double labW = 0;
  for (int i = 0; i < n; i++) labW = std::max(labW, textWidth(truncate(b[i].label, 32), t.font * 0.9, false));
  labW = std::min(labW + 10, w * 0.42);
  double sw = 46;
  Axes a{labW, 22, w - sw - 6, h - 6, y0 - 0.5, y1 + 0.5, 0, 1};
  double rh = (a.y1 - a.y0) / n;
  for (int y = y0; y <= y1; y++) {
    if ((y1 - y0) > 12 && (y - y0) % 2) continue;
    s.items.push_back(T(a.X(y), 14, std::to_string(y), t.font * 0.8, t.muted, 1));
  }
  s.items.push_back(T(w - 4, 14, "Strength", t.font * 0.8, t.muted, 2));
  double maxS = b[0].strength;
  for (int i = 0; i < n; i++) {
    double y = a.y0 + i * rh;
    if (i % 2 == 0) s.items.push_back(R(0, y, w, rh, t.grid.withA(t.grid.a * 0.4f)));
    s.items.push_back(T(labW - 6, y + rh / 2 + t.font * 0.3, truncate(b[i].label, 32), t.font * 0.9, t.fg, 2));
    s.items.push_back(R(a.X(y0 - 0.5), y + rh * 0.42, a.X(y1 + 0.5) - a.X(y0 - 0.5), rh * 0.16, t.muted.withA(0.25f)));
    Prim bar = R(a.X(b[i].start - 0.5), y + rh * 0.22, a.X(b[i].end + 0.5) - a.X(b[i].start - 0.5), rh * 0.56, Color::hex(0xd55e00).mix(t.accent, float(1 - b[i].strength / maxS) * 0.5f));
    bar.group = "hit:" + std::to_string(i);
    bar.tip = b[i].label + "\nBurst " + std::to_string(b[i].start) + (b[i].end != b[i].start ? "\xE2\x80\x93" + std::to_string(b[i].end) : "") + " \xC2\xB7 strength " + fmtNum(b[i].strength, 2) +
              (b[i].total ? "\n" + std::to_string(b[i].total) + " documents in total" : "");
    s.items.push_back(bar);
    s.items.push_back(T(w - 4, y + rh / 2 + t.font * 0.3, fmtNum(b[i].strength, 2), t.font * 0.85, t.muted, 2));
  }
  return s;
}

Scene chartSankey(const Evolution& e, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (e.themes.empty()) { s.items.push_back(T(w / 2, h / 2, "Not enough data per period for thematic maps", t.font, t.muted, 1)); return s; }
  size_t P = e.periods.size();
  double colW = 12, top = 26, bot = h - 8, gapY = 6;
  double labR = std::min(130.0, w * 0.26);
  double span = (w - 16 - colW - labR) / std::max<size_t>(1, P - 1);
  vector<double> total(P, 0);
  vector<vector<int>> byP(P);
  for (size_t i = 0; i < e.themes.size(); i++) { total[size_t(e.themes[i].period)] += e.themes[i].size; byP[size_t(e.themes[i].period)].push_back(int(i)); }
  double maxT = *std::max_element(total.begin(), total.end());
  size_t maxN = 1;
  for (auto& v : byP) maxN = std::max(maxN, v.size());
  double k = (bot - top - gapY * double(maxN - 1)) / std::max(1.0, maxT);
  vector<double> ty(e.themes.size()), th(e.themes.size()), tx(e.themes.size());
  for (size_t p = 0; p < P; p++) {
    double x = 8 + span * double(p);
    s.items.push_back(T(x, 14, std::to_string(e.periods[p].first) + "\xE2\x80\x93" + std::to_string(e.periods[p].second), t.font * 0.95, t.fg, 0, true));
    std::sort(byP[p].begin(), byP[p].end(), [&](int a, int b) { return e.themes[size_t(a)].size > e.themes[size_t(b)].size; });
    double y = top;
    for (int i : byP[p]) { tx[size_t(i)] = x; ty[size_t(i)] = y; th[size_t(i)] = std::max(4.0, e.themes[size_t(i)].size * k); y += th[size_t(i)] + gapY; }
  }
  // flows (stacked offsets per theme)
  vector<double> outOff(e.themes.size(), 0), inOff(e.themes.size(), 0);
  vector<double> outTot(e.themes.size(), 0), inTot(e.themes.size(), 0);
  for (auto& f : e.flows) { outTot[size_t(f.from)] += f.w; inTot[size_t(f.to)] += f.w; }
  int k2 = 0;
  for (auto& f : e.flows) {
    size_t a = size_t(f.from), b = size_t(f.to);
    double ha = th[a] * f.w / std::max(1e-9, outTot[a]), hb = th[b] * f.w / std::max(1e-9, inTot[b]);
    double ya = ty[a] + outOff[a], yb = ty[b] + inOff[b];
    outOff[a] += ha; inOff[b] += hb;
    double xa = tx[a] + colW, xb = tx[b], xm = (xa + xb) / 2;
    Prim p; p.type = Prim::Path; p.group = "hit:" + std::to_string(k2++); p.fill = true;
    Color col = t.series[a % t.series.size()];
    p.fillC = col.withA(0.28f);
    p.tip = e.themes[a].name + "  \xE2\x86\x92  " + e.themes[b].name + "\nShared keywords weight " + fmtNum(f.w, f.w < 10 ? 2 : 0);
    // two quadratic halves per edge approximate a cubic S-curve
    p.d = {{'M', float(xa), float(ya), 0, 0}, {'Q', float(xm), float(ya), float(xm), float((ya + yb) / 2)}, {'Q', float(xm), float(yb), float(xb), float(yb)},
           {'L', float(xb), float(yb + hb), 0, 0}, {'Q', float(xm), float(yb + hb), float(xm), float((ya + ha + yb + hb) / 2)}, {'Q', float(xm), float(ya + ha), float(xa), float(ya + ha)}, {'Z', 0, 0, 0, 0}};
    s.items.push_back(p);
  }
  for (size_t i = 0; i < e.themes.size(); i++) {
    Color col = t.series[i % t.series.size()];
    {
      Prim tr = R(tx[i], ty[i], colW, th[i], col);
      tr.tip = e.themes[i].name + "\n" + std::to_string(e.periods[size_t(e.themes[i].period)].first) + "\xE2\x80\x93" + std::to_string(e.periods[size_t(e.themes[i].period)].second) + " \xC2\xB7 " + std::to_string(e.themes[i].size) + " keywords";
      for (size_t q = 0; q < std::min<size_t>(6, e.themes[i].keywords.size()); q++) tr.tip += (q ? ", " : "\n") + e.themes[i].keywords[q];
      s.items.push_back(tr);
    }
    Prim lab = T(tx[i] + colW + 4, ty[i] + std::min(th[i] / 2, 12.0) + t.font * 0.3, truncate(e.themes[i].name, 24) + " (" + std::to_string(e.themes[i].size) + ")", t.font * 0.85, t.fg, 0);
    lab.halo = true; lab.haloC = t.bg; lab.haloW = 1.4f;
    s.items.push_back(lab);
  }
  return s;
}

Scene chartThreeField(const ThreeField& f, const string& lt, const string& mt, const string& rt, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (f.left.empty() || f.mid.empty() || f.right.empty()) { s.items.push_back(T(w / 2, h / 2, "Not enough data for a three-field plot", t.font, t.muted, 1)); return s; }
  double colW = 10, top = 24, bot = h - 6;
  double xs[3] = {w * 0.22, w * 0.5 - colW / 2, w * 0.78 - colW};
  const vector<string>* names[3] = {&f.left, &f.mid, &f.right};
  const vector<int>* cnts[3] = {&f.leftN, &f.midN, &f.rightN};
  const string titles[3] = {lt, mt, rt};
  vector<vector<double>> Y(3), H(3);
  for (int c = 0; c < 3; c++) {
    double tot = 0;
    for (int v : *cnts[c]) tot += v;
    double gap = 4, k = (bot - top - gap * double(cnts[c]->size() - 1)) / std::max(1.0, tot);
    double y = top;
    for (int v : *cnts[c]) { Y[c].push_back(y); H[c].push_back(std::max(3.0, v * k)); y += std::max(3.0, v * k) + gap; }
    s.items.push_back(T(xs[c] + colW / 2, 13, titles[c], t.font, t.fg, 1, true));
  }
  auto flows = [&](const vector<std::tuple<int, int, int>>& fl, int ca, int cb) {
    vector<double> tA(Y[ca].size(), 0), tB(Y[cb].size(), 0), oA(Y[ca].size(), 0), oB(Y[cb].size(), 0);
    for (auto& e : fl) { tA[size_t(std::get<0>(e))] += std::get<2>(e); tB[size_t(std::get<1>(e))] += std::get<2>(e); }
    for (auto& e : fl) {
      size_t a = size_t(std::get<0>(e)), b = size_t(std::get<1>(e));
      double ha = H[ca][a] * std::get<2>(e) / tA[a], hb = H[cb][b] * std::get<2>(e) / tB[b];
      double ya = Y[ca][a] + oA[a], yb = Y[cb][b] + oB[b];
      oA[a] += ha; oB[b] += hb;
      double xa = xs[ca] + colW, xb = xs[cb], xm = (xa + xb) / 2;
      Prim p; p.type = Prim::Path; p.fill = true; p.group = "flows";
      p.tip = (*names[ca])[a] + "  \xE2\x86\x92  " + (*names[cb])[b] + "\n" + std::to_string(std::get<2>(e)) + " documents";
      p.fillC = t.series[(ca == 0 ? a : b) % t.series.size()].withA(0.25f);
      p.d = {{'M', float(xa), float(ya), 0, 0}, {'Q', float(xm), float(ya), float(xm), float((ya + yb) / 2)}, {'Q', float(xm), float(yb), float(xb), float(yb)},
             {'L', float(xb), float(yb + hb), 0, 0}, {'Q', float(xm), float(yb + hb), float(xm), float((ya + ha + yb + hb) / 2)}, {'Q', float(xm), float(ya + ha), float(xa), float(ya + ha)}, {'Z', 0, 0, 0, 0}};
      s.items.push_back(p);
    }
  };
  flows(f.lm, 0, 1);
  flows(f.mr, 1, 2);
  for (int c = 0; c < 3; c++)
    for (size_t i = 0; i < Y[c].size(); i++) {
      {
        Prim nr = R(xs[c], Y[c][i], colW, H[c][i], c == 1 ? t.muted : t.series[i % t.series.size()]);
        nr.tip = (*names[c])[i] + "\n" + titles[c] + " \xC2\xB7 " + std::to_string((*cnts[c])[i]) + " documents";
        s.items.push_back(nr);
      }
      int anchor = c == 0 ? 2 : (c == 2 ? 0 : 1);
      double x = c == 0 ? xs[c] - 4 : (c == 2 ? xs[c] + colW + 4 : xs[c] + colW / 2);
      Prim lab = T(x, Y[c][i] + H[c][i] / 2 + t.font * 0.3, truncate((*names[c])[i], 26), t.font * 0.85, t.fg, anchor);
      lab.halo = true; lab.haloC = t.bg; lab.haloW = 1.4f;
      s.items.push_back(lab);
    }
  return s;
}

Scene chartBradford(const Bradford& b, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (b.sources.empty()) { s.items.push_back(T(w / 2, h / 2, "No sources", t.font, t.muted, 1)); return s; }
  double total = 0;
  for (auto& x : b.sources) total += x.second;
  Axes a{40, 20, w - 12, h - 24, 0, std::log(double(b.sources.size()) + 1), 0, total * 1.05};
  yAxis(s, a, t, "Cumulative documents");
  s.items.push_back(R(a.x0, a.y0, a.X(std::log(b.zone1 + 1.0)) - a.x0, a.y1 - a.y0, t.accent.withA(0.10f)));
  s.items.push_back(R(a.X(std::log(b.zone1 + 1.0)), a.y0, a.X(std::log(b.zone2 + 1.0)) - a.X(std::log(b.zone1 + 1.0)), a.y1 - a.y0, t.accent.withA(0.05f)));
  s.items.push_back(T(a.x0 + 4, a.y0 + t.font, "Core zone (" + std::to_string(b.zone1) + ")", t.font * 0.85, t.accent, 0, true));
  Prim line; line.type = Prim::Path; line.stroke = true; line.strokeC = t.accent; line.sw = 1.6f; line.group = "series";
  double acc = 0;
  vector<Prim> hov;
  for (size_t i = 0; i < b.sources.size(); i++) {
    acc += b.sources[i].second;
    line.d.push_back({i ? 'L' : 'M', float(a.X(std::log(i + 2.0))), float(a.Y(acc)), 0, 0});
    if (i < 400) {
      Prim hp; hp.type = Prim::Circle; hp.x = float(a.X(std::log(i + 2.0))); hp.y = float(a.Y(acc)); hp.r = 3.5f;  // no fill/stroke: hover target only
      hp.tip = "#" + std::to_string(i + 1) + "  " + b.sources[i].first + "\n" + fmtNum(b.sources[i].second, 0) + " documents \xC2\xB7 cumulative " + fmtNum(acc, 0) + " (" + fmtNum(100 * acc / std::max(1.0, total), 0) + "%)" +
               (int(i) < b.zone1 ? "\nCore zone" : (int(i) < b.zone2 ? "\nZone 2" : "\nZone 3"));
      hov.push_back(std::move(hp));
    }
  }
  s.items.push_back(line);
  for (auto& hp : hov) s.items.push_back(std::move(hp));
  s.items.push_back(T((a.x0 + a.x1) / 2, h - 6, "Source rank (log scale)", t.font * 0.85, t.muted, 1));
  return s;
}

Scene chartLotka(const Lotka& l, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (l.dist.empty()) { s.items.push_back(T(w / 2, h / 2, "No authors", t.font, t.muted, 1)); return s; }
  double maxX = 1, maxY = 1;
  for (auto& p : l.dist) { maxX = std::max(maxX, double(p.first)); maxY = std::max(maxY, double(p.second)); }
  Axes a{40, 20, w - 12, h - 24, 0, std::log10(maxX) + 0.2, 0, std::log10(maxY) + 0.25};
  for (int e = 0; e <= int(a.vy1); e++) { double y = a.Y(e); s.items.push_back(L(a.x0, y, a.x1, y, t.grid, 0.5)); s.items.push_back(T(a.x0 - 4, y + 3, fmtInt((long long)std::pow(10, e)), t.font * 0.85, t.muted, 2)); }
  for (auto& p : l.dist) {
    Prim c = C(a.X(std::log10(double(p.first))), a.Y(std::log10(double(p.second))), 3, t.accent.withA(0.85f));
    c.tip = fmtInt(p.second) + (p.second == 1 ? " author wrote " : " authors wrote ") + fmtInt(p.first) + (p.first == 1 ? " document" : " documents");
    s.items.push_back(c);
  }
  if (l.exponent > 0) {
    double y0 = std::log10(l.c), y1 = std::log10(l.c) - l.exponent * std::log10(maxX);
    s.items.push_back(L(a.X(0), a.Y(y0), a.X(std::log10(maxX)), a.Y(y1), t.accent2, 1.2, true));
  }
  s.items.push_back(T(a.x1, a.y0 + t.font, "n = " + fmtNum(l.exponent, 2) + "   R\xC2\xB2 = " + fmtNum(l.r2, 2), t.font * 0.9, t.fg, 2, true));
  s.items.push_back(T((a.x0 + a.x1) / 2, h - 6, "Documents per author (log)", t.font * 0.85, t.muted, 1));
  s.items.push_back(T(a.x0, a.y0 - 8, "Authors (log)", t.font * 0.85, t.muted, 0));
  return s;
}

Scene chartCompare(const Comparison& c, int top, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  vector<CompareRow> rows;
  for (int i = 0; i < std::min(top, int(c.emerging.size())); i++) rows.push_back(c.emerging[size_t(i)]);
  for (int i = 0; i < std::min(top, int(c.declining.size())); i++) rows.push_back(c.declining[size_t(i)]);
  if (rows.empty()) { s.items.push_back(T(w / 2, h / 2, "No differences between the periods", t.font, t.muted, 1)); return s; }
  std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.logRatio > b.logRatio; });
  double mx = 0.5;
  for (auto& r : rows) mx = std::max(mx, std::fabs(r.logRatio));
  double labW = std::min(w * 0.36, 150.0), cx = labW + (w - labW - 10) / 2, half = (w - labW - 10) / 2 - 30;
  double rh = (h - 26) / rows.size();
  s.items.push_back(T(cx - 4, 12, "\xE2\x86\x90 more in " + std::to_string(c.a0) + "\xE2\x80\x93" + std::to_string(c.a1), t.font * 0.85, t.muted, 2));
  s.items.push_back(T(cx + 4, 12, "more in " + std::to_string(c.b0) + "\xE2\x80\x93" + std::to_string(c.b1) + " \xE2\x86\x92", t.font * 0.85, t.muted, 0));
  s.items.push_back(L(cx, 18, cx, h - 4, t.muted.withA(0.5f), 0.7));
  for (size_t i = 0; i < rows.size(); i++) {
    double y = 22 + i * rh, bw = half * std::fabs(rows[i].logRatio) / mx;
    Color col = rows[i].logRatio > 0 ? Color::hex(0x009E73) : Color::hex(0xD55E00);
    Prim b = R(rows[i].logRatio > 0 ? cx : cx - bw, y + rh * 0.18, bw, rh * 0.64, col.withA(0.8f));
    b.group = "hit:" + std::to_string(i);
    b.tip = rows[i].label + "\n" + std::to_string(c.a0) + "\xE2\x80\x93" + std::to_string(c.a1) + ": " + std::to_string(rows[i].a) + " (" + fmtNum(rows[i].shareA * 100, 1) + "%)\n" +
            std::to_string(c.b0) + "\xE2\x80\x93" + std::to_string(c.b1) + ": " + std::to_string(rows[i].b) + " (" + fmtNum(rows[i].shareB * 100, 1) + "%)";
    s.items.push_back(b);
    s.items.push_back(T(labW - 6, y + rh / 2 + t.font * 0.3, truncate(rows[i].label, 26), t.font * 0.85, t.fg, 2));
    s.items.push_back(T(rows[i].logRatio > 0 ? cx + bw + 3 : cx - bw - 3, y + rh / 2 + t.font * 0.3, std::to_string(rows[i].a) + "\xE2\x86\x92" + std::to_string(rows[i].b), t.font * 0.75, t.muted, rows[i].logRatio > 0 ? 0 : 2));
  }
  return s;
}

Scene chartSensitivity(const vector<std::pair<int, int>>& sv, int current, const string& xlabel, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (sv.empty()) return s;
  double maxN = 1;
  for (auto& p : sv) maxN = std::max(maxN, double(p.second));
  Axes a{30, 10, w - 6, h - 16, 0.5, sv.back().first + 0.5, 0, maxN * 1.08};
  double bw = (a.x1 - a.x0) / sv.size() * 0.75;
  for (auto& p : sv) {
    double x = a.X(p.first), y = a.Y(p.second);
    Prim r = R(x - bw / 2, y, bw, a.y1 - y, p.first == current ? t.accent : t.muted.withA(0.35f));
    r.group = "hit:" + std::to_string(p.first);
    r.tip = "Threshold " + std::to_string(p.first) + "\n" + fmtInt(p.second) + " items" + (p.first == current ? " (current)" : "") + "\nClick to use this threshold";
    s.items.push_back(r);
    if (p.first == 1 || p.first % 5 == 0 || p.first == current) s.items.push_back(T(x, h - 5, std::to_string(p.first), t.font * 0.8, p.first == current ? t.accent : t.muted, 1, p.first == current));
  }
  s.items.push_back(T(a.x0 - 3, a.y0 + t.font * 0.6, fmtInt((long long)maxN), t.font * 0.8, t.muted, 2));
  s.items.push_back(T(a.x0 - 3, a.y1, "0", t.font * 0.8, t.muted, 2));
  (void)xlabel;
  return s;
}

Scene chartBars(const vector<std::pair<string, double>>& rows, const string& title, double w, double h, const ChartTheme& t, const string& suffix) {
  Scene s = base(w, h, t);
  if (rows.empty()) return s;
  double mx = 1e-9;
  for (auto& r : rows) mx = std::max(mx, r.second);
  double labW = std::min(w * 0.42, 170.0), top = title.empty() ? 4 : 18;
  double rh = (h - top - 4) / rows.size();
  if (!title.empty()) s.items.push_back(T(4, 12, title, t.font, t.fg, 0, true));
  for (size_t i = 0; i < rows.size(); i++) {
    double y = top + i * rh, bw = (w - labW - 44) * rows[i].second / mx;
    s.items.push_back(T(labW - 6, y + rh / 2 + t.font * 0.3, truncate(rows[i].first, 30), t.font * 0.85, t.fg, 2));
    Prim b = R(labW, y + rh * 0.2, std::max(1.0, bw), rh * 0.6, t.series[i % t.series.size()].withA(0.85f));
    b.group = "hit:" + std::to_string(i);
    b.tip = rows[i].first + "\n" + tickLabel(rows[i].second) + suffix;
    s.items.push_back(b);
    s.items.push_back(T(labW + bw + 4, y + rh / 2 + t.font * 0.3, tickLabel(rows[i].second) + suffix, t.font * 0.8, t.muted, 0));
  }
  return s;
}

Scene chartDensityMatrix(const Network& net, const vector<Color>& colors, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  // top items ordered by cluster then weight; cell = link strength (sqrt-scaled)
  // as many items as fit with legible row labels (40 on screen; fewer in a small print panel)
  int n = std::min({40, net.n(), std::max(8, int((h - 28) / std::max(1.0, t.font * 0.72)))});
  if (n < 2) { s.items.push_back(T(w / 2, h / 2, "Not enough items", t.font, t.muted, 1)); return s; }
  vector<int> idx(static_cast<size_t>(net.n()));
  for (int i = 0; i < net.n(); i++) idx[size_t(i)] = i;
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return net.weight(a) > net.weight(b); });
  idx.resize(size_t(n));
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return net.nodes[size_t(a)].cluster != net.nodes[size_t(b)].cluster ? net.nodes[size_t(a)].cluster < net.nodes[size_t(b)].cluster : net.weight(a) > net.weight(b); });
  std::unordered_map<int, int> pos;
  for (int k = 0; k < n; k++) pos[idx[size_t(k)]] = k;
  vector<double> M(size_t(n * n), 0);
  double mx = 1e-9;
  for (auto& l : net.links) {
    auto ia = pos.find(l.a), ib = pos.find(l.b);
    if (ia == pos.end() || ib == pos.end()) continue;
    M[size_t(ia->second * n + ib->second)] = M[size_t(ib->second * n + ia->second)] = l.w;
    mx = std::max(mx, l.w);
  }
  double labW = std::min(130.0, w * 0.3), cell = std::min((w - labW - 6) / n, (h - 28) / n);
  double ox = labW, oy = 22;
  for (int r = 0; r < n; r++) {
    Color cc = colors.empty() ? t.accent : colors[size_t(std::max(0, net.nodes[size_t(idx[size_t(r)])].cluster)) % colors.size()];
    s.items.push_back(T(ox - 4, oy + (r + 0.5) * cell + t.font * 0.28, truncate(net.nodes[size_t(idx[size_t(r)])].label, 24), std::min(t.font * 0.8, cell * 0.85), t.fg, 2));
    s.items.push_back(R(ox - 3, oy + r * cell, 2, cell, cc));
    for (int c = 0; c < n; c++) {
      double v = M[size_t(r * n + c)];
      if (v <= 0) continue;
      Color col = cmapAt("viridis", std::sqrt(v / mx));
      Prim p = R(ox + c * cell, oy + r * cell, cell - 0.3, cell - 0.3, col);
      p.group = "hit:" + std::to_string(r * n + c);
      p.tip = net.nodes[size_t(idx[size_t(r)])].label + "  \xC3\x97  " + net.nodes[size_t(idx[size_t(c)])].label + "\nLink strength " + fmtNum(v, v < 10 ? 2 : 0);
      s.items.push_back(p);
    }
  }
  s.items.push_back(T(4, 12, "Link strength among the top " + std::to_string(n) + " items (ordered by cluster)", t.font * 0.9, t.fg, 0, true));
  return s;
}

const Prim* tipAt(const Scene& sc, double x, double y) {
  // rectangles and points first (they sit on top of flows), then filled paths by even-odd containment
  for (int pass = 0; pass < 2; pass++)
    for (size_t k = sc.items.size(); k-- > 0;) {
      const Prim& p = sc.items[k];
      if (p.tip.empty()) continue;
      if (pass == 0) {
        if (p.type == Prim::Rect && x >= p.x - 1 && y >= p.y - 1 && x <= p.x + p.w + 1 && y <= p.y + p.h + 1) return &p;
        if (p.type == Prim::Circle && std::hypot(x - p.x, y - p.y) <= p.r + 3) return &p;
      } else if (p.type == Prim::Path && p.d.size() > 2) {
        vector<std::pair<double, double>> poly;
        double lx = 0, ly = 0;
        for (auto& d : p.d) {
          if (d.op == 'M' || d.op == 'L') { poly.push_back({d.x1, d.y1}); lx = d.x1; ly = d.y1; }
          else if (d.op == 'Q') {
            for (int q = 1; q <= 8; q++) { double t = q / 8.0, u = 1 - t; poly.push_back({u * u * lx + 2 * u * t * d.x1 + t * t * d.x2, u * u * ly + 2 * u * t * d.y1 + t * t * d.y2}); }
            lx = d.x2; ly = d.y2;
          }
        }
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
          if ((poly[i].second > y) != (poly[j].second > y) && x < (poly[j].first - poly[i].first) * (y - poly[i].second) / (poly[j].second - poly[i].second) + poly[i].first) in = !in;
        if (in) return &p;
      }
    }
  // nothing directly under the pointer: snap to the nearest bar or point within a few units, so the gaps
  // between bars and the space around thin line markers still report a value
  const Prim* best = nullptr;
  double bestD = 10;
  for (auto& p : sc.items) {
    if (p.tip.empty()) continue;
    double d = 1e9;
    if (p.type == Prim::Rect) {
      double dx = std::max({p.x - x, 0.0, x - (p.x + p.w)}), dy = std::max({p.y - y, 0.0, y - (p.y + p.h)});
      d = std::hypot(dx, dy);
    } else if (p.type == Prim::Circle) d = std::max(0.0, std::hypot(x - p.x, y - p.y) - p.r);
    if (d < bestD) { bestD = d; best = &p; }
  }
  return best;
}

int hitTag(const Scene& sc, double x, double y) {
  for (size_t k = sc.items.size(); k-- > 0;) {
    const Prim& p = sc.items[k];
    if (p.group.compare(0, 4, "hit:") != 0) continue;
    bool in = false;
    if (p.type == Prim::Rect) in = x >= p.x && y >= p.y && x <= p.x + p.w && y <= p.y + p.h;
    else if (p.type == Prim::Circle) in = std::hypot(x - p.x, y - p.y) <= p.r;
    else if (p.type == Prim::Path && !p.d.empty()) {
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      for (auto& d : p.d) { if (d.op == 'Z') continue; x0 = std::min({x0, d.x1, d.op == 'Q' ? d.x2 : d.x1}); x1 = std::max({x1, d.x1, d.op == 'Q' ? d.x2 : d.x1}); y0 = std::min({y0, d.y1, d.op == 'Q' ? d.y2 : d.y1}); y1 = std::max({y1, d.y1, d.op == 'Q' ? d.y2 : d.y1}); }
      in = x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    if (in) return toInt(p.group.substr(4), -1);
  }
  return -1;
}


// =====================================================================================
// 1.2 charts
// =====================================================================================
namespace {
void xYears(Scene& s, const Axes& a, const ChartTheme& t, int y0, int y1, bool grid) {
  int span = std::max(1, y1 - y0);
  double px = (a.x1 - a.x0) / span;  // room per year
  int step = px > 26 ? 1 : px > 13 ? 2 : px > 5.5 ? 5 : px > 2.6 ? 10 : px > 1.2 ? 20 : 50;
  for (int y = ((y0 + step - 1) / step) * step; y <= y1; y += step) {
    double x = a.X(y);
    if (grid) s.items.push_back(L(x, a.y0, x, a.y1, t.grid, 0.5));
    s.items.push_back(T(x, a.y1 + t.font * 1.15, std::to_string(y), t.font * 0.82, t.muted, 1));
  }
}
Prim message(double w, double h, const string& m, const ChartTheme& t) { return T(w / 2, h / 2, m, t.font, t.muted, 1); }
}  // namespace

Scene chartTrendTopics(const vector<TrendTopic>& v, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (v.empty()) { s.items.push_back(message(w, h, "Not enough frequent terms. Lower the minimum frequency.", t)); return s; }
  double y0 = 1e9, y1 = -1e9, fmax = 1;
  for (auto& x : v) { y0 = std::min(y0, x.q1); y1 = std::max(y1, x.q3); fmax = std::max(fmax, double(x.freq)); }
  y0 = std::floor(y0) - 0.6;
  y1 = std::ceil(y1) + 0.6;
  double labW = std::min(w * 0.38, 150.0);
  Axes a{labW, 6, w - 12, h - 22, y0, y1, 0, double(v.size())};
  xYears(s, a, t, int(std::ceil(y0)), int(std::floor(y1)), true);
  double rh = (a.y1 - a.y0) / v.size(), rmax = std::min(rh * 0.5, 8.0);
  for (size_t i = 0; i < v.size(); i++) {
    const auto& x = v[i];
    double y = a.y0 + (i + 0.5) * rh;
    s.items.push_back(T(labW - 8, y + t.font * 0.3, truncate(x.label, 28), t.font * 0.84, t.fg, 2));
    Prim l = L(a.X(x.q1), y, a.X(x.q3), y, t.accent.withA(0.5f), std::max(1.2, rh * 0.16));
    l.roundCap = true;
    l.group = "series";
    s.items.push_back(l);
    Prim c = C(a.X(x.med), y, 2.2 + (rmax - 2.2) * std::sqrt(x.freq / fmax), t.accent);
    c.stroke = true; c.strokeC = t.bg; c.sw = 0.6f;
    c.group = "hit:" + std::to_string(i);
    c.tip = x.label + "\n" + fmtInt(x.freq) + " documents\nMedian year " + fmtNum(x.med, x.med == std::floor(x.med) ? 0 : 1) + " \xC2\xB7 middle half " + fmtNum(x.q1, 0) + "\xE2\x80\x93" + fmtNum(x.q3, 0);
    s.items.push_back(c);
  }
  return s;
}

Scene chartRpys(const Rpys& rp, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (rp.counts.empty()) { s.items.push_back(message(w, h, "No dated cited references in the data", t)); return s; }
  double mx = 1, mn = 0;
  for (int c : rp.counts) mx = std::max(mx, double(c));
  for (double d : rp.dev) { mx = std::max(mx, d); mn = std::min(mn, d); }
  Axes a{38, 22, w - 12, h - 24, rp.y0 - 0.6, rp.y1 + 0.6, mn * 1.1, mx * 1.1};
  yAxis(s, a, t, "Cited references");
  s.items.push_back(L(a.x0, a.Y(0), a.x1, a.Y(0), t.muted.withA(0.5f), 0.6));
  double bw = std::max(0.5, (a.x1 - a.x0) / rp.counts.size() * 0.78);
  for (size_t i = 0; i < rp.counts.size(); i++) {
    int yr = rp.y0 + int(i);
    double x = a.X(yr), top = a.Y(rp.counts[i]);
    Prim b = R(x - bw / 2, top, bw, std::max(0.0, a.Y(0) - top), t.muted.withA(0.38f));
    b.group = "hit:" + std::to_string(yr);
    b.tip = std::to_string(yr) + "\n" + fmtInt(rp.counts[i]) + " cited references\nDeviation from 5-year median " + (rp.dev[i] > 0 ? "+" : "") + fmtNum(rp.dev[i], 1);
    s.items.push_back(b);
  }
  Prim line; line.type = Prim::Path; line.stroke = true; line.strokeC = t.accent2; line.sw = 1.4f; line.group = "series"; line.roundCap = true;
  for (size_t i = 0; i < rp.dev.size(); i++) line.d.push_back({i ? 'L' : 'M', float(a.X(rp.y0 + int(i))), float(a.Y(rp.dev[i])), 0, 0});
  s.items.push_back(line);
  for (size_t k = 0; k < rp.peaks.size() && k < 5; k++) {
    auto& p = rp.peaks[k];
    Prim tl = T(a.X(p.year), a.Y(std::max(double(p.count), p.dev)) - 4, std::to_string(p.year), t.font * 0.8, t.accent2, 1, true);
    tl.halo = true; tl.haloC = t.bg; tl.haloW = 1.6f;
    s.items.push_back(tl);
  }
  xYears(s, a, t, rp.y0, rp.y1, false);
  s.items.push_back(T(w - 12, 12, "Deviation from the 5-year median", t.font * 0.8, t.accent2, 2));
  return s;
}

Scene chartProduction(const Production& P, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (P.labels.empty()) { s.items.push_back(message(w, h, "No dated records", t)); return s; }
  size_t n = P.labels.size();
  double labW = std::min(w * 0.36, 140.0);
  Axes a{labW, 6, w - 12, h - 22, P.y0 - 0.6, P.y1 + 0.6, 0, double(n)};
  xYears(s, a, t, P.y0, P.y1, true);
  double rh = (a.y1 - a.y0) / n, rmax = std::min(rh * 0.48, 9.0);
  double dm = 1, cm = 1e-9;
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < P.docs[i].size(); j++) { dm = std::max(dm, double(P.docs[i][j])); cm = std::max(cm, P.citesPerYear[i][j]); }
  for (size_t i = 0; i < n; i++) {
    double y = a.y0 + (i + 0.5) * rh;
    s.items.push_back(T(labW - 8, y + t.font * 0.3, truncate(P.labels[i], 26), t.font * 0.84, t.fg, 2));
    int f = -1, l = -1;
    for (size_t j = 0; j < P.docs[i].size(); j++) if (P.docs[i][j]) { if (f < 0) f = int(j); l = int(j); }
    if (f >= 0 && l > f) { Prim ln = L(a.X(P.y0 + f), y, a.X(P.y0 + l), y, t.accent.withA(0.3f), 1.1); ln.group = "series"; s.items.push_back(ln); }
    for (size_t j = 0; j < P.docs[i].size(); j++) {
      int d = P.docs[i][j];
      if (!d) continue;
      double tc = P.citesPerYear[i][j];
      Prim c = C(a.X(P.y0 + int(j)), y, 1.8 + (rmax - 1.8) * std::sqrt(d / dm), t.accent.withA(float(0.3 + 0.7 * std::sqrt(tc / cm))));
      c.stroke = true; c.strokeC = t.accent; c.sw = 0.5f;
      c.group = "hit:" + std::to_string(i);
      c.tip = P.labels[i] + " \xC2\xB7 " + std::to_string(P.y0 + int(j)) + "\n" + fmtInt(d) + (d == 1 ? " document" : " documents") + "\n" + fmtNum(tc, 1) + " citations per year";
      s.items.push_back(c);
    }
  }
  return s;
}

Scene chartCollab(const vector<CountryCollab>& v, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (v.empty()) { s.items.push_back(message(w, h, "No country affiliations in the data", t)); return s; }
  double labW = std::min(w * 0.34, 130.0), top = 20;
  s.items.push_back(R(labW, 5, 9, 9, t.accent.withA(0.85f)));
  s.items.push_back(T(labW + 13, 12.5, "Single-country (SCP)", t.font * 0.8, t.muted));
  double lx = labW + 13 + textWidth("Single-country (SCP)", t.font * 0.8, false) + 12;
  s.items.push_back(R(lx, 5, 9, 9, t.accent2.withA(0.85f)));
  s.items.push_back(T(lx + 13, 12.5, "Multi-country (MCP)", t.font * 0.8, t.muted));
  double mx = 1;
  for (auto& c : v) mx = std::max(mx, double(c.scp + c.mcp));
  double rh = (h - top - 4) / v.size();
  for (size_t i = 0; i < v.size(); i++) {
    const auto& c = v[i];
    double y = top + i * rh, ws = (w - labW - 70) * c.scp / mx, wm = (w - labW - 70) * c.mcp / mx;
    int tot = c.scp + c.mcp;
    string tip = c.country + "\n" + fmtInt(tot) + " documents\nSingle-country " + fmtInt(c.scp) + " \xC2\xB7 multi-country " + fmtInt(c.mcp) + " (" + fmtNum(100.0 * c.mcp / std::max(1, tot), 0) + "%)";
    s.items.push_back(T(labW - 6, y + rh / 2 + t.font * 0.3, truncate(c.country, 24), t.font * 0.84, t.fg, 2));
    Prim a = R(labW, y + rh * 0.2, std::max(0.0, ws), rh * 0.6, t.accent.withA(0.85f));
    a.group = "hit:" + std::to_string(i); a.tip = tip;
    s.items.push_back(a);
    Prim b = R(labW + ws, y + rh * 0.2, std::max(0.0, wm), rh * 0.6, t.accent2.withA(0.85f));
    b.group = "hit:" + std::to_string(i); b.tip = tip;
    s.items.push_back(b);
    s.items.push_back(T(labW + ws + wm + 4, y + rh / 2 + t.font * 0.3, fmtInt(tot) + "  \xC2\xB7  " + fmtNum(100.0 * c.mcp / std::max(1, tot), 0) + "% MCP", t.font * 0.76, t.muted));
  }
  return s;
}

Scene chartDocBars(const vector<DocBar>& rows, double w, double h, const ChartTheme& t, const string& suffix) {
  Scene s = base(w, h, t);
  if (rows.empty()) { s.items.push_back(message(w, h, "No documents", t)); return s; }
  double mx = 1e-9;
  for (auto& r : rows) mx = std::max(mx, r.value);
  double labW = std::min(w * 0.42, 160.0), rh = (h - 8) / rows.size();
  for (size_t i = 0; i < rows.size(); i++) {
    double y = 4 + i * rh, bw = (w - labW - 48) * rows[i].value / mx;
    s.items.push_back(T(labW - 6, y + rh / 2 + t.font * 0.3, truncate(rows[i].label, 30), t.font * 0.84, t.fg, 2));
    Prim b = R(labW, y + rh * 0.2, std::max(1.0, bw), rh * 0.6, t.accent.withA(0.85f));
    b.group = "hit:" + std::to_string(rows[i].rec);
    b.tip = rows[i].tip.empty() ? rows[i].label + "\n" + tickLabel(rows[i].value) + suffix : rows[i].tip;
    s.items.push_back(b);
    s.items.push_back(T(labW + bw + 4, y + rh / 2 + t.font * 0.3, tickLabel(rows[i].value), t.font * 0.78, t.muted));
  }
  return s;
}

Scene chartHistoriograph(const Corpus& corp, const CitationIndex& X, int top, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  vector<int> ids;
  for (size_t i = 0; i < corp.recs.size() && i < X.citedBy.size(); i++) if (!X.citedBy[i].empty() && corp.recs[i].year) ids.push_back(int(i));
  std::sort(ids.begin(), ids.end(), [&](int a, int b) { return X.citedBy[size_t(a)].size() != X.citedBy[size_t(b)].size() ? X.citedBy[size_t(a)].size() > X.citedBy[size_t(b)].size() : a < b; });
  if (int(ids.size()) > top) ids.resize(size_t(top));
  if (ids.size() < 2) { s.items.push_back(message(w, h, "No citations between documents of this corpus were found", t)); return s; }
  std::unordered_set<int> S(ids.begin(), ids.end());
  int y0 = 9999, y1 = 0;
  double lmax = 1;
  std::map<int, vector<int>> col;
  for (int d : ids) { int y = corp.recs[size_t(d)].year; y0 = std::min(y0, y); y1 = std::max(y1, y); col[y].push_back(d); lmax = std::max(lmax, double(X.citedBy[size_t(d)].size())); }
  Axes a{22, 14, w - 22, h - 26, y0 - 0.5, y1 + 0.5, 0, 1};
  xYears(s, a, t, y0, y1, true);
  std::unordered_map<int, std::pair<double, double>> pos;
  for (auto& kv : col) {
    auto& v = kv.second;
    std::unordered_map<int, double> bary;
    for (int d : v) {
      double sum = 0; int k = 0;
      for (int c : X.cites[size_t(d)]) { auto it = pos.find(c); if (it != pos.end()) { sum += it->second.second; k++; } }
      bary[d] = k ? sum / k : (a.y0 + a.y1) / 2;
    }
    std::stable_sort(v.begin(), v.end(), [&](int p, int q) { return bary[p] < bary[q]; });
    double slot = (a.y1 - a.y0) / v.size();
    for (size_t j = 0; j < v.size(); j++) pos[v[j]] = {a.X(kv.first), a.y0 + (j + 0.5) * slot};
  }
  auto rad = [&](int d) { return 3.0 + 6.0 * std::sqrt(X.citedBy[size_t(d)].size() / lmax); };
  Color ec = t.muted.withA(0.5f);
  for (int d : ids)
    for (int c : X.cites[size_t(d)]) {
      if (!S.count(c)) continue;
      auto p1 = pos[d], p2 = pos[c];
      double mx = (p1.first + p2.first) / 2, my = (p1.second + p2.second) / 2 - (p1.first - p2.first) * 0.12;
      Prim e; e.type = Prim::Path; e.stroke = true; e.strokeC = ec; e.sw = 0.7f; e.group = "links";
      e.d = {{'M', float(p1.first), float(p1.second), 0, 0}, {'Q', float(mx), float(my), float(p2.first), float(p2.second)}};
      s.items.push_back(e);
      // arrowhead at the cited document
      double dx = p2.first - mx, dy = p2.second - my, len = std::max(1e-6, std::hypot(dx, dy));
      dx /= len; dy /= len;
      double r = rad(c) + 1, tx = p2.first - dx * r, ty = p2.second - dy * r, sz = 3.4;
      Prim ah; ah.type = Prim::Path; ah.fill = true; ah.fillC = ec.withA(0.8f); ah.group = "links";
      ah.d = {{'M', float(tx), float(ty), 0, 0}, {'L', float(tx - dx * sz - dy * sz * 0.6), float(ty - dy * sz + dx * sz * 0.6), 0, 0}, {'L', float(tx - dx * sz + dy * sz * 0.6), float(ty - dy * sz - dx * sz * 0.6), 0, 0}, {'Z', 0, 0, 0, 0}};
      s.items.push_back(ah);
    }
  for (int d : ids) {
    const Record& r = corp.recs[size_t(d)];
    auto p = pos[d];
    Prim c = C(p.first, p.second, rad(d), t.accent.withA(0.9f));
    c.stroke = true; c.strokeC = t.bg; c.sw = 0.8f;
    c.group = "hit:" + std::to_string(d);
    c.tip = shortCite(r) + "\n" + truncate(r.title, 90) + "\nCited by " + std::to_string(X.citedBy[size_t(d)].size()) + " in this corpus \xC2\xB7 " + fmtInt(r.cites) + " citations overall";
    s.items.push_back(c);
    string sur = firstAuthorSurname(r);
    Prim tl = T(p.first, p.second - rad(d) - 2.5, truncate(sur.empty() ? r.title : sur, 16) + " " + std::to_string(r.year), t.font * 0.7, t.fg, 1);
    tl.halo = true; tl.haloC = t.bg; tl.haloW = 1.6f;
    s.items.push_back(tl);
  }
  return s;
}

Scene chartClusterYears(const ClusterYears& Y, const vector<string>& names, const vector<Color>& colors, bool share, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (Y.docs.empty()) { s.items.push_back(message(w, h, "Needs a map built from records", t)); return s; }
  size_t K = Y.docs.size(), ny = size_t(Y.y1 - Y.y0 + 1);
  vector<double> tot(ny, 0);
  for (size_t k = 0; k < K; k++) for (size_t j = 0; j < ny; j++) tot[j] += Y.docs[k][j];
  auto val = [&](size_t k, size_t j) { return share ? (tot[j] > 0 ? 100.0 * Y.docs[k][j] / tot[j] : 0.0) : double(Y.docs[k][j]); };
  double vmax = 1;
  for (size_t j = 0; j < ny; j++) vmax = std::max(vmax, share ? 100.0 : tot[j]);
  double gx0 = Y.y0, gx1 = Y.y1;
  if (ny == 1) { gx0 -= 0.5; gx1 += 0.5; }
  Axes a{38, 22, w - 12, h - 24, gx0, gx1, 0, share ? 100.0 : vmax * 1.08};
  yAxis(s, a, t, share ? "Share of cluster activity (%)" : "Documents per cluster");
  vector<double> lo(ny, 0);
  for (size_t k = 0; k < K; k++) {
    Prim p; p.type = Prim::Path; p.fill = true; p.fillC = (colors.empty() ? t.accent : colors[k % colors.size()]).withA(0.82f);
    p.stroke = true; p.strokeC = t.bg.withA(0.9f); p.sw = 0.5f; p.group = "areas";
    vector<double> hi(ny);
    for (size_t j = 0; j < ny; j++) hi[j] = lo[j] + val(k, j);
    for (size_t j = 0; j < ny; j++) p.d.push_back({j ? 'L' : 'M', float(a.X(Y.y0 + int(j))), float(a.Y(hi[j])), 0, 0});
    if (ny == 1) p.d.push_back({'L', float(a.X(gx1)), float(a.Y(hi[0])), 0, 0});
    if (ny == 1) p.d.push_back({'L', float(a.X(gx1)), float(a.Y(lo[0])), 0, 0});
    for (size_t j = ny; j-- > 0;) p.d.push_back({'L', float(a.X(Y.y0 + int(j))), float(a.Y(lo[j])), 0, 0});
    p.d.push_back({'Z', 0, 0, 0, 0});
    int total = 0;
    for (size_t j = 0; j < ny; j++) total += Y.docs[k][j];
    p.tip = (k < names.size() ? names[k] : "Cluster " + std::to_string(k + 1)) + "\n" + fmtInt(total) + " documents";
    s.items.push_back(p);
    lo = hi;
  }
  // year columns: hover shows every cluster that year
  double cw = ny > 1 ? (a.x1 - a.x0) / (ny - 1) : (a.x1 - a.x0);
  for (size_t j = 0; j < ny; j++) {
    vector<std::pair<double, size_t>> v;
    for (size_t k = 0; k < K; k++) if (Y.docs[k][j]) v.push_back({double(Y.docs[k][j]), k});
    std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.first > y.first; });
    string tip = std::to_string(Y.y0 + int(j));
    for (size_t q = 0; q < v.size() && q < 8; q++) {
      size_t k = v[q].second;
      tip += "\n" + truncate(k < names.size() ? names[k] : "Cluster " + std::to_string(k + 1), 34) + ": " + fmtInt(int(v[q].first)) + (share && tot[j] > 0 ? " (" + fmtNum(100.0 * v[q].first / tot[j], 0) + "%)" : "");
    }
    if (v.empty()) tip += "\nNo documents";
    double x = a.X(Y.y0 + int(j));
    Prim hc = R(std::max(a.x0, x - cw / 2), a.y0, std::min(cw, a.x1 - std::max(a.x0, x - cw / 2)), a.y1 - a.y0, Color(0, 0, 0, 0));
    hc.tip = tip;
    hc.group = "hover";
    s.items.push_back(hc);
  }
  xYears(s, a, t, Y.y0, Y.y1, false);
  return s;
}


// ------------------------------------------------------------ records flow (PRISMA style)
Scene chartFlow(const RecordsFlow& f, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (!f.valid) { s.items.push_back(T(w / 2, h / 2, "Import records to see the flow", t.font, t.muted, 1)); return s; }
  struct Box { string title; int n; vector<string> lines; };
  struct Row { string stage; Box main; bool side = false; Box right; };
  vector<Row> rows;
  auto num = [](long v) { return fmtInt(v); };
  {
    Row r; r.stage = "IDENTIFICATION";
    r.main = {"Records identified", f.identified, {}};
    for (auto& sfile : f.sources) r.main.lines.push_back(sfile.first + ": " + num(sfile.second));
    r.side = true; r.right = {"Duplicates removed", f.duplicates, {}};
    rows.push_back(r);
  }
  {
    Row r; r.stage = "SCREENING";
    r.main = {"Records screened", f.screened, {}};
    int ex = 0;
    for (auto& e : f.excluded) ex += e.second;
    r.side = true; r.right = {"Records excluded", ex, {}};
    for (auto& e : f.excluded) if (e.second > 0) r.right.lines.push_back(e.first + ": " + num(e.second));
    rows.push_back(r);
  }
  {
    Row r; r.stage = "INCLUDED";
    r.main = {"Records in the analysis", f.analysed, {}};
    rows.push_back(r);
  }
  {
    string U = f.unitNoun.empty() ? string("Items") : string(1, char(toupper((unsigned char)f.unitNoun[0]))) + f.unitNoun.substr(1);
    Row r;
    if (!f.itemsKnown) {
      r.main = {U + " in the map", -1, {"Build the map to count them"}};
    } else if (f.inMap >= 0) {
      r.main = {U + " in the map", f.inMap, {"Found: " + num(f.found), "Meeting the threshold: " + num(f.meet)}};
      if (!f.thresholdText.empty()) r.main.lines.push_back(f.thresholdText);
      r.side = true;
      int notShown = std::max(0, f.found - f.inMap);
      r.right = {U + " not shown", notShown, {"Below the threshold: " + num(std::max(0, f.found - f.meet))}};
      if (f.userExcluded > 0) r.right.lines.push_back("Excluded by you: " + num(f.userExcluded));
      int other = notShown - std::max(0, f.found - f.meet) - f.userExcluded;
      if (other > 0) r.right.lines.push_back("Item limit or not connected: " + num(other));
    } else {
      r.main = {U + " found", f.found, {"Meeting the threshold: " + num(f.meet), "No map built for this analysis yet"}};
    }
    rows.push_back(r);
  }
  // natural layout at scale 1 (points), then one scale for everything
  const double pad = 8, titleS = 1.05, lineS = 0.9, gapY = 18, stageH = 13;
  auto boxH = [&](const Box& b) { return pad * 2 + t.font * titleS * 1.3 + t.font * lineS * 1.35 * (1 + b.lines.size()); };
  double H0 = 0;
  for (auto& r : rows) {
    double bh = boxH(r.main);
    if (r.side) bh = std::max(bh, boxH(r.right));
    H0 += (r.stage.empty() ? 0 : stageH) + bh + gapY;
  }
  H0 -= gapY;
  const double W0 = 430;  // width the layout needs at scale 1
  double kH = (h - 16) / std::max(1.0, H0), kW = (w - 16) / W0;
  double k = kH <= 1 ? kH : std::min(kH, std::max(1.0, kW));
  k = std::max(0.45, std::min(k, 2.2));
  double fs = t.font * k;
  double x0 = 8, cw = w - 16, gx = std::max(14.0, cw * 0.05);
  double mainW = (cw - gx) * 0.56, sideW = cw - gx - mainW;
  double y = std::max(8.0, (h - H0 * k) / 2);
  Color boxFill = t.light ? Color(0, 0, 0, 0.035f) : Color(1, 1, 1, 0.045f);
  Color boxLine = t.light ? Color(0, 0, 0, 0.18f) : Color(1, 1, 1, 0.16f);
  auto fitText = [&](const string& str, double size, bool bold, double maxW) {
    if (textWidth(str, size, bold) <= maxW) return str;
    string o = str;
    while (o.size() > 4 && textWidth(o + "\xE2\x80\xA6", size, bold) > maxW) {
      size_t cut = o.size() - 1;
      while (cut > 0 && (static_cast<unsigned char>(o[cut]) & 0xC0) == 0x80) cut--;
      o.erase(cut);
    }
    return o + "\xE2\x80\xA6";
  };
  auto drawBox = [&](const Box& b, double bx, double by, double bw, double bh) {
    Prim r = R(bx, by, bw, bh, boxFill);
    r.stroke = true; r.strokeC = boxLine; r.sw = 0.7f;
    r.tip = b.title + (b.n >= 0 ? "\nn = " + fmtInt(b.n) : string());
    for (auto& l : b.lines) r.tip += "\n" + l;
    s.items.push_back(r);
    double ty = by + pad * k + fs * titleS;
    s.items.push_back(T(bx + pad * k, ty, fitText(b.title, fs * titleS, true, bw - 2 * pad * k), fs * titleS, t.fg, 0, true));
    ty += fs * lineS * 1.45;
    if (b.n >= 0) s.items.push_back(T(bx + pad * k, ty, "n = " + fmtInt(b.n), fs * lineS, t.muted));
    else ty -= fs * lineS * 1.35;
    for (auto& l : b.lines) { ty += fs * lineS * 1.35; s.items.push_back(T(bx + pad * k, ty, fitText(l, fs * lineS, false, bw - 2 * pad * k), fs * lineS, t.muted)); }
  };
  auto arrowDown = [&](double x, double y0, double y1) {
    s.items.push_back(L(x, y0, x, y1, t.muted, 0.8));
    double a = 3.2 * k;
    s.items.push_back(L(x - a, y1 - a * 1.2, x, y1, t.muted, 0.8));
    s.items.push_back(L(x + a, y1 - a * 1.2, x, y1, t.muted, 0.8));
  };
  auto arrowRight = [&](double x0a, double x1, double yy) {
    s.items.push_back(L(x0a, yy, x1, yy, t.muted, 0.8));
    double a = 3.2 * k;
    s.items.push_back(L(x1 - a * 1.2, yy - a, x1, yy, t.muted, 0.8));
    s.items.push_back(L(x1 - a * 1.2, yy + a, x1, yy, t.muted, 0.8));
  };
  double prevBottom = -1;
  for (auto& r : rows) {
    if (!r.stage.empty()) {
      s.items.push_back(T(x0, y + stageH * k * 0.72, r.stage, fs * 0.72, t.accent, 0, true));
      s.items.push_back(L(x0 + textWidth(r.stage, fs * 0.72, true) + 6 * k, y + stageH * k * 0.5, x0 + cw, y + stageH * k * 0.5, t.grid, 0.6));
      y += stageH * k;
    }
    double bh = boxH(r.main) * k;
    double sh = r.side ? boxH(r.right) * k : 0;
    if (prevBottom >= 0) arrowDown(x0 + mainW / 2, prevBottom, y);
    drawBox(r.main, x0, y, mainW, bh);
    if (r.side) {
      drawBox(r.right, x0 + mainW + gx, y, sideW, sh);
      arrowRight(x0 + mainW, x0 + mainW + gx, y + std::min(bh, sh) / 2);
    }
    prevBottom = y + bh;
    y += std::max(bh, sh) + gapY * k;
  }
  return s;
}


// ------------------------------------------------------------ 1.8: main path, resolution sweep
Scene chartMainPath(const Corpus& corp, const MainPath& M, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (!M.ok || M.docs.empty()) { s.items.push_back(message(w, h, M.error.empty() ? "No main path was found" : M.error, t)); return s; }
  std::unordered_set<int> G(M.global.begin(), M.global.end());
  std::unordered_map<int, double> spc;
  double smax = 1e-12;
  for (size_t i = 0; i < M.docs.size(); i++) { spc[M.docs[i]] = M.spcDoc[i]; smax = std::max(smax, M.spcDoc[i]); }
  int y0 = 9999, y1 = 0;
  std::map<int, vector<int>> col;
  for (int d : M.docs) { int y = corp.recs[size_t(d)].year; y0 = std::min(y0, y); y1 = std::max(y1, y); col[y].push_back(d); }
  Axes a{26, 30, w - 26, h - 26, y0 - 0.5, y1 + 0.5, 0, 1};
  xYears(s, a, t, y0, y1, true);
  size_t most = 1;
  for (auto& kv : col) most = std::max(most, kv.second.size());
  double mid = (a.y0 + a.y1) / 2, gap = std::min(34.0, (a.y1 - a.y0) / double(most + 1));
  std::unordered_map<int, std::pair<double, double>> pos;
  for (auto& kv : col) {
    auto v = kv.second;
    std::stable_sort(v.begin(), v.end(), [&](int p, int q) { bool gp = G.count(p) > 0, gq = G.count(q) > 0; return gp != gq ? gp : spc[p] > spc[q]; });
    for (size_t j = 0; j < v.size(); j++) {
      double off = (j == 0) ? 0 : double((j + 1) / 2) * ((j % 2) ? -1 : 1);
      pos[v[j]] = {a.X(kv.first), mid + off * gap};
    }
  }
  auto rad = [&](int d) { return 3.2 + 4.8 * std::sqrt(spc[d] / smax); };
  std::set<std::pair<int, int>> gl;
  for (size_t k = 1; k < M.global.size(); k++) gl.insert({M.global[k - 1], M.global[k]});
  double lmax = 1e-12;
  for (auto& l : M.links) lmax = std::max(lmax, std::get<2>(l));
  for (int pass = 0; pass < 2; pass++)
    for (auto& l : M.links) {
      int u = std::get<0>(l), v = std::get<1>(l);
      bool onG = gl.count({u, v}) > 0;
      if ((pass == 0) == onG) continue;  // key routes first, the global path on top
      auto p1 = pos[u], p2 = pos[v];
      Color ec = onG ? t.accent.withA(0.9f) : t.muted.withA(0.55f);
      double mx = (p1.first + p2.first) / 2, my = (p1.second + p2.second) / 2 - (p2.first - p1.first) * (p1.second == p2.second ? 0.0 : 0.08);
      Prim e; e.type = Prim::Path; e.stroke = true; e.strokeC = ec; e.sw = float(0.6 + 2.6 * std::sqrt(std::get<2>(l) / lmax)); e.group = "links";
      e.d = {{'M', float(p1.first), float(p1.second), 0, 0}, {'Q', float(mx), float(my), float(p2.first), float(p2.second)}};
      e.tip = "Search path count " + fmtNum(std::get<2>(l) * 100, 1) + "% of all paths";
      s.items.push_back(e);
      double dx = p2.first - mx, dy = p2.second - my, len = std::max(1e-6, std::hypot(dx, dy));
      dx /= len; dy /= len;
      double r = rad(v) + 1, tx = p2.first - dx * r, ty = p2.second - dy * r, sz = 3.6;
      Prim ah; ah.type = Prim::Path; ah.fill = true; ah.fillC = ec; ah.group = "links";
      ah.d = {{'M', float(tx), float(ty), 0, 0}, {'L', float(tx - dx * sz - dy * sz * 0.6), float(ty - dy * sz + dx * sz * 0.6), 0, 0}, {'L', float(tx - dx * sz + dy * sz * 0.6), float(ty - dy * sz - dx * sz * 0.6), 0, 0}, {'Z', 0, 0, 0, 0}};
      s.items.push_back(ah);
    }
  for (int d : M.docs) {
    const Record& r = corp.recs[size_t(d)];
    auto p = pos[d];
    bool g = G.count(d) > 0;
    Prim c = C(p.first, p.second, rad(d), g ? t.accent : t.accent2.withA(0.9f));
    c.stroke = true; c.strokeC = t.bg; c.sw = 0.8f;
    c.group = "hit:" + std::to_string(d);
    c.tip = shortCite(r) + "\n" + truncate(r.title, 90) + "\n" + (g ? "On the global main path" : "On a key route") + " \xC2\xB7 " + fmtInt(r.cites) + " citations overall";
    s.items.push_back(c);
    string sur = firstAuthorSurname(r);
    Prim tl = T(p.first, p.second - rad(d) - 3, truncate(sur.empty() ? r.title : sur, 16) + " " + std::to_string(r.year), t.font * 0.7, g ? t.fg : t.muted, 1, g);
    tl.halo = true; tl.haloC = t.bg; tl.haloW = 1.6f;
    s.items.push_back(tl);
  }
  // key
  s.items.push_back(C(a.x0 + 4, 12, 4, t.accent));
  s.items.push_back(T(a.x0 + 12, 12 + t.font * 0.3, "Global main path", t.font * 0.8, t.muted, 0));
  s.items.push_back(C(a.x0 + 118, 12, 4, t.accent2.withA(0.9f)));
  s.items.push_back(T(a.x0 + 126, 12 + t.font * 0.3, "Key routes", t.font * 0.8, t.muted, 0));
  return s;
}

Scene chartSweep(const Sweep& S, double current, double w, double h, const ChartTheme& t) {
  Scene s = base(w, h, t);
  if (S.pts.empty()) { s.items.push_back(message(w, h, "Run the sweep to see how the clusters change with the resolution", t)); return s; }
  size_t n = S.pts.size();
  double kmax = 2;
  for (auto& p : S.pts) kmax = std::max(kmax, double(p.clustersMax));
  Axes a{34, 24, w - 36, h - 22, -0.5, double(n) - 0.5, 0, kmax * 1.1};
  yAxis(s, a, t, "Clusters");
  Axes b = a;
  b.vy0 = 0; b.vy1 = 1;
  yAxis(s, b, t, "Agreement", true);
  auto res = [](double r) {
    string v = fmtNum(r, 2);
    while (v.find('.') != string::npos && (v.back() == '0' || v.back() == '.')) { bool dot = v.back() == '.'; v.pop_back(); if (dot) break; }
    return v;
  };
  double bw = (a.x1 - a.x0) / double(n) * 0.62;
  for (size_t i = 0; i < n; i++) {
    const SweepPoint& p = S.pts[i];
    double x = a.X(double(i)), y = a.Y(p.clusters);
    bool cur = std::fabs(p.resolution - current) < 1e-6, best = int(i) == S.best;
    Prim r = R(x - bw / 2, y, bw, a.y1 - y, cur ? t.accent : best ? t.accent.withA(0.55f) : t.muted.withA(0.32f));
    r.group = "hit:" + std::to_string(i);
    r.tip = "Resolution " + res(p.resolution) + (cur ? " (current)" : "") + (best ? " (suggested)" : "") + "\n" + std::to_string(p.clusters) + " clusters" +
            (p.clustersMin != p.clustersMax ? " (" + std::to_string(p.clustersMin) + "\xE2\x80\x93" + std::to_string(p.clustersMax) + ")" : "") + "\nAgreement between seeds " + fmtFixed(p.meanAri, 2) +
            "\nLargest cluster " + fmtNum(p.largest * 100, 0) + "% of items\nClick to use this resolution";
    s.items.push_back(r);
    if (p.clustersMin != p.clustersMax) s.items.push_back(L(x, a.Y(p.clustersMin), x, a.Y(p.clustersMax), t.fg.withA(0.45f), 0.8));
    s.items.push_back(T(x, h - 6, res(p.resolution), t.font * 0.8, cur ? t.accent : t.muted, 1, cur || best));
  }
  Prim ln; ln.type = Prim::Path; ln.stroke = true; ln.strokeC = t.accent2; ln.sw = 1.6f; ln.group = "lines";
  for (size_t i = 0; i < n; i++) ln.d.push_back({i ? 'L' : 'M', float(b.X(double(i))), float(b.Y(S.pts[i].meanAri)), 0, 0});
  s.items.push_back(ln);
  for (size_t i = 0; i < n; i++) {
    Prim c = C(b.X(double(i)), b.Y(S.pts[i].meanAri), int(i) == S.best ? 4 : 2.6, t.accent2, "lines");
    c.stroke = true; c.strokeC = t.bg; c.sw = 0.8f;
    s.items.push_back(c);
  }
  return s;
}
}  // namespace vs
