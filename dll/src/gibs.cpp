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
// catches by setting +0x290 to 4 before throwing them, and a car hitting one
// (FUN_004A0A30) sets 1 or 3. The death handler, FUN_004411B0, is where every
// death ends up, so it is hooked, and a pedestrian arriving there with 4 in that
// field died in an explosion - a grenade, a rocket, or a car going up next to
// them. With the option on, 1 and 3 count too: run down.
//
// Two things make it read as a body coming apart rather than as debris:
//
//   The limbs stay. Type 1 would remove a limb as its arc ends. Just before it
//   does, the limb is given a type the particle update has no case for - the
//   switch in FUN_00490760 only covers 1..0x2C, and its default keeps the
//   particle and does nothing to it - so it lies where it landed, drawn as
//   before, until it is one of the oldest. The pool is shared with every other
//   effect, hence the cap.
//
//   Getting rid of one has to go through the game's own update. A particle's
//   sprite sits in the draw grid, and every update function takes it out of the
//   grid (FUN_00447BD0) before it decides the particle is finished; the manager
//   then frees the sprite (FUN_0048C8F0) without looking at the grid at all. So a
//   particle removed any other way - type 8, which is removed on sight - leaves a
//   grid node pointing at a freed sprite, and the node pool (FUN_00447350 takes
//   from it with no check) runs dry: that crashed the game after a few dozen
//   gibbed bodies. A particle to be removed is made type 1 with a life of 1
//   instead, and FUN_0048C270 takes it out of the grid and ends it next frame.
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
bool g_gibsCar = gta2dx9::kDefaultGibsCar;

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
// The blood spray's own type, whose update clears the grid before ending one.
constexpr int32_t kSprayType = 1;
// How many limbs lie about at once. The particle pool is shared with every fire,
// spark and puff of smoke in the game, so this is kept well short of it.
constexpr size_t kMaxResting = 36;
// Bodies hidden at once; a long fight leaves a few, never this many.
constexpr size_t kMaxHidden = 32;
// How much larger than the artwork limbs are drawn. The limb sprites are 6 to
// 20 pixels, drawn as they are beside a body 24 to 30 pixels long they read as
// crumbs.
constexpr float kGibScale = 1.5f;

// A limb, and the pedestrian it came off: a paramedic can bring that ped back,
// and then its limbs have to go.
struct Limb {
    uint8_t* particle;
    const uint8_t* ped;
};
std::vector<Limb> g_flying;     // limbs still in the air
std::deque<Limb> g_resting;     // limbs on the ground, oldest first

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

// Ends a particle the way the game ends a spray droplet: as type 1 on its last
// frame, so its update clears it out of the draw grid first. See the note at the
// top.
void EndParticle(uint8_t* p) {
    *reinterpret_cast<int32_t*>(p + game::kParticleType) = kSprayType;
    *reinterpret_cast<int16_t*>(p + game::kParticleLife) = 1;
    *reinterpret_cast<int16_t*>(p + game::kParticleLifeStart) = 2;
}

// Ends every limb, in the air or on the ground, that came off this ped.
void RemoveLimbsOf(const uint8_t* ped) {
    int removed = 0;
    for (size_t i = 0; i < g_flying.size();) {
        if (g_flying[i].ped == ped) {
            if (IsParticle(g_flying[i].particle)) EndParticle(g_flying[i].particle);
            g_flying[i] = g_flying.back();
            g_flying.pop_back();
            ++removed;
            continue;
        }
        ++i;
    }
    for (auto it = g_resting.begin(); it != g_resting.end();) {
        if (it->ped == ped) {
            if (IsParticle(it->particle) && ParticleType(it->particle) == kRestingType) {
                EndParticle(it->particle);
            }
            it = g_resting.erase(it);
            ++removed;
            continue;
        }
        ++it;
    }
    if (removed) Log("gibs: ped %p revived, %d limbs removed", ped, removed);
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

// Our own, so throwing gibs does not draw on the game's random numbers.
uint32_t Random(uint32_t below) {
    static uint32_t state = GetTickCount() | 1;
    state = state * 1664525u + 1013904223u;
    return below ? (state >> 8) % below : 0;
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

    // Then a spray per limb. One spray only fans its six particles a few
    // degrees either side of its angle (FUN_0048C9C0 adds rand(16) of 1440), so
    // six limbs out of one spray all flew the same way. Each limb gets its own
    // spray at an angle of its own instead, keeps the first particle of it and
    // lets the rest go, and flies at a speed and for a time of its own.
    //
    // New particles go on the head of the live list (FUN_0048A900), so a spray's
    // are the ones in front of the head as it was before.
    int limb = 0;
    for (int piece = 0; piece < game::kGibSprites; ++piece) {
        const uint8_t* before = game::ParticleListHead();
        spray(effects, x, y, z, Random(game::kAngleFullTurn));
        bool taken = false;
        uint8_t* particle = game::ParticleListHead();
        for (int guard = 0; particle && particle != before && guard < 16; ++guard) {
            if (ParticleType(particle) == 1) {
                void* sprite =
                    *reinterpret_cast<void* const*>(particle + game::kParticlePlacementPtr);
                if (!taken && game::PlausiblePointer(sprite)) {
                    setSprite(sprite, static_cast<uint16_t>(particleBase + game::kGibSpriteOffset +
                                                            piece));
                    // Speed along the ground, 0.6 to 1.8 of the spray's own.
                    const float speed = 0.6f + static_cast<float>(Random(1200)) / 1000.0f;
                    int32_t* velocity =
                        reinterpret_cast<int32_t*>(particle + game::kParticleVelocity);
                    velocity[0] = static_cast<int32_t>(static_cast<float>(velocity[0]) * speed);
                    velocity[1] = static_cast<int32_t>(static_cast<float>(velocity[1]) * speed);
                    // A longer life is a longer arc: the particle climbs for the
                    // first half and falls for the second.
                    const int16_t life = static_cast<int16_t>(18 + Random(15));
                    *reinterpret_cast<int16_t*>(particle + game::kParticleLife) = life;
                    *reinterpret_cast<int16_t*>(particle + game::kParticleLifeStart) = life;
                    g_flying.push_back({particle, ped});
                    taken = true;
                    ++limb;
                } else {
                    // The other five of the spray.
                    EndParticle(particle);
                }
            }
            particle = game::NextInList(particle, game::kParticleNext);
        }
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
        Log("gibs: ped %p killed (cause %d) at (%.2f, %.2f, %.2f), %d limbs thrown", ped,
            *reinterpret_cast<const int32_t*>(ped + game::kPedDamageCause),
            x * game::kFixedScale, y * game::kFixedScale, z * game::kFixedScale, limb);
    }
}

}  // namespace

// Called with the pedestrian the game is about to finish off, before its handler
// runs. File scope and __stdcall so the thunk below can call it by name.
static void __stdcall GibsOnPedDeath(uint8_t* ped) {
    if (!g_gibs || !game::PlausiblePointer(ped)) return;
    const int32_t cause = *reinterpret_cast<const int32_t*>(ped + game::kPedDamageCause);
    const bool byCar = cause == game::kDamageCarHit || cause == game::kDamageRunOver;
    if (cause != game::kDamageExplosion && !(byCar && g_gibsCar)) return;
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
void SetGibsCar(bool on) { g_gibsCar = on; }
bool GibsCar() { return g_gibsCar; }

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
    Log("gibs: hooked (%s)", g_gibs ? "on" : "off");
}

void GibsUpdate() {
    if (g_flying.empty() && g_resting.empty() && g_hidden.empty()) return;
    const int16_t particleBase = ParticleBase();

    // Limbs about to land. The update removes a type 1 particle the frame its
    // life reaches 0, so one at 2 or less is caught here, between that frame's
    // update and the next.
    for (size_t i = 0; i < g_flying.size();) {
        uint8_t* p = g_flying[i].particle;
        const bool ours = IsParticle(p) && ParticleType(p) == 1 && IsGibSprite(p, particleBase);
        if (!ours) {
            g_flying[i] = g_flying.back();
            g_flying.pop_back();
            continue;
        }
        if (*reinterpret_cast<const int16_t*>(p + game::kParticleLife) <= 2) {
            if (g_gibs) {
                *reinterpret_cast<int32_t*>(p + game::kParticleType) = kRestingType;
                // Gone when the blood is: the pool under a body lasts 800 frames
                // from the death (FUN_0048CC50), and this limb has already spent
                // its flight of it. The update counts the life down whatever the
                // type, so it is the clock.
                const int16_t flown =
                    *reinterpret_cast<const int16_t*>(p + game::kParticleLifeStart);
                *reinterpret_cast<int16_t*>(p + game::kParticleLife) =
                    static_cast<int16_t>(game::kBloodPoolLife - flown);
                g_resting.push_back(g_flying[i]);
            }
            g_flying[i] = g_flying.back();
            g_flying.pop_back();
            continue;
        }
        ++i;
    }

    // Drop any that are no longer ours, clear the ones whose blood has gone, then
    // retire the oldest past the cap - or all of them, once the option is off.
    for (auto it = g_resting.begin(); it != g_resting.end();) {
        uint8_t* p = it->particle;
        if (!IsParticle(p) || ParticleType(p) != kRestingType) {
            it = g_resting.erase(it);
        } else if (*reinterpret_cast<const int16_t*>(p + game::kParticleLife) <= 2) {
            EndParticle(p);
            it = g_resting.erase(it);
        } else {
            ++it;
        }
    }
    const size_t keep = g_gibs ? kMaxResting : 0;
    while (g_resting.size() > keep) {
        EndParticle(g_resting.front().particle);
        g_resting.pop_front();
    }

    // A hidden body stays hidden only while its ped is still dead and still owns
    // that sprite: the game recycles all three.
    //
    // The same ped with the same body, alive again, is one a paramedic brought
    // back. The game puts the whole pedestrian back on its feet, so what came off
    // it goes too - otherwise the ped stands up beside its own limbs.
    for (size_t i = 0; i < g_hidden.size();) {
        const Hidden& h = g_hidden[i];
        const bool body =
            *reinterpret_cast<const uint8_t* const*>(h.ped + game::kPedPhysics) == h.physics &&
            *reinterpret_cast<const void* const*>(h.physics + game::kPhysicsSprite) == h.sprite;
        const bool dead =
            *reinterpret_cast<const int32_t*>(h.ped + game::kPedState) == game::kPedStateDead ||
            *reinterpret_cast<const int32_t*>(h.ped + game::kPedStatePending) ==
                game::kPedStateDead;
        if (body && !dead) RemoveLimbsOf(h.ped);
        if (!(g_gibs && body && dead)) {
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

bool GameObjectIsCorpse() {
    const uint8_t* object = static_cast<const uint8_t*>(g_drawing);
    if (!object) return false;
    if (*reinterpret_cast<const int32_t*>(object + game::kSpriteObjectBase) != game::kSpriteBasePed) {
        return false;
    }
    const uint8_t* physics = *reinterpret_cast<const uint8_t* const*>(object + game::kSpriteObjectOwner);
    if (!game::PlausiblePointer(physics)) return false;
    const uint8_t* ped = *reinterpret_cast<const uint8_t* const*>(physics + game::kPhysicsPed);
    if (!game::PlausiblePointer(ped)) return false;
    const int32_t state = *reinterpret_cast<const int32_t*>(ped + game::kPedState);
    if (state == game::kPedStateDead ||
        *reinterpret_cast<const int32_t*>(ped + game::kPedStatePending) == game::kPedStateDead) {
        return true;
    }
    // Knocked down and still alive - the player included - lies just as flat.
    return state == game::kPedStateInterrupted &&
           *reinterpret_cast<const int32_t*>(ped + game::kPedSubState) ==
               game::kPedSubStateKnockedDown;
}

bool GameObjectIsPowerUp() {
    const uint8_t* object = static_cast<const uint8_t*>(g_drawing);
    if (!object) return false;
    if (*reinterpret_cast<const int32_t*>(object + game::kSpriteObjectBase) !=
        game::kSpriteBaseCodeObj) {
        return false;
    }
    const int number = *reinterpret_cast<const uint16_t*>(object + game::kSpriteObjectNumber);
    return number >= game::kPowerUpSpriteFirst &&
           number < game::kPowerUpSpriteFirst + game::kPowerUpSpriteCount;
}

float GibsCurrentScale() {
    const uint8_t* object = static_cast<const uint8_t*>(g_drawing);
    if (!object) return 1.0f;
    if (*reinterpret_cast<const int32_t*>(object + game::kSpriteObjectBase) !=
        game::kSpriteBaseCodeObj) {
        return 1.0f;
    }
    const int16_t particleBase = ParticleBase();
    if (particleBase < 0) return 1.0f;
    const int number = *reinterpret_cast<const uint16_t*>(object + game::kSpriteObjectNumber);
    const int first = particleBase + game::kGibSpriteOffset;
    return (number >= first && number < first + game::kGibSprites) ? kGibScale : 1.0f;
}

bool GibsHideCurrentSprite() {
    const void* drawing = g_drawing;
    if (!drawing) return false;
    for (const Hidden& h : g_hidden) {
        if (h.sprite == drawing) return true;
    }
    return false;
}

}  // namespace gta2dx9
