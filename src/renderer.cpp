#include "renderer.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gta2 {
namespace {

void (*g_trace)(const char*) = nullptr;

void Trace(const char* format, ...) {
    if (!g_trace) return;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_trace(line);
}

// Where Direct3D comes from, and it matters which copy.
//
// RTX Remix ships as a d3d9.dll dropped beside the game, so *anything* in the
// process that asks the loader for "d3d9.dll" gets the Remix bridge client -
// including GTA2's own DirectDraw layer, which initialises during startup long
// before the renderer is loaded. That brings the Remix runtime up on a device
// DirectDraw owns: a fullscreen A8R8G8B8 one the game never draws through.
// Remix drives a single device per process, so ours arrives second, re-hooks the
// window procedure out from under the bridge's message channel, and the game
// hangs at the menu with the runtime still waiting on a handshake that can no
// longer be answered.
//
// Renaming the bridge and asking for it by that name keeps every accidental
// consumer on the system d3d9 and leaves the renderer's device the only one
// Remix ever sees. The plain name is still tried, so a stock Remix drop-in and a
// machine with no Remix at all both keep working.
const char* const kD3D9Modules[] = {"d3d9_remix.dll", "d3d9.dll"};

IDirect3D9* CreateD3D9() {
    for (const char* name : kD3D9Modules) {
        const HMODULE module = LoadLibraryA(name);
        if (!module) continue;
        using CreateFn = IDirect3D9*(WINAPI*)(UINT);
        const auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "Direct3DCreate9"));
        if (!create) continue;
        if (IDirect3D9* d3d = create(D3D_SDK_VERSION)) {
            Trace("Direct3D from %s", name);
            return d3d;
        }
    }
    return nullptr;
}

constexpr DWORD kWorldFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

// Tiles key their transparency off palette entry 0, so an alpha test is enough;
// it also keeps geometry opaque from the path tracer's point of view, which
// behaves far better than alpha blending under Remix.
constexpr DWORD kAlphaRef = 128;

AlphaMode g_alphaMode = AlphaMode::Test;
DWORD g_alphaRef = kAlphaRef;
}  // namespace

void SetAlphaMode(AlphaMode mode) { g_alphaMode = mode; }
AlphaMode GetAlphaMode() { return g_alphaMode; }
void SetAlphaRef(int ref) { g_alphaRef = static_cast<DWORD>(ref < 1 ? 1 : (ref > 254 ? 254 : ref)); }
int GetAlphaRef() { return static_cast<int>(g_alphaRef); }

namespace {

void SetMatrix(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const Mat4& matrix) {
    D3DMATRIX d3dMatrix;
    memcpy(&d3dMatrix, matrix.m, sizeof(d3dMatrix));
    device->SetTransform(state, &d3dMatrix);
}

}  // namespace

void SetRendererTrace(void (*sink)(const char*)) { g_trace = sink; }

Renderer::~Renderer() { Shutdown(); }

void Renderer::Shutdown() {
    ReleaseResources();
    batches_.clear();
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (d3d_) {
        d3d_->Release();
        d3d_ = nullptr;
    }
}

void Renderer::ReleaseResources() {
    // An animated tile's slot points at a frame owned by tileFrames_, so those
    // must not be released twice. Everything else in textures_ is owned here.
    for (IDirect3DTexture9* texture : textures_) {
        if (!texture) continue;
        bool ownedByFrames = false;
        for (const auto& frame : tileFrames_) {
            if (frame.second == texture) {
                ownedByFrames = true;
                break;
            }
        }
        if (!ownedByFrames) texture->Release();
    }
    textures_.clear();
    for (auto& frame : tileFrames_) {
        if (frame.second) frame.second->Release();
    }
    tileFrames_.clear();
    if (vertexBuffer_) {
        vertexBuffer_->Release();
        vertexBuffer_ = nullptr;
    }
    if (indexBuffer_) {
        indexBuffer_->Release();
        indexBuffer_ = nullptr;
    }
}

// Drop the level's geometry, and nothing else.
//
// GTA2 frees its map when it returns to the menu, and the renderer used to keep
// drawing the last level regardless: DrawWorld's only guard is that a vertex
// buffer exists, and one did. So quitting to the menu left a frozen cityscape on
// screen with the menu drawn over it - and under Remix that geometry was still
// in the scene, still being path traced, still lit.
//
// Deliberately not ReleaseResources: the device, the window and the tile
// textures all outlive a level, and tearing them down here would mean rebuilding
// the device every time someone backs out to the menu.
void Renderer::ReleaseWorld() {
    batches_.clear();
    if (savedTarget_) {
        savedTarget_->Release();
        savedTarget_ = nullptr;
    }
    if (uiSurface_) {
        uiSurface_->Release();
        uiSurface_ = nullptr;
    }
    if (uiTexture_) {
        uiTexture_->Release();
        uiTexture_ = nullptr;
    }
    uiClearPending_ = true;
    if (vertexBuffer_) {
        vertexBuffer_->Release();
        vertexBuffer_ = nullptr;
    }
    if (indexBuffer_) {
        indexBuffer_->Release();
        indexBuffer_ = nullptr;
    }
}

bool Renderer::Initialize(HWND window, int width, int height, std::string* error) {
    width_ = width;
    height_ = height;

    // Kept across a retry: creating the interface is what starts RTX Remix's
    // runtime, and asking for it twice would start it twice.
    if (!d3d_) {
        Trace("Direct3DCreate9...");
        d3d_ = CreateD3D9();
        if (!d3d_) {
            *error = "Direct3DCreate9 failed";
            return false;
        }
        Trace("Direct3DCreate9 -> %p", d3d_);
    }

    // Only the first attempt is traced. Under RTX Remix this runs once a frame
    // until the device takes, and one line per attempt would bury the log.
    const bool trace = deviceAttempts_++ == 0;

    // A windowed backbuffer has to match the current display mode, and GTA2's
    // Glide wrapper may have switched the desktop to another depth while it
    // runs, so adopt whatever the adapter is in right now.
    D3DDISPLAYMODE displayMode = {};
    const HRESULT modeResult = d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &displayMode);
    if (trace) {
        Trace("display mode hr=0x%08lX %ux%u format=%d", static_cast<unsigned long>(modeResult),
              displayMode.Width, displayMode.Height, static_cast<int>(displayMode.Format));
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = displayMode.Format != D3DFMT_UNKNOWN ? displayMode.Format : D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (trace) {
        Trace("CreateDevice hwnd=%p %dx%d windowed=%d fmt=%d", window, width, height,
              (int)pp.Windowed, static_cast<int>(pp.BackBufferFormat));
    }
    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device_);
    if (FAILED(hr)) {
        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device_);
    }
    if (FAILED(hr)) {
        // Not necessarily fatal. RTX Remix hands the interface back before the
        // 64-bit runtime behind it can serve a device, and turns everything down
        // with D3DERR_INVALIDCALL until it can; it only gets there once the game
        // has run its own loop for a moment. The caller retries per frame.
        char message[160];
        snprintf(message, sizeof(message), "CreateDevice failed (hr=0x%08lX, display format=%d)",
                 static_cast<unsigned long>(hr), static_cast<int>(displayMode.Format));
        *error = message;
        return false;
    }
    Trace("device created on attempt %d", deviceAttempts_);
    return true;
}

bool Renderer::UploadWorld(const WorldMesh& mesh, const Style& style, std::string* error) {
    if (mesh.vertices.empty()) {
        *error = "world mesh is empty";
        return false;
    }
    ReleaseResources();
    batches_ = mesh.batches;

    const UINT vertexBytes = static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex));
    if (FAILED(device_->CreateVertexBuffer(vertexBytes, D3DUSAGE_WRITEONLY, kWorldFvf,
                                           D3DPOOL_MANAGED, &vertexBuffer_, nullptr))) {
        *error = "CreateVertexBuffer failed";
        return false;
    }
    void* dest = nullptr;
    vertexBuffer_->Lock(0, 0, &dest, 0);
    memcpy(dest, mesh.vertices.data(), vertexBytes);
    vertexBuffer_->Unlock();

    const UINT indexBytes = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    if (FAILED(device_->CreateIndexBuffer(indexBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
                                          D3DPOOL_MANAGED, &indexBuffer_, nullptr))) {
        *error = "CreateIndexBuffer failed";
        return false;
    }
    indexBuffer_->Lock(0, 0, &dest, 0);
    memcpy(dest, mesh.indices.data(), indexBytes);
    indexBuffer_->Unlock();

    // One texture per tile keeps Remix's texture hashes stable and per-tile
    // replaceable, instead of hiding every surface behind one atlas hash.
    textures_.assign(style.TileCount(), nullptr);
    for (const TileBatch& batch : batches_) {
        if (textures_[batch.tile]) continue;

        IDirect3DTexture9* texture = nullptr;
        if (FAILED(device_->CreateTexture(kTileSize, kTileSize, 1, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_MANAGED, &texture, nullptr))) {
            *error = "CreateTexture failed";
            return false;
        }
        D3DLOCKED_RECT rect;
        texture->LockRect(0, &rect, nullptr, 0);
        const Tile& tile = style.GetTile(batch.tile);
        for (int y = 0; y < kTileSize; ++y) {
            memcpy(static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch,
                   &tile.pixels[static_cast<size_t>(y) * kTileSize], kTileSize * 4);
        }
        texture->UnlockRect(0);
        textures_[batch.tile] = texture;
    }
    return true;
}

void Renderer::UpdateTileTexture(int tile, const uint32_t* pixels) {
    if (!HasTileTexture(tile) || !pixels) return;

    // GTA2 animates a tile - fans, screens, water, traffic lights - by changing
    // the artwork behind a fixed tile number. Writing the new frame into the
    // texture this tile already has, which is what this used to do, gives RTX
    // Remix one texture whose content changes: a single entry in the picker,
    // animating, with no way to replace an individual frame.
    //
    // So each distinct frame gets a texture of its own and the tile is pointed at
    // it instead. A frame that comes round again resolves to the same object and
    // therefore the same hash, so the cost is bounded by how much distinct
    // artwork exists rather than by how long the game runs.
    uint64_t key = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < static_cast<size_t>(kTileSize) * kTileSize; ++i) {
        key ^= pixels[i];
        key *= 0x100000001B3ULL;
    }

    const auto existing = tileFrames_.find(key);
    if (existing != tileFrames_.end()) {
        textures_[tile] = existing->second;
        return;
    }

    IDirect3DTexture9* frame = nullptr;
    if (FAILED(device_->CreateTexture(kTileSize, kTileSize, 1, 0, D3DFMT_A8R8G8B8,
                                      D3DPOOL_MANAGED, &frame, nullptr))) {
        return;
    }
    D3DLOCKED_RECT rect;
    if (FAILED(frame->LockRect(0, &rect, nullptr, 0))) {
        frame->Release();
        return;
    }
    for (int y = 0; y < kTileSize; ++y) {
        memcpy(static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch,
               pixels + static_cast<size_t>(y) * kTileSize, kTileSize * 4);
    }
    frame->UnlockRect(0);

    tileFrames_[key] = frame;   // owns it from here
    textures_[tile] = frame;
}

void Renderer::CycleCullMode() {
    cullMode_ = cullMode_ == D3DCULL_CCW ? D3DCULL_CW
                : cullMode_ == D3DCULL_CW ? D3DCULL_NONE
                                          : D3DCULL_CCW;
}

void Renderer::ApplyFixedFunctionState() {
    device_->SetRenderState(D3DRS_LIGHTING, FALSE);
    device_->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device_->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device_->SetRenderState(D3DRS_CULLMODE, cullMode_);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_FILLMODE, wireframe_ ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    device_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
}

// The 2D layer the game believes it is painting into. See the note in the
// header for why it is kept rather than cleared.
bool Renderer::BeginUiLayer(bool clearNow) {
    if (!device_) return false;
    if (!uiTexture_) {
        if (FAILED(device_->CreateTexture(width_, height_, 1, D3DUSAGE_RENDERTARGET,
                                          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &uiTexture_,
                                          nullptr))) {
            uiTexture_ = nullptr;
            return false;
        }
        if (FAILED(uiTexture_->GetSurfaceLevel(0, &uiSurface_))) {
            uiTexture_->Release();
            uiTexture_ = nullptr;
            uiSurface_ = nullptr;
            return false;
        }
        uiClearPending_ = true;
    }
    if (FAILED(device_->GetRenderTarget(0, &savedTarget_))) {
        savedTarget_ = nullptr;
        return false;
    }
    if (FAILED(device_->SetRenderTarget(0, uiSurface_))) {
        savedTarget_->Release();
        savedTarget_ = nullptr;
        return false;
    }
    // Transparent, not black: the world is drawn underneath this, so everywhere
    // the game has not painted has to let it through. The game's own clear is
    // opaque black because in its model there is nothing underneath.
    if (clearNow || uiClearPending_) {
        device_->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        uiClearPending_ = false;
    }
    return true;
}

void Renderer::EndUiLayer() {
    if (!device_ || !savedTarget_) return;
    device_->SetRenderTarget(0, savedTarget_);
    savedTarget_->Release();
    savedTarget_ = nullptr;
    if (!uiTexture_) return;

    struct Composite {
        float x, y, z, rhw;
        float u, v;
    };
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    const Composite quad[4] = {{-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
                               {w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
                               {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
                               {-0.5f, h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f}};

    device_->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetRenderState(D3DRS_LIGHTING, FALSE);
    device_->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device_->SetTexture(0, uiTexture_);
    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device_->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(Composite));

    device_->SetTexture(0, nullptr);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device_->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

bool Renderer::BeginFrame() {
    stats_ = RenderStats{};
    if (!device_) return false;
    // Black, because that is what the game asks for. GTA2's menu paints only the
    // left third of the screen plus its text - traced from the 2D stream, the
    // whole front end is one blit at (0,0)-(278,480) and a row of glyph quads -
    // and everything else is meant to be the colour the screen was cleared to.
    // Vid_ClearScreen(ctx, 0,0,0,0,0, 640,480) is the game saying so; the video
    // proxy no longer forwards it, so this is the only clear left.
    device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f,
                   0);
    return SUCCEEDED(device_->BeginScene());
}

void Renderer::EndFrame() {
    if (!device_) return;
    device_->EndScene();
    device_->Present(nullptr, nullptr, nullptr, nullptr);
}

void Renderer::DrawWorld(const Camera& camera) {
    // Nothing to draw until the game has a world loaded; the frame is still
    // cleared and presented so menus have something to sit on.
    if (!device_ || !vertexBuffer_) return;

    ApplyFixedFunctionState();
    SetMatrix(device_, D3DTS_WORLD, Identity());
    SetMatrix(device_, D3DTS_VIEW, camera.ViewMatrix());
    SetMatrix(device_, D3DTS_PROJECTION,
              camera.ProjectionMatrix(static_cast<float>(width_) / static_cast<float>(height_)));

    device_->SetFVF(kWorldFvf);
    device_->SetStreamSource(0, vertexBuffer_, 0, sizeof(Vertex));
    device_->SetIndices(indexBuffer_);

    bool alphaTestEnabled = false;
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device_->SetRenderState(D3DRS_ALPHAREF, g_alphaRef);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    for (const TileBatch& batch : batches_) {
        if (batch.needsAlphaTest != alphaTestEnabled) {
            alphaTestEnabled = batch.needsAlphaTest;
            const bool blending = g_alphaMode == AlphaMode::Blend;
            // Blend mode still keeps a very low alpha test: without it the fully
            // transparent texels are still rasterised and still write depth,
            // which puts an invisible wall in front of everything behind them.
            device_->SetRenderState(D3DRS_ALPHATESTENABLE, alphaTestEnabled ? TRUE : FALSE);
            device_->SetRenderState(D3DRS_ALPHAREF, blending ? 1u : g_alphaRef);
            device_->SetRenderState(D3DRS_ALPHABLENDENABLE,
                                    (blending && alphaTestEnabled) ? TRUE : FALSE);
        }
        device_->SetTexture(0, textures_[batch.tile]);
        device_->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, batch.vertexStart, batch.vertexCount,
                                      batch.indexStart, batch.indexCount / 3);
        stats_.drawCalls++;
        stats_.triangles += static_cast<int>(batch.indexCount / 3);
    }
}

}  // namespace gta2
