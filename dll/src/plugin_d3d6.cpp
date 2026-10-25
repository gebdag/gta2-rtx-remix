// A stand-in for the original renderer's DirectX 6 device, for plugins.
//
// Plugins for GTA2 built on the community plugin SDK reach the original
// renderer's state directly: the SDK's GetGBH() is GetModuleHandleA("d3ddll.dll")
// plus 0x485E0, and the IDirect3DDevice3 the original draws with sits at +0x28
// of what is stored there. Its CSprite2d loads .dds textures through the
// DirectX 6 SDK's sample loader (d3dtextr / compress: the device's render
// target, its DirectDraw, CreateSurface, Lock, QueryInterface for
// IDirect3DTexture2) and draws them with SetRenderState, SetTexture and
// DrawPrimitive of pre-transformed D3DTLVERTEX (FVF 0x1C4). A minimap plugin in
// common use does all of its drawing that way.
//
// Here d3ddll.dll is this renderer, so +0x485E0 is just bytes of its code, and
// such a plugin read a garbage device pointer and crashed on the first map. This
// file is what it finds instead:
//
//   - each plugin module's import of GetModuleHandleA is patched, and a call
//     that asks for "d3ddll.dll" and returns straight into `add eax, 0x485E0`
//     (05 E0 85 04 00) - the SDK's GetGBH, nothing else - gets a base address
//     from which +0x485E0 holds a pointer to a stand-in state whose +0x28 is the
//     device below. Every other call goes through untouched;
//   - the device, its DirectDraw, surfaces and textures implement what that
//     loader and that drawing need, keep textures in system memory (DXT1, DXT3,
//     DXT5 or 32-bit ARGB, the formats the SDK's .dds files come in), and hand
//     the draws to the overlay through plugin_draw.h, in the order they are
//     made, among the game's own HUD draws.
//
// Everything else in the four interfaces answers DDERR_UNSUPPORTED. What it
// cannot do: depth. The SDK's radar masks its corners round by writing depth
// with a blend that draws nothing; those draws are dropped, so the map shows
// square, clipped by the plugin's own polygon clipping.
//
// The legacy DirectX headers and d3d9.h cannot share a translation unit, hence
// the separate file and the plain interface in plugin_draw.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <initguid.h>
#include <ddraw.h>
#include <d3d.h>
#include <psapi.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "log.h"
#include "plugin_draw.h"

namespace gta2dx9 {
namespace {

// --- Pixel data -------------------------------------------------------------

enum class Format { Unknown, Argb32, Dxt1, Dxt3, Dxt5 };

Format FormatOf(const DDPIXELFORMAT& pf) {
    if (pf.dwFlags & DDPF_FOURCC) {
        switch (pf.dwFourCC) {
            case MAKEFOURCC('D', 'X', 'T', '1'): return Format::Dxt1;
            case MAKEFOURCC('D', 'X', 'T', '2'):
            case MAKEFOURCC('D', 'X', 'T', '3'): return Format::Dxt3;
            case MAKEFOURCC('D', 'X', 'T', '4'):
            case MAKEFOURCC('D', 'X', 'T', '5'): return Format::Dxt5;
            default: return Format::Unknown;
        }
    }
    if ((pf.dwFlags & DDPF_RGB) && pf.dwRGBBitCount == 32) return Format::Argb32;
    return Format::Unknown;
}

bool Compressed(Format f) { return f == Format::Dxt1 || f == Format::Dxt3 || f == Format::Dxt5; }

// Bytes a level of this size takes, and the pitch the loader is given for it.
uint32_t LevelBytes(Format f, uint32_t w, uint32_t h) {
    if (!Compressed(f)) return w * h * 4;
    const uint32_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
    return blocks * (f == Format::Dxt1 ? 8u : 16u);
}

uint32_t Rgb565ToArgb(uint16_t c) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

// The colour half of every DXT block: two 5:6:5 endpoints and 2-bit indices.
void DecodeColourBlock(const uint8_t* block, bool dxt1, uint32_t out[16]) {
    const uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    uint32_t palette[4];
    palette[0] = Rgb565ToArgb(c0);
    palette[1] = Rgb565ToArgb(c1);
    auto mix = [&](int a, int b, int wa, int wb, int d) {
        uint32_t r = 0xFF000000u;
        for (int shift = 0; shift < 24; shift += 8) {
            const uint32_t ca = (palette[a] >> shift) & 0xFF, cb = (palette[b] >> shift) & 0xFF;
            r |= ((ca * wa + cb * wb) / d) << shift;
        }
        return r;
    };
    if (!dxt1 || c0 > c1) {
        palette[2] = mix(0, 1, 2, 1, 3);
        palette[3] = mix(0, 1, 1, 2, 3);
    } else {
        palette[2] = mix(0, 1, 1, 1, 2);
        palette[3] = 0;   // DXT1's transparent black
    }
    const uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) | (static_cast<uint32_t>(block[7]) << 24);
    for (int i = 0; i < 16; ++i) out[i] = palette[(bits >> (2 * i)) & 3];
}

// A level, decoded to 0xAARRGGBB rows of `w`.
std::vector<uint32_t> Decode(Format f, const uint8_t* data, uint32_t w, uint32_t h) {
    std::vector<uint32_t> out(static_cast<size_t>(w) * h, 0);
    if (f == Format::Argb32) {
        memcpy(out.data(), data, out.size() * 4);
        return out;
    }
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t stride = f == Format::Dxt1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* block = data + (static_cast<size_t>(by) * bw + bx) * stride;
            uint32_t texels[16];
            DecodeColourBlock(f == Format::Dxt1 ? block : block + 8, f == Format::Dxt1, texels);
            if (f == Format::Dxt3) {
                for (int i = 0; i < 16; ++i) {
                    const uint32_t a4 = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
                    texels[i] = (texels[i] & 0x00FFFFFFu) | ((a4 * 17) << 24);
                }
            } else if (f == Format::Dxt5) {
                const uint32_t a0 = block[0], a1 = block[1];
                uint32_t alphas[8] = {a0, a1};
                if (a0 > a1) {
                    for (int i = 1; i < 7; ++i) alphas[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                } else {
                    for (int i = 1; i < 5; ++i) alphas[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    alphas[6] = 0;
                    alphas[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) {
                    texels[i] = (texels[i] & 0x00FFFFFFu) | (alphas[(bits >> (3 * i)) & 7] << 24);
                }
            }
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) out[static_cast<size_t>(py) * w + px] = texels[y * 4 + x];
                }
            }
        }
    }
    return out;
}

class Device;
class DDraw;
class Surface;
class Texture;

Device* g_device = nullptr;
DDraw* g_ddraw = nullptr;
Surface* g_renderTarget = nullptr;

bool SameGuid(REFIID a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

// --- The interfaces -----------------------------------------------------------
//
// Every method of the four, in vtable order, with its exact signature: the
// skeleton was generated from the Windows SDK's d3d.h and ddraw.h, and the
// compiler refuses a class that misses one, so the layout cannot drift. What a
// plugin does not use answers DDERR_UNSUPPORTED.

#define DEVICE_FIELDS \
    DWORD states_[256] = {}; \
    Texture* texture_ = nullptr;
#define DDRAW_FIELDS
#define SURFACE_FIELDS \
    Surface(const DDSURFACEDESC2& desc, Format format, bool renderTarget); \
    ~Surface(); \
    LONG refs_ = 1; \
    DDSURFACEDESC2 desc_ = {}; \
    Format format_ = Format::Unknown; \
    bool renderTarget_ = false; \
    std::vector<uint8_t> data_; \
    Surface* next_ = nullptr;      /* the next mip level, owned */ \
    Texture* texture_ = nullptr;   /* the IDirect3DTexture2 face, created on demand */ \
    int image_ = -1;               /* the overlay's copy of the top level */ \
    bool dirty_ = true; \
    int Image();
#define TEXTURE_FIELDS \
    explicit Texture(Surface* surface) : surface_(surface) {} \
    Surface* surface_;

// IDirect3DDevice3, all 42 methods in IDirect3DDevice3's vtable order.
class Device final : public IDirect3DDevice3 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPD3DDEVICEDESC a0, LPD3DDEVICEDESC a1) override;
    HRESULT STDMETHODCALLTYPE GetStats(LPD3DSTATS a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE AddViewport(LPDIRECT3DVIEWPORT3 a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DeleteViewport(LPDIRECT3DVIEWPORT3 a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE NextViewport(LPDIRECT3DVIEWPORT3 a0, LPDIRECT3DVIEWPORT3* a1, DWORD a2) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK a0, LPVOID a1) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE GetDirect3D(LPDIRECT3D3* a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetCurrentViewport(LPDIRECT3DVIEWPORT3 a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetCurrentViewport(LPDIRECT3DVIEWPORT3 * a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(LPDIRECTDRAWSURFACE4 a0, DWORD a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(LPDIRECTDRAWSURFACE4 * a0) override;
    HRESULT STDMETHODCALLTYPE Begin(D3DPRIMITIVETYPE a0, DWORD a1, DWORD a2) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BeginIndexed(D3DPRIMITIVETYPE a0, DWORD a1, LPVOID a2, DWORD a3, DWORD a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Vertex(LPVOID a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Index(WORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE End(DWORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE a0, LPDWORD a1) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE a0, DWORD a1) override;
    HRESULT STDMETHODCALLTYPE GetLightState(D3DLIGHTSTATETYPE a0, LPDWORD a1) override;
    HRESULT STDMETHODCALLTYPE SetLightState(D3DLIGHTSTATETYPE a0, DWORD a1) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE a0, LPD3DMATRIX a1) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE a0, LPD3DMATRIX a1) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE a0, LPD3DMATRIX a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE a0, DWORD a1, LPVOID a2, DWORD a3, DWORD a4) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE a0, DWORD a1, LPVOID a2, DWORD a3, LPWORD a4, DWORD a5, DWORD a6) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetClipStatus(LPD3DCLIPSTATUS a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(LPD3DCLIPSTATUS a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveStrided(D3DPRIMITIVETYPE a0, DWORD a1, LPD3DDRAWPRIMITIVESTRIDEDDATA a2, DWORD a3, DWORD a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE a0, DWORD a1, LPD3DDRAWPRIMITIVESTRIDEDDATA a2, DWORD a3, LPWORD a4, DWORD a5, DWORD a6) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveVB(D3DPRIMITIVETYPE a0, LPDIRECT3DVERTEXBUFFER a1, DWORD a2, DWORD a3, DWORD a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE a0, LPDIRECT3DVERTEXBUFFER a1, LPWORD a2, DWORD a3, DWORD a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE ComputeSphereVisibility(LPD3DVECTOR a0, LPD3DVALUE a1, DWORD a2, DWORD a3, LPDWORD a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD a0, LPDIRECT3DTEXTURE2 * a1) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD a0, LPDIRECT3DTEXTURE2 a1) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD a0, D3DTEXTURESTAGESTATETYPE a1, LPDWORD a2) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD a0, D3DTEXTURESTAGESTATETYPE a1, DWORD a2) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(LPDWORD a0) override;
    DEVICE_FIELDS
};

// IDirectDraw4, all 28 methods in IDirectDraw4's vtable order.
class DDraw final : public IDirectDraw4 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Compact() override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD a0, LPDIRECTDRAWCLIPPER* a1, IUnknown * a2) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE CreatePalette(DWORD a0, LPPALETTEENTRY a1, LPDIRECTDRAWPALETTE* a2, IUnknown * a3) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC2 a0, LPDIRECTDRAWSURFACE4 * a1, IUnknown * a2) override;
    HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE4 a0, LPDIRECTDRAWSURFACE4 * a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD a0, LPDDSURFACEDESC2 a1, LPVOID a2, LPDDENUMMODESCALLBACK2 a3) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD a0, LPDDSURFACEDESC2 a1, LPVOID a2, LPDDENUMSURFACESCALLBACK2 a3) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS a0, LPDDCAPS a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC2 a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD a0, LPDWORD a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE4 * a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Initialize(GUID * a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND a0, DWORD a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD a0, HANDLE a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS2 a0, LPDWORD a1, LPDWORD a2) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetSurfaceFromDC(HDC a0, LPDIRECTDRAWSURFACE4 * a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE RestoreAllSurfaces() override;
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
    HRESULT STDMETHODCALLTYPE GetDeviceIdentifier(LPDDDEVICEIDENTIFIER a0, DWORD a1) override { return DDERR_UNSUPPORTED; }
    DDRAW_FIELDS
};

// IDirectDrawSurface4, all 45 methods in IDirectDrawSurface4's vtable order.
class Surface final : public IDirectDrawSurface4 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE4 a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Blt(LPRECT a0, LPDIRECTDRAWSURFACE4 a1, LPRECT a2, DWORD a3, LPDDBLTFX a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH a0, DWORD a1, DWORD a2) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BltFast(DWORD a0, DWORD a1, LPDIRECTDRAWSURFACE4 a2, LPRECT a3, DWORD a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD a0, LPDIRECTDRAWSURFACE4 a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID a0, LPDDENUMSURFACESCALLBACK2 a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD a0, LPVOID a1, LPDDENUMSURFACESCALLBACK2 a2) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE4 a0, DWORD a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS2 a0, LPDIRECTDRAWSURFACE4 * a1) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS2 a0) override;
    HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER* a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD a0, LPDDCOLORKEY a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDC(HDC * a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG a0, LPLONG a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE* a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT a0) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC2 a0) override;
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW a0, LPDDSURFACEDESC2 a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE IsLost() override;
    HRESULT STDMETHODCALLTYPE Lock(LPRECT a0, LPDDSURFACEDESC2 a1, DWORD a2, HANDLE a3) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Restore() override;
    HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD a0, LPDDCOLORKEY a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG a0, LONG a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Unlock(LPRECT a0) override;
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT a0, LPDIRECTDRAWSURFACE4 a1, LPRECT a2, DWORD a3, LPDDOVERLAYFX a4) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD a0, LPDIRECTDRAWSURFACE4 a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDDInterface(LPVOID * a0) override;
    HRESULT STDMETHODCALLTYPE PageLock(DWORD a0) override;
    HRESULT STDMETHODCALLTYPE PageUnlock(DWORD a0) override;
    HRESULT STDMETHODCALLTYPE SetSurfaceDesc(LPDDSURFACEDESC2 a0, DWORD a1) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a0, LPVOID a1, DWORD a2, DWORD a3) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a0, LPVOID a1, LPDWORD a2) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetUniquenessValue(LPDWORD a0) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE ChangeUniquenessValue() override { return DDERR_UNSUPPORTED; }
    SURFACE_FIELDS
};

// IDirect3DTexture2, all 6 methods in IDirect3DTexture2's vtable order.
class Texture final : public IDirect3DTexture2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetHandle(LPDIRECT3DDEVICE2 a0, LPD3DTEXTUREHANDLE a1) override;
    HRESULT STDMETHODCALLTYPE PaletteChanged(DWORD a0, DWORD a1) override;
    HRESULT STDMETHODCALLTYPE Load(LPDIRECT3DTEXTURE2 a0) override;
    TEXTURE_FIELDS
};


// --- Device -------------------------------------------------------------------

HRESULT Device::QueryInterface(REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    if (SameGuid(riid, IID_IUnknown) || SameGuid(riid, IID_IDirect3DDevice3)) {
        *ppv = this;
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
// The device and its DirectDraw live for the process; their counts are not
// tracked.
ULONG Device::AddRef() { return 2; }
ULONG Device::Release() { return 1; }

HRESULT Device::GetCaps(LPD3DDEVICEDESC a0, LPD3DDEVICEDESC a1) {
    // A hardware device with no texture restrictions: no powers of two, no
    // squares, so the loader keeps every texture at its own size.
    for (LPD3DDEVICEDESC d : {a0, a1}) {
        if (!d || d->dwSize < sizeof(DWORD)) continue;
        const DWORD size = d->dwSize;
        memset(d, 0, size);
        d->dwSize = size;
    }
    if (a0) {
        a0->dwFlags = D3DDD_TRICAPS;
        a0->dpcTriCaps.dwSize = sizeof(D3DPRIMCAPS);
        a0->dpcTriCaps.dwTextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PERSPECTIVE;
    }
    return D3D_OK;
}

HRESULT Device::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK a0, LPVOID a1) {
    if (!a0) return DDERR_INVALIDPARAMS;
    // Exactly the formats the textures are stored in, so the loader always finds
    // an exact match and never takes its conversion path (which blits through a
    // surface pointer it never sets).
    DDPIXELFORMAT formats[4] = {};
    for (DDPIXELFORMAT& f : formats) f.dwSize = sizeof(DDPIXELFORMAT);
    formats[0].dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    formats[0].dwRGBBitCount = 32;
    formats[0].dwRBitMask = 0x00FF0000;
    formats[0].dwGBitMask = 0x0000FF00;
    formats[0].dwBBitMask = 0x000000FF;
    formats[0].dwRGBAlphaBitMask = 0xFF000000;
    formats[1].dwFlags = DDPF_FOURCC;
    formats[1].dwFourCC = MAKEFOURCC('D', 'X', 'T', '1');
    formats[2].dwFlags = DDPF_FOURCC;
    formats[2].dwFourCC = MAKEFOURCC('D', 'X', 'T', '3');
    formats[3].dwFlags = DDPF_FOURCC;
    formats[3].dwFourCC = MAKEFOURCC('D', 'X', 'T', '5');
    for (DDPIXELFORMAT& f : formats) {
        if (a0(&f, a1) != D3DENUMRET_OK) break;
    }
    return D3D_OK;
}

HRESULT Device::BeginScene() { return D3D_OK; }
HRESULT Device::EndScene() { return D3D_OK; }

HRESULT Device::GetRenderState(D3DRENDERSTATETYPE a0, LPDWORD a1) {
    if (!a1) return DDERR_INVALIDPARAMS;
    *a1 = static_cast<DWORD>(a0) < 256 ? states_[a0] : 0;
    return D3D_OK;
}
HRESULT Device::SetRenderState(D3DRENDERSTATETYPE a0, DWORD a1) {
    if (static_cast<DWORD>(a0) < 256) states_[a0] = a1;
    return D3D_OK;
}
HRESULT Device::GetLightState(D3DLIGHTSTATETYPE, LPDWORD a1) {
    if (a1) *a1 = 0;
    return D3D_OK;
}
HRESULT Device::SetLightState(D3DLIGHTSTATETYPE, DWORD) { return D3D_OK; }
HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) { return D3D_OK; }
HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE, LPD3DMATRIX a1) {
    if (a1) {
        memset(a1, 0, sizeof(D3DMATRIX));
        a1->_11 = a1->_22 = a1->_33 = a1->_44 = 1.0f;
    }
    return D3D_OK;
}
HRESULT Device::GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, LPDWORD a2) {
    if (a2) *a2 = 0;
    return D3D_OK;
}
HRESULT Device::SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) { return D3D_OK; }
HRESULT Device::ValidateDevice(LPDWORD a0) {
    if (a0) *a0 = 1;
    return D3D_OK;
}

// --- Surface --------------------------------------------------------------------

Surface::Surface(const DDSURFACEDESC2& desc, Format format, bool renderTarget)
    : desc_(desc), format_(format), renderTarget_(renderTarget) {
    if (!renderTarget_) data_.resize(LevelBytes(format_, desc_.dwWidth, desc_.dwHeight));
}

Surface::~Surface() {
    if (image_ >= 0) PluginImageRelease(image_);
    delete texture_;
    delete next_;
}

ULONG Surface::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
ULONG Surface::Release() {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0 && !renderTarget_) delete this;
    return static_cast<ULONG>(left);
}

HRESULT Surface::Lock(LPRECT, LPDDSURFACEDESC2 a1, DWORD, HANDLE) {
    if (!a1 || renderTarget_) return DDERR_INVALIDPARAMS;
    const DWORD size = a1->dwSize ? a1->dwSize : sizeof(DDSURFACEDESC2);
    *a1 = desc_;
    a1->dwSize = size;
    a1->lpSurface = data_.data();
    a1->dwFlags &= ~(DDSD_PITCH | DDSD_LINEARSIZE);
    if (Compressed(format_)) {
        a1->dwFlags |= DDSD_LINEARSIZE;
        a1->dwLinearSize = static_cast<DWORD>(data_.size());
    } else {
        a1->dwFlags |= DDSD_PITCH;
        a1->lPitch = static_cast<LONG>(desc_.dwWidth * 4);
    }
    return DD_OK;
}

HRESULT Surface::Unlock(LPRECT) {
    dirty_ = true;
    return DD_OK;
}

HRESULT Surface::GetAttachedSurface(LPDDSCAPS2, LPDIRECTDRAWSURFACE4* a1) {
    if (!a1) return DDERR_INVALIDPARAMS;
    if (!next_) {
        *a1 = nullptr;
        return DDERR_NOTFOUND;
    }
    next_->AddRef();
    *a1 = next_;
    return DD_OK;
}

HRESULT Surface::GetSurfaceDesc(LPDDSURFACEDESC2 a0) {
    if (!a0) return DDERR_INVALIDPARAMS;
    const DWORD size = a0->dwSize ? a0->dwSize : sizeof(DDSURFACEDESC2);
    *a0 = desc_;
    a0->dwSize = size;
    return DD_OK;
}

HRESULT Surface::GetCaps(LPDDSCAPS2 a0) {
    if (!a0) return DDERR_INVALIDPARAMS;
    *a0 = desc_.ddsCaps;
    return DD_OK;
}
HRESULT Surface::GetPixelFormat(LPDDPIXELFORMAT a0) {
    if (!a0) return DDERR_INVALIDPARAMS;
    *a0 = desc_.ddpfPixelFormat;
    return DD_OK;
}
HRESULT Surface::IsLost() { return DD_OK; }
HRESULT Surface::Restore() { return DD_OK; }
HRESULT Surface::PageLock(DWORD) { return DD_OK; }
HRESULT Surface::PageUnlock(DWORD) { return DD_OK; }

// The overlay's copy, rebuilt after the plugin has written new pixels.
int Surface::Image() {
    if (renderTarget_ || format_ == Format::Unknown || data_.empty()) return -1;
    if (dirty_ || image_ < 0) {
        const std::vector<uint32_t> argb =
            Decode(format_, data_.data(), desc_.dwWidth, desc_.dwHeight);
        if (image_ < 0) {
            image_ = PluginImageCreate(static_cast<int>(desc_.dwWidth),
                                       static_cast<int>(desc_.dwHeight), argb.data());
        } else {
            PluginImageUpdate(image_, static_cast<int>(desc_.dwWidth),
                              static_cast<int>(desc_.dwHeight), argb.data());
        }
        dirty_ = false;
    }
    return image_;
}

// --- Texture --------------------------------------------------------------------

HRESULT Texture::QueryInterface(REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    if (SameGuid(riid, IID_IUnknown) || SameGuid(riid, IID_IDirect3DTexture2)) {
        AddRef();
        *ppv = static_cast<IDirect3DTexture2*>(this);
        return S_OK;
    }
    return surface_->QueryInterface(riid, ppv);
}
// The texture is a face of its surface and shares its count.
ULONG Texture::AddRef() { return surface_->AddRef(); }
ULONG Texture::Release() { return surface_->Release(); }
HRESULT Texture::GetHandle(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE a1) {
    if (a1) *a1 = static_cast<D3DTEXTUREHANDLE>(reinterpret_cast<uintptr_t>(this));
    return D3D_OK;
}
HRESULT Texture::PaletteChanged(DWORD, DWORD) { return D3D_OK; }
HRESULT Texture::Load(LPDIRECT3DTEXTURE2) { return DDERR_UNSUPPORTED; }

HRESULT Surface::QueryInterface(REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    if (SameGuid(riid, IID_IUnknown) || SameGuid(riid, IID_IDirectDrawSurface4)) {
        AddRef();
        *ppv = static_cast<IDirectDrawSurface4*>(this);
        return S_OK;
    }
    if (SameGuid(riid, IID_IDirect3DTexture2) && !renderTarget_) {
        if (!texture_) texture_ = new Texture(this);
        AddRef();
        *ppv = static_cast<IDirect3DTexture2*>(texture_);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// --- DirectDraw ---------------------------------------------------------------

HRESULT DDraw::QueryInterface(REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    if (SameGuid(riid, IID_IUnknown) || SameGuid(riid, IID_IDirectDraw4)) {
        *ppv = this;
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG DDraw::AddRef() { return 2; }
ULONG DDraw::Release() { return 1; }
HRESULT DDraw::TestCooperativeLevel() { return DD_OK; }
HRESULT DDraw::RestoreAllSurfaces() { return DD_OK; }

HRESULT DDraw::CreateSurface(LPDDSURFACEDESC2 a0, LPDIRECTDRAWSURFACE4* a1, IUnknown*) {
    if (!a0 || !a1) return DDERR_INVALIDPARAMS;
    *a1 = nullptr;
    const Format format = FormatOf(a0->ddpfPixelFormat);
    const DWORD width = a0->dwWidth, height = a0->dwHeight;
    if (format == Format::Unknown || !width || !height || width > 4096 || height > 4096) {
        static int noted = 0;
        if (noted++ < 8) {
            Log("plugin d3d6: CreateSurface %lux%lu in an unsupported format (flags %lx, fourcc %08lx)",
                width, height, a0->ddpfPixelFormat.dwFlags, a0->ddpfPixelFormat.dwFourCC);
        }
        return DDERR_INVALIDPIXELFORMAT;
    }
    DWORD levels = (a0->dwFlags & DDSD_MIPMAPCOUNT) && a0->dwMipMapCount > 1 ? a0->dwMipMapCount : 1;
    if (levels > 16) levels = 16;

    Surface* top = nullptr;
    Surface* previous = nullptr;
    DWORD w = width, h = height;
    for (DWORD level = 0; level < levels; ++level) {
        DDSURFACEDESC2 desc = *a0;
        desc.dwSize = sizeof(DDSURFACEDESC2);
        desc.dwWidth = w;
        desc.dwHeight = h;
        desc.dwMipMapCount = levels - level;
        desc.lpSurface = nullptr;
        Surface* surface = new Surface(desc, format, false);
        if (!top) top = surface;
        if (previous) previous->next_ = surface;
        previous = surface;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    *a1 = top;
    return DD_OK;
}

HRESULT Surface::GetDDInterface(LPVOID* a0) {
    if (!a0) return DDERR_INVALIDPARAMS;
    *a0 = static_cast<IDirectDraw4*>(g_ddraw);
    return DD_OK;
}

HRESULT Device::GetRenderTarget(LPDIRECTDRAWSURFACE4* a0) {
    if (!a0) return DDERR_INVALIDPARAMS;
    g_renderTarget->AddRef();
    *a0 = g_renderTarget;
    return D3D_OK;
}

HRESULT Device::GetTexture(DWORD a0, LPDIRECT3DTEXTURE2* a1) {
    if (!a1) return DDERR_INVALIDPARAMS;
    *a1 = a0 == 0 ? texture_ : nullptr;
    if (*a1) (*a1)->AddRef();
    return D3D_OK;
}

HRESULT Device::SetTexture(DWORD a0, LPDIRECT3DTEXTURE2 a1) {
    if (a0 != 0) return D3D_OK;
    Texture* texture = static_cast<Texture*>(a1);
    if (texture) texture->AddRef();
    if (texture_) texture_->Release();
    texture_ = texture;
    return D3D_OK;
}

// D3DTLVERTEX, which is what FVF 0x1C4 lays out.
struct TlVertex {
    float sx, sy, sz, rhw;
    DWORD colour, specular;
    float tu, tv;
};

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE a0, DWORD a1, LPVOID a2, DWORD a3, DWORD) {
    constexpr DWORD kTlVertex = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
    if (a1 != kTlVertex || !a2 || a3 == 0) {
        static int noted = 0;
        if (noted++ < 4) Log("plugin d3d6: DrawPrimitive with FVF %08lx ignored", a1);
        return D3D_OK;
    }
    const bool blend = states_[D3DRENDERSTATE_ALPHABLENDENABLE] != 0;
    const DWORD src = states_[D3DRENDERSTATE_SRCBLEND];
    const DWORD dst = states_[D3DRENDERSTATE_DESTBLEND];
    // A blend that keeps the destination and adds nothing draws nothing: the
    // SDK's radar uses it to write a depth mask, which there is no depth for.
    if (blend && src == D3DBLEND_ZERO && dst == D3DBLEND_ONE) return D3D_OK;
    const bool additive = blend && dst == D3DBLEND_ONE;
    const bool linear = states_[D3DRENDERSTATE_TEXTUREMAG] == D3DFILTER_LINEAR ||
                        states_[D3DRENDERSTATE_TEXTUREMIN] == D3DFILTER_LINEAR;

    PluginPrimitive type;
    switch (a0) {
        case D3DPT_LINELIST: type = PluginPrimitive::LineList; break;
        case D3DPT_LINESTRIP: type = PluginPrimitive::LineStrip; break;
        case D3DPT_TRIANGLELIST: type = PluginPrimitive::TriangleList; break;
        case D3DPT_TRIANGLESTRIP: type = PluginPrimitive::TriangleStrip; break;
        case D3DPT_TRIANGLEFAN: type = PluginPrimitive::TriangleFan; break;
        default: return D3D_OK;
    }
    const int image = texture_ ? texture_->surface_->Image() : -1;
    const TlVertex* in = static_cast<const TlVertex*>(a2);
    std::vector<PluginVertex> out(a3);
    for (DWORD i = 0; i < a3; ++i) {
        out[i] = PluginVertex{in[i].sx, in[i].sy, in[i].colour, in[i].tu, in[i].tv};
    }
    PluginDraw(type, out.data(), static_cast<int>(a3), image, additive, linear);
    return D3D_OK;
}

// --- The lookup -----------------------------------------------------------------

// What the SDK's GetGBH() reads: a pointer, at d3ddll.dll + 0x485E0, to the
// original renderer's state, whose +0x28 is its device.
constexpr uintptr_t kGraphicsOffset = 0x485E0;
struct StandInGraphics {
    uint8_t before[0x28];
    IDirect3DDevice3* device;
    uint8_t after[0x100];
};
StandInGraphics g_graphics = {};
StandInGraphics* g_graphicsPointer = &g_graphics;

using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
GetModuleHandleAFn g_realGetModuleHandleA = nullptr;

// `add eax, 0x485E0` - the SDK's GetGBH, returned straight into.
bool ReturnsIntoGraphicsLookup(const void* returnAddress) {
    static const uint8_t kAddEax[] = {0x05, 0xE0, 0x85, 0x04, 0x00};
    __try {
        return memcmp(returnAddress, kAddEax, sizeof(kAddEax)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HMODULE WINAPI PluginGetModuleHandleA(LPCSTR name) {
    if (name && lstrcmpiA(name, "d3ddll.dll") == 0) {
        if (ReturnsIntoGraphicsLookup(_ReturnAddress())) {
            return reinterpret_cast<HMODULE>(reinterpret_cast<uintptr_t>(&g_graphicsPointer) -
                                             kGraphicsOffset);
        }
        // Installed as gta2dx9.dll there is no d3ddll.dll at all, and the SDK
        // would conclude there is no renderer to draw with.
        HMODULE real = g_realGetModuleHandleA(name);
        if (!real) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCSTR>(&PluginGetModuleHandleA), &real);
        return real;
    }
    return g_realGetModuleHandleA(name);
}

// Points one module's import of kernel32's GetModuleHandleA at ours.
bool PatchImport(HMODULE module) {
    uint8_t* base = reinterpret_cast<uint8_t*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool patched = false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name; ++desc) {
        if (lstrcmpiA(reinterpret_cast<const char*>(base + desc->Name), "kernel32.dll") != 0) continue;
        if (!desc->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), "GetModuleHandleA") != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                slots->u1.Function = reinterpret_cast<uintptr_t>(&PluginGetModuleHandleA);
                VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                patched = true;
            }
        }
    }
    return patched;
}

}  // namespace

void PluginD3D6Install() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    g_realGetModuleHandleA = reinterpret_cast<GetModuleHandleAFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetModuleHandleA"));
    if (!g_realGetModuleHandleA) return;

    static Device device;
    static DDraw ddraw;
    g_device = &device;
    g_ddraw = &ddraw;
    DDSURFACEDESC2 rt = {};
    rt.dwSize = sizeof(rt);
    rt.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    rt.ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE;
    rt.dwWidth = 640;
    rt.dwHeight = 480;
    rt.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    rt.ddpfPixelFormat.dwFlags = DDPF_RGB;
    rt.ddpfPixelFormat.dwRGBBitCount = 32;
    rt.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    rt.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    rt.ddpfPixelFormat.dwBBitMask = 0x000000FF;
    static Surface renderTarget(rt, Format::Argb32, true);
    g_renderTarget = &renderTarget;
    // The original renderer's defaults where the SDK reads before it writes.
    device.states_[D3DRENDERSTATE_TEXTUREMAG] = D3DFILTER_LINEAR;
    device.states_[D3DRENDERSTATE_TEXTUREMIN] = D3DFILTER_LINEAR;
    device.states_[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_SRCALPHA;
    device.states_[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_INVSRCALPHA;
    g_graphics.device = &device;

    // Every module from the game's own folder, other than the game and this
    // renderer: the plugins loaded with the game. Overlays and other software
    // injected from elsewhere are left alone.
    char folder[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, folder, MAX_PATH);
    if (char* slash = strrchr(folder, '\\')) slash[1] = '\0';
    const size_t folderLength = strlen(folder);
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&PluginD3D6Install), &self);
    HMODULE modules[512];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return;
    int patched = 0;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 512; ++i) {
        HMODULE module = modules[i];
        if (module == self || module == GetModuleHandleW(nullptr)) continue;
        char path[MAX_PATH] = "";
        if (!GetModuleFileNameA(module, path, MAX_PATH)) continue;
        if (!folderLength || _strnicmp(path, folder, folderLength) != 0) continue;
        if (PatchImport(module)) {
            ++patched;
            const char* leaf = strrchr(path, '\\');
            Log("plugin d3d6: GetModuleHandleA routed in %s", leaf ? leaf + 1 : path);
        }
    }
    if (!patched) Log("plugin d3d6: no plugin imports GetModuleHandleA; nothing to stand in for");
}

}  // namespace gta2dx9
