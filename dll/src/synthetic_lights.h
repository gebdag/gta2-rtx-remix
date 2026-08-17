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

    // A vehicle counts as driven while it has moved within this window. Covers
    // waiting at a junction without lighting up every parked car in the city.
    unsigned drivenWindowMs = 2500;
    float drivenMinMovement = 0.004f;   // tiles per frame; below this it is parked
};

SyntheticSettings& SyntheticLightsSettings();

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
};

// Ordered by type id so the menu list does not reshuffle under the cursor.
const std::vector<ParticleTypeInfo>& SyntheticParticleTypes();
void SyntheticForgetParticleTypes();

struct SyntheticStats {
    int particlesWalked = 0;
    int vehiclesWalked = 0;
    int vehiclesDriven = 0;
    int emitted[kSynthCategoryCount] = {};
    int capped[kSynthCategoryCount] = {};   // wanted a light, hit the ceiling
    bool particleListFound = false;
    bool vehicleListFound = false;
    bool trigTablesReady = false;
};
const SyntheticStats& SyntheticLightsStats();

}  // namespace gta2dx9
