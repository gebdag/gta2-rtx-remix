// The intro movie.
//
// --- What goes wrong --------------------------------------------------------
//
// GTA2 picks how to show data\movie\intro.bik from the renderer's file name
// (gta2.exe!0x004CB47C): softdll.dll is 0, 3dfx.dll is 2, anything else - the
// Direct3D renderer, and so us under either name - is 1. Only 3dfx has Bink
// decode into the game's own video surface. Everything else gets the DirectDraw
// path (0x00481FD4): BinkBufferSetDDPrimary with the video device's primary
// surface, then BinkBufferOpen on the game window, and BinkBufferBlit on every
// frame.
//
// Our video device has no primary surface - it never creates a DirectDraw screen
// at all - so Bink creates a DirectDraw device of its own, on a window whose
// picture belongs to RTX Remix and Vulkan. On some machines that survives and
// the movie is simply invisible; on others the game crashes at startup inside
// binkw32.dll.
//
// Nothing about the movie was ever drawn by us either way: the movie loop calls
// no gbh_BeginScene or gbh_EndScene, so the renderer never presents during it.
//
// --- What this does ---------------------------------------------------------
//
// Whether the intro plays is decided by intro= in gta2dx9.ini instead of
// do_play_movie. Both places the game reads that value call the stand-in below.
// Off is then exactly the game's own do_play_movie=0 path: the movie is never
// opened.
//
// On, the 3dfx path is taken for every renderer: FUN_00481df0 answers true, so
// Bink never touches DirectDraw. On that path the frame loop locks the video
// surface, has Bink copy the frame into it, and unlocks it. Our surface is
// empty, so the lock is wrapped to lend the copy a buffer of ours in 32-bit,
// and the unlock to hand the finished frame to the renderer, which draws it
// over the screen and presents. The context's own fields are put back straight
// after, so nothing outside the movie ever sees the buffer.

#include "intro_movie.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "game_access.h"
#include "log.h"

namespace gta2dx9 {
namespace {

bool g_play = kDefaultIntro;
MovieFramePresenter g_present = nullptr;

// The buffer lent to the copy, and what the context held before.
std::vector<uint32_t> g_frame;
int g_frameWidth = 0;
int g_frameHeight = 0;
bool g_lent = false;
int32_t g_savedSurface = 0;
int32_t g_savedPitch = 0;

using GameCall = void(__cdecl*)();

bool WriteCode(uintptr_t at, const void* bytes, size_t size) {
    void* target = reinterpret_cast<void*>(at);
    DWORD previous = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    memcpy(target, bytes, size);
    VirtualProtect(target, size, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// Whether `at` is a `call rel32` to `target`.
bool CallsTo(uintptr_t at, uintptr_t target) {
    const uint8_t* site = reinterpret_cast<const uint8_t*>(at);
    if (site[0] != 0xE8) return false;
    int32_t rel = 0;
    memcpy(&rel, site + 1, sizeof(rel));
    return at + 5 + static_cast<uintptr_t>(rel) == target;
}

bool Redirect(uintptr_t at, const void* to) {
    uint8_t call[5] = {0xE8};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (at + 5));
    memcpy(call + 1, &rel, sizeof(rel));
    return WriteCode(at, call, sizeof(call));
}

uint8_t* VideoContext() { return *reinterpret_cast<uint8_t**>(game::kVideoContextPtr); }

int32_t& VideoField(uint8_t* context, uintptr_t offset) {
    return *reinterpret_cast<int32_t*>(context + offset);
}

// Stands in for the game's registry read of do_play_movie. That is a __thiscall
// taking (name, default) and popping both, which a __fastcall with an unused
// edx matches exactly.
int __fastcall PlayMovieSetting(void* registry, void* unused, const char* name, int fallback) {
    (void)registry;
    (void)unused;
    (void)name;
    (void)fallback;
    return g_play ? 1 : 0;
}

// The frame loop's lock. The game's own first, then the buffer is lent: the
// copy reads the surface and pitch from the context only after this returns
// (0x00481E49), and the surface type from kBinkSurfaceType just before.
void __cdecl MovieLock() {
    reinterpret_cast<GameCall>(game::kLockScreen)();
    uint8_t* context = VideoContext();
    const int32_t* movie = *reinterpret_cast<int32_t* const*>(game::kBinkMoviePtr);
    if (!context || !movie) return;
    const int width = movie[0];
    const int height = movie[1];
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return;
    if (!g_lent) {
        g_savedSurface = VideoField(context, game::kVideoSurfaceOffset);
        g_savedPitch = VideoField(context, game::kVideoPitchOffset);
    }
    g_frame.resize(static_cast<size_t>(width) * height);
    g_frameWidth = width;
    g_frameHeight = height;
    VideoField(context, game::kVideoSurfaceOffset) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(g_frame.data()));
    VideoField(context, game::kVideoPitchOffset) = width * 4;
    // What the open chose was worked out from the pixel format of a surface we do
    // not have. 32-bit is what the renderer draws, so ask for that.
    *reinterpret_cast<int32_t*>(game::kBinkSurfaceType) = game::kBinkSurface32;
    g_lent = true;
}

// The frame loop's unlock: the context gets its own fields back, and the frame
// goes to the screen.
void __cdecl MovieUnlock() {
    reinterpret_cast<GameCall>(game::kUnlockScreen)();
    if (!g_lent) return;
    g_lent = false;
    if (uint8_t* context = VideoContext()) {
        VideoField(context, game::kVideoSurfaceOffset) = g_savedSurface;
        VideoField(context, game::kVideoPitchOffset) = g_savedPitch;
    }
    static int frames = 0;
    if (++frames == 1) {
        Log("intro: first frame, %dx%d, drawn by the renderer", g_frameWidth, g_frameHeight);
    }
    if (g_present) g_present(g_frame.data(), g_frameWidth, g_frameHeight, g_frameWidth * 4);
}

}  // namespace

void IntroInstall(bool play, MovieFramePresenter present) {
    g_play = play;
    g_present = present;

    // Every site is checked before any is written. Half of this in place is worse
    // than none: the surface path without the lent buffer copies into a null
    // pointer.
    const bool settingSites = CallsTo(game::kPlayMovieReadStartup, game::kReadRegistryInt) &&
                              CallsTo(game::kPlayMovieReadFrontEnd, game::kReadRegistryInt);
    if (!settingSites) {
        Log("intro: gta2.exe is not the build this knows; the intro is left as the game has it");
        return;
    }

    if (g_play) {
        static const uint8_t kIsThreeDfx[] = {0x83, 0x3D, 0x80, 0x35, 0x67, 0x00, 0x02,  // cmp [0x673580], 2
                                              0x0F, 0x94, 0xC0,                          // sete al
                                              0xC3};                                     // ret
        const bool movieSites =
            memcmp(reinterpret_cast<const void*>(game::kMovieUsesGameSurface), kIsThreeDfx,
                   sizeof(kIsThreeDfx)) == 0 &&
            CallsTo(game::kMovieLockCall, game::kLockScreen) &&
            CallsTo(game::kMovieUnlockCall, game::kUnlockScreen) && present;
        if (!movieSites) {
            // Skipping is the safe answer: the DirectDraw path is the crash.
            Log("intro: the movie code is not what this expects; skipping the intro instead");
            g_play = false;
        } else {
            static const uint8_t kAlwaysTrue[] = {0xB0, 0x01, 0xC3};  // mov al, 1 / ret
            const bool ok = WriteCode(game::kMovieUsesGameSurface, kAlwaysTrue,
                                      sizeof(kAlwaysTrue)) &&
                            Redirect(game::kMovieLockCall, reinterpret_cast<void*>(&MovieLock)) &&
                            Redirect(game::kMovieUnlockCall, reinterpret_cast<void*>(&MovieUnlock));
            if (!ok) {
                Log("intro: could not patch the movie code; skipping the intro instead");
                g_play = false;
            }
        }
    }

    Redirect(game::kPlayMovieReadStartup, reinterpret_cast<void*>(&PlayMovieSetting));
    Redirect(game::kPlayMovieReadFrontEnd, reinterpret_cast<void*>(&PlayMovieSetting));
    Log("intro: %s", g_play ? "plays, drawn by the renderer (no DirectDraw)" : "skipped");
}

}  // namespace gta2dx9
