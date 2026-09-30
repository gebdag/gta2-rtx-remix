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

#include <array>
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

// Corner heights of a block's lid above its own base, in the order NW, NE, SE,
// SW - matching the block's world corners (xWest,zNorth), (xEast,zNorth),
// (xEast,zSouth), (xWest,zSouth). A flat lid is {1,1,1,1}.
//
// Shared with the sprite pass, which samples the same surface this builds so a
// sprite conforms to the floor that is actually drawn under it.
using CornerHeights = std::array<float, 4>;
CornerHeights LidHeights(const SlopeInfo& slope);

// Fills out[kSlopeTypeCount] with the game's descriptors where they were read
// and derived ones where they were not. Every consumer of the slope table goes
// through this, so none of them can disagree about a ramp.
void ResolveSlopeTable(const SlopeInfo* slopes, SlopeInfo* out);

// The batch that seals the world from underneath.
//
// GTA2's map is not a closed solid. A column stores only the blocks it actually
// has, every block is a shell of the faces the original renderer needed to draw,
// and where the artwork used the colour-key index there are slits straight
// through a wall. None of that mattered against a black background; under a path
// tracer with a sky, every one of them is a hole with the sky behind it.
//
// Rather than trying to work out which faces the map is missing - which cannot
// be done reliably, because a fence block and a solid block look alike from the
// map data - one black quad goes underneath the whole city, well below the
// lowest block. Anything looking down through a hole then lands on that instead
// of on the sky, which is what the original showed. It cannot clip anything
// above ground, because it is not above ground.
//
// It gets a tile index one past the style's own so it is its own batch, its own
// texture and its own stable Remix hash, rather than being smuggled into some
// real tile's draw call.
inline int SealTileIndex(const Style& style) { return style.TileCount(); }

// The batch for the riverbed, two past the style's own tiles - see
// SetRiverbedDepth. Its own texture and so its own stable Remix hash, for a sand
// material to replace.
inline int RiverbedTileIndex(const Style& style) { return style.TileCount() + 1; }

// How far below level 0 the seal sits, and how far past the map it reaches. The
// overhang is what stops a camera near the edge of the city seeing sky under its
// own feet.
constexpr float kSealDepth = 2.0f;
constexpr float kSealOverhang = 512.0f;

// On by default. Off leaves the world exactly as the map describes it, which is
// the way to see what the seal was hiding. Read when the mesh is built, so a
// change wants a level reload.
void SetWorldSeal(bool on);
bool WorldSeal();

// How many blocks one repeat of a water tile's texture spans.
//
// GTA2 lays each water tile on its block like any other lid: one whole tile per
// block, turned however the face word says. Under the original texture that
// reads as open water. A replacement water material - an animated wave sheet
// through Remix's sprite_sheet_* inputs - shows it for what it is: the same few
// metres of waves stamped once per block, a visible grid, with seams wherever a
// lid is turned. So a water lid (Style::IsWaterTile) instead takes its texture
// coordinates from its position in the world, one repeat every this many
// blocks, the same way up everywhere. Remix wraps them itself (frac, with the
// gradients kept continuous), and so does the D3D9 sampler.
//
// A block is about 4 m (rtx.sceneScale), so the default 2.5 makes one repeat
// 10 m. 0 turns it off and gives water the ordinary per-block coordinates.
// Read when the mesh is built, so a change wants a level reload.
constexpr float kDefaultWaterUvBlocks = 2.5f;
void SetWaterUvBlocks(float blocks);
float WaterUvBlocks();

// A riverbed under the water, this many blocks below each water lid.
//
// GTA2's water is a lid with nothing under it; the original never showed
// anything through it. Through a translucent water material the path tracer
// looks straight down to the seal, three blocks below the sea, and the water
// reads as black. So every water cell gets a sand floor under it - deep enough
// that a sinking sprite still has water around it before it gets there - and so
// does every cell within kRiverbedPad of one. The quays have no walls below the
// waterline, so a view at an angle past a quay edge would otherwise find the
// black seal under the land. Under land the pad is hidden inside the ground.
//
// Where cells of different water heights meet, the lower bed wins, so no water
// surface ever has sand closer than the depth under it. 0 is no riverbed. Read
// when the mesh is built, so a change wants a level reload.
constexpr float kDefaultRiverbedDepth = 1.0f;
constexpr int kRiverbedPad = 1;
void SetRiverbedDepth(float blocks);
float RiverbedDepth();

// slopes may be null, in which case the ramp geometry is derived instead.
void BuildWorldMesh(const Map& map, const Style& style, const SlopeInfo* slopes,
                    const PartialCuts& cuts, WorldMesh* out);

}  // namespace gta2
