// VOSStudio Native — Direct3D 11 + Direct2D + DirectWrite device layer
#pragma once
#include "platform.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wincodec.h>

#include "../core/figure.h"

namespace vs {
namespace win {

template <class T> struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com& o) : p(o.p) { if (p) p->AddRef(); }
  Com& operator=(const Com& o) { if (o.p) o.p->AddRef(); reset(); p = o.p; return *this; }
  ~Com() { reset(); }
  void reset() { if (p) { p->Release(); p = nullptr; } }
  T** put() { reset(); return &p; }
  T* operator->() const { return p; }
  T* get() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

inline D2D1_COLOR_F d2c(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
inline D2D1_COLOR_F d2c(const Color& c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

class Gfx {
 public:
  HWND hwnd = nullptr;
  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  Com<IDXGISwapChain> swap;
  Com<ID3D11RenderTargetView> rtvBack;
  Com<ID3D11Texture2D> backTex, msaaTex;
  Com<ID3D11RenderTargetView> rtvMsaa;
  int msaa = 4;
  Com<ID2D1Factory1> d2f;
  Com<ID2D1Device> d2dev;
  Com<ID2D1DeviceContext> dc;
  Com<ID2D1Bitmap1> target;
  Com<IDWriteFactory> dw;
  Com<IWICImagingFactory> wic;
  Com<ID2D1SolidColorBrush> brush;
  int W = 0, H = 0;
  string adapter, featureLevel;
  string uiFamily, monoFamily;
  bool warp = false;

  bool init(HWND h, string* err);
  void resize(int w, int h);
  void beginGpu(const Color& clear);   // binds MSAA target
  void endGpu();                        // resolve into back buffer
  bool beginD2D();
  void endD2D();
  bool present(bool vsync);  // false when the window is occluded (nothing visible was presented)
  bool presentTest();        // true when presenting would be visible again
  void trim();               // release transient GPU memory (minimised)
  bool readback(vector<uint8_t>& bgra, int& w, int& h);  // current back buffer
  bool savePngWic(const string& path, int w, int h, const uint8_t* bgra, double dpi);
  bool encodeJpegWic(int w, int h, const uint8_t* bgra, float quality, string& out);  // in-memory JPEG (pictures for the live assistant)

  // Copies of the back buffer taken between two drawing passes (Direct2D commands are flushed first). They let the
  // application redraw only the layer that changed: the canvas copy holds the map (GPU render + labels) so that a
  // hover over a panel redraws just the chrome; the overlay copy holds the whole window without the Live assistant's
  // overlay so that its orb can move alone. The copies live on the GPU; nothing is read back.
  struct BackCopy {
    Com<ID3D11Texture2D> tex;
    Com<ID2D1Bitmap1> bmp;
    int w = 0, h = 0;
    bool valid = false;
  };
  BackCopy canvasCopy, overlayCopy;
  bool copyCapture(BackCopy& c);         // copy the current back buffer (call between drawing passes, inside or outside BeginDraw)
  bool copyRestore(const BackCopy& c);   // draw the copy over the whole target (inside BeginDraw); false = nothing drawn
  void copyDrop(BackCopy& c);

  IDWriteTextFormat* format(float size, int weight = 400, bool mono = false, bool italic = false, bool wrap = false);
  string resolveFamily(const vector<string>& candidates);
  ID2D1SolidColorBrush* br(const Color& c) { brush->SetColor(d2c(c)); return brush.get(); }

 private:
  std::map<uint64_t, Com<IDWriteTextFormat>> fmts_;
  void createTargets();
  void releaseTargets();
};

// Draw a figure Scene into any D2D render target (screen preview, WIC bitmap for PNG)
void drawScene(ID2D1RenderTarget* rt, ID2D1Factory* f, IDWriteFactory* dw, Gfx& g, const Scene& sc, float ox, float oy, float scale, bool serif = false);
// Render a Scene at dpi to a top-down BGRA buffer via a WIC bitmap render target
bool renderSceneBGRA(Gfx& g, const Scene& sc, double dpi, vector<uint8_t>& bgra, int& w, int& h, bool transparent = false);
// Preserve scene paint order by rasterizing each contiguous non-text run into a cropped transparent image layer;
// text primitives remain native SVG/PDF text. The raster layers are embedded (no sidecar assets).
bool makeHybridScene(Gfx& g, const Scene& source, double dpi, Scene& hybrid, string* err = nullptr);
bool exportScenePNG(Gfx& g, const Scene& sc, double dpi, const string& path, string* err, bool transparent = false);

// UTF-8 helpers for DirectWrite
std::wstring w16(const string& s);

}  // namespace win
}  // namespace vs
