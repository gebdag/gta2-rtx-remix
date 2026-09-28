// Gibs.
//
// The style file carries six severed-limb sprites and a set of splatter frames
// (code_obj 389-399) that nothing in gta2.exe ever draws: no particle picks those
// offsets from the bank, and no flag turns them on. So this is new behaviour
// rather than a restored one, built out of pieces the game already has.
//
// The piece that does the work is the game's own blood spray, FUN_0048C9C0. A
// pedestrian who is shot calls it with their position and the angle of the hit,
// and it throws six particles of type 1 outward in a spread around that angle.
// Type 1's update, FUN_0048C270, carries a particle up and then down again over
// its life - an arc - and never changes its sprite. So a spray whose particles are
// given the limb sprites afterwards is a handful of limbs flying out and falling,
// moved by the game's own code and drawn like any other particle.
//
// Which deaths: the explosion update, FUN_00490D60, marks every pedestrian it
// catches by setting +0x290 to 4 before throwing them (a car hit sets 2). The
// death handler, FUN_004411B0, is where every death ends up, so it is hooked, and
// a pedestrian arriving there with 4 in that field died in an explosion - a
// grenade, a rocket, or a car going up next to them.

#include "gibs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game_access.h"
#include "log.h"

namespace {

using gta2dx9::Log;

bool g_gibs = gta2dx9::kDefaultGibs;

// Deaths already given gibs, so a handler that runs twice for one body does not
// throw a second set. A handful is plenty: they are forgotten after a few seconds.
struct Recent {
    const uint8_t* ped = nullptr;
    DWORD tick = 0;
};
Recent g_recent[16];
int g_logged = 0;

bool AlreadyGibbed(const uint8_t* ped) {
    const DWORD now = GetTickCount();
    Recent* oldest = &g_recent[0];
    for (Recent& r : g_recent) {
        if (r.ped == ped && now - r.tick < 3000) return true;
        if (r.tick < oldest->tick) oldest = &r;
    }
    oldest->ped = ped;
    oldest->tick = now;
    return false;
}

using BloodSpray = void(__thiscall*)(void* effects, int32_t x, int32_t y, int32_t z,
                                     uint32_t angle);
using SetSprite = void(__thiscall*)(void* sprite, uint32_t number);

void ThrowGibs(const uint8_t* ped) {
    void* effects = *reinterpret_cast<void* const*>(game::kEffectSystemPtr);
    const uint8_t* bank = *reinterpret_cast<const uint8_t* const*>(game::kObjectBankPtr);
    if (!game::PlausiblePointer(effects) || !game::PlausiblePointer(bank)) return;

    const int32_t x = *reinterpret_cast<const int32_t*>(ped + game::kPedX);
    const int32_t y = *reinterpret_cast<const int32_t*>(ped + game::kPedY);
    const int32_t z = *reinterpret_cast<const int32_t*>(ped + game::kPedZ);
    const BloodSpray spray = reinterpret_cast<BloodSpray>(game::kBloodSpray);
    const SetSprite setSprite = reinterpret_cast<SetSprite>(game::kSetSprite);

    // Blood first, in two sprays facing away from each other, so it goes every
    // way at once rather than back along a bullet's line.
    const uint32_t half = game::kAngleFullTurn / 2;
    spray(effects, x, y, z, 0);
    spray(effects, x, y, z, half);

    // Then one more, whose particles become the limbs. New particles go on the
    // head of the live list (FUN_0048A900), so they are the ones in front of the
    // head as it was before.
    const uint8_t* before = game::ParticleListHead();
    spray(effects, x, y, z, half / 2);
    const int16_t particleBase = *reinterpret_cast<const int16_t*>(bank + game::kParticleSpriteBase);
    int limb = 0;
    uint8_t* particle = game::ParticleListHead();
    for (int guard = 0; particle && particle != before && guard < 32; ++guard) {
        if (*reinterpret_cast<const int32_t*>(particle + game::kParticleType) == 1) {
            void* sprite = *reinterpret_cast<void* const*>(particle + game::kParticlePlacementPtr);
            if (game::PlausiblePointer(sprite)) {
                setSprite(sprite, static_cast<uint16_t>(particleBase + game::kGibSpriteOffset +
                                                        limb % game::kGibSprites));
                // A longer life is a longer arc: the particle climbs for the first
                // half and falls for the second, so limbs fly further than drops.
                *reinterpret_cast<int16_t*>(particle + game::kParticleLife) = 24;
                *reinterpret_cast<int16_t*>(particle + game::kParticleLifeStart) = 24;
                ++limb;
            }
        }
        particle = game::NextInList(particle, game::kParticleNext);
    }
    if (g_logged < 8) {
        ++g_logged;
        Log("gibs: ped %p killed by an explosion at (%.2f, %.2f, %.2f), %d limbs thrown", ped,
            x * game::kFixedScale, y * game::kFixedScale, z * game::kFixedScale, limb);
    }
}

}  // namespace

// Called with the pedestrian the game is about to finish off, before its handler
// runs. File scope and __stdcall so the thunk below can call it by name.
static void __stdcall GibsOnPedDeath(uint8_t* ped) {
    if (!g_gibs || !game::PlausiblePointer(ped)) return;
    if (*reinterpret_cast<const int32_t*>(ped + game::kPedDamageCause) != game::kDamageExplosion) {
        return;
    }
    if (AlreadyGibbed(ped)) return;
    ThrowGibs(ped);
}

static uintptr_t s_deathResume = game::kPedDeathHandler + game::kPedDeathStolenBytes;

// Takes the place of the death handler's first three instructions. The game's
// registers and flags are kept around our call, then those instructions run as
// they would have - push esi / mov esi, ecx / test byte ptr [esi+0x21F], 1 - and
// the handler carries on from its conditional jump, which reads the flags the
// test just set.
static __declspec(naked) void PedDeathThunk() {
    __asm {
        pushad
        pushfd
        push ecx
        call GibsOnPedDeath
        popfd
        popad
        push esi
        mov esi, ecx
        test byte ptr [esi + 0x21F], 1
        jmp dword ptr [s_deathResume]
    }
}

namespace gta2dx9 {

void SetGibs(bool on) { g_gibs = on; }
bool Gibs() { return g_gibs; }

void GibsInstall() {
    uint8_t* site = reinterpret_cast<uint8_t*>(game::kPedDeathHandler);
    static const uint8_t kExpected[game::kPedDeathStolenBytes] = {0x56, 0x8B, 0xF1, 0xF6, 0x86,
                                                                  0x1F, 0x02, 0x00, 0x00, 0x01};
    if (memcmp(site, kExpected, sizeof(kExpected)) != 0) {
        Log("gibs: gta2.exe is not the build this knows; no gibs");
        return;
    }
    uint8_t patch[game::kPedDeathStolenBytes];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PedDeathThunk) -
                                            (game::kPedDeathHandler + 5));
    memcpy(patch + 1, &to, sizeof(to));
    DWORD previous = 0;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &previous)) {
        Log("gibs: could not patch %08X", static_cast<unsigned>(game::kPedDeathHandler));
        return;
    }
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(patch), previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("gibs: death handler hooked (%s)", g_gibs ? "on" : "off");
}

}  // namespace gta2dx9
