// The HUD pass.
//
// Every screen-space draw arrives through the same gbh_* entry points, so the
// entry point says nothing about whether a quad is the score in the corner or
// a "$100" floating over a pedestrian. The difference matters on a wide screen:
// the first belongs against the edge of the display, the second has to stay
// over the pedestrian. What does say it is the call stack - GTA2 draws its whole
// HUD from one function (game_access.h, kHudDraw) - so the one call to that
// function is routed through a thunk that raises a flag for its duration.
//
// The parts. The HUD draw is nothing but nineteen calls in a row, one per part
// of the HUD (FUN_004CA440):
//
// Coordinates below are the 640x480 canvas, and the first value each routine
// computes is y, the second x.
//
//    0  FUN_004C6DA0  nothing drawn
//    1  FUN_004C9C20  score, lives, multiplier (from 639), weapon and ammo (638 less
//                     half the icon), power-ups leftward from it: right. The
//                     multiplayer score list (x 16) and timer: left
//    2  FUN_004C74A0  the marker over a target               - world
//    3  FUN_004C78A0  multiplayer names over the players     - world
//    4  FUN_004C7A30  a row of icons centred on 320           - centred
//    5  FUN_004C74F0  gang respect bars, x 16-93: left
//    6  FUN_004C7B70  health hearts from 551: right
//    7  FUN_004C9890  the zone name, framed, centred on 320   - centred
//    8  FUN_004C94F0  a box centred on 320                    - centred
//    9  FUN_004C9690  a line centred on 320                   - centred
//   10  FUN_004C9430  the talking head: portrait at x 32 near the bottom and
//                     the subtitle from x 64, stacked up from 480 - centred
//   11  FUN_004C92A0  counters in boxes near x 0: left
//   12  FUN_004C8C80  text items at positions of their own
//   13  FUN_004C84C0  arrows pointing at targets              - world
//   14  FUN_004C96F0  a line centred on 320                   - centred
//   15  FUN_004C8A40  the big messages                        - centred
//   16  FUN_004C9FA0  the pause and stats screen              - centred
//   17  FUN_004C8910  multiplayer chat from x 0: left
//   18  FUN_004C8710  the quit prompt, reached by a tail jump - centred
//
// Each call is pointed at a stub of its own that notes the part and jumps on to
// the function it replaced, so the overlay can keep parts apart: one part's
// element never decides where another's goes. That was the whole class of bug
// where the zone name, appearing between the corners, pulled the corners to the
// middle with it.

#include "hud_pass.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game_access.h"
#include "log.h"

// File scope and plain C types, so the inline assembly below can name them.
static volatile uint8_t s_inHud = 0;
static volatile uint8_t s_part = 0xFF;
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
        mov byte ptr [s_part], 0xFF
        pop dword ptr [s_hudReturn]
        call dword ptr [s_hudDraw]
        mov byte ptr [s_inHud], 0
        mov byte ptr [s_part], 0xFF
        push dword ptr [s_hudReturn]
        ret
    }
}

namespace {

// The HUD draw's nineteen calls, in order: where each is, and where it goes. The
// last is a tail jump rather than a call.
struct PartCall {
    uintptr_t site;
    uintptr_t target;
    uint8_t opcode;   // 0xE8 call, 0xE9 jmp
};
const PartCall kParts[] = {
    {0x004CA450, 0x004C6DA0, 0xE8}, {0x004CA45B, 0x004C9C20, 0xE8},
    {0x004CA466, 0x004C74A0, 0xE8}, {0x004CA471, 0x004C78A0, 0xE8},
    {0x004CA47C, 0x004C7A30, 0xE8}, {0x004CA487, 0x004C74F0, 0xE8},
    {0x004CA492, 0x004C7B70, 0xE8}, {0x004CA49A, 0x004C9890, 0xE8},
    {0x004CA4A1, 0x004C94F0, 0xE8}, {0x004CA4AC, 0x004C9690, 0xE8},
    {0x004CA4B7, 0x004C9430, 0xE8}, {0x004CA4C2, 0x004C92A0, 0xE8},
    {0x004CA4CD, 0x004C8C80, 0xE8}, {0x004CA4D8, 0x004C84C0, 0xE8},
    {0x004CA4E3, 0x004C96F0, 0xE8}, {0x004CA4EE, 0x004C8A40, 0xE8},
    {0x004CA4F9, 0x004C9FA0, 0xE8}, {0x004CA504, 0x004C8910, 0xE8},
    {0x004CA510, 0x004C8710, 0xE9},
};
const int kPartCount = static_cast<int>(sizeof(kParts) / sizeof(kParts[0]));
bool g_partsKnown = false;

int32_t RelTo(uintptr_t from, uintptr_t to) { return static_cast<int32_t>(to - (from + 5)); }

// Points each of the HUD draw's calls at a stub of its own:
//   mov byte ptr [s_part], k     C6 05 <&s_part> k
//   jmp <original target>        E9 <rel32>
// Nothing else is touched: ecx, the stack and the return address are exactly
// what the call left them.
bool InstallParts() {
    for (const PartCall& p : kParts) {
        const uint8_t* site = reinterpret_cast<const uint8_t*>(p.site);
        int32_t rel = 0;
        memcpy(&rel, site + 1, sizeof(rel));
        if (site[0] != p.opcode || p.site + 5 + static_cast<uintptr_t>(rel) != p.target) return false;
    }
    const size_t kStub = 12;
    uint8_t* stubs = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kStub * kPartCount, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stubs) return false;
    const uint32_t partAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_part));
    for (int k = 0; k < kPartCount; ++k) {
        uint8_t* s = stubs + kStub * k;
        s[0] = 0xC6;
        s[1] = 0x05;
        memcpy(s + 2, &partAddress, 4);
        s[6] = static_cast<uint8_t>(k);
        s[7] = 0xE9;
        const int32_t back = RelTo(reinterpret_cast<uintptr_t>(s + 7), kParts[k].target);
        memcpy(s + 8, &back, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, kStub * kPartCount);
    for (int k = 0; k < kPartCount; ++k) {
        uint8_t* site = reinterpret_cast<uint8_t*>(kParts[k].site);
        DWORD previous = 0;
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous)) return false;
        const int32_t to = RelTo(kParts[k].site, reinterpret_cast<uintptr_t>(stubs + kStub * k));
        memcpy(site + 1, &to, 4);
        VirtualProtect(site, 5, previous, &previous);
    }
    return true;
}

}  // namespace

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
    g_partsKnown = InstallParts();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("hud pass: installed (%s)", g_partsKnown ? "parts told apart"
                                                 : "parts not told apart - one HUD for placement");
}

bool HudPassActive() { return s_inHud != 0; }

int HudPassPart() {
    if (!g_partsKnown || !s_inHud || s_part >= kPartCount) return -1;
    return s_part;
}

bool HudPartIsWorldAnchored(int part) { return part == 2 || part == 3 || part == 13; }

bool HudPartIsCentred(int part) {
    switch (part) {
        case 4: case 7: case 8: case 9: case 10: case 14: case 15: case 16: case 18:
            return true;
        default:
            return false;
    }
}

}  // namespace gta2dx9
