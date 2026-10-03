#include "gfx.h"
#include <dxgi1_3.h>

#include <cstdio>

namespace vs {
namespace win {

std::wstring w16(const string& s) { return widen(s); }

bool Gfx::init(HWND h, string* err) {
  hwnd = h;
  RECT rc;
  GetClientRect(h, &rc);
  W = std::max<int>(1, rc.right - rc.left);
  H = std::max<int>(1, rc.bottom - rc.top);
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = UINT(W);
  sd.BufferDesc.Height = UINT(H);
  sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = h;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  D3D_FEATURE_LEVEL fl{};
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3, D3D11_SDK_VERSION, &sd,
                                             swap.put(), dev.put(), &fl, ctx.put());
  if (FAILED(hr)) {
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3, D3D11_SDK_VERSION, &sd, swap.put(),
                                       dev.put(), &fl, ctx.put());
    warp = true;
  }
  if (FAILED(hr)) { if (err) { char b[64]; snprintf(b, sizeof b, "Direct3D 11 device creation failed (0x%08lx)", (unsigned long)hr); *err = b; } return false; }
  featureLevel = fl >= D3D_FEATURE_LEVEL_11_0 ? "11_0" : "10_x";
  {
    Com<IDXGIDevice> xd;
    Com<IDXGIAdapter> ad;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(xd.put()))) && SUCCEEDED(xd->GetAdapter(ad.put()))) {
      DXGI_ADAPTER_DESC d;
      if (SUCCEEDED(ad->GetDesc(&d))) adapter = narrow(d.Description);
    }
    Com<IDXGIFactory> xf;
    if (ad && SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(xf.put())))) xf->MakeWindowAssociation(h, DXGI_MWA_NO_ALT_ENTER);
  }
  UINT q = 0;
  if (FAILED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &q)) || q == 0) msaa = 1;
  D2D1_FACTORY_OPTIONS fo{};
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, reinterpret_cast<void**>(d2f.put()));
  if (FAILED(hr)) { if (err) *err = "Direct2D 1.1 is not available"; return false; }
  Com<IDXGIDevice> xd;
  dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(xd.put()));
  if (FAILED(d2f->CreateDevice(xd.get(), d2dev.put())) || FAILED(d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()))) { if (err) *err = "Direct2D device creation failed"; return false; }
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush.put());
  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put()));
  if (FAILED(hr)) { if (err) *err = "DirectWrite is not available"; return false; }
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
  uiFamily = resolveFamily({"Segoe UI Variable Text", "Segoe UI", "Tahoma", "Arial", "Liberation Sans", "DejaVu Sans"});
  monoFamily = resolveFamily({"Cascadia Mono", "Consolas", "Courier New", "Liberation Mono", "DejaVu Sans Mono"});
  createTargets();
  return true;
}

string Gfx::resolveFamily(const vector<string>& cands) {
  Com<IDWriteFontCollection> fc;
  if (FAILED(dw->GetSystemFontCollection(fc.put(), FALSE))) return cands.empty() ? "Arial" : cands.back();
  for (auto& c : cands) {
    UINT32 idx;
    BOOL exists = FALSE;
    if (SUCCEEDED(fc->FindFamilyName(widen(c).c_str(), &idx, &exists)) && exists) return c;
  }
  return "Arial";
}

void Gfx::createTargets() {
  swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backTex.put()));
  dev->CreateRenderTargetView(backTex.get(), nullptr, rtvBack.put());
  if (msaa > 1) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(W); td.Height = UINT(H); td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = UINT(msaa); td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, msaaTex.put())) || FAILED(dev->CreateRenderTargetView(msaaTex.get(), nullptr, rtvMsaa.put()))) { msaa = 1; msaaTex.reset(); rtvMsaa.reset(); }
  }
  Com<IDXGISurface> surf;
  swap->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surf.put()));
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  dc->CreateBitmapFromDxgiSurface(surf.get(), &bp, target.put());
  dc->SetTarget(target.get());
}

void Gfx::releaseTargets() {
  copyDrop(canvasCopy);
  copyDrop(overlayCopy);
  dc->SetTarget(nullptr);
  target.reset();
  rtvBack.reset();
  backTex.reset();
  rtvMsaa.reset();
  msaaTex.reset();
}

void Gfx::resize(int w, int h) {
  w = std::max(1, w);
  h = std::max(1, h);
  if (w == W && h == H) return;
  W = w;
  H = h;
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  releaseTargets();
  ctx->Flush();
  swap->ResizeBuffers(0, UINT(W), UINT(H), DXGI_FORMAT_UNKNOWN, 0);
  createTargets();
}

void Gfx::beginGpu(const Color& c) {
  float cc[4] = {c.r, c.g, c.b, 1};
  ID3D11RenderTargetView* rt = msaa > 1 ? rtvMsaa.get() : rtvBack.get();
  ctx->ClearRenderTargetView(rt, cc);
  ctx->OMSetRenderTargets(1, &rt, nullptr);
}

void Gfx::endGpu() {
  if (msaa > 1) ctx->ResolveSubresource(backTex.get(), 0, msaaTex.get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

bool Gfx::beginD2D() {
  dc->BeginDraw();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  return true;
}

void Gfx::endD2D() {
  HRESULT hr = dc->EndDraw();
  if (hr == HRESULT(D2DERR_RECREATE_TARGET)) { releaseTargets(); createTargets(); }
}

void Gfx::copyDrop(BackCopy& c) {
  c.valid = false;
  c.bmp.reset();
  c.tex.reset();
  c.w = c.h = 0;
}

bool Gfx::copyCapture(BackCopy& c) {
  c.valid = false;
  if (!backTex || !dc || W <= 0 || H <= 0) return false;
  if (!c.tex || !c.bmp || c.w != W || c.h != H) {
    c.bmp.reset();
    c.tex.reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(W); td.Height = UINT(H); td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, c.tex.put()))) { c.tex.reset(); return false; }
    Com<IDXGISurface> surf;
    if (FAILED(c.tex->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(surf.put())))) { c.tex.reset(); return false; }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(dc->CreateBitmapFromDxgiSurface(surf.get(), &bp, c.bmp.put()))) { c.bmp.reset(); c.tex.reset(); return false; }
    c.w = W;
    c.h = H;
  }
  dc->Flush();  // pending Direct2D work reaches the back buffer before it is copied (harmless outside BeginDraw)
  ctx->CopyResource(c.tex.get(), backTex.get());
  c.valid = true;
  return true;
}

bool Gfx::copyRestore(const BackCopy& c) {
  if (!c.valid || !c.bmp || c.w != W || c.h != H) return false;
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  D2D1_RECT_F r = D2D1::RectF(0, 0, float(W), float(H));
  dc->DrawBitmap(c.bmp.get(), &r, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &r);
  return true;
}

bool Gfx::present(bool vsync) { return swap->Present(vsync ? 1 : 0, 0) != HRESULT(DXGI_STATUS_OCCLUDED); }
bool Gfx::presentTest() { return swap->Present(0, DXGI_PRESENT_TEST) != HRESULT(DXGI_STATUS_OCCLUDED); }
void Gfx::trim() {
  if (!ctx) return;
  ctx->ClearState();
  ctx->Flush();
  Com<IDXGIDevice3> d3;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice3), reinterpret_cast<void**>(d3.put())))) d3->Trim();
}

bool Gfx::readback(vector<uint8_t>& bgra, int& w, int& h) {
  D3D11_TEXTURE2D_DESC td;
  backTex->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
  Com<ID3D11Texture2D> st;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, st.put()))) return false;
  ctx->CopyResource(st.get(), backTex.get());
  D3D11_MAPPED_SUBRESOURCE ms;
  if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
  w = int(td.Width);
  h = int(td.Height);
  bgra.resize(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; y++) memcpy(&bgra[size_t(y) * size_t(w) * 4], static_cast<uint8_t*>(ms.pData) + size_t(y) * ms.RowPitch, size_t(w) * 4);
  ctx->Unmap(st.get(), 0);
  return true;
}

bool Gfx::encodeJpegWic(int w, int h, const uint8_t* bgra, float quality, string& out) {
  out.clear();
  if (!wic || w <= 0 || h <= 0) return false;
  Com<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return false;
  Com<IWICBitmapEncoder> enc;
  if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, enc.put()))) return false;
  if (FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
  Com<IWICBitmapFrameEncode> frame;
  Com<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(frame.put(), props.put()))) return false;
  if (props) {
    PROPBAG2 opt{};
    wchar_t name[] = L"ImageQuality";
    opt.pstrName = name;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = std::max(0.05f, std::min(1.f, quality));
    props->Write(1, &opt, &v);
  }
  if (FAILED(frame->Initialize(props.get()))) return false;
  if (FAILED(frame->SetSize(UINT(w), UINT(h)))) return false;
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  if (FAILED(frame->SetPixelFormat(&fmt))) return false;
  bool bgr = IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) != 0;
  if (!bgr && !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) && !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGR)) return false;
  UINT stride = bgr ? UINT(w) * 3 : UINT(w) * 4;
  vector<uint8_t> row(size_t(stride) * size_t(h));
  for (int y = 0; y < h; y++) {
    const uint8_t* src = bgra + size_t(y) * size_t(w) * 4;
    uint8_t* dst = &row[size_t(y) * stride];
    if (bgr) { for (int x = 0; x < w; x++) { dst[x * 3] = src[x * 4]; dst[x * 3 + 1] = src[x * 4 + 1]; dst[x * 3 + 2] = src[x * 4 + 2]; } }
    else { memcpy(dst, src, size_t(w) * 4); for (int x = 0; x < w; x++) dst[x * 4 + 3] = 255; }
  }
  if (FAILED(frame->WritePixels(UINT(h), stride, UINT(row.size()), row.data()))) return false;
  if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;
  STATSTG st{};
  if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
  HGLOBAL hg = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.get(), &hg)) || !hg) return false;
  const char* mem = static_cast<const char*>(GlobalLock(hg));
  if (!mem) return false;
  out.assign(mem, size_t(st.cbSize.QuadPart));
  GlobalUnlock(hg);
  return !out.empty();
}

bool Gfx::savePngWic(const string& path, int w, int h, const uint8_t* bgra, double dpi) {
  // encode with the portable PNG writer (keeps pHYs DPI metadata exact); convert BGRA -> RGBA
  vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
  bool alpha = false;
  for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
    uint8_t a = bgra[i * 4 + 3];
    if (a != 255) alpha = true;
    // un-premultiply
    auto up = [&](uint8_t v) { return a ? uint8_t(std::min(255, v * 255 / a)) : uint8_t(0); };
    rgba[i * 4] = up(bgra[i * 4 + 2]); rgba[i * 4 + 1] = up(bgra[i * 4 + 1]); rgba[i * 4 + 2] = up(bgra[i * 4]); rgba[i * 4 + 3] = a;
  }
  return writeFileU(path, pngEncode(w, h, rgba.data(), alpha, dpi));
}

IDWriteTextFormat* Gfx::format(float size, int weight, bool mono, bool italic, bool wrap) {
  uint64_t key = (uint64_t(size * 16) << 20) ^ (uint64_t(weight) << 4) ^ (mono ? 1 : 0) ^ (italic ? 2 : 0) ^ (wrap ? 4 : 0);
  auto it = fmts_.find(key);
  if (it != fmts_.end()) return it->second.get();
  Com<IDWriteTextFormat> f;
  dw->CreateTextFormat(widen(mono ? monoFamily : uiFamily).c_str(), nullptr, DWRITE_FONT_WEIGHT(weight), italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put());
  if (f) f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  fmts_[key] = f;
  return f.get();
}

// ------------------------------------------------------------ Scene -> D2D
namespace {
// Collects the glyph outlines of a text layout. Text halos are drawn as one stroked outline with round joins, like
// the SVG and PDF writers do; offset copies of the text leave a lumpy, uneven edge at export resolutions.
class GlyphOutlines final : public IDWriteTextRenderer {
 public:
  explicit GlyphOutlines(ID2D1Factory* fac) : f_(fac) {}
  vector<Com<ID2D1TransformedGeometry>> geos;
  // stack object: no reference counting
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) { *out = this; return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* d) override { *d = TRUE; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override { *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0}; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override { *p = 1; return S_OK; }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, DWRITE_GLYPH_RUN const* run, DWRITE_GLYPH_RUN_DESCRIPTION const*, IUnknown*) override {
    if (!run || !run->fontFace || run->glyphCount == 0) return S_OK;
    Com<ID2D1PathGeometry> pg;
    if (FAILED(f_->CreatePathGeometry(pg.put()))) return S_OK;
    Com<ID2D1GeometrySink> sk;
    if (FAILED(pg->Open(sk.put()))) return S_OK;
    sk->SetFillMode(D2D1_FILL_MODE_WINDING);
    HRESULT hr = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets, run->glyphCount, run->isSideways,
                                                   (run->bidiLevel & 1) != 0, sk.get());
    sk->Close();
    if (FAILED(hr)) return S_OK;
    Com<ID2D1TransformedGeometry> tg;
    if (SUCCEEDED(f_->CreateTransformedGeometry(pg.get(), D2D1::Matrix3x2F::Translation(x, y), tg.put()))) geos.push_back(tg);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*, IUnknown*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*, IUnknown*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

 private:
  ID2D1Factory* f_;
};
}  // namespace

void drawScene(ID2D1RenderTarget* rt, ID2D1Factory* f, IDWriteFactory* dw, Gfx& g, const Scene& sc, float ox, float oy, float s, bool serif) {
  Com<ID2D1SolidColorBrush> b;
  rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), b.put());
  D2D1_MATRIX_3X2_F old;
  rt->GetTransform(&old);
  rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(ox, oy) * old);
  std::wstring fam = widen(serif ? g.resolveFamily({"Times New Roman", "Liberation Serif", "DejaVu Serif"}) : g.resolveFamily({"Arial", "Helvetica", "Liberation Sans", "Segoe UI", "DejaVu Sans"}));
  std::map<std::pair<int, int>, Com<IDWriteTextFormat>> fcache;
  std::map<uint32_t, Com<ID2D1GradientStopCollection>> gcache;
  Com<ID2D1StrokeStyle> haloStroke;
  std::wstring famMono;
  auto fmt = [&](float size, bool bold, bool italic = false, bool mono = false) {
    auto key = std::make_pair(int(size * 20), (bold ? 1 : 0) | (italic ? 2 : 0) | (mono ? 4 : 0));
    auto it = fcache.find(key);
    if (it != fcache.end()) return it->second.get();
    if (mono && famMono.empty()) famMono = widen(g.resolveFamily({"Consolas", "Cascadia Mono", "Courier New", "DejaVu Sans Mono"}));
    Com<IDWriteTextFormat> tf;
    dw->CreateTextFormat(mono ? famMono.c_str() : fam.c_str(), nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", tf.put());
    if (tf) tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fcache[key] = tf;
    return tf.get();
  };
  for (auto& p : sc.items) {
    switch (p.type) {
      case Prim::Rect: {
        D2D1_RECT_F r = D2D1::RectF(p.x, p.y, p.x + p.w, p.y + p.h);
        if (p.fill) { b->SetColor(d2c(p.fillC)); rt->FillRectangle(r, b.get()); }
        if (p.stroke) { b->SetColor(d2c(p.strokeC)); rt->DrawRectangle(r, b.get(), p.sw); }
        break;
      }
      case Prim::Circle: {
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(p.x, p.y), p.r, p.r);
        if (p.fill && p.sphere) {
          // same radial shading as the SVG/PDF writers and the GPU shader
          uint32_t key = (uint32_t(p.fillC.r * 255) << 16) | (uint32_t(p.fillC.g * 255) << 8) | uint32_t(p.fillC.b * 255);
          auto it = gcache.find(key);
          if (it == gcache.end()) {
            vector<D2D1_GRADIENT_STOP> gs;
            for (auto& st : sphereStops(p.fillC)) gs.push_back(D2D1::GradientStop(st.t, D2D1::ColorF(st.c.r, st.c.g, st.c.b, 1)));
            Com<ID2D1GradientStopCollection> col;
            rt->CreateGradientStopCollection(gs.data(), UINT32(gs.size()), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, col.put());
            it = gcache.emplace(key, col).first;
          }
          Com<ID2D1RadialGradientBrush> rb;
          if (it->second && SUCCEEDED(rt->CreateRadialGradientBrush(
                  D2D1::RadialGradientBrushProperties(D2D1::Point2F(p.x, p.y), D2D1::Point2F(SPHERE_FX * p.r, SPHERE_FY * p.r), p.r, p.r),
                  D2D1::BrushProperties(p.fillC.a), it->second.get(), rb.put()))) rt->FillEllipse(e, rb.get());
        } else if (p.fill) { b->SetColor(d2c(p.fillC)); rt->FillEllipse(e, b.get()); }
        if (p.stroke) { b->SetColor(d2c(p.strokeC)); rt->DrawEllipse(e, b.get(), p.sw); }
        break;
      }
      case Prim::Path: {
        Com<ID2D1PathGeometry> geo;
        f->CreatePathGeometry(geo.put());
        Com<ID2D1GeometrySink> sink;
        geo->Open(sink.put());
        // geometry is built at 16x and drawn through a 1/16 scale: figure units are small (points), and
        // polygon tessellators with absolute tolerances can leave sliver artefacts at that scale
        const float U = 16.f;
        bool open = false;
        for (auto& d : p.d) {
          if (d.op == 'M') {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(D2D1::Point2F(d.x1 * U, d.y1 * U), p.fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            open = true;
          } else if (d.op == 'L') sink->AddLine(D2D1::Point2F(d.x1 * U, d.y1 * U));
          else if (d.op == 'Q') sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(d.x1 * U, d.y1 * U), D2D1::Point2F(d.x2 * U, d.y2 * U)));
          else if (d.op == 'Z' && open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; }
        }
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        D2D1_MATRIX_3X2_F cur;
        rt->GetTransform(&cur);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(1 / U, 1 / U) * cur);
        if (p.fill) { b->SetColor(d2c(p.fillC)); rt->FillGeometry(geo.get(), b.get()); }
        if (p.stroke) {
          Com<ID2D1StrokeStyle> ss;
          D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(p.roundCap ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT, p.roundCap ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT,
                                                                        D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10, p.dash.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM, 0);
          vector<float> dashes;
          for (float d : p.dash) dashes.push_back(d / std::max(0.01f, p.sw));
          f->CreateStrokeStyle(sp, dashes.empty() ? nullptr : dashes.data(), UINT32(dashes.size()), ss.put());
          b->SetColor(d2c(p.strokeC));
          rt->DrawGeometry(geo.get(), b.get(), p.sw * U, ss.get());
        }
        rt->SetTransform(cur);
        break;
      }
      case Prim::Text: {
        IDWriteTextFormat* tf = fmt(p.size, p.bold, p.italic, p.mono);
        if (!tf) break;
        std::wstring ws = widen(p.text);
        Com<IDWriteTextLayout> lay;
        dw->CreateTextLayout(ws.c_str(), UINT32(ws.size()), tf, 10000, 1000, lay.put());
        if (!lay) break;
        if (p.underline) { DWRITE_TEXT_RANGE all{0, UINT32(ws.size())}; lay->SetUnderline(TRUE, all); }
        DWRITE_TEXT_METRICS tm;
        lay->GetMetrics(&tm);
        DWRITE_LINE_METRICS lm;
        UINT32 nl = 0;
        lay->GetLineMetrics(&lm, 1, &nl);
        float baseline = nl ? lm.baseline : p.size * 0.8f;
        float x = p.x - (p.anchor == 1 ? tm.widthIncludingTrailingWhitespace / 2 : (p.anchor == 2 ? tm.widthIncludingTrailingWhitespace : 0));
        float y = p.y - baseline;
        if (p.halo && p.haloW > 0) {
          // Smooth, filled halo: same paint-order="stroke" approach as SVG/PDF. Draw the glyph as a filled shape offset in enough
          // directions (24) at the halo radius. D2D DrawTextLayout fills glyphs with the brush colour, so every offset copy adds to a
          // continuous opaque matte; the text is filled on top. This matches the print-quality appearance of the vector exports.
          b->SetColor(d2c(p.haloC));
          float r = p.haloW;
          int steps = 24;
          for (int k = 0; k < steps; k++) {
            float a = float(k) * 6.2831853f / steps;
            rt->DrawTextLayout(D2D1::Point2F(x + r * std::cos(a), y + r * std::sin(a)), lay.get(), b.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
          }
        }
        b->SetColor(d2c(p.fillC));
        rt->DrawTextLayout(D2D1::Point2F(x, y), lay.get(), b.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        break;
      }
      case Prim::Image: {
        vector<uint8_t> bgra(p.rgba.size());
        for (size_t i = 0; i + 3 < p.rgba.size(); i += 4) {
          uint8_t a = p.rgba[i + 3];
          bgra[i] = uint8_t(p.rgba[i + 2] * a / 255); bgra[i + 1] = uint8_t(p.rgba[i + 1] * a / 255); bgra[i + 2] = uint8_t(p.rgba[i] * a / 255); bgra[i + 3] = a;
        }
        Com<ID2D1Bitmap> bmp;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(UINT32(p.imgW), UINT32(p.imgH)), bgra.data(), UINT32(p.imgW * 4), bp, bmp.put())))
          rt->DrawBitmap(bmp.get(), D2D1::RectF(p.x, p.y, p.x + p.w, p.y + p.h), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        break;
      }
    }
  }
  rt->SetTransform(old);
}

bool renderSceneBGRA(Gfx& g, const Scene& sc, double dpi, vector<uint8_t>& bgra, int& w, int& h, bool transparent) {
  if (!g.wic) return false;
  double k = dpi / 72.0;
  w = std::max(1, int(std::lround(sc.W * k)));
  h = std::max(1, int(std::lround(sc.H * k)));
  if (double(w) * h > 16384.0 * 16384.0 / 2) return false;
  Com<IWICBitmap> bmp;
  if (FAILED(g.wic->CreateBitmap(UINT(w), UINT(h), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put()))) return false;
  D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  Com<ID2D1RenderTarget> rt;
  if (FAILED(g.d2f->CreateWicBitmapRenderTarget(bmp.get(), rp, rt.put()))) return false;
  rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  rt->BeginDraw();
  rt->Clear(transparent ? D2D1::ColorF(0, 0, 0, 0) : D2D1::ColorF(1, 1, 1, 1));
  drawScene(rt.get(), g.d2f.get(), g.dw.get(), g, sc, 0, 0, float(k));
  if (FAILED(rt->EndDraw())) return false;
  WICRect r{0, 0, w, h};
  bgra.resize(size_t(w) * size_t(h) * 4);
  return SUCCEEDED(bmp->CopyPixels(&r, UINT(w * 4), UINT(bgra.size()), bgra.data()));
}

bool makeHybridScene(Gfx& g, const Scene& source, double dpi, Scene& hybrid, string* err) {
  hybrid.W = source.W; hybrid.H = source.H; hybrid.serif = source.serif;
  hybrid.warnings = source.warnings; hybrid.labels = source.labels; hybrid.candidates = source.candidates;
  hybrid.minLine = source.minLine; hybrid.minLabel = source.minLabel;
  hybrid.items.clear(); hybrid.items.reserve(source.items.size());
  auto paintable = [](const Prim& p) { return p.type == Prim::Image || p.fill || p.stroke; };
  size_t i = 0;
  while (i < source.items.size()) {
    if (source.items[i].type == Prim::Text) {
      hybrid.items.push_back(source.items[i++]);
      continue;
    }
    vector<size_t> run;
    while (i < source.items.size() && source.items[i].type != Prim::Text) {
      if (paintable(source.items[i])) run.push_back(i);
      ++i;
    }
    if (run.empty()) continue;

    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    auto bounds = [&](double ax, double ay, double bx, double by, double pad) {
      x0 = std::min(x0, std::min(ax, bx) - pad); y0 = std::min(y0, std::min(ay, by) - pad);
      x1 = std::max(x1, std::max(ax, bx) + pad); y1 = std::max(y1, std::max(ay, by) + pad);
    };
    for (size_t index : run) {
      const Prim& p = source.items[index];
      double pad = p.stroke ? std::max(0.5, double(p.sw) * 0.5) : 0.0;
      if (p.type == Prim::Rect || p.type == Prim::Image) bounds(p.x, p.y, p.x + p.w, p.y + p.h, pad);
      else if (p.type == Prim::Circle) bounds(p.x - p.r, p.y - p.r, p.x + p.r, p.y + p.r, pad);
      else if (p.type == Prim::Path) for (const PathCmd& d : p.d) {
        if (d.op == 'M' || d.op == 'L') bounds(d.x1, d.y1, d.x1, d.y1, pad);
        else if (d.op == 'Q') { bounds(d.x1, d.y1, d.x1, d.y1, pad); bounds(d.x2, d.y2, d.x2, d.y2, pad); }
      }
    }
    if (!(x0 < x1 && y0 < y1)) continue;
    // A point of transparent padding retains antialias coverage on strokes at the crop edge.
    x0 = clampv(std::floor(x0 - 1.0), 0.0, source.W); y0 = clampv(std::floor(y0 - 1.0), 0.0, source.H);
    x1 = clampv(std::ceil(x1 + 1.0), 0.0, source.W); y1 = clampv(std::ceil(y1 + 1.0), 0.0, source.H);
    if (!(x0 < x1 && y0 < y1)) continue;

    Scene crop;
    crop.W = x1 - x0; crop.H = y1 - y0; crop.serif = source.serif;
    crop.items.reserve(run.size());
    for (size_t index : run) {
      Prim p = source.items[index];
      p.x -= float(x0); p.y -= float(y0);
      if (p.type == Prim::Path) for (PathCmd& d : p.d) {
        if (d.op == 'M' || d.op == 'L' || d.op == 'Q') { d.x1 -= float(x0); d.y1 -= float(y0); }
        if (d.op == 'Q') { d.x2 -= float(x0); d.y2 -= float(y0); }
      }
      crop.items.push_back(std::move(p));
    }
    vector<uint8_t> bgra;
    int w = 0, h = 0;
    if (!renderSceneBGRA(g, crop, dpi, bgra, w, h, true)) {
      if (err) *err = "Hybrid raster layer rendering failed (the selected DPI may exceed the available image size).";
      return false;
    }
    Prim image;
    image.type = Prim::Image; image.group = "hybrid-artwork";
    image.x = float(x0); image.y = float(y0); image.w = float(crop.W); image.h = float(crop.H);
    image.imgW = w; image.imgH = h; image.rgba.resize(bgra.size());
    // renderSceneBGRA returns premultiplied BGRA; SVG PNG and PDF soft masks require straight RGBA.
    for (size_t k = 0; k + 3 < bgra.size(); k += 4) {
      uint8_t a = bgra[k + 3];
      image.rgba[k] = a ? uint8_t(std::min(255u, (unsigned(bgra[k + 2]) * 255u + a / 2u) / a)) : 0;
      image.rgba[k + 1] = a ? uint8_t(std::min(255u, (unsigned(bgra[k + 1]) * 255u + a / 2u) / a)) : 0;
      image.rgba[k + 2] = a ? uint8_t(std::min(255u, (unsigned(bgra[k]) * 255u + a / 2u) / a)) : 0;
      image.rgba[k + 3] = a;
    }
    hybrid.items.push_back(std::move(image));
  }
  return true;
}

bool exportScenePNG(Gfx& g, const Scene& sc, double dpi, const string& path, string* err, bool transparent) {
  vector<uint8_t> px;
  int w, h;
  if (!renderSceneBGRA(g, sc, dpi, px, w, h, transparent)) { if (err) *err = "PNG rendering failed (figure too large for the chosen DPI?)"; return false; }
  if (!g.savePngWic(path, w, h, px.data(), dpi)) { if (err) *err = "Could not write " + path; return false; }
  return true;
}

}  // namespace win
}  // namespace vs
