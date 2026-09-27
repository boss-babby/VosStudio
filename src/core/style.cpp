#include "style.h"

namespace vs {

const char* viewLabel(ViewKind v) {
  switch (v) {
    case ViewKind::Network: return "Network";
    case ViewKind::Overlay: return "Overlay";
    case ViewKind::Density: return "Density";
    case ViewKind::Timeline: return "Timeline";
    case ViewKind::Matrix: return "Matrix";
    case ViewKind::Geo: return "Geo";
    case ViewKind::ThreeD: return "3D";
  }
  return "";
}

const vector<LookPreset>& lookPresets() {
  static const vector<LookPreset> L = {
      {"vosviewer", "VOSviewer", "White \xC2\xB7 constant-size flat circles \xC2\xB7 centred labels"},
      {"studio", "Studio 1.3", "Previous look \xC2\xB7 shaded nodes grow with zoom"},
      {"paper", "Clean publication", "White \xC2\xB7 outlined nodes \xC2\xB7 Okabe\xE2\x80\x93Ito"},
      {"midnight", "Midnight", "Black \xC2\xB7 luminous nodes \xC2\xB7 Tol bright"},
  };
  return L;
}

void applyLook(ViewStyle& s, const string& id) {
  s.look = id;
  // rendering fields shared by the VOSviewer-style looks; "studio" restores the 1.3 defaults
  auto vosRender = [&] { s.sizing = 1; s.labelHalo = false; s.densityScheme = "vos"; s.densityFull = true; s.kernelAuto = true; s.curvature = .5f; };
  if (id == "studio") { s.sizing = 0; s.labelHalo = true; s.densityScheme = "bgy"; s.densityFull = false; s.kernelAuto = false; s.backdrop = Backdrop::Theme; s.flat = false; s.nodeOpacity = .95f; s.border = 1.2f; s.borderCol = BorderCol::Contrast; s.labelPlace = LabelPlace::Side; s.labelWeight = 450; s.linkOpacity = .5f; s.linkGeom = LinkGeom::Curved; s.curvature = .24f; s.palette = "cvd"; }
  else if (id == "vosviewer") { vosRender(); s.backdrop = Backdrop::White; s.flat = true; s.nodeOpacity = .75f; s.border = 0; s.borderCol = BorderCol::None; s.labelPlace = LabelPlace::Centre; s.labelWeight = 400; s.linkOpacity = .4f; s.linkGeom = LinkGeom::Curved; s.palette = "vosClassic"; }
  else if (id == "paper") { vosRender(); s.backdrop = Backdrop::White; s.flat = true; s.nodeOpacity = 1; s.border = .9f; s.borderCol = BorderCol::Cluster; s.labelPlace = LabelPlace::Side; s.labelWeight = 500; s.linkOpacity = .26f; s.linkGeom = LinkGeom::Curved; s.curvature = .18f; s.palette = "okabe"; }
  else if (id == "midnight") { vosRender(); s.labelHalo = true; s.backdrop = Backdrop::Black; s.flat = false; s.nodeOpacity = .92f; s.border = 0; s.borderCol = BorderCol::None; s.labelPlace = LabelPlace::Side; s.labelWeight = 500; s.linkOpacity = .42f; s.linkGeom = LinkGeom::Curved; s.curvature = .22f; s.palette = "tolBright"; }
}

static vector<Color> hexes(std::initializer_list<uint32_t> l) { vector<Color> v; for (auto h : l) v.push_back(Color::hex(h)); return v; }

const vector<PaletteInfo>& paletteList() {
  static const vector<PaletteInfo> P = {
      {"cvd", "VOSStudio (CVD-aware)", true}, {"okabe", "Okabe\xE2\x80\x93Ito", true}, {"tolBright", "Tol bright", true}, {"tolMuted", "Tol muted", true},
      {"tolVibrant", "Tol vibrant", true}, {"vosClassic", "VOSviewer classic", false}, {"pastel", "Pastel", false}, {"bold", "Bold", false},
  };
  return P;
}

const vector<Color>& palette(const string& id) {
  static const std::map<string, vector<Color>> M = {
      {"cvd", hexes({0x4f8ef7, 0xf2a33c, 0x4ec98a, 0xe168a8, 0x5cc8d6, 0xef6f5a, 0xc9d24b, 0x9b7ff0, 0xf08c3a, 0x3fb6a1, 0x7f9cf5, 0xd76fd0})},
      {"okabe", hexes({0x0072B2, 0xE69F00, 0x009E73, 0xCC79A7, 0x56B4E9, 0xD55E00, 0xF0E442, 0x999999})},
      {"tolBright", hexes({0x4477AA, 0xEE6677, 0x228833, 0xCCBB44, 0x66CCEE, 0xAA3377, 0xBBBBBB})},
      {"tolMuted", hexes({0x332288, 0x88CCEE, 0x44AA99, 0x117733, 0x999933, 0xDDCC77, 0xCC6677, 0x882255, 0xAA4499})},
      {"tolVibrant", hexes({0x0077BB, 0x33BBEE, 0x009988, 0xEE7733, 0xCC3311, 0xEE3377, 0xBBBBBB})},
      {"vosClassic", hexes({0xd9453d, 0x4cae4c, 0x3f7ec5, 0xc8c436, 0x9b6fd1, 0x3fbfbf, 0xf0913a, 0xa0674b, 0xe57fb5, 0x8c8c8c, 0x6b8e23, 0x4b5dbf})},
      {"pastel", hexes({0x8fb8f0, 0xf5c98f, 0x9fe0b5, 0xf0a8cf, 0xa9dee4, 0xf2a99a, 0xe0e28f, 0xc6b3f2, 0xf5cfa8, 0xa8d8cf})},
      {"bold", hexes({0x2f6df6, 0xff8a00, 0x00b861, 0xe5007d, 0x00b3c6, 0xff3b30, 0xa3b000, 0x7a3ff2, 0xff5c00, 0x008f7a})},
  };
  auto it = M.find(id);
  return it != M.end() ? it->second : M.at("cvd");
}

const vector<string>& colormapList() {
  static const vector<string> L = {"vos", "viridis", "plasma", "magma", "coolwarm", "bgy", "grey"};
  return L;
}

Color cmapAt(const string& name, double t) {
  static const std::map<string, vector<std::array<int, 3>>> C = {
      // VOSviewer's viridis variant (sampled from VOSviewer Online), starts at a lighter blue-violet
      {"vos", {{66, 64, 134}, {54, 93, 141}, {42, 118, 142}, {33, 143, 141}, {34, 167, 133}, {64, 189, 114}, {119, 209, 83}, {186, 222, 40}, {253, 231, 37}}},
      {"viridis", {{68, 1, 84}, {72, 40, 120}, {62, 74, 137}, {49, 104, 142}, {38, 130, 142}, {31, 158, 137}, {53, 183, 121}, {109, 205, 89}, {253, 231, 37}}},
      {"plasma", {{13, 8, 135}, {84, 2, 163}, {139, 10, 165}, {185, 50, 137}, {219, 92, 104}, {244, 136, 73}, {254, 188, 43}, {240, 249, 33}}},
      {"magma", {{0, 0, 4}, {28, 16, 68}, {79, 18, 123}, {129, 37, 129}, {181, 54, 122}, {229, 80, 100}, {251, 135, 97}, {254, 194, 135}, {252, 253, 191}}},
      {"coolwarm", {{59, 76, 192}, {110, 130, 239}, {169, 183, 242}, {221, 221, 221}, {244, 180, 152}, {220, 110, 90}, {180, 4, 38}}},
      {"bgy", {{48, 84, 196}, {64, 140, 214}, {86, 196, 186}, {146, 220, 110}, {228, 236, 80}}},
      {"grey", {{26, 26, 30}, {80, 80, 90}, {140, 140, 150}, {196, 196, 206}, {246, 246, 250}}},
  };
  auto it = C.find(name);
  const auto& c = it != C.end() ? it->second : C.at("viridis");
  if (!std::isfinite(t)) t = 0;
  t = clampv(t, 0.0, 1.0);
  double x = t * double(c.size() - 1);
  size_t i = size_t(std::floor(x)), j = std::min(c.size() - 1, i + 1);
  double f = x - double(i);
  return Color(float((c[i][0] + (c[j][0] - c[i][0]) * f) / 255), float((c[i][1] + (c[j][1] - c[i][1]) * f) / 255), float((c[i][2] + (c[j][2] - c[i][2]) * f) / 255));
}

Color simulateCvd(const Color& c, int mode) {
  static const double M[3][9] = {
      {.152286, 1.052583, -.204868, .114503, .786281, .099216, -.003882, -.048116, 1.051998},
      {.367322, .860646, -.227968, .280085, .672501, .047413, -.01182, .04294, .968881},
      {1.255528, -.076749, -.178779, -.078411, .930809, .147602, .004733, .691367, .3039},
  };
  if (mode < 1 || mode > 3) return c;
  auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  auto enc = [](double v) { v = clampv(v, 0.0, 1.0); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; };
  const double* m = M[mode - 1];
  double r = lin(c.r), g = lin(c.g), b = lin(c.b);
  return Color(float(enc(m[0] * r + m[1] * g + m[2] * b)), float(enc(m[3] * r + m[4] * g + m[5] * b)), float(enc(m[6] * r + m[7] * g + m[8] * b)), c.a);
}

Theme canvasTheme(const ViewStyle& s) {
  Theme t;
  Backdrop b = s.backdrop;
  if (b == Backdrop::Theme) b = s.darkTheme ? Backdrop::Dark : Backdrop::Light;
  switch (b) {
    case Backdrop::White: t.bg = Color::hex(0xffffff); break;
    case Backdrop::Light: t.bg = Color::hex(0xf5f6f8); break;
    case Backdrop::Black: t.bg = Color::hex(0x050608); break;
    default: t.bg = Color::hex(0x14171c); break;
  }
  t.light = t.bg.luma() > 0.5f;
  t.fg = t.light ? Color::hex(0x1a1b1f) : Color::hex(0xe9ecf2);
  t.muted = t.light ? Color::hex(0x5b5e66) : Color::hex(0x9aa1ad);
  t.halo = t.bg;
  t.grid = t.light ? Color(0, 0, 0, 0.05f) : Color(1, 1, 1, 0.045f);
  t.panel = t.light ? Color::hex(0xffffff) : Color::hex(0x1b1f26);
  return t;
}

void Encoder::prepare(const Network& n, const ViewStyle& s) {
  net = &n;
  st = &s;
  maxW = 1e-9;
  for (int i = 0; i < n.n(); i++) maxW = std::max(maxW, n.weight(i));
  maxLinkW = 1e-9;
  for (auto& l : n.links) maxLinkW = std::max(maxLinkW, l.w);
  meanW = 0;
  for (int i = 0; i < n.n(); i++) meanW += n.weight(i);
  meanW = n.n() && meanW > 0 ? meanW / n.n() : 1;
  meanLinkW = 0;
  for (auto& l : n.links) meanLinkW += l.w;
  meanLinkW = n.m() && meanLinkW > 0 ? meanLinkW / n.m() : 1;
  // score range: 5th..95th percentile unless fixed
  vector<double> sc;
  for (int i = 0; i < n.n(); i++) { double v = n.score(i); if (std::isfinite(v)) sc.push_back(v); }
  std::sort(sc.begin(), sc.end());
  if (!sc.empty()) {
    sMin = sc[size_t(0.05 * double(sc.size() - 1))];
    sMax = sc[size_t(0.95 * double(sc.size() - 1))];
    if (sMax - sMin < 1e-9) { sMin = sc.front(); sMax = sc.back(); }
    if (sMax - sMin < 1e-9) sMax = sMin + 1;
  } else { sMin = 0; sMax = 1; }
  if (std::isfinite(s.scoreMin)) sMin = s.scoreMin;
  if (std::isfinite(s.scoreMax)) sMax = s.scoreMax;
  linkOrder.clear();
  for (int k = 0; k < n.m(); k++) if (n.links[k].w >= s.minStrength) linkOrder.push_back(k);
  std::sort(linkOrder.begin(), linkOrder.end(), [&](int a, int b) { return n.links[a].w > n.links[b].w; });
  if (s.maxLines > 0 && int(linkOrder.size()) > s.maxLines) linkOrder.resize(size_t(s.maxLines));
  std::reverse(linkOrder.begin(), linkOrder.end());  // draw weak first so strong links sit on top
}

double Encoder::weightT(int i) const { return clampv(net->weight(i) / std::max(1e-9, maxW), 0.0, 1.0); }

double Encoder::radius(int i) const {
  double t = weightT(i);
  return (st->baseSize + (st->maxSize - st->baseSize) * std::pow(t, std::max(0.05, double(st->labelVar)))) * st->scale;
}

double Encoder::vosNw(int i) const { return std::max(0.0, net->weight(i)) / meanW; }
double Encoder::vosRadiusPx(int i) const { return st->scale * std::max(16 * std::pow(vosNw(i), double(st->sizeVar)), 5.0) / 2; }
double Encoder::vosLabelPx(int i) const { return st->labelSize * (st->scale * 9 + 4 * std::pow(vosNw(i), double(st->sizeVar))); }
double Encoder::vosLinkPx(int li) const {
  double rs = std::max(0.0, net->links[li].w) / meanLinkW;
  return st->linkWidth * st->scale * std::max(1.5 * std::pow(rs, double(st->linkVar)), 0.1);
}

Color lightenLab(const Color& c) {
  auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  auto enc = [](double v) { v = clampv(v, 0.0, 1.0); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; };
  double r = lin(c.r), g = lin(c.g), b = lin(c.b);
  double X = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047, Y = 0.2126 * r + 0.7152 * g + 0.0722 * b, Z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
  auto f = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 * t + 16) / 116; };
  double fx = f(X), fy = f(Y), fz = f(Z);
  double L = 116 * fy - 16, A = 500 * (fx - fy), B = 200 * (fy - fz);
  L = 0.6 * L + 40;
  fy = (L + 16) / 116; fx = fy + A / 500; fz = fy - B / 200;
  auto fi = [](double t) { return t * t * t > 216.0 / 24389 ? t * t * t : (116 * t - 16) / (24389.0 / 27); };
  X = fi(fx) * 0.95047; Y = fi(fy); Z = fi(fz) * 1.08883;
  double R = 3.2406 * X - 1.5372 * Y - 0.4986 * Z, G = -0.9689 * X + 1.8758 * Y + 0.0415 * Z, Bl = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
  return Color(float(enc(R)), float(enc(G)), float(enc(Bl)), c.a);
}

Color Encoder::linkEnd(int li, ViewKind kind, bool bEnd) const {
  if (vos() && st->linkColor == LinkColor::Cluster && kind != ViewKind::Overlay) {
    const Link& l = net->links[li];
    return lightenLab(nodeColor(bEnd ? l.b : l.a, kind));
  }
  return linkColorOf(li, kind);
}

Color Encoder::clusterColor(int c) const {
  Color col;
  auto it = st->clusterOverride.find(c);
  if (it != st->clusterOverride.end()) col = it->second;
  else if (c < 0) col = Color::hex(0x8c8c8c);
  else { const auto& p = palette(st->palette); col = p[size_t(c) % p.size()]; if (size_t(c) >= p.size()) col = col.mix(Color(1, 1, 1), 0.28f * float(size_t(c) / p.size())); }
  return simulateCvd(col, st->cvd);
}

double Encoder::scoreT(double v) const { return std::isfinite(v) ? clampv((v - sMin) / (sMax - sMin), 0.0, 1.0) : NAN; }

Color Encoder::nodeColor(int i, ViewKind kind) const {
  bool useScore = kind == ViewKind::Overlay || kind == ViewKind::Timeline || st->colorBy == ColorBy::Score;
  if (useScore) {
    double t = scoreT(net->score(i));
    if (!std::isfinite(t)) return simulateCvd(Color::hex(0x9a9a9a), st->cvd);
    return simulateCvd(cmapAt(st->scheme, t), st->cvd);
  }
  if (st->colorBy == ColorBy::Single) return simulateCvd(st->single, st->cvd);
  return clusterColor(net->nodes[i].cluster);
}

Color Encoder::linkColorOf(int li, ViewKind kind) const {
  const Link& l = net->links[li];
  switch (st->linkColor) {
    case LinkColor::Grey: return Color(0.55f, 0.55f, 0.55f);
    case LinkColor::Single: return simulateCvd(st->linkSingle, st->cvd);
    case LinkColor::Weight: return simulateCvd(cmapAt(st->scheme, l.w / maxLinkW), st->cvd);
    default: break;
  }
  Color a = nodeColor(l.a, kind), b = nodeColor(l.b, kind);
  if (kind == ViewKind::Overlay || st->linkColor == LinkColor::Gradient) return a.mix(b, 0.5f);
  return a;
}

float Encoder::linkAlpha(int li) const {
  if (vos()) return float(clampv(double(st->linkOpacity), 0.02, 1.0));
  double t = clampv(net->links[li].w / maxLinkW, 0.0, 1.0);
  return float(clampv(st->linkOpacity * (0.45 + 0.55 * t), 0.04, 1.0));
}

float Encoder::linkWidthT(int li) const { return float(std::sqrt(clampv(net->links[li].w / maxLinkW, 0.0, 1.0))); }

string Encoder::label(int i) const {
  const string& s = net->nodes[i].label;
  return st->truncate ? truncate(s, size_t(st->maxLen)) : s;
}

float Encoder::labelScale(int i) const {
  double t = weightT(i);
  return float((9.5 + 6 * st->labelVar * std::pow(t, 0.55)) / 9.5);
}

}  // namespace vs
