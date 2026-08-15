// Converts a GTA2 block map into true 3D world-space triangle geometry.
//
// World frame is left-handed, one block = one unit:
//   +X east (map x), +Y up (map level), +Z south (map y).
//
// Nothing here is screen-relative: vertices are absolute world positions, which
// is what RTX Remix needs to build acceleration structures. Geometry is grouped
// by tile so each tile becomes its own draw call with its own texture, giving
// Remix a stable per-tile hash to key asset replacements off.
#pragma once

#include <cstdint>
#include <vector>

#include "gta2_map.h"
#include "gta2_style.h"

namespace gta2 {

struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

// One draw call: all faces in the map that use a single tile.
struct TileBatch {
    int tile = 0;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStart = 0;
    uint32_t vertexCount = 0;
    bool needsAlphaTest = false;
};

struct WorldMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<TileBatch> batches;
};

// One entry of gta2.exe's slope descriptor table at 0x00662DB0, twelve bytes
// apiece, indexed by slope type. It is built at startup rather than stored in
// the image, so it has to be read out of the running game; when it is not
// available the mesh builder falls back to deriving the same values.
//
//   direction  1 = rises north, 2 = south, 3 = west, 4 = east (0 = not a ramp)
//   steps      how many blocks the whole ramp climbs over
//   step       which one of them this block is
struct SlopeInfo {
    uint8_t direction = 0;
    uint8_t steps = 0;
    uint8_t step = 0;
};

constexpr int kSlopeTypeCount = 64;

// Where the cut falls in a partial or corner block (slope types 53-61), as a
// fraction of the cell. Read from the game alongside the slope table; the
// defaults are the quarter and three-quarter marks the shapes imply.
struct PartialCuts {
    float low = 0.25f;
    float high = 0.75f;
};

// slopes may be null, in which case the ramp geometry is derived instead.
void BuildWorldMesh(const Map& map, const Style& style, const SlopeInfo* slopes,
                    const PartialCuts& cuts, WorldMesh* out);

}  // namespace gta2
