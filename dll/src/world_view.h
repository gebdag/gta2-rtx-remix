// The world-space view drawn in place of GTA2's own renderer.
//
// Reads the game's live map and camera, builds true 3D geometry, and draws it
// through D3D9 fixed-function so RTX Remix can path trace it. The game's own
// draw stream is ignored: it is pre-transformed screen space, which Remix skips.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "../../src/camera.h"
#include "../../src/renderer.h"
#include "live_geometry.h"
#include "overlay.h"

namespace gta2dx9 {

class WorldView {
public:
    // pitchDegrees: -90 looks straight down, matching GTA2's own view.
    // useGameTiles: take tile artwork from the running game rather than from our
    // own parse of the style file.
    void Configure(float pitchDegrees, float fovDegrees, bool useGameTiles);

    bool Initialize(HWND window, int width, int height, std::string* error);
    void Shutdown();
    bool Ready() const { return ready_; }

    // GTA2 hands the renderer the visible tile rectangle every frame, which is
    // exactly its zoom and centre - no need to reverse a projection.
    void SetVisibleTileBounds(float minX, float minY, float maxX, float maxY);

    // Called when the game starts a level, so the next frame rebuilds.
    void InvalidateWorld() { loadedMapObject_ = nullptr; }

    void BeginFrame() {
        overlay_.BeginFrame();
        live_.BeginFrame();
    }
    void RenderFrame();

    // The HUD and menus arrive as screen-space draws and are replayed on top of
    // the world rather than dropped.
    Overlay& GetOverlay() { return overlay_; }

    // Sprites and the block shapes the static mesh does not build, placed as
    // real 3D geometry from the world coordinates the game itself computed.
    LiveGeometry& Live() { return live_; }


    // Compares a tile the game just drew against what our own map walk says
    // belongs at that cell, to locate a placement mismatch.
    void CheckDrawnTile(const void* textureRecord);

    // Logs the world-space corners the game itself computed for a face of an
    // awkward block shape, so the mesh builder's idea of that shape can be
    // checked against the game rather than against a reading of its code.
    void CheckDrawnShape(int corners);

private:
    bool EnsureWorldLoaded();
    void RefreshAnimatedTiles();
    void UpdateCamera();
    void DumpCameraStruct() const;
    float GroundHeightAt(float tileX, float tileY) const;

    gta2::Renderer renderer_;
    Overlay overlay_;
    LiveGeometry live_;
    gta2::Camera camera_;
    gta2::Map map_;
    const void* loadedMapObject_ = nullptr;
    std::string dataDir_;

    float minX_ = 0.0f, minY_ = 0.0f, maxX_ = 0.0f, maxY_ = 0.0f;
    bool haveBounds_ = false;
    bool ready_ = false;
    int frameCount_ = 0;

    float smoothedHeight_ = 0.0f;
    bool heightSettled_ = false;
    float visibleTiles_ = 0.0f;
    bool useGameTiles_ = true;
    int tileChecks_ = 0;
    int shapeChecks_ = 0;

    // What the artwork for each tile was last taken from, so an animated tile
    // can be spotted and re-uploaded.
    struct TileSource {
        const void* record = nullptr;
        uint16_t revision = 0;
    };
    std::vector<TileSource> tileSources_;
};

}  // namespace gta2dx9
