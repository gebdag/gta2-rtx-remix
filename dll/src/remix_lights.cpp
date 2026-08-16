#include "remix_lights.h"

#include "log.h"
#include "remix_api.h"

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

RemixLightSettings g_settings;

std::vector<RemixTrackedLight>         g_tracked;
std::vector<RemixLightDesc>            g_pending;     // this frame's list, as submitted
std::map<uint64_t, RemixLightOverride> g_overrides;   // ordered: the menu lists these

RemixLightStats g_stats;
RemixLightSettings g_applied;      // what the tracked lights were last built from
uint32_t g_settingsDirtyAt = 0;

uint32_t g_frame = 0;
uint32_t g_collects = 0;           // gbh_ResetLights calls seen
uint32_t g_collectsAtLastReconcile = 0;
uint32_t g_framesSinceCollect = 0;
bool     g_collecting = false;

float    g_ambient = 0.0f;
uint64_t g_identify = 0;
uint64_t g_nextId = 1;
uint64_t g_sceneSalt = 0;
char     g_sceneName[64] = "";

bool g_overridesDirty = false;
char g_overridePath[MAX_PATH] = "gta2dx9_lights.ini";

void* g_ambientHandle = nullptr;
const uint64_t kAmbientFillId = 0xA33B1E27F111ULL;

// A light the game stopped listing is gone, but a frame of slack keeps one that
// arrives slightly out of step from flickering. GTA2 lists its lights once per
// world render; menus and load screens list none at all, and those must expire.
const uint32_t kExpireFrames = 2;

// How far a light may move between frames and still be recognised as the same
// one. A car at full speed covers well under a tile per frame; two lamps that
// close together are indistinguishable anyway.
const float kMoveMatchDistance = 3.0f;

// Radiance the identify highlight burns at - findable in a lit street without
// washing the street out.
const float kIdentifyRadiance = 40.0f;

uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ULL) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

// Positions come from the game's 16.14 fixed point, so they are exact; the
// quantisation is only insurance against a float round-trip.
int64_t Quantise(float v) { return static_cast<int64_t>(std::floor(v * 256.0f + 0.5f)); }

// Identity that survives a restart: where the light stands, how far it reaches,
// and which district it is in. Deliberately not the colour - a traffic light
// keeps its identity through red, amber and green.
uint64_t StableKey(const RemixLightDesc& d) {
    int64_t parts[4] = {Quantise(d.pos[0]), Quantise(d.pos[1]), Quantise(d.pos[2]),
                        Quantise(d.radius)};
    return HashBytes(parts, sizeof(parts), g_sceneSalt ? g_sceneSalt : 0xCBF29CE484222325ULL);
}

// Match key for the current frame: position alone. Static lights never move, so
// this pairs them up exactly; anything left over is matched by proximity.
uint64_t PositionKey(const RemixLightDesc& d) {
    int64_t parts[3] = {Quantise(d.pos[0]), Quantise(d.pos[1]), Quantise(d.pos[2])};
    return HashBytes(parts, sizeof(parts));
}

bool SameColourAndRadius(const RemixLightDesc& a, const RemixLightDesc& b) {
    return a.rgb[0] == b.rgb[0] && a.rgb[1] == b.rgb[1] && a.rgb[2] == b.rgb[2]
        && a.radius == b.radius;
}

bool SameDesc(const RemixLightDesc& a, const RemixLightDesc& b) {
    return SameColourAndRadius(a, b) && a.intensity == b.intensity && a.pos[0] == b.pos[0]
        && a.pos[1] == b.pos[1] && a.pos[2] == b.pos[2];
}

float DistanceSquared(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

remixapi_Float3D ToRemix(const float* v) {
    remixapi_Float3D out;
    out.x = v[0];
    out.y = v[1];
    out.z = v[2];
    return out;
}

void DestroyHandle(void*& handle) {
    if (!handle) return;
    if (const remixapi_Interface* api = RemixApi()) {
        api->DestroyLight(static_cast<remixapi_LightHandle>(handle));
    }
    handle = nullptr;
    ++g_stats.destroyed;
}

void ClearAll() {
    for (RemixTrackedLight& t : g_tracked) {
        DestroyHandle(t.handle);
        t.drawn = false;
    }
    g_tracked.clear();
    DestroyHandle(g_ambientHandle);
}

// Remix derives a light's handle from the hash in its info and overwrites a
// matching entry, so redefining a light means describing it again under the same
// hash. There is no update call, and destroying first would remove the very
// light being redefined - the light would blink out for a frame.
void Apply(RemixTrackedLight& t) {
    const remixapi_Interface* api = RemixApi();
    if (!api) return;

    const RemixLightOverride* ov = LightsFindOverride(t.key);
    const bool identified = t.key != 0 && t.key == g_identify;

    float pos[3];
    float rgb[3];
    for (int i = 0; i < 3; ++i) {
        pos[i] = t.def.pos[i] + (ov ? ov->offset[i] : 0.0f);
        rgb[i] = (ov && ov->recolor) ? ov->color[i] : t.def.rgb[i];
    }

    // The game's radius is the light's reach, not its size. Feeding it to the
    // emitter radius would put a glowing ball the width of the street around
    // every lamp, so it drives brightness instead and the path tracer's own
    // inverse-square falloff replaces GTA2's linear ramp and hard cutoff.
    float scale = t.def.intensity * g_settings.radianceScale;
    if (t.def.radius > 0.0f && g_settings.referenceRadius > 0.0f
        && g_settings.radiusExponent != 0.0f) {
        scale *= std::pow(t.def.radius / g_settings.referenceRadius, g_settings.radiusExponent);
    }
    if (t.moving) scale *= g_settings.movingScale;
    if (ov) scale *= ov->intensity;

    // Pull towards luminance rather than towards grey: mixing in white would
    // brighten as it desaturates, and every light would drift lighter as the
    // slider comes down.
    if (g_settings.saturation != 1.0f) {
        const float luma = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
        for (int i = 0; i < 3; ++i) {
            rgb[i] = luma + (rgb[i] - luma) * g_settings.saturation;
            if (rgb[i] < 0.0f) rgb[i] = 0.0f;   // saturation above 1 can overshoot
            if (rgb[i] > 1.0f) rgb[i] = 1.0f;
        }
    }

    if (identified) {
        rgb[0] = 1.0f; rgb[1] = 0.0f; rgb[2] = 1.0f;
        scale = kIdentifyRadiance;
    }

    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = t.id;
    info.radiance = remixapi_Float3D{rgb[0] * scale, rgb[1] * scale, rgb[2] * scale};
    // Remix puts a light it considers static to sleep after a few frames, as a
    // defence against games that ramp intensity every frame. GTA2's street lamps
    // genuinely never move, which is exactly the case that would silently go
    // dark, so every light is declared dynamic.
    info.isDynamic = 1;

    remixapi_LightInfoSphereEXT sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = ToRemix(pos);
    sphere.radius = (ov && ov->emitterRadius > 0.0f) ? ov->emitterRadius
                                                     : g_settings.emitterRadius;
    sphere.volumetricRadianceScale = 1.0f;
    info.pNext = &sphere;

    const bool existed = t.handle != nullptr;
    remixapi_LightHandle handle = static_cast<remixapi_LightHandle>(t.handle);
    const remixapi_ErrorCode rc = api->CreateLight(&info, &handle);
    if (rc != REMIXAPI_ERROR_CODE_SUCCESS) {
        // Keep whatever handle the light already had: it is still live in Remix
        // and still needs destroying later. Latch the failure so a broken API
        // costs one call per light rather than one per frame.
        t.applyFailed = true;
        ++g_stats.applyFailures;
        g_stats.lastApplyError = static_cast<int>(rc);
        return;
    }
    t.applyFailed = false;
    t.handle = handle;
    if (existed) {
        ++t.updates;
        ++g_stats.updated;
    } else {
        ++g_stats.created;
    }
}

// GTA2's ambient is a constant added to every lit vertex, which a path tracer
// has no equivalent for. This stands one dim overhead distant light in for it,
// which is not what the game did but is the nearest honest approximation.
void ApplyAmbientFill() {
    const remixapi_Interface* api = RemixApi();
    if (!api) return;

    const float level = g_ambient * g_settings.ambientFillScale;
    if (!g_settings.enabled || !g_settings.ambientFill || level <= 0.0f) {
        DestroyHandle(g_ambientHandle);
        return;
    }

    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = kAmbientFillId;
    info.radiance = remixapi_Float3D{level, level, level};
    info.isDynamic = 1;

    remixapi_LightInfoDistantEXT distant = {};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = remixapi_Float3D{0.0f, -1.0f, 0.0f};   // straight down
    distant.angularDiameterDegrees = 60.0f;                    // broad and soft
    distant.volumetricRadianceScale = 1.0f;
    info.pNext = &distant;

    remixapi_LightHandle handle = static_cast<remixapi_LightHandle>(g_ambientHandle);
    if (api->CreateLight(&info, &handle) == REMIXAPI_ERROR_CODE_SUCCESS) {
        g_ambientHandle = handle;
    }
}

bool SameSettings(const RemixLightSettings& a, const RemixLightSettings& b) {
    // Field by field rather than memcmp: the struct has padding between its
    // bools and floats that nothing ever writes.
    return a.enabled == b.enabled && a.injectStatic == b.injectStatic
        && a.injectMoving == b.injectMoving && a.radianceScale == b.radianceScale
        && a.movingScale == b.movingScale && a.referenceRadius == b.referenceRadius
        && a.radiusExponent == b.radiusExponent && a.emitterRadius == b.emitterRadius
        && a.minIntensity == b.minIntensity && a.saturation == b.saturation
        && a.ambientFill == b.ambientFill && a.ambientFillScale == b.ambientFillScale;
}

char* TrimInPlace(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

}  // namespace

bool RemixLightOverride::IsDefault() const {
    // The note alone is worth keeping: it is how a light gets labelled before
    // anything is actually changed about it.
    return !disabled && intensity == 1.0f && !recolor && emitterRadius == 0.0f
        && offset[0] == 0.0f && offset[1] == 0.0f && offset[2] == 0.0f && note.empty();
}

void LightsInit(const char* overridePath) {
    if (overridePath && *overridePath) {
        strncpy(g_overridePath, overridePath, sizeof(g_overridePath) - 1);
        g_overridePath[sizeof(g_overridePath) - 1] = '\0';
    }
    g_applied = g_settings;
    LightsLoadOverrides();
}

void LightsShutdown() {
    // Deliberately not saved here. Quitting is not a decision to keep what was
    // being tried out; the Save button is.
    ClearAll();
    g_pending.clear();
}

RemixLightSettings& LightsSettings() { return g_settings; }

void LightsBeginCollect() {
    g_pending.clear();
    g_collecting = true;
    ++g_collects;
}

void LightsAddRaw(const void* descriptor) {
    if (!descriptor || !g_collecting) return;

    // 20 bytes, laid out by gta2.exe!FUN_00461400 and decoded by
    // d3ddll.dll!gbh_AddLight. See docs/lighting-analysis.md.
    const uint8_t* p = static_cast<const uint8_t*>(descriptor);
    uint32_t packed = 0, rgb = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    memcpy(&packed, p + 0x00, 4);
    memcpy(&x, p + 0x04, 4);
    memcpy(&y, p + 0x08, 4);
    memcpy(&z, p + 0x0C, 4);
    memcpy(&rgb, p + 0x10, 4);

    // The original renderer skips anything without bit 0x10000 set and bit
    // 0x20000 clear. Nothing in the game ever sets 0x20000, but honouring the
    // test costs nothing and keeps us equivalent.
    if ((packed & 0x30000u) != 0x10000u) return;

    RemixLightDesc d;
    d.intensity = static_cast<float>(packed & 0xFFu) / 255.0f;
    // The renderer's own conversion: the byte holds radius*32, and 255 maps to
    // the 8-tile maximum.
    d.radius = static_cast<float>((packed >> 8) & 0xFFu) / 255.0f * 8.0f;
    d.rgb[0] = static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f;
    d.rgb[1] = static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f;
    d.rgb[2] = static_cast<float>(rgb & 0xFFu) / 255.0f;

    // Same axis mapping the sprite path uses: the game's x is east, y is south
    // and z is the map level, while the world mesh is +X east, +Y up, +Z north
    // with the rows mirrored.
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    if (x < -8.0f || x > gta2::kMapWidth + 8.0f || y < -8.0f || y > gta2::kMapHeight + 8.0f
        || z < -4.0f || z > 12.0f) {
        return;
    }
    d.pos[0] = x;
    d.pos[1] = z;
    d.pos[2] = static_cast<float>(gta2::kMapHeight) - y;

    g_pending.push_back(d);
}

void LightsSetAmbient(float ambient) { g_ambient = ambient; }

void LightsSetScene(const char* name) {
    const char* safe = name ? name : "";
    if (strncmp(safe, g_sceneName, sizeof(g_sceneName) - 1) == 0) return;
    strncpy(g_sceneName, safe, sizeof(g_sceneName) - 1);
    g_sceneName[sizeof(g_sceneName) - 1] = '\0';
    g_sceneSalt = *safe ? HashBytes(safe, strlen(safe)) : 0;
    // The old district's lights belong to a world that is no longer there.
    ClearAll();
    Log("remix lights: district '%s'", g_sceneName);
}

float LightsAmbient() { return g_ambient; }
const char* LightsScene() { return g_sceneName; }

void LightsReconcile() {
    ++g_frame;

    if (!g_settings.enabled) {
        if (!g_tracked.empty() || g_ambientHandle) ClearAll();
        g_stats.tracked = 0;
        g_stats.drawnLastFrame = 0;
        g_stats.submittedLastFrame = 0;
        return;
    }

    // Did the game open a list since the last time round? In menus and on load
    // screens gbh_ResetLights is never called, and the previous frame's lights
    // must not linger there. A frame of slack absorbs any jitter between the
    // game's world pass and ours.
    const bool fresh = g_collects != g_collectsAtLastReconcile;
    if (fresh) {
        g_collectsAtLastReconcile = g_collects;
        g_framesSinceCollect = 0;
    } else {
        ++g_framesSinceCollect;
    }
    const bool listValid = fresh || g_framesSinceCollect <= kExpireFrames;
    if (!listValid) g_pending.clear();

    g_stats.submittedLastFrame = static_cast<int>(g_pending.size());
    g_stats.suppressedByClass = 0;
    g_stats.suppressedByIntensity = 0;

    // --- Match this frame's list onto the tracked set ---------------------
    //
    // Static lights are matched on position, which is exact and unique for
    // almost all of them; a traffic light keeps its identity through a colour
    // change that way. Whatever is left is a light that moved - a headlight on a
    // car - and is matched to the nearest tracked light of the same colour and
    // reach.
    std::vector<bool> claimed(g_tracked.size(), false);
    std::vector<int> match(g_pending.size(), -1);

    std::unordered_multimap<uint64_t, size_t> byPosition;
    byPosition.reserve(g_tracked.size() * 2);
    for (size_t i = 0; i < g_tracked.size(); ++i) {
        byPosition.emplace(PositionKey(g_tracked[i].def), i);
    }

    for (size_t p = 0; p < g_pending.size(); ++p) {
        const uint64_t key = PositionKey(g_pending[p]);
        auto range = byPosition.equal_range(key);
        int fallback = -1;
        for (auto it = range.first; it != range.second; ++it) {
            const size_t i = it->second;
            if (claimed[i]) continue;
            if (SameColourAndRadius(g_tracked[i].def, g_pending[p])) {
                match[p] = static_cast<int>(i);
                break;
            }
            // Same spot but recoloured - a traffic light changing phase. Take it
            // only if no exact candidate turns up.
            if (fallback < 0) fallback = static_cast<int>(i);
        }
        if (match[p] < 0 && fallback >= 0) match[p] = fallback;
        if (match[p] >= 0) claimed[match[p]] = true;
    }

    for (size_t p = 0; p < g_pending.size(); ++p) {
        if (match[p] >= 0) continue;
        int best = -1;
        float bestDistance = kMoveMatchDistance * kMoveMatchDistance;
        for (size_t i = 0; i < g_tracked.size(); ++i) {
            if (claimed[i] || !SameColourAndRadius(g_tracked[i].def, g_pending[p])) continue;
            const float d2 = DistanceSquared(g_tracked[i].def.pos, g_pending[p].pos);
            if (d2 < bestDistance) {
                bestDistance = d2;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0) {
            match[p] = best;
            claimed[best] = true;
        }
    }

    // --- Fold the matches in ----------------------------------------------
    for (size_t p = 0; p < g_pending.size(); ++p) {
        if (match[p] >= 0) {
            RemixTrackedLight& t = g_tracked[match[p]];
            if (!SameDesc(t.def, g_pending[p])) {
                // A light that has been seen somewhere else is a moving one, and
                // moving lights get no persistent override key: there is nothing
                // stable to hang one on.
                if (t.def.pos[0] != g_pending[p].pos[0] || t.def.pos[1] != g_pending[p].pos[1]
                    || t.def.pos[2] != g_pending[p].pos[2]) {
                    t.moving = true;
                    t.key = 0;
                }
                t.def = g_pending[p];
                t.dirty = true;
            }
            t.lastSeenFrame = g_frame;
            continue;
        }
        RemixTrackedLight t;
        t.id = g_nextId++;
        t.def = g_pending[p];
        t.key = StableKey(t.def);
        t.lastSeenFrame = g_frame;
        t.firstFrame = g_frame;
        g_tracked.push_back(t);
    }

    // --- Expire ------------------------------------------------------------
    for (size_t i = 0; i < g_tracked.size();) {
        if (g_tracked[i].lastSeenFrame + kExpireFrames < g_frame) {
            DestroyHandle(g_tracked[i].handle);
            g_tracked[i] = g_tracked.back();
            g_tracked.pop_back();
        } else {
            ++i;
        }
    }

    // Global tuning is not attached to any one light, so a change here means
    // every light has to be described again. Dragging a slider changes it on
    // most frames; redefining hundreds of lights that often would flood the
    // bridge, so the work is coalesced into one pass every 50 ms. That is still
    // twenty updates a second, which reads as immediate.
    const uint32_t now = GetTickCount();
    if (!SameSettings(g_settings, g_applied)) {
        g_applied = g_settings;
        if (!g_settingsDirtyAt) g_settingsDirtyAt = now ? now : 1;
    }
    bool applyAll = false;
    if (g_settingsDirtyAt && now - g_settingsDirtyAt >= 50) {
        g_settingsDirtyAt = 0;
        applyAll = true;
    }

    // --- Create, redefine, suppress ----------------------------------------
    g_stats.suppressedByOverride = 0;
    g_stats.moving = 0;
    for (RemixTrackedLight& t : g_tracked) {
        if (t.moving) ++g_stats.moving;

        const RemixLightOverride* ov = LightsFindOverride(t.key);
        const bool identified = t.key != 0 && t.key == g_identify;

        bool suppressed = false;
        if (ov && ov->disabled && !identified) {
            ++g_stats.suppressedByOverride;
            suppressed = true;
        } else if (!(t.moving ? g_settings.injectMoving : g_settings.injectStatic)) {
            ++g_stats.suppressedByClass;
            suppressed = true;
        } else if (t.def.intensity < g_settings.minIntensity && !identified) {
            // The game blinks a light by taking its intensity to zero, so this is
            // how a traffic light stops glowing on all three colours at once.
            ++g_stats.suppressedByIntensity;
            suppressed = true;
        }

        if (suppressed) {
            DestroyHandle(t.handle);
            t.drawn = false;
            t.dirty = true;   // so it comes straight back when it is allowed to
            continue;
        }

        if (!t.handle || t.dirty || applyAll) {
            if (!t.handle && t.applyFailed) continue;  // refused once; do not hammer the bridge
            Apply(t);
            t.dirty = false;
        }
    }

    ApplyAmbientFill();

    g_stats.tracked = static_cast<int>(g_tracked.size());
    g_stats.collectsSeen = g_collects;
    g_stats.framesSinceCollect = g_framesSinceCollect;

    g_stats.minIntensitySeen = g_tracked.empty() ? 0.0f : 1.0f;
    g_stats.maxIntensitySeen = 0.0f;
    g_stats.atFullIntensity = 0;
    for (const RemixTrackedLight& t : g_tracked) {
        if (t.def.intensity < g_stats.minIntensitySeen) g_stats.minIntensitySeen = t.def.intensity;
        if (t.def.intensity > g_stats.maxIntensitySeen) g_stats.maxIntensitySeen = t.def.intensity;
        if (t.def.intensity >= 1.0f) ++g_stats.atFullIntensity;
    }

    // The first light through is the moment the whole path is proven, and its
    // position is the one thing that cannot be checked from inside the game: a
    // wrong axis mapping would light the far side of the city with no error
    // anywhere. Logged once, then periodically, so it can be lined up against
    // the camera bounds already in the log.
    static bool announced = false;
    if (!announced && !g_tracked.empty()) {
        announced = true;
        const RemixTrackedLight& t = g_tracked.front();
        Log("remix lights: first light -- pos %.2f,%.2f,%.2f rgb %.2f,%.2f,%.2f intensity %.2f "
            "reach %.2f tiles (%d submitted, Remix %s)",
            t.def.pos[0], t.def.pos[1], t.def.pos[2], t.def.rgb[0], t.def.rgb[1], t.def.rgb[2],
            t.def.intensity, t.def.radius, g_stats.submittedLastFrame,
            RemixApiAvailable() ? "available" : "ABSENT");
    }
    if ((g_frame & 0xFF) == 0 && (g_stats.submittedLastFrame || g_stats.tracked)) {
        Log("remix lights @%u: submitted %d, tracked %d (%d moving), drawn %d, ambient %.3f, "
            "created %u updated %u destroyed %u failures %u",
            g_frame, g_stats.submittedLastFrame, g_stats.tracked, g_stats.moving,
            g_stats.drawnLastFrame, g_ambient, g_stats.created, g_stats.updated,
            g_stats.destroyed, g_stats.applyFailures);
    }
}

void LightsDraw() {
    const remixapi_Interface* api = RemixApi();
    if (!api || !g_settings.enabled) {
        g_stats.drawnLastFrame = 0;
        return;
    }
    int drawn = 0;
    for (RemixTrackedLight& t : g_tracked) {
        if (!t.handle) {
            t.drawn = false;
            continue;
        }
        api->DrawLightInstance(static_cast<remixapi_LightHandle>(t.handle));
        t.drawn = true;
        ++drawn;
    }
    if (g_ambientHandle) {
        api->DrawLightInstance(static_cast<remixapi_LightHandle>(g_ambientHandle));
    }
    g_stats.drawnLastFrame = drawn;
}

const std::vector<RemixTrackedLight>& LightsTracked() { return g_tracked; }

RemixLightOverride* LightsFindOverride(uint64_t key) {
    if (key == 0) return nullptr;
    auto it = g_overrides.find(key);
    return it == g_overrides.end() ? nullptr : &it->second;
}

RemixLightOverride& LightsEditOverride(uint64_t key) {
    g_overridesDirty = true;
    return g_overrides[key];
}

void LightsEraseOverride(uint64_t key) {
    if (g_overrides.erase(key)) g_overridesDirty = true;
}

void LightsClearOverrides() {
    if (!g_overrides.empty()) {
        g_overrides.clear();
        g_overridesDirty = true;
    }
}

const std::map<uint64_t, RemixLightOverride>& LightsOverrides() { return g_overrides; }
bool LightsOverridesDirty() { return g_overridesDirty; }
const char* LightsOverridePath() { return g_overridePath; }

void LightsInvalidate(uint64_t key) {
    for (RemixTrackedLight& t : g_tracked) {
        if (key == 0 || t.key == key) {
            t.dirty = true;
            t.applyFailed = false;
        }
    }
}

void LightsIdentify(uint64_t key) {
    if (g_identify == key) return;
    // Both the old and the new light need redefining: one to drop the highlight,
    // one to take it.
    for (RemixTrackedLight& t : g_tracked) {
        if (t.key == g_identify || t.key == key) {
            t.dirty = true;
            t.applyFailed = false;
        }
    }
    g_identify = key;
}

uint64_t LightsIdentified() { return g_identify; }

const RemixLightStats& LightsStats() { return g_stats; }

void LightsLoadOverrides() {
    g_overrides.clear();
    g_overridesDirty = false;

    FILE* f = fopen(g_overridePath, "r");
    if (!f) return;   // first run, not a failure

    RemixLightOverride* current = nullptr;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* s = TrimInPlace(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (!close) continue;
            *close = '\0';
            const uint64_t key = _strtoui64(s + 1, nullptr, 16);
            current = key ? &g_overrides[key] : nullptr;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq || !current) continue;
        *eq = '\0';
        const char* name = TrimInPlace(s);
        char* value = TrimInPlace(eq + 1);

        if (_stricmp(name, "Disabled") == 0) {
            current->disabled = atoi(value) != 0;
        } else if (_stricmp(name, "Intensity") == 0) {
            current->intensity = static_cast<float>(atof(value));
        } else if (_stricmp(name, "Offset") == 0) {
            sscanf(value, "%f %f %f", &current->offset[0], &current->offset[1],
                   &current->offset[2]);
        } else if (_stricmp(name, "Color") == 0) {
            current->recolor = true;
            sscanf(value, "%f %f %f", &current->color[0], &current->color[1], &current->color[2]);
        } else if (_stricmp(name, "EmitterRadius") == 0) {
            current->emitterRadius = static_cast<float>(atof(value));
        } else if (_stricmp(name, "Note") == 0) {
            current->note = value;
        }
    }
    fclose(f);
    Log("remix lights: loaded %d override(s) from %s", static_cast<int>(g_overrides.size()),
        g_overridePath);
}

void LightsSaveOverrides() {
    // An entry carrying nothing is indistinguishable from no entry, and keeping
    // it would grow the file every time a light is merely inspected.
    for (auto it = g_overrides.begin(); it != g_overrides.end();) {
        it = it->second.IsDefault() ? g_overrides.erase(it) : ++it;
    }

    FILE* f = fopen(g_overridePath, "w");
    if (!f) {
        Log("remix lights: could not write %s", g_overridePath);
        return;
    }

    fprintf(f, "; GTA2 RTX Remix -- per-light overrides.\n");
    fprintf(f, ";\n");
    fprintf(f, "; Each section is a hash of one light's district, position and reach, so an\n");
    fprintf(f, "; entry keeps pointing at the same lamp across sessions. Lights that move --\n");
    fprintf(f, "; headlights, muzzle flashes -- have no stable identity and cannot be given\n");
    fprintf(f, "; one of these; tune those with the global sliders instead.\n");
    fprintf(f, ";\n");
    fprintf(f, "; Written only when Save is pressed in the F4 menu, so experiments are not\n");
    fprintf(f, "; kept by accident. Hand edits are picked up by Reload; anything left at its\n");
    fprintf(f, "; default is dropped.\n");
    fprintf(f, ";\n");
    fprintf(f, ";   Disabled=1              remove the light from the scene\n");
    fprintf(f, ";   Intensity=1.500         multiplier on the game's brightness\n");
    fprintf(f, ";   Offset=0 0.5 0          world-space nudge, in map tiles\n");
    fprintf(f, ";   Color=1.0 0.8 0.6       replaces the game's colour, 0..1\n");
    fprintf(f, ";   EmitterRadius=0.30      sphere size for this light only, in tiles\n");
    fprintf(f, ";   Note=text               free text, shown in the menu\n");

    for (const auto& entry : g_overrides) {
        const RemixLightOverride& ov = entry.second;
        fprintf(f, "\n[%016llX]\n", static_cast<unsigned long long>(entry.first));
        if (!ov.note.empty()) fprintf(f, "Note=%s\n", ov.note.c_str());
        if (ov.disabled) fprintf(f, "Disabled=1\n");
        if (ov.intensity != 1.0f) fprintf(f, "Intensity=%.3f\n", ov.intensity);
        if (ov.offset[0] || ov.offset[1] || ov.offset[2]) {
            fprintf(f, "Offset=%.3f %.3f %.3f\n", ov.offset[0], ov.offset[1], ov.offset[2]);
        }
        if (ov.recolor) {
            fprintf(f, "Color=%.4f %.4f %.4f\n", ov.color[0], ov.color[1], ov.color[2]);
        }
        if (ov.emitterRadius > 0.0f) fprintf(f, "EmitterRadius=%.3f\n", ov.emitterRadius);
    }
    fclose(f);
    g_overridesDirty = false;
    Log("remix lights: saved %d override(s) to %s", static_cast<int>(g_overrides.size()),
        g_overridePath);
}

}  // namespace gta2dx9
