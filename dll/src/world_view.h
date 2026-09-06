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
#include "ground.h"
#include "live_geometry.h"
#include "overlay.h"

namespace gta2dx9 {

// Where the renderer presents. GTA2 loads a *video* device alongside the render
// device, and that is what owns the screen; these are the ways of getting out
// from under it. own_window in gta2dx9.ini selects one.
enum class PresentWindow {
    GameWindow = 0,  // share the game's, which deadlocks Remix's first Present
    Child = 1,       // a child of the game's window
    Topmost = 2,     // a separate top-level window in the topmost band
};

class WorldView {
public:
    // pitchDegrees: -90 looks straight down, matching GTA2's own view.
    // useGameTiles: take tile artwork from the running game rather than from our
    // own parse of the style file.
    // ownWindow: present into a window of our own rather than the game's, which
    // DirectDraw has already claimed.
    void Configure(float pitchDegrees, float fovDegrees, bool useGameTiles, PresentWindow present);

    // The size to actually render and present at, which is deliberately not the
    // game's. GTA2's own resolution is whatever its options screen last wrote -
    // 640x480 out of the box, and its manager will not offer much better because
    // the list comes from DirectDraw mode enumeration. That number has no
    // business deciding how big the path traced image is: it is the resolution
    // the HUD is laid out in, and the overlay already scales that to whatever it
    // is drawn into.
    //
    // 0 x 0 means the desktop resolution, which is what you want essentially
    // always. Anything else is taken literally.
    void SetRenderSize(int width, int height);

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
    bool EnsureDevice();
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
    // The floor the sprite pass stands sprites on, rebuilt with the world. It
    // reads map_ directly, so the two can never describe different ground.
    GroundSampler ground_;
    const void* loadedMapObject_ = nullptr;
    std::string dataDir_;

    float minX_ = 0.0f, minY_ = 0.0f, maxX_ = 0.0f, maxY_ = 0.0f;
    bool haveBounds_ = false;
    bool ready_ = false;
    int frameCount_ = 0;

    // Where the device is to be created, kept because creating it can take more
    // than one attempt; roughly ten seconds of frames before giving up.
    static constexpr int kMaxDeviceAttempts = 600;
    HWND window_ = nullptr;      // what we present into
    HWND gameWindow_ = nullptr;  // what the game owns, and its video device with it
    PresentWindow present_ = PresentWindow::Child;
    int width_ = 0;
    int height_ = 0;
    // 0 means "the desktop", resolved in Initialize.
    int requestedWidth_ = 0;
    int requestedHeight_ = 0;
    int deviceAttempts_ = 0;

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
