// The floor under a point, sampled from the same map the static mesh is built
// from.
//
// The sprite pass needs this to stand a quad on the ground rather than at a
// guessed height above it. It deliberately reads the *map* rather than the
// game's draw stream: the stream only carries what is currently visible, and a
// sprite has to be placed correctly the frame it appears.
//
// Everything here works in renderer world coordinates - x east, y up, z north -
// and converts to the map's own frame internally, so callers never juggle the
// mirror.
#pragma once

#include <array>

#include "../../src/gta2_map.h"
#include "../../src/world_mesh.h"

namespace gta2dx9 {

// A plane fitted to the lid under a sprite's footprint.
//
// The normal is what the sprite is rotated onto and the height is where it is
// stood; `residual` is how far the worst sample pokes above the plane, which is
// zero on any single ramp block and grows only where a footprint straddles a
// kerb or a block edge. That is the case a plane cannot describe, so the
// residual is added back as clearance rather than pretended away.
struct GroundPlane {
    bool valid = false;
    float y = 0.0f;                     // plane height at the query centre
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;  // unit normal, always pointing up
    float residual = 0.0f;              // worst sample above the plane, >= 0
    int samples = 0;                    // how many of the query points found a lid
};

class GroundSampler {
public:
    // resolvedSlopes must already have been through gta2::ResolveSlopeTable.
    // The map pointer is held, not copied, so it has to outlive the sampler -
    // both live in WorldView, which is what makes that true.
    void Reset(const gta2::Map* map, const gta2::SlopeInfo* resolvedSlopes);
    void Clear() { map_ = nullptr; }
    bool Ready() const { return map_ && !map_->Empty(); }

    // Height of the highest lid surface at or below `ceiling` under a point.
    // False when the column has nothing under that ceiling - open water, a hole,
    // or a sprite below the bottom of its column.
    bool SurfaceAt(float x, float z, float ceiling, float* outY) const;

    // Fits a plane to the lid under `count` points. Needs three that resolve;
    // below that there is no plane and the caller falls back.
    GroundPlane Fit(const float* xs, const float* zs, int count, float ceiling,
                    float centreX, float centreZ) const;

    // How much higher than a sprite's own level a lid may be and still count as
    // the floor it is standing on.
    //
    // It has to be generous enough for the uphill end of a ramp the sprite is
    // partly on - half a block covers every 26 degree ramp - and strictly less
    // than one block, because one block is exactly the gap to the next storey.
    // At 0.75 a car half onto a ramp still fits one plane, and a pedestrian
    // beside a building never picks up its roof.
    static constexpr float kCeilingHeadroom = 0.75f;

private:
    const gta2::Map* map_ = nullptr;
    std::array<gta2::SlopeInfo, gta2::kSlopeTypeCount> slopes_ = {};
};

}  // namespace gta2dx9
