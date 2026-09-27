// The HUD pass.
//
// Every screen-space draw arrives through the same gbh_* entry points, so the
// entry point says nothing about whether a quad is the score in the corner or
// a "$100" floating over a pedestrian. The difference matters on a wide screen:
// the first belongs against the edge of the display, the second has to stay
// over the pedestrian. What does say it is the call stack - GTA2 draws its whole
// HUD from one function (game_access.h, kHudDraw) - so the one call to that
// function is routed through a thunk that raises a flag for its duration.

#include "hud_pass.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game_access.h"
#include "log.h"

// File scope and plain C types, so the inline assembly below can name them.
static volatile uint8_t s_inHud = 0;
static uintptr_t s_hudDraw = game::kHudDraw;
static uintptr_t s_hudReturn = 0;

// Called in place of the HUD draw, with the game's this pointer still in ecx and
// its arguments still on the stack: take our own return address off, call the
// real function exactly as the game would have, and put it back. Whoever cleans
// the arguments - the callee's `ret n` or the caller - finds them where it
// expects. The HUD is drawn once a frame and never recursively, so one saved
// return address is enough.
static __declspec(naked) void HudDrawThunk() {
    __asm {
        mov byte ptr [s_inHud], 1
        pop dword ptr [s_hudReturn]
        call dword ptr [s_hudDraw]
        mov byte ptr [s_inHud], 0
        push dword ptr [s_hudReturn]
        ret
    }
}

namespace gta2dx9 {

void HudPassInstall() {
    uint8_t* site = reinterpret_cast<uint8_t*>(game::kHudDrawCall);
    int32_t rel = 0;
    memcpy(&rel, site + 1, sizeof(rel));
    if (site[0] != 0xE8 || game::kHudDrawCall + 5 + static_cast<uintptr_t>(rel) != game::kHudDraw) {
        Log("hud pass: gta2.exe is not the build this knows; the HUD keeps its 4:3 placement");
        return;
    }
    uint8_t call[5] = {0xE8};
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HudDrawThunk) -
                                            (game::kHudDrawCall + 5));
    memcpy(call + 1, &to, sizeof(to));
    DWORD previous = 0;
    if (!VirtualProtect(site, sizeof(call), PAGE_EXECUTE_READWRITE, &previous)) {
        Log("hud pass: could not patch %08X", static_cast<unsigned>(game::kHudDrawCall));
        return;
    }
    memcpy(site, call, sizeof(call));
    VirtualProtect(site, sizeof(call), previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("hud pass: installed");
}

bool HudPassActive() { return s_inHud != 0; }

}  // namespace gta2dx9
