// D3D9 fixed-function renderer for GTA2 world geometry.
//
// Deliberately fixed-function and untransformed: RTX Remix path traces draws
// whose vertices are in object/world space and whose projection is perspective,
// and skips anything using pre-transformed (D3DFVF_XYZRHW) vertices. Lighting
// and shadowing are left to Remix.
#pragma once

#include <d3d9.h>

#include <map>
#include <string>
#include <vector>

#include "camera.h"
#include "gta2_style.h"
#include "world_mesh.h"

namespace gta2 {

struct RenderStats {
    int drawCalls = 0;
    int triangles = 0;
};

// Optional trace sink for device bring-up. The in-process DLL points it at
// gta2dx9.log so each D3D9 call can be timestamped and lined up against RTX
// Remix's own bridge logs; the standalone viewer leaves it null.
void SetRendererTrace(void (*sink)(const char*));

// How a cutout's edge is resolved. Alpha test is the default and is what a path
// tracer wants - a blended surface has no single depth for a ray to hit - but
// now that transparent texels carry a sensible colour (see alpha_bleed.h) it is
// worth being able to compare them by eye rather than by argument.
enum class AlphaMode { Test, Blend };
void SetAlphaMode(AlphaMode mode);
AlphaMode GetAlphaMode();
void SetAlphaRef(int ref);   // 1..254, the alpha-test cutoff
int  GetAlphaRef();

class Renderer {
public:
    ~Renderer();

    bool Initialize(HWND window, int width, int height, std::string* error);
    bool UploadWorld(const WorldMesh& mesh, const Style& style, std::string* error);

    // Split so the screen-space pass (HUD, menus, sprites) can be drawn between
    // the world and the present.
    bool BeginFrame();
    void DrawWorld(const Camera& camera);
    void EndFrame();

    // GTA2 animates a tile - ceiling fans, screens, water - by changing the
    // artwork behind its number while the map itself stays put, so the world
    // mesh never changes but its textures have to be refreshed.
    bool HasTileTexture(int tile) const {
        return tile >= 0 && tile < static_cast<int>(textures_.size()) && textures_[tile] != nullptr;
    }
    void UpdateTileTexture(int tile, const uint32_t* pixels);
    int TileTextureCount() const { return static_cast<int>(textures_.size()); }

    bool HasWorld() const { return vertexBuffer_ != nullptr; }

    // Drop the level's geometry without touching the device, the window or the
    // tile textures. The game frees its map when it returns to the menu, and
    // without this the last level goes on being drawn - and path traced - behind
    // the menu.
    void ReleaseWorld();

    int Width() const { return width_; }
    int Height() const { return height_; }

    // Releases the device and every resource. Must run before the game tears its
    // window down: a live device on a destroyed window keeps the process alive.
    void Shutdown();

    void ToggleWireframe() { wireframe_ = !wireframe_; }
    void CycleCullMode();
    const RenderStats& Stats() const { return stats_; }
    IDirect3DDevice9* Device() const { return device_; }

private:
    void ApplyFixedFunctionState();
    void ReleaseResources();

    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer_ = nullptr;
    IDirect3DIndexBuffer9* indexBuffer_ = nullptr;
    std::vector<IDirect3DTexture9*> textures_;  // indexed by tile id, may contain nulls
    // Every distinct frame of tile artwork, keyed by content, and the owner of
    // all of them. An animated tile swaps textures_[tile] to point at a
    // different one of these rather than rewriting the pixels of the one it has,
    // which is what gives each frame its own Remix hash. See UpdateTileTexture.
    std::map<uint64_t, IDirect3DTexture9*> tileFrames_;
    std::vector<TileBatch> batches_;

    int width_ = 0;
    int height_ = 0;
    int deviceAttempts_ = 0;  // Initialize is retried until the device takes
    bool wireframe_ = false;
    DWORD cullMode_ = D3DCULL_CCW;
    RenderStats stats_;
};

}  // namespace gta2
