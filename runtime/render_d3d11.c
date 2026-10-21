/* Direct3D 11 presenter: scales the 768x240 CD-i frame to any window or
   monitor resolution (up to 4K and beyond) with selectable filters and
   aspect-ratio modes, including blurred side panels for 16:9 displays. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include "render.h"
#include "runtime.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex;
static ID3D11ShaderResourceView *srv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11PixelShader *ps_xbr, *ps_dedither, *ps_refine;

/* HD filter resources: packed source, de-dithered source, upscaled picture */
typedef struct { ID3D11Texture2D *t; ID3D11ShaderResourceView *srv; ID3D11RenderTargetView *rtv; int w, h; } RT;
static RT hd_src, hd_dd, hd_out;
static ID3D11Buffer *cbuf;
static ID3D11SamplerState *samp_lin, *samp_point;
static int tex_w, tex_h, tex_mips;
static UINT buf_w, buf_h;
static HWND rwnd;

typedef struct {
    float rect[4];   /* destination rect in NDC: left, top, right, bottom */
    float src[4];    /* texture w, h, destination w, h (pixels) */
    float prm[4];    /* filter, mip level for backdrop, brightness, mode (0 image, 1 backdrop) */
    float uvx[4];    /* uv offset xy, uv scale zw */
} CB;

static const char *shader_src =
"Texture2D tex : register(t0);\n"
"SamplerState sl : register(s0);\n"
"SamplerState sp : register(s1);\n"
"cbuffer C : register(b0) { float4 rect; float4 src; float4 prm; float4 uvx; };\n"
"struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vsmain(uint id : SV_VertexID) {\n"
"  float2 c = float2(id & 1, id >> 1);\n"
"  VO o;\n"
"  o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0, 1);\n"
"  o.uv = uvx.xy + c * uvx.zw;\n"
"  return o;\n"
"}\n"
/* sharp bilinear: crisp pixels without shimmer at non-integer scales */
"float3 sharp(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 scale = src.zw / ts;\n"
"  float2 texel = uv * ts;\n"
"  float2 pre = max(floor(scale), 1.0);\n"
"  float2 fl = floor(texel);\n"
"  float2 cd = frac(texel) - 0.5;\n"
"  float2 rr = 0.5 - 0.5 / pre;\n"
"  float2 f = (cd - clamp(cd, -rr, rr)) * pre + 0.5;\n"
"  return tex.SampleLevel(sl, (fl + f) / ts, 0).rgb;\n"
"}\n"
/* Catmull-Rom bicubic with 9 bilinear taps */
"float3 bicubic(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 sp2 = uv * ts;\n"
"  float2 t1 = floor(sp2 - 0.5) + 0.5;\n"
"  float2 f = sp2 - t1;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
"  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
"  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2;\n"
"  float2 o12 = w2 / w12;\n"
"  float2 t0 = (t1 - 1) / ts, t3 = (t1 + 2) / ts, t12 = (t1 + o12) / ts;\n"
"  float3 r = 0;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;\n"
"  return saturate(r);\n"
"}\n"
/* simple CRT: gaussian scanlines + aperture grille */
"float3 crt(float2 uv, float2 pos) {\n"
"  float ty = uv.y * src.y;\n"
"  float yc = floor(ty) + 0.5;\n"
"  float3 a = sharp(float2(uv.x, yc / src.y));\n"
"  float d = ty - yc;\n"
"  float lum = dot(a, float3(0.3, 0.6, 0.1));\n"
"  float width = lerp(6.0, 2.5, lum);\n"
"  float beam = exp(-d * d * width * 4.0);\n"
/* scanlines need several output pixels per source line to look right */
"  float strength = saturate((src.w / src.y - 2.5) / 4.0);\n"
"  float3 c = a * lerp(1.0, beam * 1.35, strength);\n"
"  int m = (int)pos.x % 3;\n"
"  float3 mask = m == 0 ? float3(1.0, 0.85, 0.85) : (m == 1 ? float3(0.85, 1.0, 0.85) : float3(0.85, 0.85, 1.0));\n"
"  mask = lerp(1.0, mask, strength);\n"
"  return saturate(c * mask * lerp(1.0, 1.1, strength));\n"
"}\n"
/* ---- HD filters (linkfoe_hd) ---- */
"float3 L(int2 p) {\n"
"  int2 sz = int2(src.xy);\n"
"  return tex.Load(int3(clamp(p, int2(0, 0), sz - 1), 0)).rgb;\n"
"}\n"
"float3 yuv(float3 c) {\n"
"  return float3(dot(c, float3(0.299, 0.587, 0.114)), dot(c, float3(-0.169, -0.331, 0.5)), dot(c, float3(0.5, -0.419, -0.081)));\n"
"}\n"
/* perceptual colour distance (luma weighted, xBR style), roughly 0..1 */
"float cdist(float3 a, float3 b) { return dot(abs(yuv(a) - yuv(b)), float3(48.0, 7.0, 6.0)) / 61.0; }\n"
"bool ceq(float3 a, float3 b) { return cdist(a, b) < 0.04; }\n"
/* edge-directed upscaler (xBR level 1 rules): each of the four corners of a
   source pixel is re-coloured along a diagonal contour when the pixel pattern
   says an edge passes there; the contour is anti-aliased at output resolution */
"float3 xbr(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 pos = uv * ts;\n"
"  float2 ipf = floor(pos);\n"
"  int2 ip = int2(ipf);\n"
"  float2 fp = pos - ipf;\n"
"  float3 E = L(ip);\n"
"  float3 c = sharp(uv);\n"
"  float scale = min(src.z / ts.x, src.w / ts.y);\n"
"  float w = clamp(0.8 / scale, 0.02, 0.5);\n"
"  [unroll] for (int k = 0; k < 4; k++) {\n"
"    int sx = (k & 1) ? -1 : 1;\n"
"    int sy = (k & 2) ? -1 : 1;\n"
"    float2 lp = float2(sx > 0 ? fp.x : 1.0 - fp.x, sy > 0 ? fp.y : 1.0 - fp.y);\n"
"    if (lp.x + lp.y > 1.5 - w) {\n"
"      float3 F = L(ip + int2(sx, 0)), H = L(ip + int2(0, sy)), I = L(ip + int2(sx, sy));\n"
"      float3 C = L(ip + int2(sx, -sy)), G = L(ip + int2(-sx, sy)), D = L(ip + int2(-sx, 0)), B = L(ip + int2(0, -sy));\n"
"      float3 F4 = L(ip + int2(2 * sx, 0)), I4 = L(ip + int2(2 * sx, sy)), H5 = L(ip + int2(0, 2 * sy)), I5 = L(ip + int2(sx, 2 * sy));\n"
"      float wd1 = cdist(E, C) + cdist(E, G) + cdist(I, F4) + cdist(I, H5) + 4.0 * cdist(H, F);\n"
"      float wd2 = cdist(H, D) + cdist(H, I5) + cdist(F, I4) + cdist(F, B) + 4.0 * cdist(E, I);\n"
"      if (wd1 < wd2 && !ceq(E, F) && !ceq(E, H)) {\n"
"        float3 nc = cdist(E, F) <= cdist(E, H) ? F : H;\n"
"        c = lerp(c, nc, smoothstep(1.5 - w, 1.5 + w, lp.x + lp.y));\n"
"      }\n"
"    }\n"
"  }\n"
"  return c;\n"
"}\n"
"float4 psxbr(VO i) : SV_Target { return float4(xbr(i.uv), 1); }\n"
/* de-dither at source resolution: a perfect 2-colour checkerboard of nearby
   colours (dithered skies and gradients) becomes their average */
"float4 psdedither(VO i) : SV_Target {\n"
"  int2 p = int2(i.pos.xy);\n"
"  float3 E = L(p), A = L(p + int2(-1, -1)), B = L(p + int2(0, -1)), C = L(p + int2(1, -1)), D = L(p + int2(-1, 0));\n"
"  float3 F = L(p + int2(1, 0)), G = L(p + int2(-1, 1)), H = L(p + int2(0, 1)), I = L(p + int2(1, 1));\n"
"  bool chk = ceq(E, A) && ceq(E, C) && ceq(E, G) && ceq(E, I) && ceq(B, D) && ceq(B, F) && ceq(B, H)\n"
"             && !ceq(E, B) && cdist(E, B) < 0.35;\n"
"  return float4(chk ? (E + B) * 0.5 : E, 1);\n"
"}\n"
/* line refinement at output resolution: gentle unsharp mask plus a touch of
   extra darkness on outlines */
"float4 psrefine(VO i) : SV_Target {\n"
"  int2 p = int2(i.uv * src.xy);\n"
"  float3 c = L(p);\n"
"  float3 blur = (L(p + int2(-1, 0)) + L(p + int2(1, 0)) + L(p + int2(0, -1)) + L(p + int2(0, 1))) * 0.1875\n"
"              + (L(p + int2(-1, -1)) + L(p + int2(1, -1)) + L(p + int2(-1, 1)) + L(p + int2(1, 1))) * 0.0625;\n"
"  float3 r = c + (c - blur) * 0.6;\n"
"  float d = yuv(blur).x - yuv(c).x;\n"
"  if (d > 0.06) r *= 1.0 - min(d, 0.3) * 0.6;\n"
"  return float4(saturate(r), 1);\n"
"}\n"
"float4 psmain(VO i) : SV_Target {\n"
"  if (prm.w > 0.5) {\n"
"    float2 st = exp2(prm.y) / src.xy;\n"
"    float3 b = 0; float wsum = 0;\n"
"    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
"      float w = exp(-(x * x + y * y) / 4.0);\n"
"      b += tex.SampleLevel(sl, i.uv + float2(x, y) * st, prm.y).rgb * w; wsum += w;\n"
"    }\n"
"    return float4(b / wsum * prm.z, 1);\n"
"  }\n"
"  int f = (int)prm.x;\n"
"  float3 c;\n"
"  if (f == 0) c = sharp(i.uv);\n"
"  else if (f == 1) c = bicubic(i.uv);\n"
"  else if (f == 2) c = crt(i.uv, i.pos.xy);\n"
"  else c = tex.SampleLevel(sp, i.uv, 0).rgb;\n"
"  return float4(c, 1);\n"
"}\n";

static const char *aspect_names[] = {"4:3 + blurred sides", "4:3", "stretch to fill", "zoom to fill width"};
static const char *filter_names[] = {
#ifdef HD_BUILD
    "Cartoon HD", "Cartoon HD+",
#endif
    "sharp", "smooth", "CRT", "nearest"};
RenderSettings render_settings = {0, 0};

const char *render_aspect_name(int a) { return aspect_names[a % RENDER_ASPECTS]; }
const char *render_filter_name(int f) { return filter_names[f % RENDER_FILTERS]; }

static void release_rtv(void) {
    if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
}

static int make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    return FAILED(hr) ? -1 : 0;
}

int render_init(HWND hwnd) {
    rwnd = hwnd;
    RECT r;
    GetClientRect(hwnd, &r);
    buf_w = r.right > 0 ? (UINT)r.right : 1;
    buf_h = r.bottom > 0 ? (UINT)r.bottom : 1;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = buf_w;
    sd.BufferDesc.Height = buf_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                           &sd, &swap, &dev, NULL, &ctx);
    }
    if (FAILED(hr)) { LOG("Direct3D 11 unavailable (%08lx); using GDI", (unsigned long)hr); return -1; }
    /* we handle Alt+Enter ourselves (borderless fullscreen) */
    {
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
            SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fa)))
            IDXGIFactory_MakeWindowAssociation(fa, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (fa) IDXGIFactory_Release(fa);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
    }
    /* low latency: never queue more than one frame ahead of the display */
    {
        IDXGIDevice1 *d1 = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&d1))) {
            IDXGIDevice1_SetMaximumFrameLatency(d1, 1);
            IDXGIDevice1_Release(d1);
        }
    }
    if (make_rtv()) return -1;

    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    size_t sl = strlen(shader_src);
    if (FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "vsmain", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "psmain", "ps_4_0", 0, 0, &pb, &err))) {
        LOG("shader compile failed: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return -1;
    }
    {
        const char *entry[3] = {"psxbr", "psdedither", "psrefine"};
        ID3D11PixelShader **dst[3] = {&ps_xbr, &ps_dedither, &ps_refine};
        for (int k = 0; k < 3; k++) {
            ID3DBlob *b = NULL;
            if (FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, entry[k], "ps_4_0", 0, 0, &b, &err))) {
                LOG("shader %s failed: %s", entry[k], err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
                return -1;
            }
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, dst[k]);
            ID3D10Blob_Release(b);
        }
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);

    D3D11_SAMPLER_DESC s = {0};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_lin);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_point);
    LOG("renderer: Direct3D 11, %ux%u", buf_w, buf_h);
    return 0;
}

static int ensure_texture(int w, int h) {
    if (tex && w == tex_w && h == tex_h) return 0;
    if (srv) { ID3D11ShaderResourceView_Release(srv); srv = NULL; }
    if (tex) { ID3D11Texture2D_Release(tex); tex = NULL; }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;                 /* full chain: low mips give the blurred backdrop */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))) return -1;
    ID3D11Texture2D_GetDesc(tex, &td);
    tex_w = w; tex_h = h; tex_mips = (int)td.MipLevels;
    return 0;
}

static void draw_sz(float l, float t, float r, float b, float u0, float v0, float us, float vs_, int backdrop,
                    float mip, float bright, float dw, float dh, float sw, float sh) {
    CB c;
    c.rect[0] = l; c.rect[1] = t; c.rect[2] = r; c.rect[3] = b;
    c.src[0] = sw; c.src[1] = sh; c.src[2] = dw; c.src[3] = dh;
    c.prm[0] = (float)(render_settings.filter - RENDER_HD_FILTERS); c.prm[1] = mip; c.prm[2] = bright; c.prm[3] = (float)backdrop;
    c.uvx[0] = u0; c.uvx[1] = v0; c.uvx[2] = us; c.uvx[3] = vs_;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cbuf, 0, NULL, &c, 0, 0);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
}

static void draw(float l, float t, float r, float b, float u0, float v0, float us, float vs_, int backdrop,
                 float mip, float bright, float dw, float dh) {
    draw_sz(l, t, r, b, u0, v0, us, vs_, backdrop, mip, bright, dw, dh, (float)tex_w, (float)tex_h);
}

static void rt_free(RT *r) {
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->t) ID3D11Texture2D_Release(r->t);
    memset(r, 0, sizeof *r);
}

static int rt_ensure(RT *r, int w, int h, int target) {
    if (r->t && r->w == w && r->h == h) return 0;
    rt_free(r);
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (target ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &r->t))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)r->t, NULL, &r->srv))) return -1;
    if (target && FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)r->t, NULL, &r->rtv))) return -1;
    r->w = w; r->h = h;
    return 0;
}

static void bind_target(ID3D11RenderTargetView *v, float w, float h) {
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &v, NULL);
    D3D11_VIEWPORT vp = {0, 0, w, h, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
}

/* HD filters: upload the frame at its true resolution (normal-resolution
   content is stored with doubled pixels; undo that so the edge detector sees
   real neighbours), optionally de-dither, upscale with the edge-directed
   shader, optionally refine lines at output resolution. */
static int present_hd(const uint32_t *fb, int w, int h, float l, float t, float r, float b, double pw, double ph,
                      UINT cw, UINT ch) {
    static uint32_t packed[384 * 480];
    const uint32_t *src = fb;
    int sw = w;
    int doubled = (w == 768 && h <= 480);
    for (int i = 0; doubled && i < w * h; i += 2)
        if (fb[i] != fb[i + 1]) doubled = 0;
    if (doubled) {
        for (int i = 0; i < 384 * h; i++) packed[i] = fb[i * 2];
        src = packed;
        sw = 384;
    }
    int plus = render_settings.filter == 1;
    if (rt_ensure(&hd_src, sw, h, 0)) return -1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)hd_src.t, 0, NULL, src, (UINT)sw * 4, 0);
    ID3D11ShaderResourceView *in = hd_src.srv;
    if (plus) {
        if (rt_ensure(&hd_dd, sw, h, 1)) return -1;
        bind_target(hd_dd.rtv, (float)sw, (float)h);
        ID3D11DeviceContext_PSSetShader(ctx, ps_dedither, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &in);
        draw_sz(-1, 1, 1, -1, 0, 0, 1, 1, 0, 0, 1, (float)sw, (float)h, (float)sw, (float)h);
        in = hd_dd.srv;
    }
    int ow = (int)(pw + 0.5), oh = (int)(ph + 0.5);
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    if (plus) {
        if (rt_ensure(&hd_out, ow, oh, 1)) return -1;
        bind_target(hd_out.rtv, (float)ow, (float)oh);
        ID3D11DeviceContext_PSSetShader(ctx, ps_xbr, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &in);
        draw_sz(-1, 1, 1, -1, 0, 0, 1, 1, 0, 0, 1, (float)ow, (float)oh, (float)sw, (float)h);
        bind_target(rtv, (float)cw, (float)ch);
        ID3D11DeviceContext_PSSetShader(ctx, ps_refine, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &hd_out.srv);
        draw_sz(l, t, r, b, 0, 0, 1, 1, 0, 0, 1, (float)ow, (float)oh, (float)ow, (float)oh);
    } else {
        bind_target(rtv, (float)cw, (float)ch);
        ID3D11DeviceContext_PSSetShader(ctx, ps_xbr, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &in);
        draw_sz(l, t, r, b, 0, 0, 1, 1, 0, 0, 1, (float)pw, (float)ph, (float)sw, (float)h);
    }
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    return 0;
}

int render_present(const uint32_t *fb, int w, int h) {
    if (!dev) return -1;
    RECT cr;
    GetClientRect(rwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1, ch = cr.bottom > 0 ? (UINT)cr.bottom : 1;
    if (IsIconic(rwnd)) return 0;
    if (cw != buf_w || ch != buf_h) {
        release_rtv();
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return -1;
        buf_w = cw; buf_h = ch;
        if (make_rtv()) return -1;
    }
    if (ensure_texture(w, h)) return -1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, fb, (UINT)w * 4, 0);
    ID3D11DeviceContext_GenerateMips(ctx, srv);

    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)cw, (float)ch, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11SamplerState *ss[2] = {samp_lin, samp_point};
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);

    /* picture size in pixels for the chosen aspect mode */
    double win_aspect = (double)cw / ch, aspect = 4.0 / 3.0;
    double pw, ph;
    int mode = render_settings.aspect % RENDER_ASPECTS;
    if (mode == 2) { pw = cw; ph = ch; }
    else if (mode == 3) { pw = cw; ph = cw / aspect; if (ph < ch) { ph = ch; pw = ch * aspect; } }
    else if (win_aspect > aspect) { ph = ch; pw = ch * aspect; }
    else { pw = cw; ph = cw / aspect; }
    float l = (float)(-pw / cw), r = (float)(pw / cw), t = (float)(ph / ch), b = (float)(-ph / ch);

    if (mode == 0 && pw < cw - 1) {
        /* blurred, dimmed copy of the picture zoomed to cover the whole window */
        double cover_w = cw, cover_h = cw / aspect;
        if (cover_h < ch) { cover_h = ch; cover_w = ch * aspect; }
        float us = (float)(cw / cover_w), vs2 = (float)(ch / cover_h);
        float mip = (float)(tex_mips > 3 ? 3 : tex_mips - 1);
        draw(-1, 1, 1, -1, (1 - us) / 2, (1 - vs2) / 2, us, vs2, 1, mip, 0.5f, (float)cw, (float)ch);
    }
    if (render_settings.filter < RENDER_HD_FILTERS) {
        if (present_hd(fb, w, h, l, t, r, b, pw, ph, cw, ch)) return -1;
    } else {
        draw(l, t, r, b, 0, 0, 1, 1, 0, 0, 1, (float)pw, (float)ph);
    }
    IDXGISwapChain_Present(swap, 0, 0);
    return 0;
}
