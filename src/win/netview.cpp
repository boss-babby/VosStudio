#include "netview.h"

#include <d3dcompiler.h>

#include <cstdio>

namespace vs {
namespace win {

namespace {
const char* HLSL = R"HLSL(
cbuffer CB : register(b0) {
  row_major float4x4 VP;
  float vpW, vpH, pxPerWorld, is3D;
  float4 bg;
  float curvature, flatShade, borderPx, dimAlpha;
  float4 accent;
  float gammaD, byCluster, fadeT, denMax;
  float lineMul, bundleCount, denScale, sigmaW;
  float4 hoverCol;
};
Buffer<float2> bpts : register(t0);
Texture2D denTex : register(t0);
Texture2D lutTex : register(t1);
SamplerState samp : register(s0);

static const float2 CORNER[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };

struct NodeI { float3 pos : POS; float rad : RAD; float4 col : COL; float4 bcol : BCOL; float flg : FLG; };
struct NodeV { float4 pos : SV_Position; float2 uv : UV; float rpx : RPX; float4 col : COL; float4 bcol : BCOL; float flg : FLG; };

NodeV vsNode(uint vid : SV_VertexID, NodeI n) {
  NodeV o;
  float4 c = mul(float4(n.pos, 1), VP);
  float rpx = n.rad < 0 ? -n.rad : max(1.2, n.rad * pxPerWorld / c.w);
  float ext = rpx + 5.0;
  float2 k = CORNER[vid];
  o.pos = c + float4(k * ext * float2(2.0 / vpW, 2.0 / vpH) * c.w, 0, 0);
  o.uv = k * ext; o.rpx = rpx; o.col = n.col; o.bcol = n.bcol; o.flg = n.flg;
  return o;
}
float4 psNode(NodeV i) : SV_Target {
  float d = length(i.uv), r = i.rpx;
  float inside = saturate(r - d + 0.5);
  float3 col = i.col.rgb;
  if (flatShade < 0.5) {
    float2 q = i.uv / r;
    float nz = sqrt(saturate(1 - dot(q, q)));
    float3 N = float3(q.x, q.y, nz);
    float3 L = normalize(float3(-0.45, 0.6, 0.75));  // uv is y-up: light from the upper left (matches the SVG/PDF/PNG exporters)
    float dif = saturate(dot(N, L));
    float spec = pow(saturate(dot(reflect(-L, N), float3(0, 0, 1))), 28);
    col = col * (0.66 + 0.44 * dif) + spec * 0.22;
  }
  if (borderPx > 0.01 && i.bcol.a > 0) {
    float b = saturate(d - (r - borderPx) + 0.5);
    col = lerp(col, i.bcol.rgb, b * i.bcol.a);
  }
  uint f = (uint)(i.flg + 0.5);
  float a = i.col.a * inside;
  if (f & 4) a *= dimAlpha;
  float4 o = float4(col * a, a);
  if (f & 3) {
    float4 rc = (f & 1) ? accent : hoverCol;
    float ring = saturate(1.4 - abs(d - (r + 2.6)));
    o = float4(rc.rgb * ring, ring) + o * (1 - ring);
  }
  if (f & 64) {
    float ring = saturate(1.2 - abs(d - (r + 4.2)) ) * 0.9;
    o = float4(float3(1, 0.8, 0.2) * ring, ring) + o * (1 - ring);
  }
  return o;
}

struct LinkI { float3 a : PA; float3 b : PB; float4 ca : CA; float4 cb : CB; float wid : WID; float bund : BND; float lfl : LFL; };
struct LinkV { float4 pos : SV_Position; float4 col : COL; float across : ACR; float hw : HW; };
float3 curvePt(LinkI L, float t, uint inst) {
  if (L.bund >= 0) {
    float fi = t * (bundleCount - 1);
    uint i0 = (uint)floor(fi); uint i1 = min(i0 + 1, (uint)bundleCount - 1);
    float2 p = lerp(bpts.Load((uint)L.bund + i0), bpts.Load((uint)L.bund + i1), fi - i0);
    return float3(p, 0);
  }
  if (curvature <= 0.001 || L.bund < -1.5) return lerp(L.a, L.b, t);
  if (is3D > 0.5) {
    // 3D: bend in world space. The offset is cross(d, z): the same curve as the network view when
    // seen from the front, and links running along the depth axis stay straight.
    float3 d3 = L.b - L.a; float len3 = max(1e-6, length(d3));
    float3 m3 = (L.a + L.b) * 0.5 + float3(d3.y, -d3.x, 0) / len3 * (curvature * len3 * 0.22 * 2);
    float u3 = 1 - t;
    return u3 * u3 * L.a + 2 * u3 * t * m3 + t * t * L.b;
  }
  float2 d = L.b.xy - L.a.xy; float len = max(1e-6, length(d));
  float off = curvature * len * 0.22;
  float2 m = (L.a.xy + L.b.xy) * 0.5 + float2(d.y, -d.x) / len * off * 2;
  float u = 1 - t;
  return float3(u * u * L.a.xy + 2 * u * t * m + t * t * L.b.xy, 0);
}
LinkV vsLink(uint vid : SV_VertexID, uint inst : SV_InstanceID, LinkI L) {
  LinkV o;
  uint seg = vid / 6, k = vid % 6;
  uint nseg = L.bund >= 0 ? (uint)bundleCount - 1 : ((curvature <= 0.001 || L.bund < -1.5) ? 1 : 14);
  if (seg >= nseg) { o.pos = float4(0, 0, -2, 1); o.col = 0; o.across = 0; o.hw = 1; return o; }
  float t0 = (float)seg / nseg, t1 = (float)(seg + 1) / nseg;
  float4 c0 = mul(float4(curvePt(L, t0, inst), 1), VP), c1 = mul(float4(curvePt(L, t1, inst), 1), VP);
  float2 s0 = c0.xy / c0.w * float2(vpW, -vpH) * 0.5, s1 = c1.xy / c1.w * float2(vpW, -vpH) * 0.5;
  float2 dir = s1 - s0; float l = length(dir); dir = l > 1e-5 ? dir / l : float2(1, 0);
  float2 nrm = float2(-dir.y, dir.x);
  float2 kk = CORNER[k];
  bool endSide = kk.x > 0;
  float2 s = endSide ? s1 + dir * 0.35 : s0 - dir * 0.35;
  float w = max(1.0, L.wid);
  float hw = w * 0.5 + 1.0;
  s += nrm * kk.y * hw;
  float4 cc = endSide ? c1 : c0;
  o.pos = float4(s.x / (vpW * 0.5), -s.y / (vpH * 0.5), cc.z / cc.w, 1);
  float t = endSide ? t1 : t0;
  float4 col = lerp(L.ca, L.cb, t);
  uint f = (uint)(L.lfl + 0.5);
  if (f & 1) col.a *= dimAlpha * 0.6;
  if (f & 2) col.a = min(1, col.a * 2.2);
  col.a *= min(1, L.wid) * lineMul;
  o.col = col; o.across = kk.y * hw; o.hw = w * 0.5;
  return o;
}
float4 psLink(LinkV i) : SV_Target {
  float cov = saturate(i.hw - abs(i.across) + 0.5);
  float a = i.col.a * cov;
  return float4(i.col.rgb * a, a);
}

struct SplatV { float4 pos : SV_Position; float2 uv : UV; float4 col : COL; };
SplatV vsSplat(uint vid : SV_VertexID, NodeI n) {
  SplatV o;
  float4 c = mul(float4(n.pos, 1), VP);
  float spx = sigmaW * pxPerWorld * denScale;
  float ext = spx * 3.0;
  float2 k = CORNER[vid];
  o.pos = c + float4(k * ext * float2(2.0 / (vpW * denScale), 2.0 / (vpH * denScale)) * c.w, 0, 0);
  o.uv = k * 3.0; o.col = n.col;
  return o;
}
float4 psSplat(SplatV i) : SV_Target {
  float w = exp(-0.5 * dot(i.uv, i.uv)) * i.col.a;
  return float4(i.col.rgb * w, w);
}

struct FullV { float4 pos : SV_Position; float2 uv : UV; };
FullV vsFull(uint vid : SV_VertexID) {
  FullV o;
  float2 p = float2((vid << 1) & 2, vid & 2);
  o.uv = p; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
Texture2D bgTex : register(t2);
float4 psTex(FullV i) : SV_Target { return bgTex.Sample(samp, i.uv); }
float4 psMap(FullV i) : SV_Target {
  float4 d = denTex.Sample(samp, i.uv);
  float v = saturate(d.a / max(1e-6, denMax));
  float t = pow(v, gammaD);
  float3 col;
  float fade;
  if (byCluster > 0.5) { col = d.a > 1e-6 ? d.rgb / d.a : bg.rgb; fade = saturate(t * 1.1); }
  else { col = lutTex.Sample(samp, float2(t, 0.5)).rgb; fade = fadeT <= 0 ? 1.0 : saturate(t / fadeT); }  // fadeT 0 = fill the whole canvas
  return float4(lerp(bg.rgb, col, fade), 1);
}
)HLSL";

struct CBData {
  float VP[16];
  float vpW, vpH, pxPerWorld, is3D;
  float bg[4];
  float curvature, flatShade, borderPx, dimAlpha;
  float accent[4];
  float gammaD, byCluster, fadeT, denMax;
  float lineMul, bundleCount, denScale, sigmaW;
  float hoverCol[4];
};
struct NodeInst { float x, y, z, r; float col[4]; float bcol[4]; float flg; float pad[3]; };
struct LinkInst { float ax, ay, az, bx, by, bz; float ca[4]; float cb[4]; float wid, bund, lfl, pad; };

bool compile(ID3D11Device* dev, const char* entry, const char* target, ID3DBlob** out, string* err) {
  Com<ID3DBlob> e;
  HRESULT hr = D3DCompile(HLSL, strlen(HLSL), "netview.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, e.put());
  if (FAILED(hr)) { if (err) *err = string("Shader ") + entry + ": " + (e ? string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize()) : "compile failed"); return false; }
  (void)dev;
  return true;
}
void setC(float* d, const Color& c) { d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = c.a; }
}  // namespace

bool NetView::init(Gfx& g, string* err) {
  g_ = &g;
  ID3D11Device* d = g.dev.get();
  Com<ID3DBlob> b;
  auto mkVS = [&](const char* e, Com<ID3D11VertexShader>& vs, Com<ID3DBlob>& keep) {
    if (!compile(d, e, "vs_4_0", keep.put(), err)) return false;
    return SUCCEEDED(d->CreateVertexShader(keep->GetBufferPointer(), keep->GetBufferSize(), nullptr, vs.put()));
  };
  auto mkPS = [&](const char* e, Com<ID3D11PixelShader>& ps) {
    Com<ID3DBlob> bb;
    if (!compile(d, e, "ps_4_0", bb.put(), err)) return false;
    return SUCCEEDED(d->CreatePixelShader(bb->GetBufferPointer(), bb->GetBufferSize(), nullptr, ps.put()));
  };
  Com<ID3DBlob> bn, bl, bs, bf;
  if (!mkVS("vsNode", vsNode_, bn) || !mkVS("vsLink", vsLink_, bl) || !mkVS("vsSplat", vsSplat_, bs) || !mkVS("vsFull", vsFull_, bf)) return false;
  if (!mkPS("psNode", psNode_) || !mkPS("psLink", psLink_) || !mkPS("psSplat", psSplat_) || !mkPS("psMap", psMap_) || !mkPS("psTex", psTex_)) return false;
  D3D11_INPUT_ELEMENT_DESC nl[] = {
      {"POS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"RAD", 0, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"COL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"BCOL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"FLG", 0, DXGI_FORMAT_R32_FLOAT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  if (FAILED(d->CreateInputLayout(nl, 5, bn->GetBufferPointer(), bn->GetBufferSize(), ilNode_.put()))) { if (err) *err = "Node input layout failed"; return false; }
  D3D11_INPUT_ELEMENT_DESC ll[] = {
      {"PA", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"PB", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"CA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"CB", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"WID", 0, DXGI_FORMAT_R32_FLOAT, 0, 56, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"BND", 0, DXGI_FORMAT_R32_FLOAT, 0, 60, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"LFL", 0, DXGI_FORMAT_R32_FLOAT, 0, 64, D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  if (FAILED(d->CreateInputLayout(ll, 7, bl->GetBufferPointer(), bl->GetBufferSize(), ilLink_.put()))) { if (err) *err = "Link input layout failed"; return false; }
  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(CBData);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  d->CreateBuffer(&cbd, nullptr, cb_.put());
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  d->CreateBlendState(&bd, blendPremul_.put());
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  d->CreateBlendState(&bd, blendAdd_.put());
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = FALSE;
  rd.MultisampleEnable = TRUE;
  d->CreateRasterizerState(&rd, rs_.put());
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  d->CreateSamplerState(&sd, samp_.put());
  return true;
}

void NetView::setData(const Network* net, const ViewStyle* st, const Bundles* bundles) {
  net_ = net;
  st_ = st;
  bundles_ = bundles;
  if (net && st) enc_.prepare(*net, *st);
  if (net && int(flags.size()) != net->n()) flags.assign(static_cast<size_t>(net->n()), 0);
  if (net && int(pos.size()) != net->n()) {
    pos.resize(static_cast<size_t>(net->n()));
    for (int i = 0; i < net->n(); i++) pos[i] = {float(net->nodes[i].x), float(net->nodes[i].y), float(net->nodes[i].z)};
  }
  nodesDirty = linksDirty = true;
}

float NetView::nodeRadiusPx(int i) const {
  if (!net_) return 0;
  if (enc_.vos() && !is3D()) return float(enc_.vosRadiusPx(i) * uiScale);
  double r = enc_.radius(i);
  if (!is3D()) return float(std::max(1.2, r * cam.zoom));
  float sx, sy, dep;
  if (!worldToScreen(pos[i][0], pos[i][1], pos[i][2], sx, sy, &dep)) return 0;
  return float(std::max(1.2, r * pxPerWorld_ / std::max(1e-6f, dep)));
}

void NetView::buildMatrices() {
  auto& M = viewProj_;
  M.fill(0);
  if (!is3D()) {
    double z = cam.zoom;
    M[0] = float(2 * z / vp.w);
    M[5] = float(-2 * z / vp.h);
    M[12] = float(-cam.x * 2 * z / vp.w);
    M[13] = float(cam.y * 2 * z / vp.h);
    M[14] = 0.5f;
    M[15] = 1;
    pxPerWorld_ = float(z);
    return;
  }
  // 3D orbit around the layout centroid
  double cx = 0, cy = 0, cz = 0, R = 1;
  int n = int(pos.size());
  for (auto& p : pos) { cx += p[0]; cy += p[1]; cz += p[2]; }
  if (n) { cx /= n; cy /= n; cz /= n; }
  for (auto& p : pos) R = std::max(R, std::sqrt(sq(p[0] - cx) + sq(p[1] - cy) + sq(p[2] - cz)));
  double dist = R * 2.7 * cam.dist3;
  double cp = std::cos(cam.pitch), spch = std::sin(cam.pitch);
  double ex = cx + dist * cp * std::sin(cam.yaw), ey = -cy + dist * spch, ez = cz - dist * cp * std::cos(cam.yaw);
  double tx = cx, ty = -cy, tz = cz;
  double zx = tx - ex, zy = ty - ey, zz = tz - ez, zl = std::sqrt(zx * zx + zy * zy + zz * zz);
  zx /= zl; zy /= zl; zz /= zl;
  double ux = 0, uy = 1, uz = 0;
  double xx = uy * zz - uz * zy, xy = uz * zx - ux * zz, xz = ux * zy - uy * zx, xl = std::sqrt(xx * xx + xy * xy + xz * xz);
  xx /= xl; xy /= xl; xz /= xl;
  double yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
  double V[16] = {xx, yx, zx, 0, xy, yy, zy, 0, xz, yz, zz, 0, -(xx * ex + xy * ey + xz * ez), -(yx * ex + yy * ey + yz * ez), -(zx * ex + zy * ey + zz * ez), 1};
  // flip world y (screen-down convention) : row 1 negated
  for (int k = 0; k < 4; k++) V[4 + k] = -V[4 + k];
  double fov = 40 * M_PI / 180, ys = 1 / std::tan(fov / 2), xs = ys / (vp.w / std::max(1.f, vp.h));
  double zn = dist * 0.02, zf = dist * 4;
  double P[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      double s = 0;
      for (int k = 0; k < 4; k++) s += V[r * 4 + k] * P[k * 4 + c];
      M[r * 4 + c] = float(s);
    }
  pxPerWorld_ = float(ys * vp.h / 2);
}

bool NetView::worldToScreen(double x, double y, double z, float& sx, float& sy, float* depth) const {
  const auto& M = viewProj_;
  double cx = x * M[0] + y * M[4] + z * M[8] + M[12];
  double cy = x * M[1] + y * M[5] + z * M[9] + M[13];
  double cw = x * M[3] + y * M[7] + z * M[11] + M[15];
  if (cw <= 1e-6) return false;
  sx = float(vp.x + (cx / cw * 0.5 + 0.5) * vp.w);
  sy = float(vp.y + (0.5 - cy / cw * 0.5) * vp.h);
  if (depth) *depth = float(cw);
  return true;
}

void NetView::screenToWorld(float sx, float sy, double& x, double& y) const {
  x = cam.x + (sx - (vp.x + vp.w / 2)) / cam.zoom;
  y = cam.y + (sy - (vp.y + vp.h / 2)) / cam.zoom;
}

void NetView::fit(double pad) {
  if (!net_ || pos.empty()) return;
  if (is3D()) { cam.dist3 = 1; return; }
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  for (size_t i = 0; i < pos.size(); i++) {
    if (i < flags.size() && (flags[i] & NF_HIDDEN)) continue;
    double r = enc_.vos() ? 0 : enc_.radius(int(i));
    x0 = std::min(x0, pos[i][0] - r); x1 = std::max(x1, pos[i][0] + r);
    y0 = std::min(y0, pos[i][1] - r); y1 = std::max(y1, pos[i][1] + r);
  }
  if (fitX0 < fitX1) { x0 = fitX0; y0 = fitY0; x1 = fitX1; y1 = fitY1; }
  if (x0 > x1) return;
  cam.x = (x0 + x1) / 2;
  cam.y = (y0 + y1) / 2;
  double padX = pad * uiScale + 40 * uiScale;  // room for side labels
  double padY = pad * uiScale;
  if (enc_.vos()) {
    // constant-pixel circles and centred labels: a VOSviewer-like margin plus the largest radius and half a label
    double mr = 0;
    for (int i = 0; i < net_->n(); i++) mr = std::max(mr, enc_.vosRadiusPx(i));
    padY = (40 + mr) * uiScale;
    padX = padY + 30 * uiScale;
  }
  cam.zoom = clampv(std::min((vp.w - 2 * padX) / std::max(1.0, x1 - x0), (vp.h - 2 * padY) / std::max(1.0, y1 - y0)), 0.0005, 60.0);
  fitZoom_ = cam.zoom;
}

int NetView::hitNode(float sx, float sy) const {
  if (!net_) return -1;
  int best = -1;
  float bd = 1e9f, bestDepth = 1e18f;
  for (int i = 0; i < net_->n() && i < int(pos.size()); i++) {
    if (flags[i] & NF_HIDDEN) continue;
    float x, y, dep = 0;
    if (!worldToScreen(pos[i][0], pos[i][1], pos[i][2], x, y, &dep)) continue;
    float r = nodeRadiusPx(i) + 3;
    float d = std::hypot(x - sx, y - sy);
    if (d <= r) {
      if (is3D() ? dep < bestDepth : d / r < bd) { best = i; bd = d / r; bestDepth = dep; }
    }
  }
  return best;
}

vector<int> NetView::nodesInRect(const Rect& r) const {
  vector<int> out;
  if (!net_) return out;
  for (int i = 0; i < net_->n() && i < int(pos.size()); i++) {
    if (flags[i] & NF_HIDDEN) continue;
    float x, y;
    if (worldToScreen(pos[i][0], pos[i][1], pos[i][2], x, y) && r.has(x, y)) out.push_back(i);
  }
  return out;
}

void NetView::uploadNodes() {
  const Network& N = *net_;
  const ViewStyle& S = *st_;
  Theme th = canvasTheme(S);
  vector<int> order;
  for (int i = 0; i < N.n(); i++) if (!(flags[i] & NF_HIDDEN)) order.push_back(i);
  if (is3D()) {
    // back-to-front for correct alpha blending
    vector<float> dep(static_cast<size_t>(N.n()), 0);
    for (int i : order) { float sx, sy, d = 0; worldToScreen(pos[i][0], pos[i][1], pos[i][2], sx, sy, &d); dep[i] = d; }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return dep[a] > dep[b]; });
  } else {
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      int fa = (flags[a] & (NF_SELECTED | NF_HOVER)) ? 1 : 0, fb = (flags[b] & (NF_SELECTED | NF_HOVER)) ? 1 : 0;
      if (fa != fb) return fa < fb;
      return enc_.radius(a) > enc_.radius(b);
    });
  }
  vector<NodeInst> inst(order.size());
  for (size_t k = 0; k < order.size(); k++) {
    int i = order[k];
    NodeInst& v = inst[k];
    v.x = pos[i][0]; v.y = pos[i][1]; v.z = is3D() ? pos[i][2] : 0;
    v.r = enc_.vos() && !is3D() ? -nodeRadiusPx(i) : float(enc_.radius(i));
    Color c = enc_.nodeColor(i, ck());
    c.a = S.nodeOpacity;
    setC(v.col, c);
    Color bc(0, 0, 0, 0);
    if (S.border > 0) {
      if (S.borderCol == BorderCol::Contrast) bc = th.bg.withA(0.85f);
      else if (S.borderCol == BorderCol::Cluster) bc = c.mix(Color(0, 0, 0), 0.35f).withA(1);
      else if (S.borderCol == BorderCol::Custom) bc = S.borderCustom;
    }
    setC(v.bcol, bc);
    v.flg = float(flags[i] & (NF_SELECTED | NF_HOVER | NF_DIM | NF_MATCH));
  }
  nNodeInst_ = int(inst.size());
  if (nNodeInst_ > nodeCap_) {
    nodeCap_ = std::max(nNodeInst_, nodeCap_ * 2 + 64);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(nodeCap_ * sizeof(NodeInst));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_->dev->CreateBuffer(&bd, nullptr, nodeBuf_.put());
  }
  if (nNodeInst_ && nodeBuf_) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_->ctx->Map(nodeBuf_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, inst.data(), inst.size() * sizeof(NodeInst)); g_->ctx->Unmap(nodeBuf_.get(), 0); }
  }
}

void NetView::linkStyle(int li, Color& ca, Color& cb, float& wid) const {
  const Network& N = *net_;
  const ViewStyle& S = *st_;
  const Link& l = N.links[li];
  bool vos = enc_.vos() && !is3D();
  if (S.linkColor == LinkColor::Cluster && ck() != ViewKind::Overlay && kind != ViewKind::Timeline) {
    if (vos) {
      // VOSviewer: gradient between both endpoint colours, each lightened in Lab (L* -> 0.6 L* + 40)
      ca = enc_.linkEnd(li, ck(), false); cb = enc_.linkEnd(li, ck(), true);
    } else {
      // Studio: the source cluster's colour, blending towards the target cluster between clusters
      ca = enc_.nodeColor(l.a, ck());
      Color cbb = enc_.nodeColor(l.b, ck());
      cb = N.nodes[l.a].cluster == N.nodes[l.b].cluster ? ca : ca.mix(cbb, 0.6f);
    }
  } else if (S.linkColor == LinkColor::Gradient || ck() == ViewKind::Overlay || kind == ViewKind::Timeline) {
    ca = enc_.nodeColor(l.a, ck()); cb = enc_.nodeColor(l.b, ck());
  } else { ca = cb = enc_.linkColorOf(li, ck()); }
  if (vos) wid = float(enc_.vosLinkPx(li) * uiScale);
  else {
    double zf = is3D() ? 1 : clampv(std::sqrt(cam.zoom / std::max(1e-9, fitZoom_)), 0.7, 2.5);
    wid = float((0.5 + enc_.linkWidthT(li) * (S.linkMaxW - 0.5)) * S.linkWidth * uiScale * zf);
  }
}

// Links to draw: the capped list (the strongest "Max. lines" above the minimum strength) plus, for
// selected nodes, every one of their links, so clicking a node always shows all of its connections
// whatever the limits are. The extra links are weaker than the capped ones, so they go first
// (weak first, strong on top).
static void linksToDraw(const Network& N, const vector<int>& order, const vector<uint32_t>& flags, vector<int>& out) {
  out = order;
  if (flags.size() != size_t(N.n())) return;
  bool anySel = false;
  for (auto f : flags) if (f & NF_SELECTED) { anySel = true; break; }
  if (!anySel) return;
  vector<char> in(size_t(N.m()), 0);
  for (int li : order) in[size_t(li)] = 1;
  vector<int> extra;
  for (int k = 0; k < N.m(); k++) {
    if (in[size_t(k)]) continue;
    const Link& l = N.links[size_t(k)];
    if ((flags[size_t(l.a)] | flags[size_t(l.b)]) & NF_SELECTED) extra.push_back(k);
  }
  if (extra.empty()) return;
  std::sort(extra.begin(), extra.end(), [&](int a, int b) { return N.links[size_t(a)].w < N.links[size_t(b)].w; });
  out.assign(extra.size() + order.size(), 0);
  std::copy(extra.begin(), extra.end(), out.begin());
  std::copy(order.begin(), order.end(), out.begin() + std::ptrdiff_t(extra.size()));
}

void NetView::uploadLinks() {
  const Network& N = *net_;
  const ViewStyle& S = *st_;
  bool bundled = S.bundle && bundles_ && bundles_->valid() && !is3D() && kind != ViewKind::Timeline && kind != ViewKind::Geo &&
                 bundles_->pts.size() == size_t(N.m()) * size_t((bundles_->P + 2) * 2);
  bundleStride_ = bundled ? bundles_->P + 2 : 0;
  if (bundled) {
    int cnt = int(bundles_->pts.size() / 2);
    if (cnt > bundleCap_) {
      bundleCap_ = cnt;
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = UINT(cnt * 8);
      bd.Usage = D3D11_USAGE_DYNAMIC;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      g_->dev->CreateBuffer(&bd, nullptr, bundleBuf_.put());
      D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
      sv.Format = DXGI_FORMAT_R32G32_FLOAT;
      sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sv.Buffer.FirstElement = 0;
      sv.Buffer.NumElements = UINT(cnt);
      g_->dev->CreateShaderResourceView(bundleBuf_.get(), &sv, bundleSrv_.put());
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (bundleBuf_ && SUCCEEDED(g_->ctx->Map(bundleBuf_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, bundles_->pts.data(), bundles_->pts.size() * 4); g_->ctx->Unmap(bundleBuf_.get(), 0); }
  }
  vector<int> draw;
  linksToDraw(N, enc_.linkOrder, flags, draw);
  vector<LinkInst> inst;
  inst.reserve(draw.size());
  bool anyFocus = false;
  for (auto f : flags) if (f & (NF_SELECTED | NF_HOVER)) { anyFocus = true; break; }
  for (int li : draw) {
    const Link& l = N.links[li];
    if ((flags[l.a] | flags[l.b]) & NF_HIDDEN) continue;
    LinkInst v{};
    v.ax = pos[l.a][0]; v.ay = pos[l.a][1]; v.az = is3D() ? pos[l.a][2] : 0;
    v.bx = pos[l.b][0]; v.by = pos[l.b][1]; v.bz = is3D() ? pos[l.b][2] : 0;
    Color ca, cb;
    float wid;
    linkStyle(li, ca, cb, wid);
    float a = enc_.linkAlpha(li);
    ca.a = cb.a = a;
    setC(v.ca, ca);
    setC(v.cb, cb);
    v.wid = wid;
    v.bund = -1.f;
    uint32_t fl = 0;
    if (anyFocus) {
      bool inc = ((flags[l.a] | flags[l.b]) & (NF_SELECTED | NF_HOVER)) != 0;
      fl = inc ? 2 : 1;
    }
    if (li == hoverLink) fl = 2;
    // a selected node's links stay readable even when they are hairlines at this zoom
    if ((flags[l.a] | flags[l.b]) & NF_SELECTED) v.wid = std::max(v.wid, 0.9f * uiScale);
    v.lfl = float(fl);
    if (bundled) {
      // expand the bundled polyline into straight segments on the CPU (no typed-buffer reads in the
      // vertex shader, so it works on every D3D11 driver); re-anchor it to the live node positions so
      // bundles stay attached while nodes are dragged or animating
      const int K = bundleStride_;
      const float* bp = &bundles_->pts[size_t(li) * size_t(K) * 2];
      float dax = pos[l.a][0] - bp[0], day = pos[l.a][1] - bp[1];
      float dbx = pos[l.b][0] - bp[(K - 1) * 2], dby = pos[l.b][1] - bp[(K - 1) * 2 + 1];
      for (int k = 0; k < K - 1; k++) {
        LinkInst sg = v;
        float t0 = float(k) / float(K - 1), t1 = float(k + 1) / float(K - 1);
        sg.ax = bp[k * 2] + dax + (dbx - dax) * t0; sg.ay = bp[k * 2 + 1] + day + (dby - day) * t0;
        sg.bx = bp[k * 2 + 2] + dax + (dbx - dax) * t1; sg.by = bp[k * 2 + 3] + day + (dby - day) * t1;
        sg.az = sg.bz = 0;
        for (int q = 0; q < 4; q++) { sg.ca[q] = v.ca[q] + (v.cb[q] - v.ca[q]) * t0; sg.cb[q] = v.ca[q] + (v.cb[q] - v.ca[q]) * t1; }
        sg.bund = -2.f;
        inst.push_back(sg);
      }
    } else inst.push_back(v);
  }
  // incident (highlighted) links last so they draw on top
  if (anyFocus) std::stable_partition(inst.begin(), inst.end(), [](const LinkInst& v) { return v.lfl < 1.5f; });
  nLinkInst_ = int(inst.size());
  if (nLinkInst_ > linkCap_) {
    linkCap_ = std::max(nLinkInst_, linkCap_ * 2 + 64);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(linkCap_ * sizeof(LinkInst));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_->dev->CreateBuffer(&bd, nullptr, linkBuf_.put());
  }
  if (nLinkInst_ && linkBuf_) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_->ctx->Map(linkBuf_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, inst.data(), inst.size() * sizeof(LinkInst)); g_->ctx->Unmap(linkBuf_.get(), 0); }
  }
}

void NetView::ensureDensityTarget(int w, int h) {
  if (w == denW_ && h == denH_ && denTex_) return;
  denW_ = w; denH_ = h;
  denTex_.reset(); denRtv_.reset(); denSrv_.reset();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(w); td.Height = UINT(h); td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(g_->dev->CreateTexture2D(&td, nullptr, denTex_.put()))) { td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; g_->dev->CreateTexture2D(&td, nullptr, denTex_.put()); }
  if (denTex_) { g_->dev->CreateRenderTargetView(denTex_.get(), nullptr, denRtv_.put()); g_->dev->CreateShaderResourceView(denTex_.get(), nullptr, denSrv_.put()); }
}

void NetView::updateLut(const string& name) {
  if (name == lutName_ && lutTex_) return;
  lutName_ = name;
  uint8_t px[256 * 4];
  for (int i = 0; i < 256; i++) {
    Color c = simulateCvd(cmapAt(name, i / 255.0), st_ ? st_->cvd : 0);
    px[i * 4] = uint8_t(c.r * 255 + .5f); px[i * 4 + 1] = uint8_t(c.g * 255 + .5f); px[i * 4 + 2] = uint8_t(c.b * 255 + .5f); px[i * 4 + 3] = 255;
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{px, 256 * 4, 0};
  lutTex_.reset(); lutSrv_.reset();
  g_->dev->CreateTexture2D(&td, &sd, lutTex_.put());
  if (lutTex_) g_->dev->CreateShaderResourceView(lutTex_.get(), nullptr, lutSrv_.put());
}

double NetView::avgItemDist() {
  int n = net_ ? net_->n() : 0;
  if (n < 2 || pos.size() != size_t(n)) return 0;
  double sig = n;
  for (int i = 0; i < n; i++) sig += pos[i][0] * 1.7 + pos[i][1] * 0.3 + (flags[i] & NF_HIDDEN ? 7.0 : 0.0);
  if (sig == avgSig_ && avgDist_ > 0) return avgDist_;
  avgSig_ = sig;
  vector<int> vis;
  for (int i = 0; i < n; i++) if (!(flags[i] & NF_HIDDEN)) vis.push_back(i);
  int m = int(vis.size());
  double sum = 0, cnt = 0;
  if (m <= 2000) {
    for (int a = 0; a < m; a++)
      for (int b = a + 1; b < m; b++) { sum += std::hypot(pos[vis[a]][0] - pos[vis[b]][0], pos[vis[a]][1] - pos[vis[b]][1]); cnt++; }
  } else {
    Rng rnd(12345);
    for (int k = 0; k < 400000; k++) {
      int a = vis[size_t(rnd() * m) % size_t(m)], b = vis[size_t(rnd() * m) % size_t(m)];
      if (a == b) continue;
      sum += std::hypot(pos[a][0] - pos[b][0], pos[a][1] - pos[b][1]); cnt++;
    }
  }
  avgDist_ = cnt > 0 ? sum / cnt : 0;
  return avgDist_;
}

float NetView::densityT(float sx, float sy) const {
  if (!net_ || denWt_.size() != size_t(net_->n()) || densityMax <= 0) return 0;
  double x, y;
  screenToWorld(sx, sy, x, y);
  double sig = denSig_, cut2 = sq(3 * sig), s = 0;
  for (int j = 0; j < net_->n(); j++) {
    if (flags[j] & NF_HIDDEN) continue;
    double d2 = sq(x - pos[j][0]) + sq(y - pos[j][1]);
    if (d2 < cut2) s += denWt_[size_t(j)] * std::exp(-0.5 * d2 / (sig * sig));
  }
  double v = clampv(s / densityMax, 0.0, 1.0);
  return float(std::pow(v, 1.0 / std::max(0.2, double(st_->densityAlpha)) * 0.55));
}

void NetView::renderGpu(const Color& bg) {
  if (!net_ || !st_ || vp.w < 4 || vp.h < 4 || pos.size() != size_t(net_->n())) return;
  frame_++;
  auto* c = g_->ctx.get();
  buildMatrices();
  if (is3D()) nodesDirty = true;  // depth order changes with the orbit
  if (nodesDirty) { uploadNodes(); }
  if (linksDirty || nodesDirty) { uploadLinks(); }
  nodesDirty = linksDirty = false;
  const ViewStyle& S = *st_;
  CBData cbd{};
  memcpy(cbd.VP, viewProj_.data(), sizeof cbd.VP);
  cbd.vpW = vp.w; cbd.vpH = vp.h; cbd.pxPerWorld = pxPerWorld_; cbd.is3D = is3D() ? 1.f : 0.f;
  setC(cbd.bg, bg);
  cbd.curvature = S.linkGeom == LinkGeom::Straight ? 0 : (S.linkGeom == LinkGeom::Arc ? 1.f : S.curvature);
  cbd.flatShade = S.flat ? 1.f : 0.f;
  cbd.borderPx = S.border * uiScale;
  cbd.dimAlpha = 0.16f;
  setC(cbd.accent, Color::hex(0x4f8ef7));
  setC(cbd.hoverCol, canvasTheme(S).fg.withA(0.9f));
  cbd.gammaD = float(1.0 / std::max(0.2, double(S.densityAlpha)) * 0.55);
  cbd.byCluster = S.densityByCluster && S.colorBy == ColorBy::Cluster ? 1.f : 0.f;
  cbd.fadeT = S.densityFull ? 0.f : 0.12f;
  cbd.lineMul = 1;
  cbd.bundleCount = float(std::max(2, bundleStride_));
  cbd.denScale = 0.5f;
  // kernel: VOSviewer / van Eck & Waltman (2010) use exp(-(r / (0.125 * dbar))^2) with dbar the average distance
  // between items, i.e. a Gaussian with sigma = 0.125 * dbar / sqrt(2); Studio used a fixed world width
  cbd.sigmaW = float(S.kernelAuto ? 0.125 * avgItemDist() / std::sqrt(2.0) * S.kernel : 38.5 * S.kernel);
  if (!(cbd.sigmaW > 0)) cbd.sigmaW = float(38.5 * S.kernel);
  denSig_ = cbd.sigmaW;
  denWt_.assign(size_t(net_->n()), 0.f);
  for (int i = 0; i < net_->n(); i++)
    denWt_[size_t(i)] = float(S.kernelAuto ? std::max(0.0, enc_.weightT(i)) : 0.25 + 0.75 * std::sqrt(enc_.weightT(i)));
  D3D11_VIEWPORT v{vp.x, vp.y, vp.w, vp.h, 0, 1};
  c->RSSetViewports(1, &v);
  c->RSSetState(rs_.get());
  float bf[4] = {0, 0, 0, 0};
  if (kind == ViewKind::Density) {
    // estimate the field maximum on the CPU (kernel sum at item positions, grid-accelerated)
    double sig = cbd.sigmaW, cut = 3 * sig;
    std::unordered_map<int64_t, vector<int>> grid;
    auto key = [&](double x, double y) { return (int64_t(std::floor(x / cut)) << 32) ^ int64_t(uint32_t(int32_t(std::floor(y / cut)))); };
    for (int i = 0; i < net_->n(); i++) if (!(flags[i] & NF_HIDDEN)) grid[key(pos[i][0], pos[i][1])].push_back(i);
    double mx = 1e-6;
    for (int i = 0; i < net_->n(); i++) {
      if (flags[i] & NF_HIDDEN) continue;
      double s = 0;
      int gx = int(std::floor(pos[i][0] / cut)), gy = int(std::floor(pos[i][1] / cut));
      for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
          auto it = grid.find((int64_t(gx + dx) << 32) ^ int64_t(uint32_t(int32_t(gy + dy))));
          if (it == grid.end()) continue;
          for (int j : it->second) {
            double d2 = sq(pos[i][0] - pos[j][0]) + sq(pos[i][1] - pos[j][1]);
            s += denWt_[size_t(j)] * std::exp(-0.5 * d2 / (sig * sig));
          }
        }
      mx = std::max(mx, s);
    }
    densityMax = mx;
    cbd.denMax = float(mx);
    int dw = std::max(8, int(vp.w * 0.5f)), dh = std::max(8, int(vp.h * 0.5f));
    ensureDensityTarget(dw, dh);
    updateLut(S.densityScheme);
  }
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(c->Map(cb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &cbd, sizeof cbd); c->Unmap(cb_.get(), 0); }
  ID3D11Buffer* cbs[] = {cb_.get()};
  c->VSSetConstantBuffers(0, 1, cbs);
  c->PSSetConstantBuffers(0, 1, cbs);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (kind == ViewKind::Density && denRtv_) {
    ID3D11RenderTargetView* prev = nullptr;
    c->OMGetRenderTargets(1, &prev, nullptr);
    float z[4] = {0, 0, 0, 0};
    c->ClearRenderTargetView(denRtv_.get(), z);
    ID3D11RenderTargetView* drt = denRtv_.get();
    c->OMSetRenderTargets(1, &drt, nullptr);
    D3D11_VIEWPORT dv{0, 0, float(denW_), float(denH_), 0, 1};
    c->RSSetViewports(1, &dv);
    // splat instances: node buffer carries colour; weight goes into alpha
    vector<NodeInst> sp;
    bool byCl = cbd.byCluster > 0.5f;
    for (int i = 0; i < net_->n(); i++) {
      if (flags[i] & NF_HIDDEN) continue;
      NodeInst s{};
      s.x = pos[i][0]; s.y = pos[i][1];
      Color col = byCl ? enc_.clusterColor(net_->nodes[i].cluster) : Color(1, 1, 1);
      col.a = denWt_[size_t(i)];
      setC(s.col, col);
      sp.push_back(s);
    }
    Com<ID3D11Buffer> sb;
    if (!sp.empty()) {
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = UINT(sp.size() * sizeof(NodeInst));
      bd.Usage = D3D11_USAGE_IMMUTABLE;
      bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      D3D11_SUBRESOURCE_DATA sd{sp.data(), 0, 0};
      g_->dev->CreateBuffer(&bd, &sd, sb.put());
      UINT stride = sizeof(NodeInst), off = 0;
      ID3D11Buffer* vb = sb.get();
      c->IASetVertexBuffers(0, 1, &vb, &stride, &off);
      c->IASetInputLayout(ilNode_.get());
      c->VSSetShader(vsSplat_.get(), nullptr, 0);
      c->PSSetShader(psSplat_.get(), nullptr, 0);
      c->OMSetBlendState(blendAdd_.get(), bf, 0xffffffff);
      c->DrawInstanced(6, UINT(sp.size()), 0, 0);
    }
    c->OMSetRenderTargets(1, &prev, nullptr);
    if (prev) prev->Release();
    c->RSSetViewports(1, &v);
    ID3D11ShaderResourceView* srv[2] = {denSrv_.get(), lutSrv_.get()};
    c->PSSetShaderResources(0, 2, srv);
    ID3D11SamplerState* ss = samp_.get();
    c->PSSetSamplers(0, 1, &ss);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(vsFull_.get(), nullptr, 0);
    c->PSSetShader(psMap_.get(), nullptr, 0);
    c->OMSetBlendState(nullptr, bf, 0xffffffff);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {nullptr, nullptr};
    c->PSSetShaderResources(0, 2, nulls);
    return;
  }
  c->OMSetBlendState(blendPremul_.get(), bf, 0xffffffff);
  // background layer (Geo basemap) painted by Direct2D into bgTex_
  if (backgroundPainter && bgSrv_ && bgW_ == int(vp.w) && bgH_ == int(vp.h)) {
    ID3D11ShaderResourceView* srv = bgSrv_.get();
    c->PSSetShaderResources(2, 1, &srv);
    ID3D11SamplerState* ss = samp_.get();
    c->PSSetSamplers(0, 1, &ss);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(vsFull_.get(), nullptr, 0);
    c->PSSetShader(psTex_.get(), nullptr, 0);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    c->PSSetShaderResources(2, 1, &nul);
  }
  // links
  if (S.linksVisible && !hideMarks && nLinkInst_ > 0 && linkBuf_) {
    UINT stride = sizeof(LinkInst), off = 0;
    ID3D11Buffer* vb = linkBuf_.get();
    c->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    c->IASetInputLayout(ilLink_.get());
    c->VSSetShader(vsLink_.get(), nullptr, 0);
    c->PSSetShader(psLink_.get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = nullptr;
    c->VSSetShaderResources(0, 1, &srv);
    int segs = 14;
    c->DrawInstanced(UINT(segs * 6), UINT(nLinkInst_), 0, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    c->VSSetShaderResources(0, 1, &nul);
  }
  // nodes
  if (!hideMarks && nNodeInst_ > 0 && nodeBuf_) {
    UINT stride = sizeof(NodeInst), off = 0;
    ID3D11Buffer* vb = nodeBuf_.get();
    c->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    c->IASetInputLayout(ilNode_.get());
    c->VSSetShader(vsNode_.get(), nullptr, 0);
    c->PSSetShader(psNode_.get(), nullptr, 0);
    c->DrawInstanced(6, UINT(nNodeInst_), 0, 0);
  }
}

IDWriteTextLayout* NetView::layout(const string& s, float size, int weight) {
  string key = s + "\x1f" + std::to_string(int(size * 4)) + "\x1f" + std::to_string(weight);
  auto it = layouts_.find(key);
  if (it != layouts_.end()) { it->second.second = frame_; return it->second.first.get(); }
  if (layouts_.size() > 6000) {
    for (auto i = layouts_.begin(); i != layouts_.end();) { if (frame_ - i->second.second > 60) i = layouts_.erase(i); else ++i; }
  }
  std::wstring w = widen(s);
  Com<IDWriteTextLayout> L;
  g_->dw->CreateTextLayout(w.c_str(), UINT32(w.size()), g_->format(std::round(size * 4) / 4, weight), 4000, 200, L.put());
  layouts_[key] = {L, frame_};
  return L.get();
}

void NetView::drawOverlay(ID2D1DeviceContext* dc, const Theme& th, bool showLabels, int maxLabels) {
  labelsShown = 0;
  capLabels.clear();
  capHulls.clear();
  if (!net_ || !st_ || pos.size() != size_t(net_->n())) return;
  const ViewStyle& S = *st_;
  const Network& N = *net_;
  dc->PushAxisAlignedClip(D2D1::RectF(vp.x, vp.y, std::max(vp.x, vp.r()), std::max(vp.y, vp.b())), D2D1_ANTIALIAS_MODE_ALIASED);
  auto* b = g_->brush.get();
  double zRel = std::max(1e-9, cam.zoom / std::max(1e-9, fitZoom_));
  // hulls
  if (S.hulls && (kind == ViewKind::Network || kind == ViewKind::Density)) {
    for (int c = 0; c < N.nClusters; c++) {
      vector<P2> pts;
      for (int i = 0; i < N.n(); i++) {
        if (N.nodes[i].cluster != c || (flags[i] & NF_HIDDEN)) continue;
        float sx, sy;
        if (!worldToScreen(pos[i][0], pos[i][1], pos[i][2], sx, sy)) continue;
        double r = nodeRadiusPx(i) + 12 * uiScale;
        for (int a = 0; a < 8; a++) pts.push_back({sx + r * std::cos(a * M_PI / 4), sy + r * std::sin(a * M_PI / 4)});
      }
      if (pts.size() < 24) continue;
      auto hull = convexHull(pts);
      if (hull.size() < 3) continue;
      Com<ID2D1PathGeometry> geo;
      g_->d2f->CreatePathGeometry(geo.put());
      Com<ID2D1GeometrySink> sk;
      geo->Open(sk.put());
      size_t n = hull.size();
      auto mid = [&](size_t a, size_t bb) { return D2D1::Point2F(float((hull[a].x + hull[bb].x) / 2), float((hull[a].y + hull[bb].y) / 2)); };
      sk->BeginFigure(mid(n - 1, 0), D2D1_FIGURE_BEGIN_FILLED);
      for (size_t i = 0; i < n; i++) sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(float(hull[i].x), float(hull[i].y)), mid(i, (i + 1) % n)));
      sk->EndFigure(D2D1_FIGURE_END_CLOSED);
      sk->Close();
      Color col = enc_.clusterColor(c);
      b->SetColor(d2c(col, 0.07f));
      dc->FillGeometry(geo.get(), b);
      b->SetColor(d2c(col, 0.4f));
      dc->DrawGeometry(geo.get(), b, 1.0f * uiScale);
      CapHull ch;
      for (auto& hp : hull) ch.pts.push_back({float(hp.x - vp.x), float(hp.y - vp.y)});
      ch.fill = col.withA(0.07f); ch.stroke = col.withA(0.4f); ch.sw = 1.0f * uiScale;
      capHulls.push_back(std::move(ch));
    }
  }
  // labels
  vector<Rect> placed;
  auto hitsAny = [&](const Rect& R) {
    for (auto& o : placed) if (R.x < o.r() && R.r() > o.x && R.y < o.b() && R.b() > o.y) return true;
    return false;
  };
  // cluster names first (fade out when zoomed in)
  float namesA = S.clusterNames ? float(clampv(1.0 - (zRel - 1.1) / 1.4, 0.0, 1.0)) : 0.f;
  if (namesA > 0.02f && ck() != ViewKind::Overlay && kind != ViewKind::Timeline && kind != ViewKind::Geo && N.nClusters > 0) {
    vector<double> cx(size_t(N.nClusters), 0), cy(size_t(N.nClusters), 0), cz(size_t(N.nClusters), 0), cw(size_t(N.nClusters), 0);
    vector<int> cn(size_t(N.nClusters), 0);
    for (int i = 0; i < N.n(); i++) {
      int c = N.nodes[i].cluster;
      if (c < 0 || c >= N.nClusters || (flags[i] & NF_HIDDEN)) continue;
      double w = 1 + std::sqrt(N.weight(i));
      cx[c] += pos[i][0] * w; cy[c] += pos[i][1] * w; cz[c] += pos[i][2] * w; cw[c] += w; cn[c]++;
    }
    int maxN = std::max(1, *std::max_element(cn.begin(), cn.end()));
    for (int c = 0; c < std::min(16, N.nClusters); c++) {
      if (cn[c] < 3) continue;
      float sx, sy;
      if (!worldToScreen(cx[c] / cw[c], cy[c] / cw[c], cz[c] / cw[c], sx, sy)) continue;
      float size = float((15 + 7 * std::sqrt(double(cn[c]) / maxN)) * uiScale);
      string nm = truncate(N.clusterName(c), 36);
      IDWriteTextLayout* L = layout(nm, size, 700);
      if (!L) continue;
      DWRITE_TEXT_METRICS tm;
      L->GetMetrics(&tm);
      float x = sx - tm.width / 2, y = sy - tm.height / 2;
      {  // keep the whole name inside the canvas (clusters near the edge would otherwise lose their first or last letters)
        float m = 8 * uiScale;
        if (tm.width + 2 * m < vp.w) x = std::min(std::max(x, vp.x + m), vp.x + vp.w - m - tm.width);
        if (tm.height + 2 * m < vp.h) y = std::min(std::max(y, vp.y + m), vp.y + vp.h - m - tm.height);
      }
      Color col = enc_.clusterColor(c);
      col = th.light ? col.mix(Color(0, 0, 0), 0.35f) : col.mix(Color(1, 1, 1), 0.4f);
      b->SetColor(d2c(th.halo, 0.75f * namesA));
      for (int k = 0; k < 24; k++) { float a = k * 6.2831853f / 24; dc->DrawTextLayout(D2D1::Point2F(x + 2.2f * uiScale * std::cos(a), y + 2.2f * uiScale * std::sin(a)), L, b); }
      b->SetColor(d2c(col, namesA));
      dc->DrawTextLayout(D2D1::Point2F(x, y), L, b);
      {
        DWRITE_LINE_METRICS lm{}; UINT32 nl = 0;
        L->GetLineMetrics(&lm, 1, &nl);
        CapLabel cl; cl.text = nm; cl.x = x - vp.x; cl.y = y + lm.baseline - vp.y; cl.size = size; cl.bold = true; cl.col = col.withA(namesA);
        cl.halo = true; cl.haloC = th.halo.withA(0.75f * namesA); cl.haloR = 2.2f * uiScale;
        capLabels.push_back(std::move(cl));
      }
      if (namesA > 0.5f) placed.push_back({x, y, tm.width, tm.height});
    }
  }
  if (showLabels && S.maxLabels >= 0) {
    vector<int> order;
    for (int i = 0; i < N.n(); i++) if (!(flags[i] & NF_HIDDEN)) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int bb) {
      int fa = (flags[a] & (NF_SELECTED | NF_HOVER | NF_MATCH)) ? 1 : 0, fb = (flags[bb] & (NF_SELECTED | NF_HOVER | NF_MATCH)) ? 1 : 0;
      if (fa != fb) return fa > fb;
      return N.weight(a) > N.weight(bb);
    });
    bool anyFocus = false;
    for (auto f : flags) if (f & (NF_SELECTED | NF_HOVER)) { anyFocus = true; break; }
    int limit = maxLabels > 0 ? maxLabels : 100000;
    double zfont = clampv(std::pow(zRel, 0.5), 0.85, 2.4);
    int weight = S.labelWeight >= 600 ? 600 : (S.labelWeight >= 500 ? 500 : 400);
    for (int i : order) {
      if (labelsShown >= limit && !(flags[i] & (NF_SELECTED | NF_HOVER))) break;
      float sx, sy, dep = 1;
      if (!worldToScreen(pos[i][0], pos[i][1], pos[i][2], sx, sy, &dep)) continue;
      if (!vp.has(sx, sy)) continue;
      float r = kind == ViewKind::Density ? 0 : nodeRadiusPx(i);
      bool vosL = enc_.vos() && !is3D();
      float size = float(11.5 * uiScale * enc_.labelScale(i) * S.labelSize * zfont);
      if (vosL) size = float(enc_.vosLabelPx(i) * uiScale);  // VOSviewer: 9 + 4 nw^0.5 px, constant on zoom
      if (is3D()) size = float(clampv(size * 1.0, 8.0 * uiScale, 30.0 * uiScale));
      size = vosL ? clampv(size, 6.f * uiScale, 60.f * uiScale) : clampv(size, 8.5f * uiScale, 40.f * uiScale);
      string text = enc_.label(i);
      IDWriteTextLayout* L = layout(text, size, (flags[i] & (NF_SELECTED | NF_HOVER)) ? 600 : weight);
      if (!L) continue;
      DWRITE_TEXT_METRICS tm;
      L->GetMetrics(&tm);
      float tw = tm.width, thh = tm.height;
      struct Cd { float x, y; };
      vector<Cd> cands;
      float gap = r + size * 0.3f;
      bool centre = S.labelPlace == LabelPlace::Centre || kind == ViewKind::Density;
      if (centre) cands.push_back({sx - tw / 2, sy - thh / 2});
      if (!(centre && vosL)) {  // VOSviewer: a label sits on its circle or is hidden until zoomed in
        cands.push_back({sx + gap, sy - thh / 2});
        cands.push_back({sx - gap - tw, sy - thh / 2});
        cands.push_back({sx - tw / 2, sy - gap - thh});
        cands.push_back({sx - tw / 2, sy + gap});
      }
      bool forced = (flags[i] & (NF_SELECTED | NF_HOVER)) != 0;
      const Cd* pick = nullptr;
      for (auto& cd : cands) {
        Rect R{cd.x - 3 * uiScale, cd.y + thh * 0.02f, tw + 6 * uiScale, thh * 0.96f};
        if (!forced && (R.x < vp.x || R.y < vp.y || R.r() > vp.r() || R.b() > vp.b())) continue;
        if (forced || !hitsAny(R)) { pick = &cd; placed.push_back(R); break; }
      }
      if (!pick) continue;
      labelsShown++;
      Color col = th.fg;
      if (S.labelByCluster && ck() != ViewKind::Overlay) { Color cc = enc_.clusterColor(N.nodes[i].cluster); col = th.light ? cc.mix(Color(0, 0, 0), 0.3f) : cc.mix(Color(1, 1, 1), 0.2f); }
      if (kind == ViewKind::Density) {
        bool darkText = th.light;
        if (S.densityFull && !(S.densityByCluster && S.colorBy == ColorBy::Cluster)) darkText = cmapAt(S.densityScheme, densityT(sx, sy)).luma() > 0.45f;
        col = darkText ? Color(0.05f, 0.05f, 0.07f) : Color(0.97f, 0.98f, 1.f);
      } else if (vosL && !S.labelByCluster) col = th.light ? Color(0, 0, 0, 0.8f) : Color(1, 1, 1, 0.8f);
      float alpha = (anyFocus && !(flags[i] & (NF_SELECTED | NF_HOVER | NF_NEIGHBOUR))) ? 0.34f : 1.f;
      if (flags[i] & NF_DIM) alpha = std::min(alpha, 0.22f);
      if (S.labelHalo) {
        Color hc = kind == ViewKind::Density ? (th.light ? Color(1, 1, 1) : Color(0, 0, 0)) : th.halo;
        b->SetColor(d2c(hc, 0.72f * alpha));
        float hr = std::max(1.f, size * 0.09f);
        for (int k = 0; k < 24; k++) { float a = k * 6.2831853f / 24; dc->DrawTextLayout(D2D1::Point2F(pick->x + hr * std::cos(a), pick->y + hr * std::sin(a)), L, b); }
      }
      b->SetColor(d2c(col, alpha));
      dc->DrawTextLayout(D2D1::Point2F(pick->x, pick->y), L, b);
      {
        DWRITE_LINE_METRICS lm{}; UINT32 nl = 0;
        L->GetLineMetrics(&lm, 1, &nl);
        CapLabel cl; cl.text = text; cl.x = pick->x - vp.x; cl.y = pick->y + lm.baseline - vp.y; cl.size = size;
        cl.bold = (flags[i] & (NF_SELECTED | NF_HOVER)) != 0 || weight >= 600; cl.col = col.withA(col.a * alpha);
        if (S.labelHalo) { cl.halo = true; cl.haloC = (kind == ViewKind::Density ? (th.light ? Color(1, 1, 1) : Color(0, 0, 0)) : th.halo).withA(0.72f * alpha); cl.haloR = std::max(1.f, size * 0.09f); }
        capLabels.push_back(std::move(cl));
      }
    }
  }
  dc->PopAxisAlignedClip();
}

// Paint the optional background layer with Direct2D into a texture the GPU composites under links and nodes.
// Must be called outside Gfx::beginD2D/endD2D and before Gfx::beginGpu.
void NetView::drawBackgroundLayer() {
  if (!backgroundPainter || vp.w < 4 || vp.h < 4) return;
  int w = int(vp.w), h = int(vp.h);
  if (w != bgW_ || h != bgH_ || !bgBmp_) {
    bgBmp_.reset(); bgSrv_.reset(); bgTex_.reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(w); td.Height = UINT(h); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_->dev->CreateTexture2D(&td, nullptr, bgTex_.put()))) return;
    g_->dev->CreateShaderResourceView(bgTex_.get(), nullptr, bgSrv_.put());
    Com<IDXGISurface> surf;
    if (FAILED(bgTex_->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(surf.put())))) return;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(g_->dc->CreateBitmapFromDxgiSurface(surf.get(), &bp, bgBmp_.put()))) return;
    bgW_ = w; bgH_ = h;
  }
  auto* dc = g_->dc.get();
  Com<ID2D1Image> old;
  dc->GetTarget(old.put());
  dc->SetTarget(bgBmp_.get());
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0, 0, 0, 0));
  dc->SetTransform(D2D1::Matrix3x2F::Translation(-vp.x, -vp.y));
  backgroundPainter(dc);
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->EndDraw();
  dc->SetTarget(old.get());
}

// ------------------------------------------------------------------ WYSIWYG capture
void NetView::captureInto(Scene& sc, const Theme& th, double k, bool labels) const {
  if (!net_ || !st_ || pos.size() != size_t(net_->n())) return;
  const Network& N = *net_;
  const ViewStyle& S = *st_;
  auto X = [&](float sx) { return float((sx - vp.x) * k); };
  auto Y = [&](float sy) { return float((sy - vp.y) * k); };
  // hulls
  for (auto& h : capHulls) {
    size_t n = h.pts.size();
    if (n < 3) continue;
    Prim p; p.type = Prim::Path; p.group = "hulls";
    auto mid = [&](size_t a, size_t b) { return std::make_pair((h.pts[a].first + h.pts[b].first) / 2 * float(k), (h.pts[a].second + h.pts[b].second) / 2 * float(k)); };
    auto m0 = mid(n - 1, 0);
    p.d.push_back({'M', m0.first, m0.second});
    for (size_t i = 0; i < n; i++) { auto m = mid(i, (i + 1) % n); p.d.push_back({'Q', h.pts[i].first * float(k), h.pts[i].second * float(k), m.first, m.second}); }
    p.d.push_back({'Z'});
    p.fill = true; p.fillC = h.fill; p.stroke = true; p.strokeC = h.stroke; p.sw = float(h.sw * k);
    sc.items.push_back(std::move(p));
  }
  bool anyFocus = false;
  for (auto f : flags) if (f & (NF_SELECTED | NF_HOVER)) { anyFocus = true; break; }
  // links
  if (S.linksVisible && !hideMarks && kind != ViewKind::Density) {
    bool bundled = S.bundle && bundles_ && bundles_->valid() && !is3D() && kind != ViewKind::Timeline && kind != ViewKind::Geo &&
                   bundles_->pts.size() == size_t(N.m()) * size_t((bundles_->P + 2) * 2);
    float curv = S.linkGeom == LinkGeom::Straight ? 0 : (S.linkGeom == LinkGeom::Arc ? 1.f : S.curvature);
    vector<Prim> focusLinks;
    vector<int> draw;
    linksToDraw(N, enc_.linkOrder, flags, draw);
    for (int li : draw) {
      const Link& l = N.links[li];
      if ((flags[l.a] | flags[l.b]) & NF_HIDDEN) continue;
      Color ca, cb;
      float wid;
      linkStyle(li, ca, cb, wid);
      if ((flags[l.a] | flags[l.b]) & NF_SELECTED) wid = std::max(wid, 0.9f * uiScale);
      float a = enc_.linkAlpha(li) * std::min(1.f, wid);
      bool inc = anyFocus && ((flags[l.a] | flags[l.b]) & (NF_SELECTED | NF_HOVER)) != 0;
      if (anyFocus && !inc) a *= 0.16f * 0.6f;
      if (inc || li == hoverLink) a = std::min(1.f, a * 2.2f);
      // polyline in screen space
      vector<std::pair<float, float>> pl;
      float ax, ay, bx, by;
      if (!worldToScreen(pos[l.a][0], pos[l.a][1], is3D() ? pos[l.a][2] : 0, ax, ay) || !worldToScreen(pos[l.b][0], pos[l.b][1], is3D() ? pos[l.b][2] : 0, bx, by)) continue;
      if (bundled) {
        const int K = bundles_->P + 2;
        const float* bp = &bundles_->pts[size_t(li) * size_t(K) * 2];
        float dax = pos[l.a][0] - bp[0], day = pos[l.a][1] - bp[1], dbx = pos[l.b][0] - bp[(K - 1) * 2], dby = pos[l.b][1] - bp[(K - 1) * 2 + 1];
        for (int q = 0; q < K; q++) {
          float t = float(q) / float(K - 1), sx, sy;
          worldToScreen(bp[q * 2] + dax + (dbx - dax) * t, bp[q * 2 + 1] + day + (dby - day) * t, 0, sx, sy);
          pl.push_back({sx, sy});
        }
      } else if (curv > 0.001f && is3D()) {
        // same world-space curve as the 3D view (see curvePt in the shader), projected point by point
        float A3[3] = {float(pos[l.a][0]), float(pos[l.a][1]), float(pos[l.a][2])}, B3[3] = {float(pos[l.b][0]), float(pos[l.b][1]), float(pos[l.b][2])};
        float d3[3] = {B3[0] - A3[0], B3[1] - A3[1], B3[2] - A3[2]};
        float len3 = std::max(1e-6f, std::sqrt(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]));
        float k3 = curv * len3 * 0.22f * 2 / len3;
        float M3[3] = {(A3[0] + B3[0]) / 2 + d3[1] * k3, (A3[1] + B3[1]) / 2 - d3[0] * k3, (A3[2] + B3[2]) / 2};
        bool ok = true;
        for (int q = 0; q <= 16 && ok; q++) {
          float t = q / 16.f, u = 1 - t, sx, sy;
          ok = worldToScreen(u * u * A3[0] + 2 * u * t * M3[0] + t * t * B3[0], u * u * A3[1] + 2 * u * t * M3[1] + t * t * B3[1], u * u * A3[2] + 2 * u * t * M3[2] + t * t * B3[2], sx, sy);
          pl.push_back({sx, sy});
        }
        if (!ok) continue;
      } else if (curv > 0.001f && !is3D()) {
        float dx = bx - ax, dy = by - ay, len = std::max(1e-6f, std::sqrt(dx * dx + dy * dy));
        float off = curv * len * 0.22f;
        float mx = (ax + bx) / 2 + dy / len * off * 2, my = (ay + by) / 2 - dx / len * off * 2;
        for (int q = 0; q <= 16; q++) { float t = q / 16.f, u = 1 - t; pl.push_back({u * u * ax + 2 * u * t * mx + t * t * bx, u * u * ay + 2 * u * t * my + t * t * by}); }
      } else { pl.push_back({ax, ay}); pl.push_back({bx, by}); }
      bool grad = std::fabs(ca.r - cb.r) + std::fabs(ca.g - cb.g) + std::fabs(ca.b - cb.b) > 0.02f;
      int pieces = grad ? std::min<int>(6, int(pl.size()) - 1) : 1;
      if (pieces < 1) pieces = 1;
      size_t per = (pl.size() - 1 + size_t(pieces) - 1) / size_t(pieces);
      for (size_t s0 = 0; s0 + 1 < pl.size(); s0 += per) {
        size_t s1 = std::min(pl.size() - 1, s0 + per);
        float t = pieces > 1 ? float(s0 + s1) / 2 / float(pl.size() - 1) : 0.f;
        // multi-piece gradient links use butt caps: overlapping round caps would show as darker beads at each joint
        Prim p; p.type = Prim::Path; p.group = "links"; p.stroke = true; p.roundCap = pieces == 1;
        p.strokeC = ca.mix(cb, t).withA(a); p.sw = float(std::max(1.f, wid) * k);
        p.d.push_back({'M', X(pl[s0].first), Y(pl[s0].second)});
        for (size_t q = s0 + 1; q <= s1; q++) p.d.push_back({'L', X(pl[q].first), Y(pl[q].second)});
        (inc ? focusLinks : sc.items).push_back(std::move(p));
      }
    }
    for (auto& p : focusLinks) sc.items.push_back(std::move(p));
  }
  // nodes (same order as the GPU)
  if (kind != ViewKind::Density && !hideMarks) {
    vector<int> order;
    for (int i = 0; i < N.n(); i++) if (!(flags[i] & NF_HIDDEN)) order.push_back(i);
    vector<float> dep(size_t(N.n()), 0);
    for (int i : order) { float sx, sy, d = 1; worldToScreen(pos[i][0], pos[i][1], is3D() ? pos[i][2] : 0, sx, sy, &d); dep[size_t(i)] = d; }
    if (is3D()) std::sort(order.begin(), order.end(), [&](int a, int b) { return dep[size_t(a)] > dep[size_t(b)]; });
    else std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      int fa = (flags[a] & (NF_SELECTED | NF_HOVER)) ? 1 : 0, fb = (flags[b] & (NF_SELECTED | NF_HOVER)) ? 1 : 0;
      if (fa != fb) return fa < fb;
      return enc_.radius(a) > enc_.radius(b);
    });
    for (int i : order) {
      float sx, sy;
      if (!worldToScreen(pos[i][0], pos[i][1], is3D() ? pos[i][2] : 0, sx, sy)) continue;
      float r = nodeRadiusPx(i);
      if (sx + r < vp.x || sy + r < vp.y || sx - r > vp.r() || sy - r > vp.b()) continue;
      Color c = enc_.nodeColor(i, ck());
      c.a = S.nodeOpacity;
      if (flags[i] & NF_DIM) c.a *= 0.16f;
      Prim p; p.type = Prim::Circle; p.group = "nodes"; p.x = X(sx); p.y = Y(sy); p.r = float(r * k);
      p.fill = true; p.fillC = c; p.sphere = !S.flat;
      sc.items.push_back(p);
      float bpx = S.border * uiScale;
      if (bpx > 0.01f && S.borderCol != BorderCol::None) {
        Color bc = S.borderCol == BorderCol::Contrast ? th.bg.withA(0.85f) : (S.borderCol == BorderCol::Cluster ? c.mix(Color(0, 0, 0), 0.35f).withA(1) : S.borderCustom);
        if (flags[i] & NF_DIM) bc.a *= 0.16f;
        Prim bpr; bpr.type = Prim::Circle; bpr.group = "nodes"; bpr.x = p.x; bpr.y = p.y; bpr.r = float(std::max(0.5f, r - bpx / 2) * k);
        bpr.stroke = true; bpr.strokeC = bc; bpr.sw = float(std::min(bpx, r) * k);
        sc.items.push_back(bpr);
      }
      auto ring = [&](float rr, Color col, float w) { Prim q; q.type = Prim::Circle; q.group = "nodes"; q.x = p.x; q.y = p.y; q.r = float(rr * k); q.stroke = true; q.strokeC = col; q.sw = float(w * k); sc.items.push_back(q); };
      if (flags[i] & NF_SELECTED) ring(r + 2.6f, Color::hex(0x4f8ef7), 2.2f);
      else if (flags[i] & NF_HOVER) ring(r + 2.6f, th.fg.withA(0.9f), 2.2f);
      if (flags[i] & NF_MATCH) ring(r + 4.2f, Color(1, 0.8f, 0.2f, 0.9f), 2.f);
    }
  }
  // labels and cluster names
  if (labels) {
    for (auto& l : capLabels) {
      Prim t; t.type = Prim::Text; t.group = "labels"; t.x = float(l.x * k); t.y = float(l.y * k); t.size = float(l.size * k * 0.96);
      t.text = l.text; t.bold = l.bold; t.fill = true; t.fillC = l.col;
      if (l.halo) { t.halo = true; t.haloC = l.haloC; t.haloW = float(l.haloR * k); }
      sc.items.push_back(std::move(t));
    }
  }
  sc.labels = int(capLabels.size());
}

}  // namespace win
}  // namespace vs
