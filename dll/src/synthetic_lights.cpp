#include "synthetic_lights.h"

#include "game_access.h"
#include "log.h"
#include "remix_lights.h"

#include "../../src/gta2_map.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace gta2dx9 {
namespace {

SyntheticSettings g_settings;
SyntheticStats    g_stats;

int  g_binding[kMaxParticleType];
bool g_bindingReady = false;
bool g_dirty = false;
char g_path[MAX_PATH] = "gta2dx9_effects.ini";

std::vector<ParticleTypeInfo>          g_types;      // ordered by type id
std::unordered_map<int, size_t>        g_typeIndex;

// One entry per vehicle we have seen, keyed by its address in the game's heap.
// Addresses are recycled when a car is destroyed and another takes its slot,
// which is harmless here: the worst case is one frame of a new car inheriting
// the old one's "driven" state.
struct VehicleTrack {
    float lastPos[3] = {};
    unsigned lastMovedTick = 0;
    unsigned lastSeenTick = 0;
};
std::unordered_map<uintptr_t, VehicleTrack> g_vehicles;

// A list walk in another process's heap needs a stop, in case a pointer is
// stale mid-frame. Comfortably above anything GTA2 has live at once.
const int kMaxWalk = 4096;

// Nothing outside the map is a real effect; a value this far out is a stale
// slot rather than a position the game just computed.
const float kWorldMargin = 8.0f;

const char* const kCategoryNames[kSynthCategoryCount] = {
    "Muzzle flash", "Bullet", "Sparks", "Cigarette", "Headlights"};

// The game's frame is x east, y south, z the level. Ours is x east, y up,
// z north, with the rows mirrored - the same conversion live_geometry.cpp does
// for sprites.
void ToWorld(float gx, float gy, float gz, float* out) {
    out[0] = gx;
    out[1] = gz;
    out[2] = static_cast<float>(gta2::kMapHeight) - gy;
}

bool InsideWorld(float gx, float gy, float gz) {
    return std::isfinite(gx) && std::isfinite(gy) && std::isfinite(gz)
        && gx > -kWorldMargin && gx < gta2::kMapWidth + kWorldMargin && gy > -kWorldMargin
        && gy < gta2::kMapHeight + kWorldMargin && gz > -4.0f && gz < 12.0f;
}

uint64_t MixPointer(uintptr_t p, uint64_t salt) {
    uint64_t h = 0xCBF29CE484222325ULL ^ salt;
    for (int i = 0; i < 8; ++i) {
        h ^= (p >> (i * 8)) & 0xFF;
        h *= 0x100000001B3ULL;
    }
    return h ? h : 1;
}

void EnsureBinding() {
    if (g_bindingReady) return;
    g_bindingReady = true;
    for (int i = 0; i < kMaxParticleType; ++i) g_binding[i] = -1;

    // The only binding the static analysis supports on its own. FUN_0048CD10 in
    // the weapon module fires two particles per shot - 0x28 and 0x29 - so both
    // go to the muzzle flash. Everything else is left unbound on purpose: a
    // wrong guess puts light on the wrong effect and nothing in the picture says
    // so, whereas an unbound category says exactly that in the menu.
    g_binding[0x28] = kSynthMuzzle;
    g_binding[0x29] = kSynthMuzzle;

    SyntheticCategorySettings& muzzle = g_settings.category[kSynthMuzzle];
    muzzle.rgb[0] = 1.0f; muzzle.rgb[1] = 0.82f; muzzle.rgb[2] = 0.45f;
    muzzle.intensity = 1.0f;
    muzzle.radius = 3.0f;
    muzzle.heightOffset = 0.05f;

    SyntheticCategorySettings& bullet = g_settings.category[kSynthBullet];
    bullet.rgb[0] = 1.0f; bullet.rgb[1] = 0.75f; bullet.rgb[2] = 0.35f;
    bullet.intensity = 0.35f;
    bullet.radius = 1.0f;

    SyntheticCategorySettings& spark = g_settings.category[kSynthSpark];
    spark.rgb[0] = 1.0f; spark.rgb[1] = 0.72f; spark.rgb[2] = 0.28f;
    spark.intensity = 0.6f;
    spark.radius = 1.25f;

    SyntheticCategorySettings& cig = g_settings.category[kSynthCigarette];
    cig.rgb[0] = 1.0f; cig.rgb[1] = 0.22f; cig.rgb[2] = 0.06f;
    cig.intensity = 0.12f;
    cig.radius = 0.6f;
    cig.heightOffset = 0.08f;

    SyntheticCategorySettings& head = g_settings.category[kSynthHeadlight];
    head.rgb[0] = 1.0f; head.rgb[1] = 0.93f; head.rgb[2] = 0.80f;
    head.intensity = 1.0f;
    head.radius = 7.0f;
    head.heightOffset = 0.12f;
    head.maxLights = 40;
}

ParticleTypeInfo& TypeSlot(int type) {
    auto it = g_typeIndex.find(type);
    if (it != g_typeIndex.end()) return g_types[it->second];

    // Kept sorted by type id: the menu lists these, and a list that reshuffles
    // between frames cannot be clicked.
    size_t at = 0;
    while (at < g_types.size() && g_types[at].type < type) ++at;
    ParticleTypeInfo info;
    info.type = type;
    info.firstSeenTick = GetTickCount();
    g_types.insert(g_types.begin() + at, info);
    g_typeIndex.clear();
    for (size_t i = 0; i < g_types.size(); ++i) g_typeIndex[g_types[i].type] = i;
    return g_types[g_typeIndex[type]];
}

bool Room(int category) {
    if (g_stats.emitted[category] < g_settings.category[category].maxLights) return true;
    ++g_stats.capped[category];
    return false;
}

void SubmitPoint(int category, const float* world, float extraIntensity) {
    if (!Room(category)) return;
    const SyntheticCategorySettings& c = g_settings.category[category];
    RemixLightDesc d;
    d.pos[0] = world[0];
    d.pos[1] = world[1] + c.heightOffset;
    d.pos[2] = world[2];
    d.rgb[0] = c.rgb[0];
    d.rgb[1] = c.rgb[1];
    d.rgb[2] = c.rgb[2];
    d.intensity = c.intensity * extraIntensity;
    d.radius = c.radius;
    d.source = static_cast<uint8_t>(kLightSourceMuzzle + category);
    LightsSubmitExtra(d);
    ++g_stats.emitted[category];
}

void WalkParticles() {
    for (ParticleTypeInfo& info : g_types) info.liveNow = 0;

    uint8_t* entry = game::ParticleListHead();
    g_stats.particleListFound = entry != nullptr;
    g_stats.particlesWalked = 0;

    const unsigned now = GetTickCount();
    for (int guard = 0; entry && guard < kMaxWalk; ++guard) {
        uint8_t* next = game::NextInList(entry, game::kParticleNext);
        ++g_stats.particlesWalked;

        const int type = *reinterpret_cast<const int32_t*>(entry + game::kParticleType);
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        const bool placed = game::ReadPlacement(entry, game::kParticlePlacementPtr, &gx, &gy, &gz);

        if (placed && static_cast<unsigned>(type) < kMaxParticleType && InsideWorld(gx, gy, gz)) {
            ParticleTypeInfo& info = TypeSlot(type);
            ++info.liveNow;
            ++info.totalSeen;
            info.lastSeenTick = now;
            info.lastLife = *reinterpret_cast<const int16_t*>(entry + game::kParticleLife);
            ToWorld(gx, gy, gz, info.lastPos);

            const int category = g_binding[type];
            if (category >= 0 && g_settings.category[category].enabled) {
                SubmitPoint(category, info.lastPos, 1.0f);
            }
        }
        entry = next;
    }
}

void WalkVehicles() {
    const SyntheticCategorySettings& c = g_settings.category[kSynthHeadlight];
    uint8_t* entry = game::VehicleListHead();
    g_stats.vehicleListFound = entry != nullptr;
    g_stats.vehiclesWalked = 0;
    g_stats.vehiclesDriven = 0;

    const unsigned now = GetTickCount();
    for (int guard = 0; entry && guard < kMaxWalk; ++guard) {
        uint8_t* next = game::NextInList(entry, game::kVehicleNext);
        ++g_stats.vehiclesWalked;

        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        if (!game::ReadPlacement(entry, game::kVehiclePlacementPtr, &gx, &gy, &gz)
            || !InsideWorld(gx, gy, gz)) {
            entry = next;
            continue;
        }

        VehicleTrack& track = g_vehicles[reinterpret_cast<uintptr_t>(entry)];
        const float dx = gx - track.lastPos[0];
        const float dy = gy - track.lastPos[1];
        const bool first = track.lastSeenTick == 0;
        if (!first && dx * dx + dy * dy
                          > g_settings.drivenMinMovement * g_settings.drivenMinMovement) {
            track.lastMovedTick = now ? now : 1;
        }
        track.lastPos[0] = gx;
        track.lastPos[1] = gy;
        track.lastPos[2] = gz;
        track.lastSeenTick = now ? now : 1;

        // Parked cars stay dark. Someone in the driving seat is the test that
        // actually means what it says; the movement window is a fallback for a
        // car being pushed, and is off by default.
        const bool occupied = g_settings.drivenNeedsDriver && game::VehicleHasDriver(entry);
        const bool rolling = g_settings.drivenAllowsMovement && track.lastMovedTick != 0
                          && now - track.lastMovedTick <= g_settings.drivenWindowMs;
        const bool driven = occupied || rolling;
        if (!driven) {
            entry = next;
            continue;
        }
        ++g_stats.vehiclesDriven;
        if (!c.enabled) {
            entry = next;
            continue;
        }

        float fx, fy;
        if (!game::ReadFacing(entry, game::kVehiclePlacementPtr, &fx, &fy)) {
            g_stats.trigTablesReady = false;
            entry = next;
            continue;
        }
        g_stats.trigTablesReady = true;

        // Forward and right in the game's frame, then converted the same way the
        // positions are: our +Z is north, which is the game's -y.
        const float rx = fy, ry = -fx;   // right-hand perpendicular
        const float pitch = c.pitchDegrees * 3.14159265f / 180.0f;
        const float horizontal = std::cos(pitch);

        for (int side = 0; side < 2; ++side) {
            if (!Room(kSynthHeadlight)) break;
            const float sign = side == 0 ? -1.0f : 1.0f;
            const float px = gx + fx * c.forwardOffset + rx * c.sideOffset * sign;
            const float py = gy + fy * c.forwardOffset + ry * c.sideOffset * sign;

            RemixLightDesc d;
            ToWorld(px, py, gz, d.pos);
            d.pos[1] += c.heightOffset;
            d.rgb[0] = c.rgb[0];
            d.rgb[1] = c.rgb[1];
            d.rgb[2] = c.rgb[2];
            d.intensity = c.intensity;
            d.radius = c.radius;
            d.spot = true;
            d.dir[0] = fx * horizontal;
            d.dir[1] = -std::sin(pitch);      // tilted down towards the road
            d.dir[2] = -fy * horizontal;      // game south is our -Z
            d.coneAngleDeg = c.coneAngleDeg;
            d.source = kLightSourceHeadlight;
            // One identity per beam per car, so a handle survives the whole
            // drive however far it goes rather than being rediscovered by
            // proximity every frame.
            d.explicitId = MixPointer(reinterpret_cast<uintptr_t>(entry),
                                      static_cast<uint64_t>(side) + 0x48EAD11);
            LightsSubmitExtra(d);
            ++g_stats.emitted[kSynthHeadlight];
        }
        entry = next;
    }

    // Vehicles that have not been seen for a while are gone; their slots can be
    // reused by another car, so holding the entry would hand it a stale history.
    for (auto it = g_vehicles.begin(); it != g_vehicles.end();) {
        it = (now - it->second.lastSeenTick > 5000) ? g_vehicles.erase(it) : ++it;
    }
}

char* TrimInPlace(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Structure probe
//
// Finding a field by eye in a decompiler is how the position ended up wrong in
// the first place, so this finds it from the running game instead - and the
// test that actually works is motion, not proximity.
//
// "Near the camera" was the first attempt and it is too weak: most of the
// seventy-odd cars in the list are nowhere near the camera, so the real field
// scores badly while any small constant scores well. A position is the only
// field that is always a plausible map coordinate AND changes by a fraction of
// a tile from one second to the next. Counters jump, constants never move,
// pointers are neither. So this takes two snapshots a second apart and scores
// the difference.
// ---------------------------------------------------------------------------
namespace {

// Wide enough for a GTA2 car, which is a good deal bigger than the 0xC0 the
// first pass looked at - and the position turned out not to be in that window.
const int kProbeSpan = 0x200;
const int kProbeSlots = kProbeSpan / 4;
const int kChildSpan = 0x80;
const int kChildSlots = kChildSpan / 4;
const int kProbeEntities = 48;

bool g_probeRan = false;
int  g_probeStage = 0;
int  g_probeWaitFrames = 0;

bool ReadDword(const uint8_t* base, int offset, uint32_t* out) {
    // The tail of the window runs off the end of a smaller structure, and there
    // is no length to check it against. Lean on the page fault.
    __try {
        *out = *reinterpret_cast<const uint32_t*>(base + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Snapshot {
    const uint8_t* entity = nullptr;
    uint32_t direct[kProbeSlots] = {};
    bool     directOk[kProbeSlots] = {};
    uint32_t child[kProbeSlots][kChildSlots] = {};
    bool     childOk[kProbeSlots] = {};
};

struct Candidate {
    int moved = 0;      // changed by a believable per-second distance
    int inRange = 0;    // stayed a plausible map coordinate throughout
    int samples = 0;
    float exampleFrom = 0.0f;
    float exampleTo = 0.0f;
};

void Capture(const uint8_t* entity, Snapshot* snap) {
    snap->entity = entity;
    for (int i = 0; i < kProbeSlots; ++i) {
        snap->directOk[i] = ReadDword(entity, i * 4, &snap->direct[i]);
        if (!snap->directOk[i]) break;
    }
    for (int i = 0; i < kProbeSlots; ++i) {
        if (!snap->directOk[i]) continue;
        const uint8_t* child =
            reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(snap->direct[i]));
        if (!game::PlausiblePointer(child)) continue;
        bool ok = true;
        for (int c = 0; c < kChildSlots && ok; ++c) ok = ReadDword(child, c * 4, &snap->child[i][c]);
        snap->childOk[i] = ok;
    }
}

// A map coordinate in 16.14, and a change small enough to be a second of
// driving rather than a counter ticking over.
void Score(uint32_t before, uint32_t after, Candidate* c) {
    ++c->samples;
    const float a = static_cast<int32_t>(before) * game::kFixedScale;
    const float b = static_cast<int32_t>(after) * game::kFixedScale;
    if (a < -8.0f || a > gta2::kMapWidth + 8.0f || b < -8.0f || b > gta2::kMapWidth + 8.0f) return;
    ++c->inRange;
    const float delta = std::fabs(b - a);
    if (delta > 0.0005f && delta < 6.0f) {
        ++c->moved;
        if (c->exampleTo == 0.0f) {
            c->exampleFrom = a;
            c->exampleTo = b;
        }
    }
}

// Ranked, not thresholded: the point is to see the top of the list even when
// only a handful of entities happened to be moving.
void ReportBest(const char* what, Candidate* candidates, int count, bool viaPointer) {
    for (int rank = 0; rank < 8; ++rank) {
        int best = -1;
        for (int i = 0; i < count; ++i) {
            if (candidates[i].samples < 4 || !candidates[i].moved) continue;
            if (best < 0 || candidates[i].moved > candidates[best].moved) best = i;
        }
        if (best < 0) {
            if (!rank) Log("probe:   %s -- nothing here moves like a position", what);
            break;
        }
        const Candidate& c = candidates[best];
        if (viaPointer) {
            Log("probe:   %s *(+0x%02X) + 0x%02X   moved %d/%d, in range %d   e.g. %.3f -> %.3f",
                what, (best / kChildSlots) * 4, (best % kChildSlots) * 4, c.moved, c.samples,
                c.inRange, c.exampleFrom, c.exampleTo);
        } else {
            Log("probe:   %s +0x%02X   moved %d/%d, in range %d   e.g. %.3f -> %.3f", what,
                best * 4, c.moved, c.samples, c.inRange, c.exampleFrom, c.exampleTo);
        }
        candidates[best].moved = 0;   // so the next rank picks the next one
    }
}

void DumpOne(const char* what, const uint8_t* entity, int span) {
    char line[256];
    Log("probe: %s at %p", what, entity);
    for (int row = 0; row < span; row += 32) {
        int used = snprintf(line, sizeof(line), "probe:   +0x%03X ", row);
        for (int col = 0; col < 32 && row + col < span; col += 4) {
            uint32_t raw = 0;
            if (!ReadDword(entity, row + col, &raw)) break;
            used += snprintf(line + used, sizeof(line) - used, " %08X", raw);
        }
        Log("%s", line);
    }
}

std::vector<Snapshot> g_vehicleSnap;
std::vector<Snapshot> g_particleSnap;

void CaptureList(std::vector<Snapshot>& into, uint8_t* head, uintptr_t nextOffset) {
    into.clear();
    for (int guard = 0; head && guard < kProbeEntities; ++guard) {
        Snapshot snap;
        Capture(head, &snap);
        into.push_back(snap);
        head = game::NextInList(head, nextOffset);
    }
}

void CompareList(const char* what, const std::vector<Snapshot>& before, uint8_t* head,
                 uintptr_t nextOffset) {
    std::unordered_map<uintptr_t, const Snapshot*> byAddress;
    for (const Snapshot& s : before) byAddress[reinterpret_cast<uintptr_t>(s.entity)] = &s;

    std::vector<Candidate> direct(kProbeSlots);
    std::vector<Candidate> child(static_cast<size_t>(kProbeSlots) * kChildSlots);
    int compared = 0;

    // Which fields tell a car that is being driven from one that is parked. Now
    // that the position is read correctly this finally means something: the
    // first attempt classified every car as parked, so every column read zero.
    std::vector<int> ptrOnMovers(kProbeSlots), ptrOnParked(kProbeSlots);
    int movers = 0, parked = 0;

    for (int guard = 0; head && guard < kProbeEntities * 4; ++guard) {
        auto it = byAddress.find(reinterpret_cast<uintptr_t>(head));
        if (it != byAddress.end()) {
            const Snapshot& was = *it->second;
            Snapshot now;
            Capture(head, &now);
            ++compared;

            const int place = static_cast<int>(game::kVehiclePlacementPtr) / 4;
            bool moved = false;
            if (was.childOk[place] && now.childOk[place]) {
                const int xs = static_cast<int>(game::kPlaceX) / 4;
                const int ys = static_cast<int>(game::kPlaceY) / 4;
                const float dx = (static_cast<int32_t>(now.child[place][xs])
                                  - static_cast<int32_t>(was.child[place][xs])) * game::kFixedScale;
                const float dy = (static_cast<int32_t>(now.child[place][ys])
                                  - static_cast<int32_t>(was.child[place][ys])) * game::kFixedScale;
                moved = dx * dx + dy * dy > 0.01f;
            }
            if (moved) ++movers; else ++parked;
            for (int i = 0; i < kProbeSlots; ++i) {
                if (!now.directOk[i]) break;
                const void* value =
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(now.direct[i]));
                if (!game::PlausiblePointer(value)) continue;
                if (moved) ++ptrOnMovers[i]; else ++ptrOnParked[i];
            }
            for (int i = 0; i < kProbeSlots; ++i) {
                if (was.directOk[i] && now.directOk[i]) {
                    Score(was.direct[i], now.direct[i], &direct[i]);
                }
                if (!was.childOk[i] || !now.childOk[i]) continue;
                for (int c = 0; c < kChildSlots; ++c) {
                    Score(was.child[i][c], now.child[i][c],
                          &child[static_cast<size_t>(i) * kChildSlots + c]);
                }
            }
        }
        head = game::NextInList(head, nextOffset);
    }

    Log("probe: %s -- %d entit%s present in both snapshots", what, compared,
        compared == 1 ? "y" : "ies");
    ReportBest(what, direct.data(), kProbeSlots, false);
    ReportBest(what, child.data(), static_cast<int>(child.size()), true);

    if (movers && parked) {
        Log("probe: %s -- %d moving, %d parked; fields that separate them:", what, movers, parked);
        bool any = false;
        for (int i = 0; i < kProbeSlots; ++i) {
            const int m = ptrOnMovers[i] * 100 / movers;
            const int q = ptrOnParked[i] * 100 / parked;
            if (m - q < 50) continue;   // wants to be present on movers and absent on parked
            any = true;
            Log("probe:   +0x%02X   pointer on %d%% of movers, %d%% of parked", i * 4, m, q);
        }
        if (!any) Log("probe:   nothing separates them -- no driver pointer in this window");
    } else {
        Log("probe: %s -- %d moving, %d parked; need both to find a driver field", what, movers,
            parked);
    }
}

}  // namespace

bool SyntheticProbeHasRun() { return g_probeRan; }

void SyntheticProbe(const char* reason) {
    if (g_probeStage == 0) {
        Log("=== structure probe (%s) : snapshot 1 ===", reason ? reason : "on demand");
        float camX = 0.0f, camY = 0.0f;
        game::CameraPosition(&camX, &camY);
        Log("probe: camera at %.2f, %.2f", camX, camY);

        uint8_t* vehicles = game::VehicleListHead();
        Log("probe: vehicle pool [0x%08X] -> %p", game::kVehiclePoolPtr, vehicles);
        if (vehicles) DumpOne("vehicle", vehicles, kProbeSpan);
        CaptureList(g_vehicleSnap, vehicles, game::kVehicleNext);

        const uint8_t* manager = *reinterpret_cast<uint8_t* const*>(game::kParticleManagerPtr);
        Log("probe: particle manager [0x%08X] = %p", game::kParticleManagerPtr, manager);
        uint8_t* particles = game::ParticleListHead();
        if (particles) DumpOne("particle", particles, kProbeSpan);
        CaptureList(g_particleSnap, particles, game::kParticleNext);

        Log("probe: captured %d vehicle(s), %d particle(s); comparing in a second",
            static_cast<int>(g_vehicleSnap.size()), static_cast<int>(g_particleSnap.size()));
        g_probeStage = 1;
        g_probeWaitFrames = 60;
        return;
    }

    Log("=== structure probe : snapshot 2 ===");
    float camX = 0.0f, camY = 0.0f;
    game::CameraPosition(&camX, &camY);
    Log("probe: camera now at %.2f, %.2f", camX, camY);
    CompareList("vehicle", g_vehicleSnap, game::VehicleListHead(), game::kVehicleNext);
    CompareList("particle", g_particleSnap, game::ParticleListHead(), game::kParticleNext);
    Log("=== end structure probe ===");
    g_probeStage = 0;
    g_probeRan = true;
}

const char* SyntheticCategoryName(int category) {
    return category >= 0 && category < kSynthCategoryCount ? kCategoryNames[category] : "?";
}

SyntheticSettings& SyntheticLightsSettings() {
    EnsureBinding();
    return g_settings;
}

int SyntheticTypeBinding(int particleType) {
    EnsureBinding();
    return static_cast<unsigned>(particleType) < kMaxParticleType ? g_binding[particleType] : -1;
}

void SyntheticBindType(int particleType, int category) {
    EnsureBinding();
    if (static_cast<unsigned>(particleType) >= kMaxParticleType) return;
    // Headlights come off the vehicle list, so binding a particle to them would
    // silently do nothing.
    if (category == kSynthHeadlight) return;
    g_binding[particleType] = (category >= 0 && category < kSynthCategoryCount) ? category : -1;
    g_dirty = true;
}

void SyntheticLightsUpdate() {
    EnsureBinding();
    LightsBeginExtra();
    for (int i = 0; i < kSynthCategoryCount; ++i) {
        g_stats.emitted[i] = 0;
        g_stats.capped[i] = 0;
    }

    if (!g_settings.enabled) {
        g_stats.particlesWalked = 0;
        g_stats.vehiclesWalked = 0;
        g_stats.vehiclesDriven = 0;
        return;
    }
    // Nothing to walk in the menus, and the pool pointers hold whatever the last
    // level left in them.
    if (!game::MapObject()) {
        g_stats.particleListFound = false;
        g_stats.vehicleListFound = false;
        return;
    }

    WalkParticles();
    WalkVehicles();

    // Once, a few seconds into a level, so an ordinary run leaves the evidence
    // in the log without anyone having to think to ask for it. By now there is
    // traffic moving, which is what makes the position and driver columns mean
    // something.
    static int inWorldFrames = 0;
    ++inWorldFrames;

    // Per category, periodically. "Nothing is happening" was impossible to
    // diagnose from the outside: a wrong binding, a wrong offset and a category
    // switched off all look identical in the picture. This says which it is.
    if ((inWorldFrames & 0xFF) == 0) {
        Log("synthetic: particles %d, vehicles %d (%d driven) | muzzle %d bullet %d spark %d "
            "cigarette %d headlight %d",
            g_stats.particlesWalked, g_stats.vehiclesWalked, g_stats.vehiclesDriven,
            g_stats.emitted[kSynthMuzzle], g_stats.emitted[kSynthBullet],
            g_stats.emitted[kSynthSpark], g_stats.emitted[kSynthCigarette],
            g_stats.emitted[kSynthHeadlight]);
    }
    if (g_probeStage == 1) {
        if (--g_probeWaitFrames <= 0) SyntheticProbe(nullptr);
    } else if (!g_probeRan && inWorldFrames > 400) {
        SyntheticProbe("400 frames into a level");
    }
}

const std::vector<ParticleTypeInfo>& SyntheticParticleTypes() { return g_types; }

void SyntheticForgetParticleTypes() {
    g_types.clear();
    g_typeIndex.clear();
}

const SyntheticStats& SyntheticLightsStats() { return g_stats; }

const char* SyntheticLightsPath() { return g_path; }
bool SyntheticLightsDirty() { return g_dirty; }
void SyntheticLightsMarkDirty() { g_dirty = true; }

void SyntheticLightsLoad(const char* path) {
    EnsureBinding();
    if (path && *path) {
        strncpy(g_path, path, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = '\0';
    }
    FILE* f = fopen(g_path, "r");
    if (!f) return;   // first run

    int section = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* s = TrimInPlace(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (!close) continue;
            *close = '\0';
            const char* name = s + 1;
            section = -1;
            if (_stricmp(name, "general") == 0) {
                section = -2;
            } else {
                for (int i = 0; i < kSynthCategoryCount; ++i) {
                    if (_stricmp(name, kCategoryNames[i]) == 0) section = i;
                }
            }
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = TrimInPlace(s);
        char* value = TrimInPlace(eq + 1);

        if (section == -2) {
            if (_stricmp(key, "Enabled") == 0) g_settings.enabled = atoi(value) != 0;
            else if (_stricmp(key, "DrivenNeedsDriver") == 0) g_settings.drivenNeedsDriver = atoi(value) != 0;
            else if (_stricmp(key, "DrivenAllowsMovement") == 0) g_settings.drivenAllowsMovement = atoi(value) != 0;
            else if (_stricmp(key, "DrivenWindowMs") == 0) g_settings.drivenWindowMs = atoi(value);
            else if (_stricmp(key, "DrivenMinMovement") == 0) {
                g_settings.drivenMinMovement = static_cast<float>(atof(value));
            } else if (_stricmp(key, "Bind") == 0) {
                // Bind=<type hex> <category index>
                int type = 0, category = -1;
                if (sscanf(value, "%x %d", &type, &category) == 2
                    && static_cast<unsigned>(type) < kMaxParticleType) {
                    g_binding[type] = (category >= 0 && category < kSynthCategoryCount) ? category
                                                                                        : -1;
                }
            }
            continue;
        }
        if (section < 0) continue;
        SyntheticCategorySettings& c = g_settings.category[section];
        if (_stricmp(key, "Enabled") == 0) c.enabled = atoi(value) != 0;
        else if (_stricmp(key, "Color") == 0) sscanf(value, "%f %f %f", &c.rgb[0], &c.rgb[1], &c.rgb[2]);
        else if (_stricmp(key, "Intensity") == 0) c.intensity = static_cast<float>(atof(value));
        else if (_stricmp(key, "Radius") == 0) c.radius = static_cast<float>(atof(value));
        else if (_stricmp(key, "Height") == 0) c.heightOffset = static_cast<float>(atof(value));
        else if (_stricmp(key, "MaxLights") == 0) c.maxLights = atoi(value);
        else if (_stricmp(key, "ConeAngle") == 0) c.coneAngleDeg = static_cast<float>(atof(value));
        else if (_stricmp(key, "Forward") == 0) c.forwardOffset = static_cast<float>(atof(value));
        else if (_stricmp(key, "Side") == 0) c.sideOffset = static_cast<float>(atof(value));
        else if (_stricmp(key, "Pitch") == 0) c.pitchDegrees = static_cast<float>(atof(value));
    }
    fclose(f);
    g_dirty = false;
    Log("synthetic lights: loaded %s", g_path);
}

void SyntheticLightsSave() {
    EnsureBinding();
    FILE* f = fopen(g_path, "w");
    if (!f) {
        Log("synthetic lights: could not write %s", g_path);
        return;
    }
    fprintf(f, "; GTA2 RTX Remix -- lights for effects the game does not light itself.\n");
    fprintf(f, ";\n");
    fprintf(f, "; Bind lines map a particle type id to a category. GTA2 gives every particle\n");
    fprintf(f, "; a type at +0x38; which id is which effect is not written down anywhere, so\n");
    fprintf(f, "; the F4 menu's Effects tab lists the ids as they appear and you bind them by\n");
    fprintf(f, "; triggering the effect and watching which one lights up.\n");
    fprintf(f, ";\n");
    fprintf(f, "; Categories: ");
    for (int i = 0; i < kSynthCategoryCount; ++i) {
        fprintf(f, "%d=%s%s", i, kCategoryNames[i], i + 1 < kSynthCategoryCount ? ", " : "\n");
    }
    fprintf(f, "; Headlights take no particle types -- they come off the vehicle list.\n");

    fprintf(f, "\n[general]\n");
    fprintf(f, "Enabled=%d\n", g_settings.enabled ? 1 : 0);
    fprintf(f, "DrivenWindowMs=%u\n", g_settings.drivenWindowMs);
    fprintf(f, "DrivenMinMovement=%.5f\n", g_settings.drivenMinMovement);
    for (int t = 0; t < kMaxParticleType; ++t) {
        if (g_binding[t] >= 0) fprintf(f, "Bind=%02X %d\n", t, g_binding[t]);
    }

    for (int i = 0; i < kSynthCategoryCount; ++i) {
        const SyntheticCategorySettings& c = g_settings.category[i];
        fprintf(f, "\n[%s]\n", kCategoryNames[i]);
        fprintf(f, "Enabled=%d\n", c.enabled ? 1 : 0);
        fprintf(f, "Color=%.4f %.4f %.4f\n", c.rgb[0], c.rgb[1], c.rgb[2]);
        fprintf(f, "Intensity=%.3f\n", c.intensity);
        fprintf(f, "Radius=%.3f\n", c.radius);
        fprintf(f, "Height=%.3f\n", c.heightOffset);
        if (i == kSynthHeadlight) {
            fprintf(f, "ConeAngle=%.2f\n", c.coneAngleDeg);
            fprintf(f, "Forward=%.3f\n", c.forwardOffset);
            fprintf(f, "Side=%.3f\n", c.sideOffset);
            fprintf(f, "Pitch=%.2f\n", c.pitchDegrees);
        }
    }
    fclose(f);
    g_dirty = false;
    Log("synthetic lights: saved %s", g_path);
}

}  // namespace gta2dx9
