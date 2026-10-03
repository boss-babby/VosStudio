// VOSStudio Native — GPU network canvas (Direct3D 11 instanced rendering + Direct2D overlay)
#pragma once
#include <functional>

#include "gfx.h"

namespace vs {
namespace win {

enum NodeFlag : uint32_t { NF_SELECTED = 1, NF_HOVER = 2, NF_DIM = 4, NF_HIDDEN = 8, NF_NEIGHBOUR = 16, NF_PINNED = 32, NF_MATCH = 64 };

struct Camera {
  double x = 0, y = 0, zoom = 1;         // 2D: world point at canvas centre, px per world unit
  float yaw = -0.55f, pitch = 0.32f;     // 3D orbit
  double dist3 = 1;                      // 3D distance multiplier
};

struct Rect { float x = 0, y = 0, w = 0, h = 0; bool has(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; } float r() const { return x + w; } float b() const { return y + h; } };

class NetView {
 public:
  bool init(Gfx& g, string* err);
  void setData(const Network* net, const ViewStyle* st, const Bundles* bundles);
  // display positions (x, y, z) in world units; may differ from node positions (timeline, 3D, geo)
  vector<std::array<float, 3>> pos;
  vector<uint32_t> flags;
  ViewKind kind = ViewKind::Network;
  // Geo layers: colour as the overlay view, or hide nodes and links (density raster underneath, labels stay)
  ViewKind geoTint = ViewKind::Geo;
  bool hideMarks = false;
  ViewKind ck() const { return kind == ViewKind::Geo && geoTint == ViewKind::Overlay ? ViewKind::Overlay : kind; }
  Camera cam;
  Rect vp;                  // canvas rect in window px
  float uiScale = 1;
  bool linksDirty = true, nodesDirty = true;
  int hoverLink = -1;

  void renderGpu(const Color& bg);                        // inside Gfx::beginGpu/endGpu
  void drawOverlay(ID2D1DeviceContext* dc, const Theme& th, bool showLabels, int maxLabels);  // labels, hulls, names
  // camera
  void fit(double pad = 60);
  // Call when display positions or visibility change; invalidates projection-, bounds- and density-dependent caches.
  void positionsChanged();
  void visibilityChanged();
  double fitZoom() const { return fitZoom_; }
  bool worldToScreen(double x, double y, double z, float& sx, float& sy, float* depth = nullptr) const;
  void screenToWorld(float sx, float sy, double& x, double& y) const;
  int hitNode(float sx, float sy);
  vector<int> nodesInRect(const Rect& r);
  float nodeRadiusPx(int i) const;
  const Encoder& enc() const { return enc_; }
  int labelsShown = 0;
  double densityMax = 1;

  // ---- WYSIWYG capture (vector export of exactly what is on screen)
  struct CapLabel { string text; float x = 0, y = 0, size = 10; bool bold = false; Color col; bool halo = false; Color haloC; float haloR = 0; };
  struct CapHull { vector<std::pair<float, float>> pts; Color fill, stroke; float sw = 1; };
  vector<CapLabel> capLabels;  // filled by drawOverlay (labels and cluster names as placed this frame)
  vector<CapHull> capHulls;
  // Links, nodes, hulls and labels in canvas pixels (origin = vp top-left), scaled by k.
  void captureInto(Scene& sc, const Theme& th, double k, bool labels) const;

  // ---- optional background layer drawn under links and nodes (e.g. the Geo basemap), painted with Direct2D into a texture
  std::function<void(ID2D1DeviceContext*)> backgroundPainter;
  // paints backgroundPainter into an offscreen layer composited under links/nodes; call before Gfx::beginGpu
  void drawBackgroundLayer();
  // fixed bounds for fit() (Geo: the whole world); ignored when fitX0 >= fitX1
  double fitX0 = 0, fitY0 = 0, fitX1 = 0, fitY1 = 0;
  string lastError;

 private:
  Gfx* g_ = nullptr;
  const Network* net_ = nullptr;
  const ViewStyle* st_ = nullptr;
  const Bundles* bundles_ = nullptr;
  Encoder enc_;
  double fitZoom_ = 1;
  bool bounds3Valid_ = false;
  double bounds3X_ = 0, bounds3Y_ = 0, bounds3Z_ = 0, bounds3R_ = 1;
  // Node depth order changes with 3D yaw/pitch, but not with camera distance. Remember the last sorted view so
  // zooming in 3D does not rebuild CPU instance buffers when only the view-projection constants changed.
  bool depthOrderValid_ = false;
  float depthOrderYaw_ = 0, depthOrderPitch_ = 0;
  struct ScreenNode { float x = 0, y = 0, depth = 0, radius = 0; bool projected = false; };
  std::unordered_map<uint64_t, vector<int>> hitGrid_;
  vector<ScreenNode> hitScreen_;
  vector<uint32_t> hitSeen_;
  uint32_t hitQuery_ = 0;
  bool hitGridValid_ = false;
  uint64_t positionRevision_ = 0, hitPositionRevision_ = 0;
  Camera hitCam_;
  Rect hitVp_;
  float hitUiScale_ = 0, hitGridCell_ = 64;
  ViewKind hitKind_ = ViewKind::Network;
  void ensureHitGrid();
  void rebuildHitGrid();
  Com<ID3D11VertexShader> vsNode_, vsLink_, vsSplat_, vsFull_;
  Com<ID3D11PixelShader> psNode_, psLink_, psSplat_, psMap_;
  Com<ID3D11InputLayout> ilNode_, ilLink_;
  Com<ID3D11Buffer> cb_, nodeBuf_, linkBuf_, bundleBuf_;
  Com<ID3D11ShaderResourceView> bundleSrv_, lutSrv_;
  Com<ID3D11Texture2D> lutTex_, denTex_;
  Com<ID3D11RenderTargetView> denRtv_;
  Com<ID3D11ShaderResourceView> denSrv_;
  Com<ID3D11BlendState> blendPremul_, blendAdd_;
  Com<ID3D11Texture2D> bgTex_;
  Com<ID3D11ShaderResourceView> bgSrv_;
  Com<ID2D1Bitmap1> bgBmp_;
  Com<ID3D11PixelShader> psTex_;
  int bgW_ = 0, bgH_ = 0;
  Com<ID3D11RasterizerState> rs_;
  Com<ID3D11SamplerState> samp_;
  int nodeCap_ = 0, linkCap_ = 0, bundleCap_ = 0, denW_ = 0, denH_ = 0;
  int nNodeInst_ = 0, nLinkInst_ = 0, bundleStride_ = 0;
  string lutName_;
  std::array<float, 16> viewProj_{};
  float pxPerWorld_ = 1;
  bool is3D() const { return kind == ViewKind::ThreeD; }
  void buildMatrices();
  void uploadNodes();
  void uploadLinks();
  void ensureDensityTarget(int w, int h);
  // link colours at both ends and width in px (shared by the GPU upload and the vector capture)
  void linkStyle(int li, Color& ca, Color& cb, float& wid) const;
  // Density caches use display positions and visibility, not camera transforms. The world-space grid serves both
  // the CPU maximum estimate and exact local density-label queries; splats stay in a reusable dynamic GPU buffer.
  double avgItemDist();
  double avgDist_ = 0, denSig_ = 1;
  bool avgDistValid_ = false;
  vector<float> denWt_;
  bool denAuto_ = true, densityWeightsValid_ = false;
  bool densityGridValid_ = false, densitySplatValid_ = false, densitySplatByCluster_ = false;
  double densityGridCell_ = 0, densityGridSigma_ = 0;
  std::unordered_map<uint64_t, vector<int>> densityGrid_;
  Com<ID3D11Buffer> splatBuf_;
  int splatCap_ = 0, nSplatInst_ = 0;
  float densityT(float sx, float sy) const;  // 0..1 colormap position at a screen point
  void updateLut(const string& name);
  std::map<string, std::pair<Com<IDWriteTextLayout>, int>> layouts_;
  int frame_ = 0;
  IDWriteTextLayout* layout(const string& s, float size, int weight);
};

}  // namespace win
}  // namespace vs
