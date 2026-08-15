#include "renderer.h"

#include <cstring>

namespace gta2 {
namespace {

constexpr DWORD kWorldFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

// Tiles key their transparency off palette entry 0, so an alpha test is enough;
// it also keeps geometry opaque from the path tracer's point of view, which
// behaves far better than alpha blending under Remix.
constexpr DWORD kAlphaRef = 128;

void SetMatrix(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const Mat4& matrix) {
    D3DMATRIX d3dMatrix;
    memcpy(&d3dMatrix, matrix.m, sizeof(d3dMatrix));
    device->SetTransform(state, &d3dMatrix);
}

}  // namespace

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
    for (IDirect3DTexture9* texture : textures_) {
        if (texture) texture->Release();
    }
    textures_.clear();
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

    d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) {
        *error = "Direct3DCreate9 failed";
        return false;
    }

    // A windowed backbuffer has to match the current display mode, and GTA2's
    // Glide wrapper may have switched the desktop to another depth while it
    // runs, so adopt whatever the adapter is in right now.
    D3DDISPLAYMODE displayMode = {};
    d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &displayMode);

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

    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device_);
    if (FAILED(hr)) {
        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device_);
    }
    if (FAILED(hr)) {
        char message[160];
        snprintf(message, sizeof(message), "CreateDevice failed (hr=0x%08lX, display format=%d)",
                 static_cast<unsigned long>(hr), static_cast<int>(displayMode.Format));
        *error = message;
        return false;
    }
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
    D3DLOCKED_RECT rect;
    if (FAILED(textures_[tile]->LockRect(0, &rect, nullptr, 0))) return;
    for (int y = 0; y < kTileSize; ++y) {
        memcpy(static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch,
               pixels + static_cast<size_t>(y) * kTileSize, kTileSize * 4);
    }
    textures_[tile]->UnlockRect(0);
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

bool Renderer::BeginFrame() {
    stats_ = RenderStats{};
    if (!device_) return false;
    device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 28, 38), 1.0f,
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
    device_->SetRenderState(D3DRS_ALPHAREF, kAlphaRef);

    for (const TileBatch& batch : batches_) {
        if (batch.needsAlphaTest != alphaTestEnabled) {
            alphaTestEnabled = batch.needsAlphaTest;
            device_->SetRenderState(D3DRS_ALPHATESTENABLE, alphaTestEnabled ? TRUE : FALSE);
        }
        device_->SetTexture(0, textures_[batch.tile]);
        device_->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, batch.vertexStart, batch.vertexCount,
                                      batch.indexStart, batch.indexCount / 3);
        stats_.drawCalls++;
        stats_.triangles += static_cast<int>(batch.indexCount / 3);
    }
}

}  // namespace gta2
