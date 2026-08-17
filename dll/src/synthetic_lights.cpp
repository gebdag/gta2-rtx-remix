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
        float gx, gy, gz;
        game::ReadPlacement(entry, &gx, &gy, &gz);

        if (static_cast<unsigned>(type) < kMaxParticleType && InsideWorld(gx, gy, gz)) {
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

        float gx, gy, gz;
        game::ReadPlacement(entry, &gx, &gy, &gz);
        if (!InsideWorld(gx, gy, gz)) {
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

        // Parked cars stay dark. A car waiting at a junction keeps its beams for
        // the length of the window, which is what stops them flickering off at
        // every red light.
        const bool driven = track.lastMovedTick != 0
                         && now - track.lastMovedTick <= g_settings.drivenWindowMs;
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
        if (!game::ReadFacing(entry, &fx, &fy)) {
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
