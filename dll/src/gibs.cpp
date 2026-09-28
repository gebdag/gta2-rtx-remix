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
//
// Two things make it read as a body coming apart rather than as debris:
//
//   The limbs stay. Type 1 would remove a limb as its arc ends. Just before it
//   does, the limb is given a type the particle update has no case for - the
//   switch in FUN_00490760 only covers 1..0x2C, and its default keeps the
//   particle and does nothing to it - so it lies where it landed, drawn as
//   before, until it is one of the oldest and is handed type 8, which the
//   update removes. The pool is shared with every other effect, hence the cap.
//
//   The body goes. A dead pedestrian's physics object (ped+0x168) keeps its
//   sprite (+0x80) on the ground as a corpse. The object draw, FUN_004BE060, is
//   entered with that sprite object in ecx, so its entry is hooked to note which
//   object is being drawn, and the sprite pass skips it for as long as that ped
//   is still dead and still owns that sprite. Only the picture goes; the game's
//   own idea of the body is untouched.

#include "gibs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "game_access.h"
#include "log.h"

namespace {

using gta2dx9::Log;

bool g_gibs = gta2dx9::kDefaultGibs;
bool g_gibsStay = gta2dx9::kDefaultGibsStay;

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

// A type the particle update has no case for: kept, never moved, never removed.
constexpr int32_t kRestingType = 0x40;
// The type the update removes on sight.
constexpr int32_t kRemoveType = 8;
// How many limbs lie about at once. The particle pool is shared with every fire,
// spark and puff of smoke in the game, so this is kept well short of it.
constexpr size_t kMaxResting = 36;
// Bodies hidden at once; a long fight leaves a few, never this many.
constexpr size_t kMaxHidden = 32;

std::vector<uint8_t*> g_flying;     // limbs still in the air
std::deque<uint8_t*> g_resting;     // limbs on the ground, oldest first

struct Hidden {
    const uint8_t* ped;
    const uint8_t* physics;
    const void* sprite;
};
std::vector<Hidden> g_hidden;

// The sprite object the game's object draw was last entered with.
const void* volatile g_drawing = nullptr;

// Particles live inside the manager's own allocation (FUN_00491B90, 0x947C
// bytes), so anything outside it is not one - a stale pointer from a level that
// has since been freed, say.
bool IsParticle(const uint8_t* p) {
    const uint8_t* manager = *reinterpret_cast<const uint8_t* const*>(game::kParticleManagerPtr);
    if (!game::PlausiblePointer(manager) || !game::PlausiblePointer(p)) return false;
    return p > manager && p + 0x50 <= manager + game::kParticleManagerBytes;
}

int32_t ParticleType(const uint8_t* p) {
    return *reinterpret_cast<const int32_t*>(p + game::kParticleType);
}

bool IsGibSprite(const uint8_t* p, int16_t particleBase) {
    const void* sprite = *reinterpret_cast<void* const*>(p + game::kParticlePlacementPtr);
    if (!game::PlausiblePointer(sprite)) return false;
    const int number = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(sprite) +
                                                          game::kSpriteObjectNumber);
    const int first = particleBase + game::kGibSpriteOffset;
    return number >= first && number < first + game::kGibSprites;
}

int16_t ParticleBase() {
    const uint8_t* bank = *reinterpret_cast<const uint8_t* const*>(game::kObjectBankPtr);
    if (!game::PlausiblePointer(bank)) return -1;
    return *reinterpret_cast<const int16_t*>(bank + game::kParticleSpriteBase);
}

using BloodSpray = void(__thiscall*)(void* effects, int32_t x, int32_t y, int32_t z,
                                     uint32_t angle);
using SetSprite = void(__thiscall*)(void* sprite, uint32_t number);

void ThrowGibs(const uint8_t* ped) {
    void* effects = *reinterpret_cast<void* const*>(game::kEffectSystemPtr);
    const int16_t particleBase = ParticleBase();
    if (!game::PlausiblePointer(effects) || particleBase < 0) return;

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
    int limb = 0;
    uint8_t* particle = game::ParticleListHead();
    for (int guard = 0; particle && particle != before && guard < 32; ++guard) {
        if (ParticleType(particle) == 1) {
            void* sprite = *reinterpret_cast<void* const*>(particle + game::kParticlePlacementPtr);
            if (game::PlausiblePointer(sprite)) {
                setSprite(sprite, static_cast<uint16_t>(particleBase + game::kGibSpriteOffset +
                                                        limb % game::kGibSprites));
                // A longer life is a longer arc: the particle climbs for the first
                // half and falls for the second, so limbs fly further than drops.
                *reinterpret_cast<int16_t*>(particle + game::kParticleLife) = 24;
                *reinterpret_cast<int16_t*>(particle + game::kParticleLifeStart) = 24;
                g_flying.push_back(particle);
                ++limb;
            }
        }
        particle = game::NextInList(particle, game::kParticleNext);
    }

    // The body's sprite, so the sprite pass can leave it out.
    const uint8_t* physics = *reinterpret_cast<const uint8_t* const*>(ped + game::kPedPhysics);
    if (game::PlausiblePointer(physics)) {
        const void* sprite =
            *reinterpret_cast<const void* const*>(physics + game::kPhysicsSprite);
        if (game::PlausiblePointer(sprite)) {
            if (g_hidden.size() >= kMaxHidden) g_hidden.erase(g_hidden.begin());
            g_hidden.push_back({ped, physics, sprite});
        }
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
static uintptr_t s_drawResume = game::kObjectDrawBegin + game::kObjectDrawStolenBytes;
static const void* volatile* s_drawing = &g_drawing;

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

// The object draw's first instruction, sub esp, 0xA4, with a note of the object
// it was entered for. Nothing else is touched.
static __declspec(naked) void ObjectDrawThunk() {
    __asm {
        push eax
        mov eax, dword ptr [s_drawing]
        mov dword ptr [eax], ecx
        pop eax
        sub esp, 0xA4
        jmp dword ptr [s_drawResume]
    }
}

namespace {

bool Patch(uintptr_t at, const uint8_t* expected, int length, const void* thunk, const char* what) {
    uint8_t* site = reinterpret_cast<uint8_t*>(at);
    if (memcmp(site, expected, length) != 0) {
        Log("gibs: gta2.exe is not the build this knows at %s; no gibs", what);
        return false;
    }
    uint8_t patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(thunk) - (at + 5));
    memcpy(patch + 1, &to, sizeof(to));
    DWORD previous = 0;
    if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &previous)) {
        Log("gibs: could not patch %s at %08X", what, static_cast<unsigned>(at));
        return false;
    }
    memcpy(site, patch, length);
    VirtualProtect(site, length, previous, &previous);
    return true;
}

}  // namespace

namespace gta2dx9 {

void SetGibs(bool on) { g_gibs = on; }
bool Gibs() { return g_gibs; }
void SetGibsStay(bool on) { g_gibsStay = on; }
bool GibsStay() { return g_gibsStay; }

void GibsInstall() {
    static const uint8_t kDeath[game::kPedDeathStolenBytes] = {0x56, 0x8B, 0xF1, 0xF6, 0x86,
                                                               0x1F, 0x02, 0x00, 0x00, 0x01};
    static const uint8_t kDraw[game::kObjectDrawStolenBytes] = {0x81, 0xEC, 0xA4, 0x00, 0x00, 0x00};
    // The draw hook first: a death hook without it would throw limbs over a body
    // that stays.
    if (!Patch(game::kObjectDrawBegin, kDraw, sizeof(kDraw), &ObjectDrawThunk, "the object draw")) {
        return;
    }
    Patch(game::kPedDeathHandler, kDeath, sizeof(kDeath), &PedDeathThunk, "the death handler");
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("gibs: hooked (%s, limbs %s)", g_gibs ? "on" : "off", g_gibsStay ? "stay" : "vanish");
}

void GibsUpdate() {
    if (g_flying.empty() && g_resting.empty() && g_hidden.empty()) return;
    const int16_t particleBase = ParticleBase();

    // Limbs about to land. The update removes a type 1 particle the frame its
    // life reaches 0, so one at 2 or less is caught here, between that frame's
    // update and the next.
    for (size_t i = 0; i < g_flying.size();) {
        uint8_t* p = g_flying[i];
        const bool ours = IsParticle(p) && ParticleType(p) == 1 && IsGibSprite(p, particleBase);
        if (!ours) {
            g_flying[i] = g_flying.back();
            g_flying.pop_back();
            continue;
        }
        if (*reinterpret_cast<const int16_t*>(p + game::kParticleLife) <= 2) {
            if (g_gibsStay && g_gibs) {
                *reinterpret_cast<int32_t*>(p + game::kParticleType) = kRestingType;
                g_resting.push_back(p);
            }
            g_flying[i] = g_flying.back();
            g_flying.pop_back();
            continue;
        }
        ++i;
    }

    // Drop any that are no longer ours, then retire the oldest past the cap - or
    // all of them, once the option is off.
    for (auto it = g_resting.begin(); it != g_resting.end();) {
        it = (IsParticle(*it) && ParticleType(*it) == kRestingType) ? it + 1 : g_resting.erase(it);
    }
    const size_t keep = (g_gibsStay && g_gibs) ? kMaxResting : 0;
    while (g_resting.size() > keep) {
        *reinterpret_cast<int32_t*>(g_resting.front() + game::kParticleType) = kRemoveType;
        g_resting.pop_front();
    }

    // A hidden body stays hidden only while its ped is still dead and still owns
    // that sprite: the game recycles all three.
    for (size_t i = 0; i < g_hidden.size();) {
        const Hidden& h = g_hidden[i];
        const bool same =
            g_gibs &&
            *reinterpret_cast<const uint8_t* const*>(h.ped + game::kPedPhysics) == h.physics &&
            *reinterpret_cast<const void* const*>(h.physics + game::kPhysicsSprite) == h.sprite &&
            (*reinterpret_cast<const int32_t*>(h.ped + game::kPedState) == game::kPedStateDead ||
             *reinterpret_cast<const int32_t*>(h.ped + game::kPedStatePending) ==
                 game::kPedStateDead);
        if (!same) {
            g_hidden.erase(g_hidden.begin() + i);
            continue;
        }
        ++i;
    }
}

void GibsReset() {
    g_flying.clear();
    g_resting.clear();
    g_hidden.clear();
    g_drawing = nullptr;
}

const void* GameObjectBeingDrawn() { return g_drawing; }

bool GibsHideCurrentSprite() {
    const void* drawing = g_drawing;
    if (!drawing) return false;
    for (const Hidden& h : g_hidden) {
        if (h.sprite == drawing) return true;
    }
    return false;
}

}  // namespace gta2dx9
