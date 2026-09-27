// VOSStudio Native — visual encoding shared by the live GPU canvas and all exports
#pragma once
#include "model.h"

namespace vs {

enum class ViewKind { Network, Overlay, Density, Timeline, Matrix, Geo, ThreeD };
const char* viewLabel(ViewKind v);
enum class ColorBy { Cluster, Score, Single };
enum class LinkColor { Cluster, Gradient, Grey, Single, Weight };
enum class LinkGeom { Straight, Curved, Arc };
enum class LabelPlace { Side, Centre };
enum class BorderCol { Contrast, None, Cluster, Custom };
enum class Backdrop { Theme, White, Light, Dark, Black };

struct ViewStyle {
  string look = "vosviewer";
  string palette = "vosClassic";
  std::map<int, Color> clusterOverride;
  // Sizing model. 1 = VOSviewer: circles, labels and lines have a constant on-screen pixel size (radius
  // max(16*nw^v, 5)/2, label 9 + 4*nw^v px, line max(1.5*rs^v, 0.1) px, nw = weight / mean weight); zooming
  // spreads items apart instead of inflating them. 0 = Studio (1.3): world-sized nodes that grow with zoom.
  int sizing = 1;
  float sizeVar = 0.5f, linkVar = 0.5f;
  bool flat = true;
  float nodeOpacity = 0.75f, border = 0.f;
  BorderCol borderCol = BorderCol::None;
  Color borderCustom = Color::hex(0xffffff);
  float baseSize = 3.4f, maxSize = 13.f, scale = 1.f, labelVar = 0.75f;
  ColorBy colorBy = ColorBy::Cluster;
  Color single = Color::hex(0x4f8ef7);
  string scheme = "viridis";
  double scoreMin = NAN, scoreMax = NAN;  // NaN = auto (5th–95th percentile)
  LabelPlace labelPlace = LabelPlace::Centre;
  int labelWeight = 400;
  float labelSize = 1.f;
  int maxLen = 30, maxLabels = 0;  // 0 = automatic (collision driven)
  bool truncate = true, labelHalo = false, labelByCluster = false;
  bool linksVisible = true;
  LinkColor linkColor = LinkColor::Cluster;
  Color linkSingle = Color::hex(0x8fa3c8);
  float linkOpacity = 0.4f, curvature = 0.5f, linkWidth = 1.f, linkMaxW = 6.f;
  LinkGeom linkGeom = LinkGeom::Curved;
  int maxLines = 1000;
  double minStrength = 0;
  float kernel = 1.f, densityAlpha = 0.55f;
  string densityScheme = "vos";
  // densityFull: colour the whole canvas (VOSviewer) instead of fading low density into the background.
  // kernelAuto: kernel width = 0.125 x average item distance (van Eck & Waltman 2010) times `kernel`.
  bool densityFull = true, kernelAuto = true;
  bool densityByCluster = false;
  Backdrop backdrop = Backdrop::White;
  bool darkTheme = true;
  bool hulls = false, clusterNames = false, bundle = false;
  int cvd = 0;  // 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia (simulation)
  float zScale = 1.f;
};

struct LookPreset { const char* id; const char* label; const char* sub; };
const vector<LookPreset>& lookPresets();
void applyLook(ViewStyle& s, const string& id);

struct PaletteInfo { const char* id; const char* label; bool cvdSafe; };
const vector<PaletteInfo>& paletteList();
const vector<Color>& palette(const string& id);
const vector<string>& colormapList();
Color cmapAt(const string& name, double t);
Color simulateCvd(const Color& c, int mode);
Color lightenLab(const Color& c);  // VOSviewer link tint: L* -> 0.6 L* + 40

// Themed colours for the canvas backdrop
struct Theme { Color bg, fg, muted, halo, grid, panel; bool light = false; };
Theme canvasTheme(const ViewStyle& s);

struct Encoder {
  const Network* net = nullptr;
  const ViewStyle* st = nullptr;
  double maxW = 1, maxLinkW = 1, sMin = 0, sMax = 1, meanW = 1, meanLinkW = 1;
  vector<int> linkOrder;  // links to draw (heaviest first, capped)
  void prepare(const Network& n, const ViewStyle& s);
  double radius(int i) const;              // world units (≈ px at zoom 1)
  double weightT(int i) const;             // 0..1 relative weight
  Color clusterColor(int c) const;
  Color nodeColor(int i, ViewKind kind) const;
  Color linkColorOf(int li, ViewKind kind) const;
  float linkAlpha(int li) const;
  float linkWidthT(int li) const;          // 0..1
  double scoreT(double v) const;
  string label(int i) const;
  float labelScale(int i) const;           // relative label size factor
  // VOSviewer sizing model (screen pixels at scale 1; multiply by UI scale or by points-per-pixel in figures)
  bool vos() const { return st && st->sizing == 1; }
  double vosNw(int i) const;
  double vosRadiusPx(int i) const;
  double vosLabelPx(int i) const;
  double vosLinkPx(int li) const;
  Color linkEnd(int li, ViewKind kind, bool bEnd) const;  // per-endpoint colour (VOS gradient in VOS mode)
};

}  // namespace vs
