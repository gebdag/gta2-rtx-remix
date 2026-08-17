// Bridges GTA2's own lights into the RTX Remix scene.
//
// GTA2 already computes every light in the frame and hands it to the renderer
// DLL: gbh_ResetLights opens the list, gbh_AddLight adds one, gbh_SetAmbient
// sets the floor brightness. The descriptors are world space - the same tile
// coordinates the world mesh is built in - so nothing has to be unprojected.
// The original D3D renderer used them for per-vertex lighting; here they become
// real Remix sphere lights instead. See docs/lighting-analysis.md.
//
// Remix keeps two collections for API lights: the ones created, keyed by the
// hash in their info, and a per-frame list of which to actually render. The
// second is cleared every frame, so a light is in the scene exactly when
// DrawLightInstance is called for it that frame. That gives the module its
// shape:
//
//   LightsBeginCollect()   gbh_ResetLights - the game is about to list the frame
//   LightsAddRaw()         gbh_AddLight - one descriptor, decoded and stashed
//   LightsReconcile()      once per rendered frame: match, create, update, expire
//   LightsDraw()           DrawLightInstance for everything live, before Present
//
// Handle work is kept out of AddRaw deliberately: creating or destroying
// mid-list would land after the draw pass had already run for that light.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gta2dx9 {

// Where a light came from. The game's own lights all arrive through
// gbh_AddLight; everything above kLightSourceGame is invented by
// synthetic_lights.cpp from effects GTA2 draws but does not light.
enum LightSource : uint8_t {
    kLightSourceGame = 0,
    kLightSourceMuzzle,
    kLightSourceBullet,
    kLightSourceSpark,
    kLightSourceCigarette,
    kLightSourceFire,
    kLightSourceHeadlight,
    kLightSourceCount
};

const char* LightSourceName(uint8_t source);

// One light, already converted into renderer world space and 0..1 colour.
struct RemixLightDesc {
    float pos[3] = {};      // world units: +X east, +Y up, +Z north
    float rgb[3] = {};      // 0..1
    float intensity = 0.0f; // 0..1
    float radius = 0.0f;    // reach in tiles; drives brightness, not emitter size

    // Cone shaping, for headlights. dir is a unit vector in world space.
    bool  spot = false;
    float dir[3] = {};
    float coneAngleDeg = 0.0f;

    uint8_t source = kLightSourceGame;

    // When non-zero this light carries its own identity and is matched on it
    // rather than by position or proximity. Headlights use it so a car's beams
    // keep one handle for as long as the car exists, however far it drives.
    uint64_t explicitId = 0;
};

// A user's per-light edits, persisted to gta2dx9_lights.ini beside the game.
// Keyed by the light's placement and the district it is in, so an entry keeps
// pointing at the same lamp across runs.
struct RemixLightOverride {
    bool  disabled = false;
    float intensity = 1.0f;   // multiplier on the game's value
    float offset[3] = {};
    bool  recolor = false;
    float color[3] = {1.0f, 1.0f, 1.0f};
    float emitterRadius = 0.0f;  // 0 falls back to the global default

    // Free text, so a file full of hashes stays readable.
    std::string note;

    bool IsDefault() const;
};

// One light being tracked, as the menu sees it.
struct RemixTrackedLight {
    uint64_t id = 0;             // session identity; this is the Remix hash
    uint64_t key = 0;            // identity that survives a restart, 0 if it moves
    RemixLightDesc def;
    void* handle = nullptr;      // remixapi_LightHandle, opaque here
    bool moving = false;         // has been seen at more than one position
    bool drawn = false;          // reached Remix last frame
    bool dirty = true;           // needs redefining before the next draw
    bool applyFailed = false;    // CreateLight refused it; stop retrying
    unsigned updates = 0;
    uint32_t lastSeenFrame = 0;
    uint32_t firstFrame = 0;
};

// Global tuning, all of it exposed in the F4 menu.
struct RemixLightSettings {
    bool  enabled = true;
    bool  injectStatic = true;
    bool  injectMoving = true;   // vehicle headlights, muzzle flashes, train lamps

    // Radiance = colour * intensity * radianceScale * (radius/reference)^exponent.
    //
    // radianceScale is the master brightness and the knob that matters: GTA2
    // sets 96% of its map lights to intensity 1.0, so the game's own intensity
    // carries almost no variation. What variation there is lives in the radius
    // (2, 3 and 4 tiles account for 78% of them) and in the colour, which is
    // why the reach term below is the one that spreads the lights apart.
    float radianceScale = 20.0f;
    float movingScale = 1.0f;    // extra multiplier for lights that move
    float referenceRadius = 3.0f;  // the modal map light radius, in tiles
    float radiusExponent = 1.0f;

    // GTA2's palette is far more saturated than anything real - pure #FF8000
    // sodium, pure #00FFFF neon - and a path tracer bouncing that around a room
    // exaggerates it further. Pulls each colour towards its own luminance, so
    // desaturating does not also darken. 1 leaves the game's colours alone.
    float saturation = 1.0f;

    // Physical size of the emitting sphere in world units (1 unit = 1 map tile).
    // NOT the game's radius: that is the light's reach, and using it here would
    // put a glowing ball the width of the street around every lamp.
    float emitterRadius = 0.15f;

    // The game blinks a light by taking its intensity to zero. Emitting those
    // would leave a traffic light glowing red, amber and green at once.
    float minIntensity = 0.004f;

    // GTA2's ambient is an additive floor, which a path tracer has no direct
    // equivalent for. Optionally stand one up as a dim overhead distant light.
    bool  ambientFill = false;
    float ambientFillScale = 1.0f;

    // Softness of the headlight cone edge, 0 is a hard rim.
    float coneSoftness = 0.35f;
};

void LightsInit(const char* overridePath);
void LightsShutdown();

RemixLightSettings& LightsSettings();

// --- The game's stream ---

// gbh_ResetLights: the game is about to list this frame's lights.
void LightsBeginCollect();

// gbh_AddLight: one 20-byte descriptor, exactly as the game built it.
//   +0x00 u32   byte0 intensity 0..255, byte1 radius (tiles*32), byte2 enable
//   +0x04 float x   tiles east
//   +0x08 float y   tiles south
//   +0x0C float z   tiles up (map level)
//   +0x10 u32   0x00RRGGBB
void LightsAddRaw(const void* descriptor);

// gbh_SetAmbient, in the game's own 0..1 scale.
void LightsSetAmbient(float ambient);

// --- Lights we invent ---
//
// Kept in a list of their own rather than appended to the game's, because the
// game's is discarded when it goes stale (menus, load screens) and these are
// rebuilt from live state every frame regardless.

// Clears the invented list. Call once per frame before submitting.
void LightsBeginExtra();
void LightsSubmitExtra(const RemixLightDesc& desc);

// Which district is loaded. Salts the override keys so two maps cannot share an
// entry just because a lamp happens to stand in the same place in both.
void LightsSetScene(const char* name);

// --- Per-frame ---

// Matches this frame's list against the tracked set, then creates, redefines and
// expires handles. Once per rendered frame, after the game has listed its lights.
void LightsReconcile();

// DrawLightInstance for every live handle. Must run before Present.
void LightsDraw();

// --- Menu support ---

const std::vector<RemixTrackedLight>& LightsTracked();
float LightsAmbient();
const char* LightsScene();

RemixLightOverride* LightsFindOverride(uint64_t key);

// Returns the entry for editing and marks the file dirty. Only call when
// something actually changed: merely inspecting a light must not create a row.
RemixLightOverride& LightsEditOverride(uint64_t key);

void LightsEraseOverride(uint64_t key);
void LightsClearOverrides();
void LightsSaveOverrides();
void LightsLoadOverrides();
const std::map<uint64_t, RemixLightOverride>& LightsOverrides();
bool LightsOverridesDirty();
const char* LightsOverridePath();

// Marks one light for redefining from its current state and overrides, or every
// light when key is 0. The handle is kept: Remix redefines a light when it is
// described again under the same hash, so there is no frame in which it is
// missing.
void LightsInvalidate(uint64_t key);

// Paints one light magenta so it can be picked out of the scene. Beats a
// disable, so a light already switched off can still be located. 0 clears.
void LightsIdentify(uint64_t key);
uint64_t LightsIdentified();

struct RemixLightStats {
    int tracked = 0;
    int drawnLastFrame = 0;
    int submittedLastFrame = 0;   // what the game listed
    int moving = 0;
    int suppressedByOverride = 0;
    int suppressedByClass = 0;
    int suppressedByIntensity = 0;
    int bySource[kLightSourceCount] = {};   // tracked lights, by where they came from
    unsigned created = 0;
    unsigned destroyed = 0;
    unsigned updated = 0;
    unsigned applyFailures = 0;
    int lastApplyError = 0;
    // What the game is actually asking for right now, so "does GTA2 vary its
    // light intensity at all?" can be answered by looking rather than guessing.
    float minIntensitySeen = 0.0f;
    float maxIntensitySeen = 0.0f;
    int atFullIntensity = 0;
    uint32_t collectsSeen = 0;    // how many times the game opened a light list
    uint32_t framesSinceCollect = 0;
};
const RemixLightStats& LightsStats();

}  // namespace gta2dx9
