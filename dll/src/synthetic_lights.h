// Lights for the things GTA2 draws but never lights.
//
// The lights in remix_lights.cpp are the game's own: street lamps, traffic
// lights, headlamp objects. Gunfire, bullets in flight, the sparks off a car
// scraping a wall and the cigarette a bored pedestrian lights are all pure
// sprites - they were never light sources, because the original renderer had no
// dynamic lighting worth the name. Under a path tracer they should be.
//
// So these are invented, from state read straight out of the running game:
//
//   particles   the manager at 0x00669E70 is its own pool; walk the live list
//               and each entry carries a position and a type id
//   vehicles    the pool at 0x005E4CA0, same shape; each carries a position and
//               a heading, which is what a headlight cone needs
//
// Two things are deliberately decided at runtime rather than baked in. Which
// particle type is which effect is bound in the F4 menu and saved, because
// reading it out of fifteen spawner functions is slow and silently wrong when
// it is wrong, while watching the type appear the moment you pull a trigger
// cannot be. And "is this car being driven" is answered by whether it has moved
// recently, which needs no reverse engineering and covers the player and the
// traffic alike.
#pragma once

#include <cstdint>
#include <vector>

namespace gta2dx9 {

// One effect category. Indexes match LightSource minus one.
enum SyntheticCategory {
    kSynthMuzzle = 0,
    kSynthBullet,
    kSynthSpark,
    kSynthCigarette,
    kSynthFire,
    kSynthHeadlight,
    kSynthCategoryCount
};

const char* SyntheticCategoryName(int category);

// Particle type ids run small - everything observed is under 0x40 - but the
// field is a full int, so the bindable range is capped rather than assumed.
constexpr int kMaxParticleType = 256;

struct SyntheticCategorySettings {
    bool  enabled = true;
    float rgb[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 2.0f;       // reach in tiles, fed to the same radiance curve
    float heightOffset = 0.0f; // tiles above the effect's own position

    // Every emitted light is a CreateLight across the 32-bit Remix bridge, and
    // some particle types come in bursts of dozens. Binding one of those without
    // a ceiling turns a frame into hundreds of round trips.
    int maxLights = 24;

    // Headlights only.
    float coneAngleDeg = 32.0f;
    float forwardOffset = 0.45f;  // from the car's centre to its nose, in tiles
    float sideOffset = 0.22f;     // half the spacing between the two beams
    float pitchDegrees = 8.0f;    // tilted down towards the road
};

struct SyntheticSettings {
    bool enabled = true;
    SyntheticCategorySettings category[kSynthCategoryCount];

    // A vehicle counts as driven when someone is sitting in it. That is the
    // honest test and it needs no window: a car waiting at a red light with a
    // driver in it keeps its beams, and a parked one never gets them.
    bool drivenNeedsDriver = true;
    // Kept as an alternative rather than deleted: a car being pushed, or one
    // whose occupant field ever turns out to mean something else, still reads as
    // driven if it is moving.
    bool drivenAllowsMovement = false;
    unsigned drivenWindowMs = 2500;
    float drivenMinMovement = 0.004f;   // tiles per frame; below this it is parked
};

SyntheticSettings& SyntheticLightsSettings();

// A GTA2 car model is a fixed sprite, and one cone width cannot suit a bike, a
// bus and a squad car. The model id sits at vehicle+0x84, so the beam geometry
// can be tuned per model and saved. Zero on any field means "use the category
// default", which is what every model starts on.
struct BeamOverride {
    float coneAngleDeg = 0.0f;
    float sideOffset = 0.0f;
    float forwardOffset = 0.0f;
};

// Models seen this session, so the menu can list them rather than making you
// guess an id. Ordered by id.
struct VehicleModelInfo {
    int model = 0;
    int seen = 0;        // live right now
    int driven = 0;
    unsigned lastSeenTick = 0;
};
const std::vector<VehicleModelInfo>& SyntheticVehicleModels();

BeamOverride* SyntheticFindBeam(int model);        // null when unset
BeamOverride& SyntheticEditBeam(int model);
void SyntheticEraseBeam(int model);

// Paints one model's beams magenta so the model id in the list can be matched to
// the car on screen. -1 clears.
void SyntheticHighlightModel(int model);
int  SyntheticHighlightedModel();

// Which particle types feed which category. A type may be bound to at most one.
// kSynthHeadlight takes no types - it comes off the vehicle list.
int  SyntheticTypeBinding(int particleType);        // category, or -1
void SyntheticBindType(int particleType, int category);   // -1 unbinds

// Walks the game's particle and vehicle lists and submits this frame's invented
// lights. Call once per frame, before LightsReconcile.
void SyntheticLightsUpdate();

void SyntheticLightsLoad(const char* path);
void SyntheticLightsSave();
const char* SyntheticLightsPath();
bool SyntheticLightsDirty();
void SyntheticLightsMarkDirty();

// --- The inspector ---
//
// What the game is running right now, so a type can be bound by watching it
// appear rather than by guessing. This is the whole reason the binding is a
// setting: fire a gun and the new id is the one that lights up.

struct ParticleTypeInfo {
    int type = 0;
    int liveNow = 0;
    unsigned totalSeen = 0;
    unsigned lastSeenTick = 0;
    unsigned firstSeenTick = 0;
    float lastPos[3] = {};    // renderer world space
    int lastLife = 0;

    // What the type *behaves* like, which is what actually tells the effects
    // apart. A bullet is fast and travels; a spark arrives in a burst, moves a
    // little and dies at once; a fire sits still for a long time; a cigarette
    // sits still on its own. None of that is legible in a decompiler.
    unsigned spawns = 0;
    int      maxBurst = 0;      // most that appeared in a single frame
    int      maxLive = 0;
    float    meanSpeed = 0.0f;  // tiles per frame
    float    peakSpeed = 0.0f;
    float    meanLife = 0.0f;   // frames, from the game's own countdown
    float    meanHeight = 0.0f; // map level
    // Smoke rises and fire does not, which is the one measurement that tells
    // those two apart - they otherwise look identical: long-lived, clustered,
    // many at once.
    float    meanRise = 0.0f;   // tiles per frame, signed
    double   riseSum = 0.0;
    double   speedSum = 0.0;
    unsigned speedSamples = 0;
    double   lifeSum = 0.0;
    double   heightSum = 0.0;
    unsigned lifeSamples = 0;
};

// Ordered by type id so the menu list does not reshuffle under the cursor.
const std::vector<ParticleTypeInfo>& SyntheticParticleTypes();
void SyntheticForgetParticleTypes();

// Paints every particle of one type with a hard magenta light, whatever it is
// bound to. This is how a type gets identified: the numbers narrow it down, but
// only looking at the screen while one type is lit tells you whether that is the
// fire, the smoke above it, or the litter on the pavement. 0 clears.
void SyntheticHighlightType(int particleType);
int  SyntheticHighlightedType();

struct SyntheticStats {
    int particlesWalked = 0;
    int vehiclesWalked = 0;
    int vehiclesDriven = 0;
    int emitted[kSynthCategoryCount] = {};
    int capped[kSynthCategoryCount] = {};   // wanted a light, hit the ceiling
    // Cumulative, because the per-frame count is a single instant and a spark
    // burst is gone in three frames: sampling it almost always reads zero and
    // makes a working category look dead.
    unsigned totalEmitted[kSynthCategoryCount] = {};
    bool particleListFound = false;
    bool vehicleListFound = false;
    bool trigTablesReady = false;
};
const SyntheticStats& SyntheticLightsStats();

// --- Structure probe ---
//
// Where a field sits inside a particle or a vehicle was read off a decompiler,
// and a wrong offset produces a light in the wrong place with nothing to say so.
// This finds them from the running game instead: it walks the lists and scores
// every offset by how often it holds something that could only be what we are
// looking for.
//
// The camera position is the key. A vehicle on screen is within a dozen tiles
// of it, so the offset whose 16.14 value tracks the camera across many vehicles
// is the position - no other field behaves like that. The driver pointer is
// found the same way: the offset that is non-null on cars that move and null on
// cars that do not.
//
// Results go to gta2dx9.log. Runs on demand from the F4 menu, and once
// automatically a few seconds into a level so a plain run leaves the evidence
// behind without anyone having to ask for it.
void SyntheticProbe(const char* reason);
bool SyntheticProbeHasRun();

}  // namespace gta2dx9
