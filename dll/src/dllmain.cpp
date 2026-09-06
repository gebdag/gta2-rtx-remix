// gta2dx9.dll - in-process renderer for GTA2, loaded via the `rendername`
// registry value.
//
// Two modes, selected by gta2dx9.ini next to the game:
//   backend=3dfx.dll  mode=proxy     forward everything to the original renderer
//   mode=takeover                    own the device and draw the world ourselves
//
// Proxy is the default so a bad build can never leave the game unbootable.
//
// Every export is __stdcall: the originals end in `ret N`. The argument counts
// below come from those epilogues, and getting one wrong unbalances the stack.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>
#include <psapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "frame_limiter.h"
#include "game_access.h"
#include "live_geometry.h"
#include "log.h"
#include "remix_lights.h"
#include "settings.h"
#include "synthetic_lights.h"
#include "texture_store.h"
#include "time_of_day.h"
#include "world_view.h"

using gta2dx9::Log;

namespace {

enum class Mode { Proxy, Takeover };

Mode g_mode = Mode::Proxy;
HMODULE g_self = nullptr;
char g_backendName[MAX_PATH] = "3dfx.dll";
HMODULE g_backend = nullptr;
void* g_system = nullptr;
gta2dx9::WorldView g_world;

// gbh_GetGlobals hands the game a counter block; the original exposes polys and
// texture swaps drawn this frame.
struct Globals {
    int polygons = 0;
    int textureSwaps = 0;
    int spare[6] = {};
} g_globals;

void PathBesideGame(const char* leaf, char* out, size_t size) {
    GetModuleFileNameA(nullptr, out, static_cast<DWORD>(size));
    char* slash = strrchr(out, '\\');
    if (slash) strcpy(slash + 1, leaf);
    else strncpy(out, leaf, size - 1);
}

#define SLOT(name) void* g_p_##name = nullptr;
SLOT(gbh_InitDLL) SLOT(gbh_CloseDLL) SLOT(gbh_Init) SLOT(gbh_DrawTile) SLOT(gbh_DrawTilePart)
SLOT(gbh_DrawQuad) SLOT(gbh_DrawQuadClipped) SLOT(gbh_DrawTriangle) SLOT(gbh_Plot)
SLOT(gbh_SetWindow) SLOT(gbh_PrintBitmap) SLOT(gbh_SetColourDepth) SLOT(gbh_GetGlobals)
SLOT(gbh_ConvertColour) SLOT(gbh_RegisterTexture) SLOT(gbh_BeginScene) SLOT(gbh_EndScene)
SLOT(gbh_BeginLevel) SLOT(gbh_EndLevel) SLOT(ConvertColourBank) SLOT(DrawLine)
SLOT(MakeScreenTable) SLOT(SetShadeTableA) SLOT(gbh_UnlockTexture) SLOT(gbh_RegisterPalette)
SLOT(gbh_FreePalette) SLOT(gbh_FreeTexture) SLOT(gbh_AssignPalette) SLOT(gbh_LockTexture)
SLOT(gbh_GetUsedCache) SLOT(gbh_SetCamera) SLOT(gbh_ResetLights) SLOT(gbh_AddLight)
SLOT(gbh_SetAmbient) SLOT(gbh_InitImageTable) SLOT(gbh_FreeImageTable) SLOT(gbh_LoadImage)
SLOT(gbh_BlitImage) SLOT(gbh_BlitBuffer) SLOT(gbh_DrawFlatRect) SLOT(gbh_CloseScreen)
SLOT(gbh_Convert16BitGraphic)
#undef SLOT

struct Binding { const char* name; void** slot; };

const Binding kBindings[] = {
#define BIND(name) {#name, &g_p_##name},
    BIND(gbh_InitDLL) BIND(gbh_CloseDLL) BIND(gbh_Init) BIND(gbh_DrawTile) BIND(gbh_DrawTilePart)
    BIND(gbh_DrawQuad) BIND(gbh_DrawQuadClipped) BIND(gbh_DrawTriangle) BIND(gbh_Plot)
    BIND(gbh_SetWindow) BIND(gbh_PrintBitmap) BIND(gbh_SetColourDepth) BIND(gbh_GetGlobals)
    BIND(gbh_ConvertColour) BIND(gbh_RegisterTexture) BIND(gbh_BeginScene) BIND(gbh_EndScene)
    BIND(gbh_BeginLevel) BIND(gbh_EndLevel) BIND(ConvertColourBank) BIND(DrawLine)
    BIND(MakeScreenTable) BIND(SetShadeTableA) BIND(gbh_UnlockTexture) BIND(gbh_RegisterPalette)
    BIND(gbh_FreePalette) BIND(gbh_FreeTexture) BIND(gbh_AssignPalette) BIND(gbh_LockTexture)
    BIND(gbh_GetUsedCache) BIND(gbh_SetCamera) BIND(gbh_ResetLights) BIND(gbh_AddLight)
    BIND(gbh_SetAmbient) BIND(gbh_InitImageTable) BIND(gbh_FreeImageTable) BIND(gbh_LoadImage)
    BIND(gbh_BlitImage) BIND(gbh_BlitBuffer) BIND(gbh_DrawFlatRect) BIND(gbh_CloseScreen)
    BIND(gbh_Convert16BitGraphic)
#undef BIND
};

// What the loader knows this DLL as. deploy.bat installs a second copy of it as
// d3ddll.dll, because the renderer list in "gta2 manager.exe" and in the game's
// own options screen is a hardcoded table of 3dfx.dll / d3ddll.dll / softdll.dll
// (strings at 0x39D08 in the manager) and can never be made to offer a fourth
// name. Claiming the Direct3D slot is what stops either of them silently
// swapping the renderer out the moment you go in to change the resolution.
const char* OwnModuleLeaf() {
    static char leaf[MAX_PATH] = "";
    if (!leaf[0]) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(g_self, path, sizeof(path));
        const char* slash = strrchr(path, '\\');
        strncpy(leaf, slash ? slash + 1 : path, sizeof(leaf) - 1);
    }
    return leaf;
}

void LoadConfig() {
    char ini[MAX_PATH];
    PathBesideGame("gta2dx9.ini", ini, sizeof(ini));

    // Proxy mode forwards to the original renderer. Which one that is depends on
    // the name we were loaded under: as d3ddll.dll the original D3D renderer has
    // been moved aside to d3ddll_orig.dll, and forwarding to "d3ddll.dll" would
    // just load us again.
    const bool wearingD3dName = _stricmp(OwnModuleLeaf(), "d3ddll.dll") == 0;
    GetPrivateProfileStringA("renderer", "backend", wearingD3dName ? "d3ddll_orig.dll" : "3dfx.dll",
                             g_backendName, sizeof(g_backendName), ini);
    if (_stricmp(g_backendName, OwnModuleLeaf()) == 0) {
        Log("backend '%s' is this DLL; using %s instead", g_backendName,
            wearingD3dName ? "d3ddll_orig.dll" : "3dfx.dll");
        strcpy(g_backendName, wearingD3dName ? "d3ddll_orig.dll" : "3dfx.dll");
    }
    char mode[32] = {};
    GetPrivateProfileStringA("renderer", "mode", "proxy", mode, sizeof(mode), ini);
    g_mode = _stricmp(mode, "takeover") == 0 ? Mode::Takeover : Mode::Proxy;

    // Tenths of a degree, because the ini API only reads integers.
    const int pitch = GetPrivateProfileIntA("camera", "pitch_tenths", -900, ini);
    const int fov = GetPrivateProfileIntA("camera", "fov_tenths", 400, ini);
    const bool gameTiles = GetPrivateProfileIntA("renderer", "use_game_tiles", 1, ini) != 0;
    const int ownWindow = GetPrivateProfileIntA("renderer", "own_window", 1, ini);
    g_world.Configure(pitch / 10.0f, fov / 10.0f, gameTiles,
                      static_cast<gta2dx9::PresentWindow>(ownWindow));

    // What we render at, which is nothing to do with what GTA2 thinks its screen
    // is. Its own resolution only lays the HUD out, and the overlay pass scales
    // that to whatever it is drawn into - so 640x480 in the game's options does
    // not have to mean 640x480 of path traced image. 0 means the desktop.
    g_world.SetRenderSize(GetPrivateProfileIntA("renderer", "render_width", 0, ini),
                          GetPrivateProfileIntA("renderer", "render_height", 0, ini));

    // Thousandths of a block, for the same reason the camera angles are tenths
    // of a degree: the ini API only reads integers.
    const int lift = GetPrivateProfileIntA(
        "renderer", "sprite_lift_thousandths",
        static_cast<int>(gta2dx9::kDefaultSpriteLift * 1000.0f + 0.5f), ini);
    gta2dx9::SetSpriteLift(lift / 1000.0f);

    // Stand sprites on the ground plane under them instead of at a fixed height
    // above their own level. 0 restores the fixed lift exactly, which is the way
    // back if the map read ever misdescribes a district's floor.
    gta2dx9::SetSpriteConform(GetPrivateProfileIntA("renderer", "sprite_conform", 1, ini) != 0);

    // How high the object a sprite stands in for sits above the road, in
    // thousandths of a block. Not the same thing as the lift above: that is a
    // depth-buffer clearance, this is what gives a car a shadow.
    const int height = GetPrivateProfileIntA(
        "renderer", "sprite_height_thousandths",
        static_cast<int>(gta2dx9::kDefaultSpriteHeight * 1000.0f + 0.5f), ini);
    gta2dx9::SetSpriteHeight(height / 1000.0f);

    // How much of a sideways tilt a sprite keeps, in thousandths. A car crossing
    // a ramp at an angle stands on a plane tilted both ways, so following it
    // whole rolls the body and lifts a corner - which a car on wheels does not
    // do. 0 keeps sprites level side to side, 1000 is the full fitted plane.
    gta2dx9::SetSpriteRoll(
        GetPrivateProfileIntA("renderer", "sprite_roll_thousandths",
                              static_cast<int>(gta2dx9::kDefaultSpriteRoll * 1000.0f + 0.5f), ini)
        / 1000.0f);

    // Dump every distinct frame of artwork as a TGA, and write the classification
    // numbers beside it. On by default: the files are a few kilobytes each and a
    // few hundred per session, and it is the only way to find a sprite whose
    // black rim was missed.
    gta2dx9::SetTextureDumping(GetPrivateProfileIntA("renderer", "dump_textures", 1, ini) != 0);

    // Our own frame cap, because GTA2's is a checkbox: its pacer waits on a
    // hardcoded 33 ms step and the two registry values only turn that waiting on
    // and off. 0 leaves the game free-running. Note that the game advances its
    // simulation one step per frame, so this sets the speed as well as the
    // smoothness - see frame_limiter.h.
    gta2dx9::FrameLimitSet(
        static_cast<float>(GetPrivateProfileIntA("renderer", "fps_cap", 30, ini)));

    // The day/night cycle. Hours and minutes rather than a float, because the ini
    // API only reads integers; 0200 is 2 am.
    gta2dx9::TimeOfDaySettings& tod = gta2dx9::TimeOfDay();
    tod.enabled = GetPrivateProfileIntA("timeofday", "enabled", 1, ini) != 0;
    const int start = GetPrivateProfileIntA("timeofday", "start_hhmm", 200, ini);
    tod.startHour = static_cast<float>(start / 100) + static_cast<float>(start % 100) / 60.0f;
    // Tenths, like the camera angles: the ini API only reads integers, and 10 is
    // the default one real second to one game minute.
    tod.minutesPerSecond =
        GetPrivateProfileIntA("timeofday", "game_minutes_per_second_tenths", 10, ini) / 10.0f;
    tod.latitudeDeg =
        GetPrivateProfileIntA("timeofday", "latitude_tenths", 400, ini) / 10.0f;
    tod.declinationDeg =
        GetPrivateProfileIntA("timeofday", "declination_tenths", 0, ini) / 10.0f;
    gta2dx9::TimeOfDayReset();

    // The effective sprite numbers, not the compiled-in ones. gta2dx9.ini's
    // template used to carry a stale copy of the ride height and quietly override
    // the code with it for several builds; printing what actually took effect is
    // what would have caught that on the first run.
    Log("loaded as %s; mode=%s backend=%s fps_cap=%.0f tod=%s@%05.2f", OwnModuleLeaf(),
        g_mode == Mode::Takeover ? "takeover" : "proxy", g_backendName,
        gta2dx9::FrameLimitFps(), tod.enabled ? "on" : "off", tod.startHour);
    Log("sprites: conform=%s height=%.3f roll=%.2f fallback_lift=%.3f stack_step=%.3f blocks",
        gta2dx9::SpriteConform() ? "on" : "off", gta2dx9::SpriteHeight(), gta2dx9::SpriteRoll(),
        gta2dx9::SpriteLift(), gta2dx9::SpriteStackStep());
}

bool BindBackend() {
    g_backend = LoadLibraryA(g_backendName);
    if (g_backend == g_self) {
        // Belt and braces against the name juggling above: loading ourselves as
        // the backend would recurse through every export until the stack ran out.
        Log("FATAL: backend '%s' resolved to this DLL; refusing to proxy to myself",
            g_backendName);
        g_backend = nullptr;
        return false;
    }
    if (!g_backend) {
        Log("FATAL: cannot load backend '%s' (error %lu)", g_backendName, GetLastError());
        return false;
    }
    int missing = 0;
    for (const Binding& binding : kBindings) {
        *binding.slot = reinterpret_cast<void*>(GetProcAddress(g_backend, binding.name));
        if (!*binding.slot) ++missing;
    }
    Log("backend bound, %d missing", missing);
    return true;
}

bool Proxying() { return g_mode == Mode::Proxy; }

template <typename Fn>
Fn Backend(void* slot) { return reinterpret_cast<Fn>(slot); }

using gta2dx9::TextureRecord;

WNDPROC g_originalWndProc = nullptr;

// Alt+F4 does not necessarily reach gbh_CloseScreen, so the device is also
// released straight off the window's own close message. Without this the game
// exits its loop while D3D9 still holds the window, leaving the process alive
// and the desktop in a half-torn-down state.
LRESULT CALLBACK ClosingWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE || message == WM_DESTROY) {
        Log("window closing (msg %u): releasing renderer", message);
        g_world.Shutdown();
    }
    return CallWindowProc(g_originalWndProc, window, message, wparam, lparam);
}

void HookWindowClose(HWND window) {
    if (g_originalWndProc || !window) return;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ClosingWndProc)));
}

}  // namespace

extern "C" {

// What is already in the process by the time the game reaches its renderer.
// Under RTX Remix this is how you tell whether something else has already
// dragged Direct3D 9 in and taken the window: whatever did so owns the display
// before we exist, and our own device then loses the argument.
void LogGraphicsModules() {
    HMODULE modules[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) {
        char path[MAX_PATH];
        if (!GetModuleFileNameA(modules[i], path, MAX_PATH)) continue;
        const char* leaf = strrchr(path, '\\');
        leaf = leaf ? leaf + 1 : path;
        if (_stricmp(leaf, "d3d9.dll") == 0 || _stricmp(leaf, "ddraw.dll") == 0 ||
            _stricmp(leaf, "dxgi.dll") == 0 || _stricmp(leaf, "d3d9_remix.dll") == 0 ||
            _stricmp(leaf, "dciman32.dll") == 0) {
            Log("already loaded: %s", path);
        }
    }
}

__declspec(dllexport) void __stdcall gbh_InitDLL(void* system) {
    g_system = system;
    Log("gbh_InitDLL(system=%p)", system);
    LogGraphicsModules();
    LoadConfig();
    if (Proxying()) {
        if (BindBackend() && g_p_gbh_InitDLL) {
            Backend<void(__stdcall*)(void*)>(g_p_gbh_InitDLL)(system);
        }
        Log("proxy live");
    } else {
        Log("takeover mode: the original renderer is not loaded");
        char path[MAX_PATH];
        PathBesideGame("gta2dx9_lights.ini", path, sizeof(path));
        gta2dx9::LightsInit(path);
        PathBesideGame("gta2dx9_effects.ini", path, sizeof(path));
        gta2dx9::SyntheticLightsLoad(path);
        // Last, so it can override anything gta2dx9.ini set: deploy rewrites that
        // file on every build, this one is the user's own tuning.
        PathBesideGame("gta2dx9_settings.ini", path, sizeof(path));
        gta2dx9::SettingsInit(path);
        gta2dx9::SettingsLoadAll();
    }
}

// Both teardown paths release the device. The game closes the screen before it
// destroys its window, and leaving a live D3D9 device attached to a window that
// is going away keeps the process alive after Alt+F4 with the display left in a
// half-torn-down state.
__declspec(dllexport) void __stdcall gbh_CloseScreen(void* param) {
    g_world.Shutdown();
    if (Proxying() && g_p_gbh_CloseScreen) {
        Backend<void(__stdcall*)(void*)>(g_p_gbh_CloseScreen)(param);
    }
}

__declspec(dllexport) void __stdcall gbh_CloseDLL() {
    g_world.Shutdown();
    gta2dx9::FrameLimitShutdown();
    if (Proxying() && g_p_gbh_CloseDLL) Backend<void(__stdcall*)()>(g_p_gbh_CloseDLL)();
}

__declspec(dllexport) int __stdcall gbh_Init(int param) {
    if (Proxying() && g_p_gbh_Init) return Backend<int(__stdcall*)(int)>(g_p_gbh_Init)(param);

    HWND window = game::MainWindow();
    RECT client = {};
    GetClientRect(window, &client);
    const int width = client.right > 0 ? client.right : 640;
    const int height = client.bottom > 0 ? client.bottom : 480;

    // Remix drives its runtime through window messages sent to the thread that
    // owns the game window, so which thread that is matters.
    const DWORD windowThread = GetWindowThreadProcessId(window, nullptr);
    Log("gbh_Init: hwnd=%p client=%dx%d owner thread=%lu, we are thread=%lu", window, width, height,
        windowThread, GetCurrentThreadId());

    // Not fatal if the device is not up yet: RTX Remix needs the game's message
    // loop to have run before it can hand one over, which has not happened at
    // this point in startup. The frame loop keeps asking.
    std::string error;
    if (!g_world.Initialize(window, width, height, &error)) {
        Log("world view init deferred: %s", error.c_str());
    }
    HookWindowClose(window);
    return 0;
}

// Intercepted rather than ignored: the texture the game passes identifies the
// tile it believes belongs at the cell it is drawing, which is the ground truth
// our own map walk is checked against.
__declspec(dllexport) void __stdcall gbh_DrawTile(unsigned flags, void* texture, float* vertices,
                                                  unsigned char shade) {
    if (Proxying() && g_p_gbh_DrawTile) {
        Backend<void(__stdcall*)(unsigned, void*, float*, unsigned char)>(g_p_gbh_DrawTile)(
            flags, texture, vertices, shade);
        return;
    }
    g_world.CheckDrawnShape(4);
    g_world.CheckDrawnTile(texture);
}

__declspec(dllexport) void __stdcall gbh_BeginScene() {
    if (Proxying() && g_p_gbh_BeginScene) { Backend<void(__stdcall*)()>(g_p_gbh_BeginScene)(); return; }
    g_world.BeginFrame();
}

__declspec(dllexport) void __stdcall gbh_EndScene() {
    if (Proxying() && g_p_gbh_EndScene) { Backend<void(__stdcall*)()>(g_p_gbh_EndScene)(); return; }
    g_world.RenderFrame();
}

__declspec(dllexport) void __stdcall gbh_BeginLevel() {
    if (Proxying() && g_p_gbh_BeginLevel) { Backend<void(__stdcall*)()>(g_p_gbh_BeginLevel)(); return; }
    g_world.InvalidateWorld();
    // Every run starts at the same time of day, which is the point of a start
    // hour: 2 am, and dark.
    gta2dx9::TimeOfDayReset();
}

__declspec(dllexport) void __stdcall gbh_EndLevel() {
    if (Proxying() && g_p_gbh_EndLevel) { Backend<void(__stdcall*)()>(g_p_gbh_EndLevel)(); return; }
    g_world.InvalidateWorld();
}

__declspec(dllexport) void __stdcall gbh_SetCamera(float minX, float minY, float maxX, float maxY) {
    if (Proxying() && g_p_gbh_SetCamera) {
        Backend<void(__stdcall*)(float, float, float, float)>(g_p_gbh_SetCamera)(minX, minY, maxX, maxY);
        return;
    }
    g_world.SetVisibleTileBounds(minX, minY, maxX, maxY);
}

__declspec(dllexport) void* __stdcall gbh_RegisterTexture(unsigned short width, unsigned short height,
                                                          void* pixels, int palette, char flag) {
    if (Proxying() && g_p_gbh_RegisterTexture) {
        return Backend<void*(__stdcall*)(unsigned short, unsigned short, void*, int, char)>(
            g_p_gbh_RegisterTexture)(width, height, pixels, palette, flag);
    }
    TextureRecord* record = static_cast<TextureRecord*>(calloc(1, sizeof(TextureRecord)));
    if (record) {
        record->width = width;
        record->height = height;
        record->palette = static_cast<uint16_t>(palette);
        record->paletteLow = static_cast<uint8_t>(palette);
        record->pixels = pixels;
    }
    return record;
}

__declspec(dllexport) void __stdcall gbh_FreeTexture(void* texture) {
    if (Proxying() && g_p_gbh_FreeTexture) { Backend<void(__stdcall*)(void*)>(g_p_gbh_FreeTexture)(texture); return; }
    gta2dx9::ForgetDeviceTexture(texture);
    free(texture);
}

__declspec(dllexport) void __stdcall gbh_LockTexture(void* texture) {
    if (Proxying() && g_p_gbh_LockTexture) { Backend<void(__stdcall*)(void*)>(g_p_gbh_LockTexture)(texture); return; }
    if (texture) static_cast<TextureRecord*>(texture)->flags |= 1;
}

// The game edits a texture's pixels between lock and unlock, so an unlock is
// the signal that any cached copy of it is stale.
__declspec(dllexport) void __stdcall gbh_UnlockTexture(void* texture) {
    if (Proxying() && g_p_gbh_UnlockTexture) { Backend<void(__stdcall*)(void*)>(g_p_gbh_UnlockTexture)(texture); return; }
    if (!texture) return;
    TextureRecord* record = static_cast<TextureRecord*>(texture);
    record->flags &= ~1;
    ++record->revision;  // invalidates any cached copy of its pixels
}

__declspec(dllexport) void __stdcall gbh_AssignPalette(void* texture, int palette) {
    if (Proxying() && g_p_gbh_AssignPalette) {
        Backend<void(__stdcall*)(void*, int)>(g_p_gbh_AssignPalette)(texture, palette);
        return;
    }
    if (!texture) return;
    TextureRecord* record = static_cast<TextureRecord*>(texture);
    record->palette = static_cast<uint16_t>(palette);
    record->paletteLow = static_cast<uint8_t>(palette);
}

__declspec(dllexport) void __stdcall gbh_RegisterPalette(int index, unsigned* palette) {
    gta2dx9::StorePalette(index, palette);
    if (Proxying() && g_p_gbh_RegisterPalette) {
        Backend<void(__stdcall*)(int, unsigned*)>(g_p_gbh_RegisterPalette)(index, palette);
    }
}

__declspec(dllexport) void __stdcall gbh_FreePalette(int index) {
    // We hold a pointer into the game's palette page rather than a copy, so this
    // is where it stops being safe to read.
    gta2dx9::ForgetPalette(index);
    if (Proxying() && g_p_gbh_FreePalette) Backend<void(__stdcall*)(int)>(g_p_gbh_FreePalette)(index);
}

__declspec(dllexport) unsigned __stdcall gbh_ConvertColour(unsigned r, unsigned g, unsigned b) {
    if (Proxying() && g_p_gbh_ConvertColour) {
        return Backend<unsigned(__stdcall*)(unsigned, unsigned, unsigned)>(g_p_gbh_ConvertColour)(r, g, b);
    }
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);  // 5:6:5
}

__declspec(dllexport) void* __stdcall gbh_GetGlobals() {
    if (Proxying() && g_p_gbh_GetGlobals) return Backend<void*(__stdcall*)()>(g_p_gbh_GetGlobals)();
    return &g_globals;
}

__declspec(dllexport) int __stdcall gbh_GetUsedCache(int bank) {
    if (Proxying() && g_p_gbh_GetUsedCache) return Backend<int(__stdcall*)(int)>(g_p_gbh_GetUsedCache)(bank);
    return 0;
}

// Not an opaque handle: the game passes the return value straight back to
// gbh_BlitImage as an index into the image table (gta2.exe!FUN_00453020), and
// the original returns -1 on failure.
__declspec(dllexport) void* __stdcall gbh_LoadImage(void* spec) {
    if (Proxying() && g_p_gbh_LoadImage) return Backend<void*(__stdcall*)(void*)>(g_p_gbh_LoadImage)(spec);
    const int index = g_world.GetOverlay().LoadImage(spec);
    return reinterpret_cast<void*>(static_cast<intptr_t>(index));
}

// ---------------------------------------------------------------------------
// The screen-space stream: HUD, menus and sprites.
//
// GTA2 transforms these itself and hands over D3DFVF_XYZRHW-style vertices, so
// they cannot join the world mesh. Ignoring them - which is what this DLL used
// to do - is why the menus, the HUD and every car and pedestrian were missing.
// They are replayed as a 2D pass over the world instead.
// ---------------------------------------------------------------------------

// Sprites - cars, pedestrians, powerups - and the 2D UI share this entry point,
// so they are told apart by which call site reached it: the object draw
// (FUN_004be060) is the only producer of world sprites. Those are placed as real
// 3D geometry from the world coordinates the game itself computed; the rest is
// genuine 2D and goes to the screen-space pass.
__declspec(dllexport) void __stdcall gbh_DrawQuad(unsigned flags, void* texture, float* vertices,
                                                  unsigned char shade) {
    if (Proxying() && g_p_gbh_DrawQuad) {
        Backend<void(__stdcall*)(unsigned, void*, float*, unsigned char)>(g_p_gbh_DrawQuad)(
            flags, texture, vertices, shade);
        return;
    }
    if (game::IsObjectDraw(_ReturnAddress())) {
        g_world.Live().AddSprite(flags, texture, vertices, 4);
        return;
    }
    g_world.GetOverlay().Quad(flags, texture, vertices, shade);
}

// The original ignores the extra argument and shares gbh_DrawQuad's body.
__declspec(dllexport) void __stdcall gbh_DrawQuadClipped(unsigned flags, void* texture,
                                                         float* vertices, unsigned char shade,
                                                         int clip) {
    if (Proxying() && g_p_gbh_DrawQuadClipped) {
        Backend<void(__stdcall*)(unsigned, void*, float*, unsigned char, int)>(
            g_p_gbh_DrawQuadClipped)(flags, texture, vertices, shade, clip);
        return;
    }
    if (game::IsObjectDraw(_ReturnAddress())) {
        g_world.Live().AddSprite(flags, texture, vertices, 4);
        return;
    }
    g_world.GetOverlay().Quad(flags, texture, vertices, shade);
}

// Not UI at all: the only caller is FUN_0046c210, the triangular half-wall of a
// partial block, so this is map geometry and belongs in the world.
__declspec(dllexport) void __stdcall gbh_DrawTriangle(unsigned flags, void* texture,
                                                      float* vertices, unsigned char shade) {
    if (Proxying() && g_p_gbh_DrawTriangle) {
        Backend<void(__stdcall*)(unsigned, void*, float*, unsigned char)>(g_p_gbh_DrawTriangle)(
            flags, texture, vertices, shade);
        return;
    }
    // World geometry, and the static mesh already builds it, so the stream is
    // ignored exactly the way gbh_DrawTile is - but it is the only place the
    // awkward block shapes show their real corners, so they are logged.
    (void)flags; (void)texture; (void)vertices; (void)shade;
    g_world.CheckDrawnShape(3);
}

__declspec(dllexport) void __stdcall gbh_DrawFlatRect(float* vertices, unsigned colour) {
    if (Proxying() && g_p_gbh_DrawFlatRect) {
        Backend<void(__stdcall*)(float*, unsigned)>(g_p_gbh_DrawFlatRect)(vertices, colour);
        return;
    }
    g_world.GetOverlay().FlatRect(vertices, colour);
}

// ---------------------------------------------------------------------------
// Lighting.
//
// The one part of the gbh_* interface that is genuinely world space. GTA2 works
// out every light in the frame itself - map lamps, traffic lights, headlights -
// and lists them here in tile coordinates. The original renderer used them for
// per-vertex lighting; we hand them to RTX Remix as real lights instead.
// ---------------------------------------------------------------------------

// Opens the frame's list. Only called during a world render, and only when the
// game's own `lighting` option is on.
__declspec(dllexport) void __stdcall gbh_ResetLights() {
    if (Proxying() && g_p_gbh_ResetLights) {
        Backend<void(__stdcall*)()>(g_p_gbh_ResetLights)();
        return;
    }
    gta2dx9::LightsBeginCollect();
}

// One 20-byte descriptor. Decoded in remix_lights.cpp; the layout is in
// docs/lighting-analysis.md.
__declspec(dllexport) void __stdcall gbh_AddLight(void* descriptor) {
    if (Proxying() && g_p_gbh_AddLight) {
        Backend<void(__stdcall*)(void*)>(g_p_gbh_AddLight)(descriptor);
        return;
    }
    gta2dx9::LightsAddRaw(descriptor);
}

__declspec(dllexport) void __stdcall gbh_SetAmbient(float ambient) {
    if (Proxying() && g_p_gbh_SetAmbient) {
        Backend<void(__stdcall*)(float)>(g_p_gbh_SetAmbient)(ambient);
        return;
    }
    gta2dx9::LightsSetAmbient(ambient);
}

// Floats, not ints. The game's only call site builds them with fild/fstp at
// gta2.exe!0x004CAF1F and passes gbh_SetWindow(0.0f, 0.0f, width-1, height-1).
// Declared as ints, 639.0f arrives as 1142312960, which NoteWindow rejected as
// out of range - so this call had never once told the overlay anything, and the
// menu was laid out to a default that happened to be right until a level had
// been played.
// Called by our video device proxy when GTA2 clears its screen.
//
// The menu paints incrementally and clears only when it wants a repaint, so this
// is the only moment the 2D layer may be discarded. The proxy is a separate DLL
// - it wears the game's videoname - so the two are joined by this one export
// rather than by sharing state.
extern "C" __declspec(dllexport) void __stdcall gta2dx9_NoteScreenClear() {
    g_world.NoteScreenClear();
}

__declspec(dllexport) void __stdcall gbh_SetWindow(float a, float b, float c, float d) {
    if (Proxying() && g_p_gbh_SetWindow) {
        Backend<void(__stdcall*)(float, float, float, float)>(g_p_gbh_SetWindow)(a, b, c, d);
        return;
    }
    g_world.GetOverlay().NoteWindow(a, b, c, d);
}

__declspec(dllexport) void __stdcall gbh_InitImageTable(int count) {
    if (Proxying() && g_p_gbh_InitImageTable) {
        Backend<void(__stdcall*)(int)>(g_p_gbh_InitImageTable)(count);
        return;
    }
    g_world.GetOverlay().InitImageTable(count);
}

__declspec(dllexport) void __stdcall gbh_FreeImageTable() {
    if (Proxying() && g_p_gbh_FreeImageTable) { Backend<void(__stdcall*)()>(g_p_gbh_FreeImageTable)(); return; }
    g_world.GetOverlay().FreeImageTable();
}

__declspec(dllexport) void __stdcall gbh_BlitImage(int image, int srcX1, int srcY1, int srcX2,
                                                   int srcY2, int dstX, int dstY) {
    if (Proxying() && g_p_gbh_BlitImage) {
        Backend<void(__stdcall*)(int, int, int, int, int, int, int)>(g_p_gbh_BlitImage)(
            image, srcX1, srcY1, srcX2, srcY2, dstX, dstY);
        return;
    }
    g_world.GetOverlay().BlitImage(image, srcX1, srcY1, srcX2, srcY2, dstX, dstY);
}

__declspec(dllexport) void __stdcall DrawLine(int x1, int y1, int x2, int y2, int colour) {
    if (Proxying() && g_p_DrawLine) {
        Backend<void(__stdcall*)(int, int, int, int, int)>(g_p_DrawLine)(x1, y1, x2, y2, colour);
        return;
    }
    g_world.GetOverlay().Line(x1, y1, x2, y2, static_cast<uint32_t>(colour));
}

__declspec(dllexport) void __stdcall gbh_Plot(int x, int y, int colour, int spare) {
    if (Proxying() && g_p_gbh_Plot) {
        Backend<void(__stdcall*)(int, int, int, int)>(g_p_gbh_Plot)(x, y, colour, spare);
        return;
    }
    g_world.GetOverlay().Line(x, y, x + 1, y, static_cast<uint32_t>(colour));
}

// The remaining entry points either drive the screen-space draw stream, which is
// unusable for path tracing, or the software UI surface we do not present.
#define PASSTHROUGH_0(name)                                                    \
    __declspec(dllexport) void __stdcall name() {                              \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)()>(g_p_##name)(); \
    }
#define PASSTHROUGH_1(name, T0)                                                \
    __declspec(dllexport) void __stdcall name(T0 a) {                          \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)(T0)>(g_p_##name)(a); \
    }
#define PASSTHROUGH_2(name, T0, T1)                                            \
    __declspec(dllexport) void __stdcall name(T0 a, T1 b) {                    \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)(T0, T1)>(g_p_##name)(a, b); \
    }
#define PASSTHROUGH_3(name, T0, T1, T2)                                        \
    __declspec(dllexport) void __stdcall name(T0 a, T1 b, T2 c) {              \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)(T0, T1, T2)>(g_p_##name)(a, b, c); \
    }
#define PASSTHROUGH_4(name, T0, T1, T2, T3)                                    \
    __declspec(dllexport) void __stdcall name(T0 a, T1 b, T2 c, T3 d) {        \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)(T0, T1, T2, T3)>(g_p_##name)(a, b, c, d); \
    }
#define PASSTHROUGH_5(name, T0, T1, T2, T3, T4)                                \
    __declspec(dllexport) void __stdcall name(T0 a, T1 b, T2 c, T3 d, T4 e) {  \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)(T0, T1, T2, T3, T4)>(g_p_##name)(a, b, c, d, e); \
    }
#define PASSTHROUGH_6(name, T0, T1, T2, T3, T4, T5)                            \
    __declspec(dllexport) void __stdcall name(T0 a, T1 b, T2 c, T3 d, T4 e, T5 f) { \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)(T0, T1, T2, T3, T4, T5)>(g_p_##name)(a, b, c, d, e, f); \
    }
#define PASSTHROUGH_7(name, T0, T1, T2, T3, T4, T5, T6)                        \
    __declspec(dllexport) void __stdcall name(T0 a, T1 b, T2 c, T3 d, T4 e, T5 f, T6 g) { \
        if (Proxying() && g_p_##name) Backend<void(__stdcall*)(T0, T1, T2, T3, T4, T5, T6)>(g_p_##name)(a, b, c, d, e, f, g); \
    }

// gbh_DrawTilePart shares gbh_DrawTile's implementation in the original, so it
// is world geometry too and is ignored for the same reason.
PASSTHROUGH_4(gbh_DrawTilePart, unsigned, void*, float*, unsigned char)
PASSTHROUGH_1(gbh_PrintBitmap, void*)
PASSTHROUGH_0(gbh_SetColourDepth)
PASSTHROUGH_1(ConvertColourBank, int)
PASSTHROUGH_3(MakeScreenTable, void*, unsigned, int)
PASSTHROUGH_5(SetShadeTableA, int, int, int, int, int)
PASSTHROUGH_6(gbh_BlitBuffer, int, int, int, int, int, int)
PASSTHROUGH_2(gbh_Convert16BitGraphic, int, int)

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        gta2dx9::OpenLog();
        Log("gta2dx9.dll attached to pid %lu", GetCurrentProcessId());
    } else if (reason == DLL_PROCESS_DETACH) {
        gta2dx9::CloseLog();
    }
    return TRUE;
}
