// Scene -> Enhanced Metafile (EMF+ dual), the vector picture format every Windows application pastes:
// Word / PowerPoint / Excel take it from the clipboard (Ctrl+V gives a scalable drawing, not a bitmap) and Word reads
// it from the VML shapes of clipboard HTML. Drawn with GDI+, whose recorder writes the metafile; the drawing
// model mirrors drawScene (gfx.cpp) and the SVG / PDF writers: rectangles, circles (sphere shading as a path
// gradient), paths with quadratic curves, text (halos as stroked glyph outlines), images.
#include <windows.h>
#include <objidl.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
using std::min;
using std::max;
#include <gdiplus.h>
#include "../core/common.h"
#include "../core/figure.h"
#include "platform.h"

namespace vs {
namespace win {

namespace {
bool gdiplusReady() {
  static ULONG_PTR token = 0;
  static bool tried = false, ok = false;
  if (!tried) {
    tried = true;
    Gdiplus::GdiplusStartupInput si;
    ok = Gdiplus::GdiplusStartup(&token, &si, nullptr) == Gdiplus::Ok;
  }
  return ok;
}

Gdiplus::Color gc(const Color& c, float alphaMul = 1.f) {
  auto b = [](float v) { return BYTE(std::lround(std::min(1.f, std::max(0.f, v)) * 255)); };
  return Gdiplus::Color(b(c.a * alphaMul), b(c.r), b(c.g), b(c.b));
}

const wchar_t* familyFor(const Prim& p, bool serif, std::wstring& tmp) {
  if (!p.family.empty()) { tmp = widen(p.family); return tmp.c_str(); }
  if (p.mono) return L"Consolas";
  bool useSerif = p.face == 2 || (p.face == 0 && serif);
  return useSerif ? L"Times New Roman" : L"Arial";
}

struct FontKey { std::wstring fam; int size; int style; bool operator<(const FontKey& o) const { return std::tie(fam, size, style) < std::tie(o.fam, o.size, o.style); } };

void drawPrims(Gdiplus::Graphics& g, const Scene& sc) {
  using namespace Gdiplus;
  std::map<std::wstring, std::unique_ptr<FontFamily>> families;
  auto family = [&](const wchar_t* name) -> FontFamily* {
    auto it = families.find(name);
    if (it != families.end()) return it->second.get();
    std::unique_ptr<FontFamily> ff(new FontFamily(name));
    if (!ff->IsAvailable()) {
      const wchar_t* alt = std::wstring(name) == L"Consolas" ? L"Courier New" : std::wstring(name) == L"Times New Roman" ? L"Georgia" : L"Segoe UI";
      ff.reset(new FontFamily(alt));
      if (!ff->IsAvailable()) ff.reset(new FontFamily(L"Microsoft Sans Serif"));
    }
    return families.emplace(name, std::move(ff)).first->second.get();
  };
  StringFormat sf(StringFormat::GenericTypographic());
  sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
  for (auto& p : sc.items) {
    switch (p.type) {
      case Prim::Rect: {
        if (p.fill) { SolidBrush b(gc(p.fillC)); g.FillRectangle(&b, p.x, p.y, p.w, p.h); }
        if (p.stroke && p.sw > 0) { Pen pen(gc(p.strokeC), p.sw); g.DrawRectangle(&pen, p.x, p.y, p.w, p.h); }
        break;
      }
      case Prim::Circle: {
        RectF e(p.x - p.r, p.y - p.r, 2 * p.r, 2 * p.r);
        if (p.fill && p.sphere && p.r > 0.05f) {
          GraphicsPath path;
          path.AddEllipse(e);
          PathGradientBrush pg(&path);
          vector<SphereStop> stops = sphereStops(p.fillC);
          // GDI+ path gradients interpolate from the boundary (0) to the centre (1); the shading model runs from the
          // focal point (t = 0) to the far rim (t = 1)
          std::vector<Gdiplus::Color> cols;
          std::vector<REAL> pos;
          for (auto it = stops.rbegin(); it != stops.rend(); ++it) { cols.push_back(gc(it->c, p.fillC.a)); pos.push_back(REAL(std::min(1.f, std::max(0.f, 1.f - it->t)))); }
          if (!pos.empty()) { pos.front() = 0; pos.back() = 1; }
          pg.SetCenterPoint(PointF(p.x + SPHERE_FX * p.r, p.y + SPHERE_FY * p.r));
          if (cols.size() >= 2) pg.SetInterpolationColors(cols.data(), pos.data(), INT(cols.size()));
          else { pg.SetCenterColor(gc(p.fillC)); Gdiplus::Color sc1 = gc(p.fillC); INT n = 1; pg.SetSurroundColors(&sc1, &n); }
          g.FillEllipse(&pg, e);
        } else if (p.fill) { SolidBrush b(gc(p.fillC)); g.FillEllipse(&b, e); }
        if (p.stroke && p.sw > 0) { Pen pen(gc(p.strokeC), p.sw); g.DrawEllipse(&pen, e); }
        break;
      }
      case Prim::Path: {
        GraphicsPath path(p.evenOdd ? FillModeAlternate : FillModeWinding);
        float cx = 0, cy = 0, sx = 0, sy = 0;
        bool open = false;
        for (auto& d : p.d) {
          if (d.op == 'M') { path.StartFigure(); open = true; cx = sx = d.x1; cy = sy = d.y1; }
          else if (d.op == 'L') { path.AddLine(cx, cy, d.x1, d.y1); cx = d.x1; cy = d.y1; }
          else if (d.op == 'Q') {
            float c1x = cx + 2.f / 3.f * (d.x1 - cx), c1y = cy + 2.f / 3.f * (d.y1 - cy);
            float c2x = d.x2 + 2.f / 3.f * (d.x1 - d.x2), c2y = d.y2 + 2.f / 3.f * (d.y1 - d.y2);
            path.AddBezier(cx, cy, c1x, c1y, c2x, c2y, d.x2, d.y2);
            cx = d.x2; cy = d.y2;
          } else if (d.op == 'Z') { if (open) path.CloseFigure(); cx = sx; cy = sy; open = false; }
        }
        if (p.fill) { SolidBrush b(gc(p.fillC)); g.FillPath(&b, &path); }
        if (p.stroke && p.sw > 0) {
          Pen pen(gc(p.strokeC), p.sw);
          pen.SetLineJoin(LineJoinRound);
          if (p.roundCap) { pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetDashCap(DashCapRound); }
          if (!p.dash.empty()) {
            std::vector<REAL> dash;
            for (float d : p.dash) dash.push_back(REAL(std::max(0.01f, d / std::max(0.01f, p.sw))));
            if (dash.size() % 2) dash.insert(dash.end(), dash.begin(), dash.end());
            pen.SetDashPattern(dash.data(), INT(dash.size()));
          }
          g.DrawPath(&pen, &path);
        }
        break;
      }
      case Prim::Text: {
        if (p.text.empty() || p.size <= 0) break;
        std::wstring tmp;
        FontFamily* ff = family(familyFor(p, sc.serif, tmp));
        if (!ff) break;
        INT style = (p.bold ? FontStyleBold : 0) | (p.italic ? FontStyleItalic : 0) | (p.underline ? FontStyleUnderline : 0);
        if (!ff->IsStyleAvailable(style)) style = FontStyleRegular;
        std::wstring ws = widen(p.text);
        Font font(ff, p.size, style, UnitPoint);
        RectF bounds;
        g.MeasureString(ws.c_str(), INT(ws.size()), &font, PointF(0, 0), &sf, &bounds);
        float ascent = float(ff->GetCellAscent(style)) * p.size / float(std::max<UINT16>(1, ff->GetEmHeight(style)));
        float x = p.x - (p.anchor == 1 ? bounds.Width / 2 : p.anchor == 2 ? bounds.Width : 0);
        float y = p.y - ascent;
        if (p.halo && p.haloW > 0) {
          // halo = the glyph outlines stroked behind the fill, as the SVG / PDF writers do (paint-order stroke)
          GraphicsPath glyphs;
          glyphs.AddString(ws.c_str(), INT(ws.size()), ff, style, p.size, PointF(x, y), &sf);
          Pen halo(gc(p.haloC), p.haloW * 2);
          halo.SetLineJoin(LineJoinRound);
          g.DrawPath(&halo, &glyphs);
          SolidBrush b(gc(p.fillC));
          g.FillPath(&b, &glyphs);
        } else {
          SolidBrush b(gc(p.fillC));
          g.DrawString(ws.c_str(), INT(ws.size()), &font, PointF(x, y), &sf, &b);
        }
        break;
      }
      case Prim::Image: {
        if (p.imgW <= 0 || p.imgH <= 0 || p.rgba.size() < size_t(p.imgW) * size_t(p.imgH) * 4) break;
        std::vector<BYTE> bgra(p.rgba.size());
        for (size_t i = 0; i + 3 < p.rgba.size(); i += 4) { bgra[i] = p.rgba[i + 2]; bgra[i + 1] = p.rgba[i + 1]; bgra[i + 2] = p.rgba[i]; bgra[i + 3] = p.rgba[i + 3]; }
        Bitmap bmp(p.imgW, p.imgH, p.imgW * 4, PixelFormat32bppARGB, bgra.data());
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(&bmp, RectF(p.x, p.y, p.w, p.h), 0.f, 0.f, REAL(p.imgW), REAL(p.imgH), UnitPixel);
        break;
      }
    }
  }
}
}  // namespace

string renderSceneEMF(const Scene& sc, double wPt, double hPt) {
  if (!gdiplusReady() || sc.W <= 0 || sc.H <= 0) return string();
  if (wPt <= 0) wPt = sc.W;
  if (hPt <= 0) hPt = wPt * sc.H / sc.W;
  HDC ref = GetDC(nullptr);
  if (!ref) return string();
  string out;
  {
    Gdiplus::RectF frame(0, 0, Gdiplus::REAL(wPt), Gdiplus::REAL(hPt));
    Gdiplus::Metafile mf(ref, frame, Gdiplus::MetafileFrameUnitPoint, Gdiplus::EmfTypeEmfPlusDual, L"VOSStudio figure");
    if (mf.GetLastStatus() == Gdiplus::Ok) {
      {
        Gdiplus::Graphics g(&mf);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetPageUnit(Gdiplus::UnitPoint);
        g.ScaleTransform(Gdiplus::REAL(wPt / sc.W), Gdiplus::REAL(hPt / sc.H));
        drawPrims(g, sc);
      }
      HENHMETAFILE h = mf.GetHENHMETAFILE();
      if (h) {
        UINT n = GetEnhMetaFileBits(h, 0, nullptr);
        if (n) { out.resize(n); GetEnhMetaFileBits(h, n, reinterpret_cast<LPBYTE>(&out[0])); }
        DeleteEnhMetaFile(h);
      }
    }
  }
  ReleaseDC(nullptr, ref);
  return out;
}

}  // namespace win
}  // namespace vs
