#include "world_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/gta2_style.h"
#include "../../src/world_mesh.h"
#include "debug_overlay.h"
#include "frame_limiter.h"
#include "game_access.h"
#include "log.h"
#include "remix_api.h"
#include "remix_lights.h"
#include "synthetic_lights.h"
#include "texture_store.h"
#include "time_of_day.h"

namespace gta2dx9 {
namespace {

constexpr uint32_t kMaxColumnWords = 1 << 20;
constexpr uint32_t kMaxBlocks = 1 << 18;
constexpr size_t kMatchBlocks = 4000;

// Per-frame easing toward the street height under the camera.
constexpr float kHeightFollowRate = 0.06f;
constexpr float kDegreesToRadians = 3.14159265f / 180.0f;

// The desktop in real pixels.
//
// Not GetSystemMetrics: this DLL is DPI-unaware, so SM_CXSCREEN reports the
// scaled desktop - 1536x960 for a 3840x2400 screen at 250% - and rendering at
// that would throw away more than half the resolution. EnumDisplaySettings
// returns physical pixels whatever the process's DPI awareness.
//
// It also reports whatever mode is current, so if GTA2 has already taken the
// display into an exclusive fullscreen 640x480 this reads 640x480. That is why
// deploy forces start_mode=0: windowed, the game leaves the display alone.
void DesktopSize(int* width, int* height) {
    // ENUM_REGISTRY_SETTINGS, not ENUM_CURRENT_SETTINGS: GTA2's video device puts
    // the display into its own mode during startup, before the renderer is even
    // loaded, so "current" reads back 640x480 and there is no way to ask what the
    // desktop was. The registry mode is the desktop's persistent one and survives
    // that.
    DEVMODEA mode = {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsA(nullptr, ENUM_REGISTRY_SETTINGS, &mode) && mode.dmPelsWidth >= 640) {
        *width = static_cast<int>(mode.dmPelsWidth);
        *height = static_cast<int>(mode.dmPelsHeight);
        return;
    }
    mode = DEVMODEA{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth) {
        *width = static_cast<int>(mode.dmPelsWidth);
        *height = static_cast<int>(mode.dmPelsHeight);
        return;
    }
    // GetSystemMetrics last: this DLL is DPI-unaware, so it reports the *scaled*
    // desktop - 1536x960 for a 3840x2400 screen at 250% - and rendering at that
    // throws away more than half the resolution.
    *width = GetSystemMetrics(SM_CXSCREEN);
    *height = GetSystemMetrics(SM_CYSCREEN);
}

std::string GameDirectory() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
    return std::string(path) + "\\data";
}

// Reads the game's DMAP arrays straight out of its map object. The in-memory
// layout is identical to the file, so the same parser handles both.
bool ReadLiveMap(const uint8_t* mapObject, gta2::Map* map, std::string* error) {
    const uint8_t* base = mapObject;
    const uint8_t* columns = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectColumnsPtr);
    const uint8_t* blocks = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectBlocksPtr);
    if (!columns || !blocks) {
        *error = "map arrays are not populated";
        return false;
    }

    // Neither array stores its length, so the extents come from the indices the
    // base array actually references.
    uint32_t maxWord = 0;
    for (size_t i = 0; i < gta2::kDmapBaseBytes; i += 4) {
        uint32_t word;
        memcpy(&word, base + i, 4);
        if (word > maxWord && word < kMaxColumnWords) maxWord = word;
    }
    const size_t columnBytes = (static_cast<size_t>(maxWord) + 1 + gta2::kMapLevels) * 4;

    uint32_t maxBlock = 0;
    for (size_t i = 0; i < gta2::kDmapBaseBytes; i += 4) {
        uint32_t word;
        memcpy(&word, base + i, 4);
        const size_t header = static_cast<size_t>(word) * 4;
        if (header + 4 > columnBytes) continue;
        const int count = columns[header] - columns[header + 1];
        for (int b = 0; b < count; ++b) {
            uint32_t index;
            memcpy(&index, columns + header + 4 + static_cast<size_t>(b) * 4, 4);
            if (index > maxBlock && index < kMaxBlocks) maxBlock = index;
        }
    }

    return map->LoadFromDmap(base, columns, columnBytes, blocks,
                             (static_cast<size_t>(maxBlock) + 1) * 12, error);
}

std::vector<uint8_t> FileBlockArray(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? static_cast<size_t>(size) : 0);
    const bool ok = !data.empty() && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok || data.size() < 6 || memcmp(data.data(), "GBMP", 4) != 0) return {};

    size_t offset = 6;
    while (offset + 8 <= data.size()) {
        uint32_t chunk;
        memcpy(&chunk, &data[offset + 4], 4);
        const size_t body = offset + 8;
        if (memcmp(&data[offset], "DMAP", 4) == 0) {
            const size_t pool = body + gta2::kDmapBaseBytes;
            if (pool + 4 > data.size()) return {};
            uint32_t words;
            memcpy(&words, &data[pool], 4);
            const size_t countAt = pool + 4 + static_cast<size_t>(words) * 4;
            if (countAt + 4 > data.size()) return {};
            uint32_t count;
            memcpy(&count, &data[countAt], 4);
            const size_t start = countAt + 4;
            const size_t bytes = (std::min)(static_cast<size_t>(count) * 12, data.size() - start);
            return std::vector<uint8_t>(data.begin() + start, data.begin() + start + bytes);
        }
        offset = body + chunk;
    }
    return {};
}

// The game does not keep the style filename anywhere obvious, so the district is
// identified by matching its live blocks against the .gmp files on disk; the
// style shares the map's basename.
std::string DetectStylePath(const uint8_t* mapObject, const std::string& dataDir) {
    const uint8_t* blocks = *reinterpret_cast<const uint8_t* const*>(mapObject + gta2::kMapObjectBlocksPtr);
    if (!blocks) return {};

    WIN32_FIND_DATAA find = {};
    HANDLE handle = FindFirstFileA((dataDir + "\\*.gmp").c_str(), &find);
    if (handle == INVALID_HANDLE_VALUE) return {};

    std::string bestName;
    double bestScore = 0.0;
    do {
        const std::vector<uint8_t> fileBlocks = FileBlockArray(dataDir + "\\" + find.cFileName);
        const size_t common = (std::min)(kMatchBlocks * 12, fileBlocks.size());
        if (common < 12) continue;

        size_t same = 0;
        for (size_t i = 0; i + 12 <= common; i += 12) {
            if (memcmp(blocks + i, &fileBlocks[i], 12) == 0) ++same;
        }
        const double score = static_cast<double>(same) / (common / 12);
        if (score > bestScore) {
            bestScore = score;
            bestName = find.cFileName;
        }
    } while (FindNextFileA(handle, &find));
    FindClose(handle);

    if (bestScore < 0.9 || bestName.empty()) return {};
    std::string stem = bestName.substr(0, bestName.find_last_of('.'));
    const size_t dash = stem.find('-');
    if (dash != std::string::npos) stem = stem.substr(0, dash);
    return dataDir + "\\" + stem + ".sty";
}

// A window of our own to present into.
//
// GTA2 sets DirectDraw up on its own window during startup, and on Windows 10
// and 11 DirectDraw is itself implemented on Direct3D 9: it takes the window
// into an exclusive fullscreen device before the renderer is even loaded. A
// second swap chain on that same window is tolerated by Microsoft's D3D9 but
// not by RTX Remix, whose Vulkan presentation never completes - the game hangs
// on its first Present with the Remix runtime spinning. Presenting into a
// window we own leaves DirectDraw in sole possession of the game's.
//
// It has to be a *child* of the game's window rather than a popup over it. A
// popup loses the z-order the moment the game is activated, and DirectDraw's
// empty primary surface is then what you see: black while the game has focus,
// and the path traced picture only when you alt-tab away and the game's window
// drops back down. A child window is always painted above its parent's client
// area, so there is no ordering to lose, and it can never take the focus or the
// input away from the game either.
HWND CreatePresentWindow(HWND gameWindow, int width, int height, PresentWindow mode) {
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleA(nullptr);
    if (!registered) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = "gta2dx9_present";
        if (!RegisterClassA(&wc)) return nullptr;
        registered = true;
    }
    if (!gameWindow) return nullptr;

    // Without this the game's window is free to paint its own background over
    // the child.
    SetWindowLongA(gameWindow, GWL_STYLE, GetWindowLongA(gameWindow, GWL_STYLE) | WS_CLIPCHILDREN);

    if (mode == PresentWindow::Child) {
        // Child coordinates are relative to the parent's client area, which is
        // the rectangle gbh_Init measured.
        HWND window = CreateWindowExA(WS_EX_NOACTIVATE, "gta2dx9_present", "GTA2",
                                      WS_CHILD | WS_VISIBLE, 0, 0, width, height, gameWindow,
                                      nullptr, instance, nullptr);
        if (window) {
            SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return window;
    }

    // Topmost: a separate top-level window in the topmost band, which sits above
    // the game's window even while the game is the activated one. A child cannot
    // help against a video device that presents past the window manager
    // altogether; this at least competes in the right band.
    // Positioned over the game's window but sized to the render resolution, not
    // to it: the game's window is whatever GTA2's options screen last wrote, and
    // 640x480 of path traced image is not the goal. Centred on the desktop when
    // the render size is larger than the game's window, so a 4K image over a
    // 640x480 game window still lands somewhere sensible.
    RECT gameRect = {0, 0, width, height};
    GetWindowRect(gameWindow, &gameRect);
    int left = gameRect.left;
    int top = gameRect.top;
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    if (width >= screenW && height >= screenH) {
        left = 0;
        top = 0;
    } else {
        if (left + width > screenW) left = (screenW - width) / 2;
        if (top + height > screenH) top = (screenH - height) / 2;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
    }
    HWND window = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  "gta2dx9_present", "GTA2", WS_POPUP, left, top, width, height,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return nullptr;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return window;
}

}  // namespace

void WorldView::Configure(float pitchDegrees, float fovDegrees, bool useGameTiles,
                          PresentWindow present) {
    camera_.SetOrientation(0.0f, pitchDegrees * kDegreesToRadians);
    camera_.SetFovY(fovDegrees * kDegreesToRadians);
    useGameTiles_ = useGameTiles;
    present_ = present;
    Log("camera pitch %.1f deg, fov %.1f deg; tile artwork from %s", pitchDegrees, fovDegrees,
        useGameTiles ? "the game" : "the style file");
}

void WorldView::SetRenderSize(int width, int height) {
    requestedWidth_ = width;
    requestedHeight_ = height;
}

bool WorldView::Initialize(HWND window, int width, int height, std::string* error) {
    dataDir_ = GameDirectory();
    // Both live here, so the pointer is good for the process. The sampler stays
    // empty until a map is loaded, and the sprite pass falls back until it is.
    live_.SetGround(&ground_);
    gameWindow_ = window;
    window_ = window;

    // What the game thinks its screen is only sets out the HUD; the overlay pass
    // scales that to whatever it is drawn into. So the render size is ours to
    // choose, and defaults to the desktop rather than to GTA2's 640x480.
    int desktopW = 0, desktopH = 0;
    DesktopSize(&desktopW, &desktopH);
    width_ = requestedWidth_ > 0 ? requestedWidth_ : desktopW;
    height_ = requestedHeight_ > 0 ? requestedHeight_ : desktopH;
    if (width_ < 320 || height_ < 240) {
        width_ = width;
        height_ = height;
    }
    Log("render size %dx%d (desktop %dx%d, game screen %dx%d, ini asked for %dx%d)", width_,
        height_, desktopW, desktopH, width, height, requestedWidth_, requestedHeight_);
    if (desktopW <= 800 && requestedWidth_ <= 0) {
        Log("WARNING: even the registry display mode reads %dx%d. Set render_width and "
            "render_height in gta2dx9.ini explicitly.", desktopW, desktopH);
    }
    width = width_;
    height = height_;
    if (present_ != PresentWindow::GameWindow) {
        window_ = CreatePresentWindow(window, width, height, present_);
        Log("presenting into our own %s window %p over the game's %p",
            present_ == PresentWindow::Child ? "child" : "topmost", window_, window);
        if (!window_) window_ = window;
    }
    gta2::SetRendererTrace([](const char* line) { Log("d3d: %s", line); });
    if (EnsureDevice()) return true;
    *error = "device not up yet, retrying from the frame loop";
    return false;
}

// The device does not necessarily come up on the first ask. Under RTX Remix the
// runtime backing D3D9 is a separate 64-bit process that is still starting when
// the game asks its renderer to initialise, and it only finishes once the game
// has run its message loop for a moment - which first happens between frames.
// Retrying from the frame loop is what gives it that moment; failing outright
// here is what used to leave the game sitting at a black menu.
bool WorldView::EnsureDevice() {
    if (ready_) return true;
    if (!window_ || deviceAttempts_ >= kMaxDeviceAttempts) return false;

    ++deviceAttempts_;
    std::string error;
    if (!renderer_.Initialize(window_, width_, height_, &error)) {
        // One line per attempt would be hundreds of them, so only the first and
        // the last are worth keeping.
        if (deviceAttempts_ == 1 || deviceAttempts_ == kMaxDeviceAttempts) {
            Log("device not up on attempt %d: %s", deviceAttempts_, error.c_str());
        }
        return false;
    }
    ready_ = true;
    Log("world view up on hwnd %p at %dx%d after %d attempt(s)", window_, width_, height_,
        deviceAttempts_);
    return true;
}

void WorldView::Shutdown() {
    if (!ready_ && !loadedMapObject_) return;
    ready_ = false;
    loadedMapObject_ = nullptr;
    const HWND ours = window_ != gameWindow_ ? window_ : nullptr;
    window_ = nullptr;  // stops the frame loop bringing the device back up
    // ImGui and the Remix lights both hold device-side or bridge-side objects, so
    // they go first: the menu's font texture belongs to the device we are about
    // to release, and a light handle outliving the bridge is a leak on the far
    // side of it.
    DebugMenuShutdown();
    LightsShutdown();
    RemixApiShutdown();
    // Device-owned textures have to go before the device does.
    overlay_.ReleaseResources();
    live_.ReleaseResources();
    // Before the frames go: the report is the only record of what this session
    // actually built and drew.
    WriteTextureReport();
    ReleaseDeviceTextures();
    renderer_.Shutdown();
    if (ours) DestroyWindow(ours);
    Log("world view shut down, device released");
}

void WorldView::SetVisibleTileBounds(float minX, float minY, float maxX, float maxY) {
    minX_ = minX;
    minY_ = minY;
    maxX_ = maxX;
    maxY_ = maxY;
    haveBounds_ = maxX > minX && maxY > minY;
}

bool WorldView::EnsureWorldLoaded() {
    const uint8_t* mapObject = game::MapObject();
    if (!mapObject) {
        loadedMapObject_ = nullptr;
        return false;
    }
    if (mapObject == loadedMapObject_) return true;

    std::string error;
    // The old ground describes the old map. Cleared here rather than after the
    // rebuild, so a level that fails to load leaves sprites on the fixed lift
    // instead of on a floor from the district before it.
    ground_.Clear();
    if (!ReadLiveMap(mapObject, &map_, &error)) {
        Log("live map read failed: %s", error.c_str());
        loadedMapObject_ = mapObject;  // Do not retry every frame on a bad map.
        return false;
    }

    const std::string stylePath = DetectStylePath(mapObject, dataDir_);
    gta2::Style style;
    if (stylePath.empty()) {
        Log("could not identify the district for map object %p", mapObject);
        loadedMapObject_ = mapObject;
        return false;
    }
    if (!style.Load(stylePath, &error)) {
        Log("style load failed (%s): %s", stylePath.c_str(), error.c_str());
        loadedMapObject_ = mapObject;
        return false;
    }

    // The district's name salts the per-light override keys, so a lamp standing
    // in the same spot in two districts does not share one entry.
    {
        const size_t slash = stylePath.find_last_of('\\');
        std::string district = slash == std::string::npos ? stylePath : stylePath.substr(slash + 1);
        const size_t dot = district.find_last_of('.');
        if (dot != std::string::npos) district = district.substr(0, dot);
        LightsSetScene(district.c_str());
    }

    // Prefer the game's own artwork over our parse of the style file: the game
    // resolves tile numbers through a remap table, so indexing the file directly
    // yields the right set of textures in the wrong order.
    int overridden = 0;
    tileSources_.assign(style.TileCount(), TileSource{});
    if (useGameTiles_) {
        std::vector<uint32_t> pixels(gta2::kTilePixels);
        for (int tile = 1; tile < style.TileCount(); ++tile) {
            if (ResolveTileImage(tile, pixels.data())) {
                style.OverrideTile(tile, pixels.data());
                const void* record = game::TextureForTile(tile);
                tileSources_[tile].record = record;
                tileSources_[tile].revision =
                    record ? static_cast<const TextureRecord*>(record)->revision : 0;
                ++overridden;
            }
        }
    }
    Log("tile artwork from the game: %d/%d (palettes captured: %d)", overridden,
        style.TileCount(), PaletteCount());

    // Ramp shapes come from the game's own slope table rather than a derived
    // formula: it is the only statement of which way each type climbs and over
    // how many blocks, and it exists only in the running process.
    std::array<gta2::SlopeInfo, gta2::kSlopeTypeCount> slopes = {};
    {
        uint8_t directions[gta2::kSlopeTypeCount] = {};
        uint8_t steps[gta2::kSlopeTypeCount] = {};
        uint8_t stepIndices[gta2::kSlopeTypeCount] = {};
        game::ReadSlopeTable(directions, steps, stepIndices, gta2::kSlopeTypeCount);
        int ramps = 0;
        for (int type = 0; type < gta2::kSlopeTypeCount; ++type) {
            slopes[type].direction = directions[type];
            slopes[type].steps = steps[type];
            slopes[type].step = stepIndices[type];
            if (directions[type] >= 1 && directions[type] <= 4 && steps[type]) ++ramps;
        }
        Log("slope table: %d ramp types read from the game", ramps);
    }

    // The sprite pass stands sprites on the lid this same table shapes. Resolved
    // once, here, and handed to both consumers: two readings of a ramp would be
    // two different floors, and a sprite conformed to the wrong one is exactly
    // the bug conforming exists to remove.
    std::array<gta2::SlopeInfo, gta2::kSlopeTypeCount> resolvedSlopes = {};
    gta2::ResolveSlopeTable(slopes.data(), resolvedSlopes.data());
    ground_.Reset(&map_, resolvedSlopes.data());

    // Where a partial or corner block is cut, likewise straight from the game
    // rather than assumed. The two outer constants are a sanity check: they must
    // read as an empty and a whole cell, or the addresses are wrong.
    gta2::PartialCuts cuts;
    {
        const float zero = game::CellFraction(game::kCellZeroPtr);
        const float one = game::CellFraction(game::kCellOnePtr);
        const float low = game::CellFraction(game::kCellLowPtr);
        const float high = game::CellFraction(game::kCellHighPtr);
        const bool sane = std::abs(zero) < 0.01f && std::abs(one - 1.0f) < 0.01f && low > 0.0f &&
                          low < high && high < 1.0f;
        if (sane) {
            cuts.low = low;
            cuts.high = high;
        }
        Log("partial block cuts: %.4f / %.4f (bounds %.4f..%.4f) %s", low, high, zero, one,
            sane ? "accepted" : "REJECTED, using defaults");
    }

    gta2::WorldMesh mesh;
    gta2::BuildWorldMesh(map_, style, slopes.data(), cuts, &mesh);
    if (!renderer_.UploadWorld(mesh, style, &error)) {
        Log("world upload failed: %s", error.c_str());
        loadedMapObject_ = mapObject;
        return false;
    }

    if (game::ViewRotation() != 0xFF) {
        Log("WARNING: view rotation is %u, not the 0xFF the tile orientation assumes",
            game::ViewRotation());
    }
    Log("world loaded: %s, %zu blocks, %zu triangles, %zu batches", stylePath.c_str(),
        map_.BlockCount(), mesh.indices.size() / 3, mesh.batches.size());
    loadedMapObject_ = mapObject;
    return true;
}

// GTA2 animates ceiling fans, screens and water by changing the artwork behind
// a tile number, not by moving anything: either the style's remap sends the tile
// at a different texture record, or the record's own pixels are rewritten
// between a lock and an unlock. Both show up here, so the static world mesh
// keeps its geometry and only the affected textures are re-uploaded.
void WorldView::RefreshAnimatedTiles() {
    if (!useGameTiles_ || tileSources_.empty()) return;

    std::vector<uint32_t> pixels;
    int updated = 0;
    for (int tile = 1; tile < static_cast<int>(tileSources_.size()); ++tile) {
        if (!renderer_.HasTileTexture(tile)) continue;  // not used by any face
        const void* record = game::TextureForTile(tile);
        if (!record) continue;
        const uint16_t revision = static_cast<const TextureRecord*>(record)->revision;
        TileSource& source = tileSources_[tile];
        if (record == source.record && revision == source.revision) continue;

        if (pixels.empty()) pixels.resize(gta2::kTilePixels);
        if (!ResolveTileImage(tile, pixels.data())) continue;
        renderer_.UpdateTileTexture(tile, pixels.data());
        source.record = record;
        source.revision = revision;
        // A handful of tiles animate; a cap keeps a pathological frame from
        // stalling on hundreds of texture uploads.
        if (++updated >= 64) break;
    }
}

// Street level under a point. Deliberately the *bottom* of the column rather
// than the top: the topmost block is a building roof, so following it makes the
// camera leap whenever it crosses a building.
float WorldView::GroundHeightAt(float tileX, float tileY) const {
    const int x = (std::clamp)(static_cast<int>(tileX), 0, gta2::kMapWidth - 1);
    const int y = (std::clamp)(static_cast<int>(tileY), 0, gta2::kMapHeight - 1);
    const gta2::Column& column = map_.ColumnAt(x, y);
    return column.blocks.empty() ? 0.0f : static_cast<float>(column.offset + 1);
}

void WorldView::UpdateCamera() {
    float tileX = 0.0f, tileY = 0.0f;
    if (!game::CameraPosition(&tileX, &tileY)) return;

    // Ground height is a per-tile step function, so following it directly makes
    // the camera bob every time it crosses a tile edge. Ease toward it instead.
    const float desired = GroundHeightAt(tileX, tileY);
    if (!heightSettled_) {
        smoothedHeight_ = desired;
        heightSettled_ = true;
    } else {
        smoothedHeight_ += (desired - smoothedHeight_) * kHeightFollowRate;
    }

    // Map rows run north to south, so the row is mirrored exactly the way the
    // mesh builder mirrors it.
    const gta2::Vec3 target{tileX, smoothedHeight_,
                            static_cast<float>(gta2::kMapHeight) - tileY};

    // Zoom comes from the camera struct's continuous visible extent. The tile
    // rectangle handed to gbh_SetCamera is the same window rounded out to whole
    // tiles, so driving zoom from it makes the view step as the camera crosses
    // tile boundaries even when the game is not zooming at all.
    game::ViewExtent extent;
    if (game::VisibleExtent(&extent)) {
        visibleTiles_ = extent.Height();
    }
    if (visibleTiles_ <= 0.01f) return;

    const float distance = (visibleTiles_ * 0.5f) / std::tan(camera_.FovY() * 0.5f);
    camera_.FrameTarget(target, distance);
}

// The game's zoom has to come from the camera struct, not from the visible tile
// rectangle: that rectangle is a whole number of tiles, so using it as the zoom
// source can only ever change in steps. This dumps the struct so the continuous
// field driving zoom can be identified from real gameplay rather than guessed.
void WorldView::DumpCameraStruct() const {
    const uint8_t* camera = game::CameraStruct();
    if (!camera) return;

    char line[512];
    int used = snprintf(line, sizeof(line), "cam[%d] bounds=%.1f,%.1f..%.1f,%.1f |", frameCount_,
                        minX_, minY_, maxX_, maxY_);
    for (uintptr_t offset = 0x60; offset <= 0xC0 && used > 0 && used < (int)sizeof(line) - 32;
         offset += 4) {
        const int32_t raw = *reinterpret_cast<const int32_t*>(camera + offset);
        const float asFloat = *reinterpret_cast<const float*>(camera + offset);
        // Report both readings; the struct mixes 16.14 fixed point with floats.
        used += snprintf(line + used, sizeof(line) - used, " %02X:%.3f/%.3f", (unsigned)offset,
                         raw * game::kFixedScale, asFloat);
    }
    Log("%s", line);
}

void WorldView::CheckDrawnTile(const void* textureRecord) {
    if (map_.Empty() || tileChecks_ >= 24) return;

    float cellX = 0.0f, cellY = 0.0f;
    int level = 0;
    if (!game::CurrentCell(&cellX, &cellY, &level)) return;

    const int x = static_cast<int>(cellX + 0.5f);
    const int y = static_cast<int>(cellY + 0.5f);
    if (x < 0 || x >= gta2::kMapWidth || y < 0 || y >= gta2::kMapHeight) return;

    const int drawn = game::TileForTexture(textureRecord);
    if (drawn <= 0) return;

    // What our own walk of the map has at that cell and level.
    const gta2::Column& column = map_.ColumnAt(x, y);
    const int slot = level - column.offset;
    int ours[5] = {-1, -1, -1, -1, -1};
    if (slot >= 0 && slot < static_cast<int>(column.blocks.size())) {
        const gta2::Block& block = map_.BlockAt(column.blocks[slot]);
        ours[0] = block.left.Tile();
        ours[1] = block.right.Tile();
        ours[2] = block.top.Tile();
        ours[3] = block.bottom.Tile();
        ours[4] = block.lid.Tile();
    }

    const bool match = drawn == ours[0] || drawn == ours[1] || drawn == ours[2] ||
                       drawn == ours[3] || drawn == ours[4];
    Log("tilecheck cell=(%d,%d) lvl=%d game=%d ours[l/r/t/b/lid]=%d/%d/%d/%d/%d %s", x, y, level,
        drawn, ours[0], ours[1], ours[2], ours[3], ours[4], match ? "MATCH" : "MISMATCH");
    ++tileChecks_;
}

// The game writes each projected vertex's absolute world position four slots
// further along the vertex array, so the shape it actually draws for a block can
// be read straight off. Positions are in the game's own frame: x east, y south,
// z the level.
void WorldView::CheckDrawnShape(int corners) {
    const int slopeType = game::CurrentBlockSlopeType();
    if (slopeType < 45 || slopeType > 61) return;
    if (shapeChecks_ >= 40) return;

    float cellX = 0.0f, cellY = 0.0f;
    int level = 0;
    if (!game::CurrentCell(&cellX, &cellY, &level)) return;

    char line[320];
    int used = snprintf(line, sizeof(line), "shape type=%d cell=(%.0f,%.0f) lvl=%d %s", slopeType,
                        cellX, cellY, level, corners == 3 ? "tri" : "quad");
    for (int i = 0; i < corners && used > 0 && used < static_cast<int>(sizeof(line)) - 40; ++i) {
        const float* world = reinterpret_cast<const float*>(
            game::kTileVertexArray +
            static_cast<uintptr_t>(i + game::kWorldShadowSlots) * game::kVertexStride);
        used += snprintf(line + used, sizeof(line) - used, "  (%.3f %.3f %.3f)", world[0], world[1],
                         world[2]);
    }
    Log("%s", line);
    ++shapeChecks_;
}

void WorldView::RenderFrame() {
    if (!EnsureDevice()) return;

    if (window_ != gameWindow_) {
        // The game pumps its own window, not ours, and an unpumped window is a
        // hung one as far as Windows is concerned.
        MSG message;
        while (PeekMessageA(&message, window_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        // The game is free to reorder or resize its own children; re-stating
        // where ours belongs occasionally is cheaper than trusting it not to.
        if ((frameCount_ & 0x3F) == 0) {
            SetWindowPos(window_, present_ == PresentWindow::Topmost ? HWND_TOPMOST : HWND_TOP, 0, 0,
                         width_, height_,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW |
                             (present_ == PresentWindow::Topmost ? SWP_NOMOVE : 0));
        }
    }

    EnsureWorldLoaded();
    RefreshAnimatedTiles();
    UpdateCamera();

    // The game's own screen size is what its HUD coordinates are expressed in,
    // and it is not our back buffer size. Prefer the camera struct, which holds
    // it as plain pixel integers; gbh_SetWindow covers the menus, where no
    // camera exists yet.
    if (const uint8_t* camera = game::CameraStruct()) {
        overlay_.SetGameScreenSize(*reinterpret_cast<const int32_t*>(camera + game::kScreenWidthOffset),
                                   *reinterpret_cast<const int32_t*>(camera + game::kScreenHeightOffset));
    }

    // The bridge server is a separate process that is still starting while the
    // game asks its renderer to initialise, so this is retried from the frame
    // loop until it takes, exactly like the device is.
    RemixApiInit();
    // Not conditional on that: when Remix is absent the menu is the thing that
    // says so, which it cannot do if it only exists once Remix is there.
    DebugMenuInit(gameWindow_, window_, renderer_.Device());
    DebugMenuPoll();

    // The sky. Runs whether or not Remix answered - the clock is ours and the
    // menu shows it either way - and pushes the sun when it can.
    TimeOfDayUpdate();

    // Lights for the effects the game draws but never lights, read straight off
    // its particle and vehicle lists. Before the reconcile, so they are matched
    // and handled in the same pass as the game's own.
    SyntheticLightsUpdate();

    // The game lists this frame's lights during its own world pass, which runs
    // between gbh_BeginScene and the gbh_EndScene that brought us here - so by
    // now the list is complete and can be matched against what Remix already has.
    LightsReconcile();

    if (renderer_.BeginFrame()) {
        renderer_.DrawWorld(camera_);
        // Sprites and the captured block shapes are real world geometry, so they
        // are depth tested against the static mesh rather than painted over it.
        live_.Draw(renderer_.Device(), camera_, renderer_.Width(), renderer_.Height());
        overlay_.Flush(renderer_.Device(), renderer_.Width(), renderer_.Height());
        // ImGui issues ordinary D3D9 draws, so it belongs inside the scene.
        DebugMenuRender();
        // Remix clears its per-frame light list every frame, so a light is in the
        // scene exactly when it is drawn here. Last thing before the flip.
        LightsDraw();
        renderer_.EndFrame();
    }

    ++frameCount_;
    if ((frameCount_ & 0xFF) == 0) {
        // What conforming actually did, which is also the answer to the one
        // question the design could not settle by reading the game: whether GTA2
        // gives a sprite the true height of the ramp under it or the top of the
        // block. A street full of traffic with maxgap near zero says it gives the
        // true height and the conform is doing its job; a persistent gap the size
        // of a ramp step says it does not, and the tilt fade is quietly switching
        // itself off on every slope in the city.
        const LiveGeometry::Conform& c = live_.ConformCounts();
        Log("frame %d: %d sprite quads | conform grounded=%d airborne=%d noground=%d capped=%d "
            "straddled=%d maxgap=%.3f maxresidual=%.3f",
            frameCount_, live_.SpriteQuads(), c.grounded, c.airborne, c.noGround, c.capped,
            c.straddled, c.maxGap, c.maxResidual);
    }
    // Anything here is a sprite the player should have seen and did not, or a
    // texture built from artwork that was not ready.
    if ((frameCount_ & 0x3F) == 0) {
        const LiveGeometry::Drops& drops = live_.DropCounts();
        // accepted != drawn is the flicker itself: a sprite the game asked for,
        // that we took, that never reached the device.
        if (!drops.Quiet() || !g_trouble.Quiet() || drops.accepted != drops.drawn) {
            Log("sprite trouble @%d: accepted=%d drawn=%d | dropped expand=%d wrongarray=%d "
                "nonfinite=%d outside=%d degenerate=%d notexture=%d | textures built=%d "
                "nopalette=%d blackpalette=%d pixelsmoved=%d whilelocked=%d lockfail=%d",
                frameCount_, drops.accepted, drops.drawn, drops.expandFlag, drops.notTheSpriteArray,
                drops.notFinite, drops.outOfWorld, drops.degenerate, drops.noTexture,
                g_trouble.built, g_trouble.paletteMissing, g_trouble.paletteAllBlack,
                g_trouble.pixelsMovedSilently, g_trouble.builtWhileLocked, g_trouble.lockFailed);
            live_.ClearDropCounts();
            g_trouble = TextureTrouble{};
        }
    }
    if ((frameCount_ & 0x3F) == 0) DumpCameraStruct();

    // Last thing in the frame, after the flip: GTA2's own pacer is a checkbox
    // between 30 fps and none at all, so the number lives here instead. See
    // frame_limiter.h for what a cap above 30 does to the game's speed.
    FrameLimitWait();
}

}  // namespace gta2dx9
