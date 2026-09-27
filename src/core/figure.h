// VOSStudio Native — publication figures: one display list (points), SVG + PDF writers.
// The Windows layer renders the same Scene through Direct2D for PNG/clipboard output.
#pragma once
#include <array>
#include <functional>
#include "algo.h"
#include "style.h"

namespace vs {

constexpr double MM2PT = 72.0 / 25.4;

struct PathCmd { char op = 'M'; float x1 = 0, y1 = 0, x2 = 0, y2 = 0; };  // M L Q(x1,y1 ctrl, x2,y2 end) Z

struct Prim {
  enum Type { Rect, Circle, Path, Text, Image } type = Rect;
  float x = 0, y = 0, w = 0, h = 0, r = 0;
  vector<PathCmd> d;
  bool fill = false, stroke = false, roundCap = false, evenOdd = false;
  Color fillC, strokeC;
  float sw = 0;
  vector<float> dash;
  string text;
  float size = 8;
  bool bold = false;
  int anchor = 0;  // 0 start, 1 middle, 2 end
  bool halo = false;
  Color haloC;
  float haloW = 0;
  int imgW = 0, imgH = 0;
  vector<uint8_t> rgba;  // imgW*imgH*4
  string group;          // svg layer id: background, links, nodes, labels, legend, text
  bool sphere = false;   // Circle: shaded like the on-screen 3D spheres (radial gradient in SVG/PDF/PNG)
  string tip;            // optional hover text (interactive charts); not exported
};

// Sphere shading shared by the GPU shader and every exporter: light from the upper left.
// The gradient focal point (specular highlight) in unit-radius coordinates, y down.
constexpr float SPHERE_FX = -0.23f, SPHERE_FY = -0.306f;
struct SphereStop { float t; Color c; };
vector<SphereStop> sphereStops(const Color& base);  // stops from the focal point (t=0) to the far rim (t=1)

struct Scene {
  double W = 0, H = 0;  // points
  vector<Prim> items;
  vector<string> warnings;
  int labels = 0, candidates = 0;
  double minLine = 1e9, minLabel = 1e9;
};

enum class FigTheme { Print, Screen, Slide };
struct FigureSpec {
  double wmm = 180, hmm = 120;
  double labelPt = 7, linkMaxPt = 1.2;
  FigTheme theme = FigTheme::Print;
  bool panelNetwork = true, panelOverlay = false, panelDensity = false;
  bool panelTimeline = false, panelGeo = false, panel3D = false, panelMatrix = false;  // 1.2: every view can be a panel
  bool legend = true, sizeLegend = true, colorbar = true, letters = true;
  bool transparent = false, serif = false, footer = false;
  bool pdfPages = false;  // PDF export: one page per panel instead of one page with all panels
  int shading = 0;  // nodes: 0 = match the view (spheres unless the look uses flat nodes), 1 = flat, 2 = spheres
  string title, caption;
  double dpi = 300;
};
struct FigPreset { const char* id; const char* label; double wmm, hmm, labelPt; };
const vector<FigPreset>& figPresets();

// text metrics (Helvetica AFM; serif is approximated) — width in points
double textWidth(const string& utf8, double size, bool bold);
vector<string> wrapText(const string& text, double size, double width, bool bold = false);

vector<uint8_t> densityRaster(const Network& net, const Encoder& enc, const Theme& th, int pw, int ph,
                              double s, double ox, double oy, double rx, double ry, double kx);

// ---- per-panel data for views whose geometry lives outside the core (Geo basemap, 3D projection)
// Panels of kinds Network / Overlay / Density / Timeline / Matrix are built by the core itself; a FigPanelDef
// with the same kind replaces the default geometry of that panel.
struct FigPanelCtx {
  double s = 1, ox = 0, oy = 0;          // world → figure: x * s + ox, y * s + oy (points)
  double rx = 0, ry = 0, rw = 0, rh = 0;  // panel region (points)
  Color bg, fg, muted;
  bool light = true;
  double fs = 7;                          // label size (pt)
};
struct FigPanelDef {
  ViewKind kind = ViewKind::Network;
  vector<std::array<double, 2>> pos;      // node positions in world units; empty = layout positions
  vector<float> rMul, fade;               // per node: radius multiplier (3D perspective), fade toward the background
  vector<int> order;                      // node paint order (back to front); empty = largest first
  vector<char> hidden;                    // per node: 1 = not drawn (no position in this view)
  vector<std::array<double, 2>> linkCtrl; // per link: curve control point in world units (3D panels bend in 3D); empty = bend in 2D
  bool bounds = false;                    // fit these world bounds instead of the nodes
  double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
  bool nodes = true, links = true, labels = true, clusterNames = true, clusterLegend = true;
  double bandBottom = 0;                  // strip reserved at the bottom of the panel (points)
  int colorAs = -1;                       // colour nodes and links as this ViewKind (Overlay colours on a Geo panel)
  std::function<void(Scene&, const FigPanelCtx&)> under, over;
  string note;                            // shown in the panel when it has nothing to draw
};

Scene buildFigure(const Network& net, const ViewStyle& style, const FigureSpec& spec, const Bundles* bundles,
                  const string& methodsShort = "", const vector<FigPanelDef>* panelDefs = nullptr);
// the panels a spec asks for, in view order
vector<ViewKind> figurePanels(const FigureSpec& spec);

string toSVG(const Scene& sc, bool serif = false);
string toPDF(const Scene& sc, const string& title = "VOSStudio figure");
// multi-page PDF: one page per scene (each page takes its scene size)
string toPDF(const vector<Scene>& pages, const string& title = "VOSStudio figure");
// a figure split into one scene per panel (same size, title and caption on every page, no panel letters)
vector<Scene> buildFigurePages(const Network& net, const ViewStyle& style, const FigureSpec& spec, const Bundles* bundles,
                               const string& methodsShort = "", const vector<FigPanelDef>* panelDefs = nullptr);

// Generic SVG/PDF helpers for charts (stats pages export their own scenes)
Scene chartScene(double wpt, double hpt, bool light);

}  // namespace vs
