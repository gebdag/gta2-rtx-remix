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

// ---------------------------------------------------------------------------
// The one patch gta2.exe needs to reason about a wide frame, and the switch
// that takes it back out. The sites keep the bytes they had, so
// spawn_offscreen=0 - or the checkbox in the menu, mid-game - leaves the running
// process byte for byte as it shipped.
// ---------------------------------------------------------------------------

bool g_spawnOffscreen = kDefaultSpawnOffscreen;
bool g_spawnOffscreenActive = false;

// One patched run of bytes and what was there before it.
struct BytePatch {
    uintptr_t at = 0;
    size_t size = 0;
    uint8_t original[8] = {};
    bool applied = false;
};

bool WriteCode(uintptr_t at, const void* bytes, size_t size) {
    void* target = reinterpret_cast<void*>(at);
    DWORD previous = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    memcpy(target, bytes, size);
    VirtualProtect(target, size, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// Saves what is there, then writes. A site that does not hold what it is
// supposed to is left alone rather than half patched - that is the guard
// against a different build of gta2.exe.
bool ApplyPatch(BytePatch* patch, uintptr_t at, const void* expected, const void* bytes,
                size_t size, const char* what) {
    if (patch->applied) return true;
    if (size > sizeof(patch->original)) return false;
    if (memcmp(reinterpret_cast<const void*>(at), expected, size) != 0) {
        Log("%s: %08X does not hold what this expects - leaving gta2.exe alone", what,
            static_cast<unsigned>(at));
        return false;
    }
    memcpy(patch->original, reinterpret_cast<const void*>(at), size);
    if (!WriteCode(at, bytes, size)) {
        Log("%s: could not write %08X", what, static_cast<unsigned>(at));
        return false;
    }
    patch->at = at;
    patch->size = size;
    patch->applied = true;
    return true;
}

void RevertPatch(BytePatch* patch) {
    if (!patch->applied) return;
    WriteCode(patch->at, patch->original, patch->size);
    patch->applied = false;
}

// The two Fixed values FUN_0041E7A0 reads in place of its own 0.5 and 0.75 once
// its operands point here. See game_access.h kViewHalfScaleSite. They hold the
// game's own values until a frame says otherwise, so a patch applied before the
// first frame changes nothing.
int32_t g_viewHalfScale = game::kViewHalfScaleStock;
int32_t g_viewAspect = game::kViewAspectStock;
BytePatch g_viewHalfScalePatch;
BytePatch g_viewAspectPatch;

bool ApplyGamePatches() {
    struct Site {
        BytePatch* patch;
        uintptr_t at;
        uintptr_t expected;
        int32_t* replacement;
        const char* what;
    };
    const Site sites[2] = {
        {&g_viewHalfScalePatch, game::kViewHalfScaleSite, game::kViewHalfScale, &g_viewHalfScale,
         "view width"},
        {&g_viewAspectPatch, game::kViewAspectSite, game::kViewAspect, &g_viewAspect,
         "view aspect"},
    };
    bool ok = true;
    for (const Site& site : sites) {
        if (*reinterpret_cast<const uint8_t*>(site.at - 1) != 0x68) {
            Log("%s: %08X is not a PUSH", site.what, static_cast<unsigned>(site.at));
            ok = false;
            break;
        }
        ok &= ApplyPatch(site.patch, site.at, &site.expected, &site.replacement, sizeof(void*),
                         site.what);
    }
    // Width without height, or the other way round, is a wrong rectangle rather
    // than a stock one - so it is both or neither.
    if (!ok) {
        RevertPatch(&g_viewHalfScalePatch);
        RevertPatch(&g_viewAspectPatch);
    }
    Log("spawn offscreen: %s", ok ? "gta2.exe patched" : "not applied, see above");
    return ok;
}

void RevertGamePatches() {
    RevertPatch(&g_viewHalfScalePatch);
    RevertPatch(&g_viewAspectPatch);
    Log("spawn offscreen: gta2.exe put back the way it shipped");
}

constexpr float kDegreesToRadians = 3.14159265f / 180.0f;

// The desktop, in the physical pixels a window is really sized in.
//
// Three APIs answer "how big is the screen" and on a scaled desktop they
// disagree. Measured here on a 1920x1080 display at 125%:
//
//   EnumDisplaySettings(ENUM_CURRENT_SETTINGS)   1920x1080   the mode in force
//   EnumDisplaySettings(ENUM_REGISTRY_SETTINGS)  3840x2400   a mode from months ago
//   GetSystemMetrics / MONITORINFO               1536x864    the DPI-scaled desktop
//
// CreateWindowEx sizes land on screen as physical pixels, so the current display
// mode is the one to build a present window from. The other two each broke it in
// their own direction:
//
//   - the registry mode is the display's *persistent* mode, not the mode it is
//     in. It was preferred here, on the reasoning that ENUM_CURRENT_SETTINGS
//     reads back GTA2's own 640x480 once its video device has taken the display.
//     It does survive that - but it also survived the desktop being moved from
//     3840x2400 down to 1920x1080, so the present window was built at twice the
//     size of the screen and only its top-left quarter was ever visible. That is
//     what a front end "zoomed in" is.
//   - GetSystemMetrics reports the desktop as a DPI-unaware process may address
//     it, which is 4/5 of the real thing at 125%. Sizing the window from that
//     leaves a fifth of the screen showing the desktop behind it.
//
// The takeover case the old comment worried about is guarded rather than
// assumed: a mode too small to be a desktop is not believed, and the registry
// mode - the one the display will go back to - is used instead. As deployed it
// cannot arise anyway, because configure_game.py asserts dxwrapper's
// EnableWindowMode and GTA2 then never changes the mode at all.
void DesktopSize(int* width, int* height) {
    // Anything smaller than this is GTA2's own screen, not a desktop.
    const DWORD kMinDesktopWidth = 800;
    const DWORD kMinDesktopHeight = 600;

    // DWORD, not int: ENUM_CURRENT_SETTINGS and ENUM_REGISTRY_SETTINGS are
    // (DWORD)-1 and (DWORD)-2, which narrow.
    const DWORD modes[2] = {ENUM_CURRENT_SETTINGS, ENUM_REGISTRY_SETTINGS};
    for (DWORD which : modes) {
        DEVMODEA mode = {};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsA(nullptr, which, &mode) && mode.dmPelsWidth >= kMinDesktopWidth
            && mode.dmPelsHeight >= kMinDesktopHeight) {
            *width = static_cast<int>(mode.dmPelsWidth);
            *height = static_cast<int>(mode.dmPelsHeight);
            return;
        }
    }
    // Last, and the one that is wrong on a scaled desktop - but a window that is
    // too small still shows all of itself, which is the better failure.
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
    // The same measure the render size came from, so the two are comparable. On
    // a scaled desktop GetSystemMetrics answers a different question - see
    // DesktopSize - and mixing the two here put the window half off the screen.
    int screenW = 0, screenH = 0;
    DesktopSize(&screenW, &screenH);
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

void SetSpawnOffscreen(bool on) { g_spawnOffscreen = on; }
bool SpawnOffscreen() { return g_spawnOffscreen; }
bool SpawnOffscreenActive() { return g_spawnOffscreenActive; }

// Make GTA2's own view rectangle the shape of the frame we draw.
//
// Cars and pedestrians appeared and vanished inside a 16:9 frame's left and
// right margins because everything in gta2.exe that asks "is this on screen" -
// where traffic may be created, the ring pedestrians are placed on, which grid
// cells are drawn, what the recycler may destroy - measures against one
// rectangle, and FUN_0041E7A0 builds that rectangle 4:3. Earlier attempts moved
// the consumers one at a time (a detoured car test, a widened pedestrian ring, a
// padded grid walk, a padded rectangle), and each one that moved alone fell out
// of step with the rest: a spawn point pushed past what the recycler counts as
// seen is destroyed before it arrives, which is how the streets emptied.
//
// This moves the source instead, so every consumer stays consistent with every
// other exactly as it is in the stock game - only wider. Height is untouched:
// the renderer frames the game's own vertical extent, and the half-width scale
// and the aspect are changed by reciprocal amounts so their product, the
// half-height, stays what it was.
void WorldView::MatchGameViewToFrame() {
    // Follow the switch, in both directions and at any time. Applying reads and
    // keeps the original bytes, so turning it off mid-game is a real revert.
    if (g_spawnOffscreen != g_spawnOffscreenActive) {
        if (g_spawnOffscreen) {
            // A site that does not match is a different gta2.exe: give up for
            // good rather than ask again every frame.
            if (!ApplyGamePatches()) g_spawnOffscreen = false;
        } else {
            RevertGamePatches();
        }
        g_spawnOffscreenActive = g_spawnOffscreen;
    }

    // Never narrower than the game's own: a 4:3 or taller frame is already
    // covered by the stock rectangle.
    float widen = 1.0f;
    if (renderer_.Width() > 0 && renderer_.Height() > 0) {
        const float aspect =
            static_cast<float>(renderer_.Width()) / static_cast<float>(renderer_.Height());
        widen = (std::max)(1.0f, aspect / game::kViewAspectRatioStock);
    }
    g_viewHalfScale = static_cast<int32_t>(game::kViewHalfScaleStock * widen + 0.5f);
    g_viewAspect = static_cast<int32_t>(game::kViewAspectStock / widen + 0.5f);
}

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
    // The first frames the game draws are the front end, which is the same 2D
    // stream the menus use later. Traced once so a broken menu can be compared
    // against a working one.
    overlay_.TraceNextFrames(3);
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
    // Everything the sizing depends on, in one line: the three answers Windows
    // gives for "how big is the screen" disagree whenever DPI scaling is on, and
    // which one arrived is the first thing to know when the picture is the wrong
    // size.
    {
        typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
        static GetDpiForWindowFn getDpi = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
        DEVMODEA mode = {};
        mode.dmSize = sizeof(mode);
        EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode);
        // The client rect of our own window is what ImGui lays the F4 menu out
        // in and what the mouse is measured against, so a disagreement with the
        // back buffer is a misplaced cursor rather than a wrong-sized picture.
        Log("render size %dx%d (monitor %dx%d, system metrics %dx%d, display mode %ux%u, "
            "game window %u dpi, game screen %dx%d, ini asked for %dx%d)",
            width_, height_, desktopW, desktopH, GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN), mode.dmPelsWidth, mode.dmPelsHeight,
            getDpi && window ? getDpi(window) : 0, width, height, requestedWidth_,
            requestedHeight_);
    }
    if (desktopW <= 800 && requestedWidth_ <= 0) {
        Log("WARNING: even the registry display mode reads %dx%d. Set render_width and "
            "render_height in gta2dx9.ini explicitly.", desktopW, desktopH);
    }
    width = width_;
    height = height_;
    if (present_ != PresentWindow::GameWindow) {
        window_ = CreatePresentWindow(window, width, height, present_);
        RECT client = {0, 0, 0, 0};
        if (window_) GetClientRect(window_, &client);
        Log("presenting into our own %s window %p (client %ldx%ld) over the game's %p",
            present_ == PresentWindow::Child ? "child" : "topmost", window_,
            client.right - client.left, client.bottom - client.top, window);
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
        // Back at the menu. The game has freed its map, so the level's geometry
        // has to go with it - otherwise the last frame of the city stays in the
        // scene, drawn behind the menu and still path traced by Remix, and the
        // menu never appears to come back.
        if (worldUploaded_) {
            Log("level ended: releasing the world mesh");
            renderer_.ReleaseWorld();
            worldUploaded_ = false;
            // The sprite pass must not conform to a map that is no longer there,
            // and the next level's camera should not ease down from this one's
            // rooftops.
            ground_.Clear();
            tileSources_.clear();
            heightSettled_ = false;
            // Back to the menu's own scale immediately, rather than keeping the
            // level's until the game next calls gbh_SetWindow.
            overlay_.RevertToWindowSize();
            // Back at the menu is exactly where the 2D stream wants looking at.
            overlay_.TraceNextFrames(3);
        }
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
    Log("world loaded: %s, %zu blocks, %zu triangles, %zu batches, floor %s", stylePath.c_str(),
        map_.BlockCount(), mesh.indices.size() / 3, mesh.batches.size(),
        gta2::WorldSeal() ? "sealed" : "OPEN (the sky shows through any gap)");
    loadedMapObject_ = mapObject;
    worldUploaded_ = true;
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
    // Only while a world is loaded. The camera struct pointer outlives the level
    // it described, so at the menu this reads a freed allocation - which laid the
    // front end out to the wrong scale, and made it jitter as the allocator
    // handed that memory to something else. gbh_SetWindow is the authority there,
    // which is what its own comment in overlay.cpp has always said.
    if (worldUploaded_) {
        if (const uint8_t* camera = game::CameraStruct()) {
            const int32_t screenW =
                *reinterpret_cast<const int32_t*>(camera + game::kScreenWidthOffset);
            const int32_t screenH =
                *reinterpret_cast<const int32_t*>(camera + game::kScreenHeightOffset);
            if (screenW >= 320 && screenW <= 8192 && screenH >= 200 && screenH <= 8192) {
                overlay_.SetGameScreenSize(screenW, screenH);
            }
        }
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
        // The 2D stream goes into a layer that is kept between frames, because
        // GTA2's menu only redraws what changed and clears the screen just when
        // it wants a repaint. In a level that reasoning does not apply - the HUD
        // is redrawn every frame and the world under it moves - so the layer is
        // cleared every frame there, which is what it always did.
        if (renderer_.BeginUiLayer(worldUploaded_)) {
            overlay_.Flush(renderer_.Device(), renderer_.Width(), renderer_.Height());
            renderer_.EndUiLayer();
        } else {
            overlay_.Flush(renderer_.Device(), renderer_.Width(), renderer_.Height());
        }
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
    // The game's own view rectangle against the frame we draw. With the view
    // patch in, the two widths agree; if cars ever pop in the margins again
    // this line says at once whether gta2.exe is still reasoning 4:3.
    if ((frameCount_ & 0xFF) == 0) {
        game::ViewExtent view;
        if (game::VisibleExtent(&view) && renderer_.Height() > 0) {
            const float aspect =
                static_cast<float>(renderer_.Width()) / static_cast<float>(renderer_.Height());
            Log("view @%d: game %.2f x %.2f tiles | frame %.2f x %.2f | %s", frameCount_,
                view.Width(), view.Height(), visibleTiles_ * aspect, visibleTiles_,
                SpawnOffscreenActive() ? "gta2.exe patched" : "gta2.exe as shipped");
        }
    }

    if ((frameCount_ & 0x3F) == 0) DumpCameraStruct();

    // Last thing in the frame, after the flip: GTA2's own pacer is a checkbox
    // between 30 fps and none at all, so the number lives here instead. See
    // frame_limiter.h for what a cap above 30 does to the game's speed.
    FrameLimitWait();
}

}  // namespace gta2dx9
