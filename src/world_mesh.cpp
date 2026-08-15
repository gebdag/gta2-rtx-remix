#include "world_mesh.h"

#include <array>
#include <cmath>
#include <utility>

namespace gta2 {
namespace {

// Tiles are packed edge-to-edge in 256x256 pages, so sampling must stay half a
// texel inside the tile or neighbours bleed in at the seams.
constexpr float kUvInset = 0.5f / kTileSize;
constexpr float kUvMin = kUvInset;
constexpr float kUvMax = 1.0f - kUvInset;

// Flat faces are decals sitting on a neighbouring block's surface; nudging them
// along their normal keeps them off the coincident wall plane.
constexpr float kFlatFaceOffset = 0.002f;

struct Uv {
    float u, v;
};

// Corner heights within a block, in the order NW, NE, SE, SW.
using CornerHeights = std::array<float, 4>;

// The heights the game itself uses for a ramp block, taken straight from its
// slope descriptor (gta2.exe!FUN_00471ce0): the raised edge sits at
// (steps - step)/steps and the opposite one a single step lower. Deriving
// "step + 1" instead - the obvious reading - runs every ramp backwards.
CornerHeights RampHeights(const SlopeInfo& slope) {
    if (slope.direction < 1 || slope.direction > 4 || slope.steps == 0) {
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }
    const float steps = static_cast<float>(slope.steps);
    const float high = static_cast<float>(slope.steps - slope.step) / steps;
    const float low = static_cast<float>(slope.steps - slope.step - 1) / steps;
    switch (slope.direction) {
        case 1: return {high, high, low, low};   // rises north
        case 2: return {low, low, high, high};   // rises south
        case 3: return {high, low, low, high};   // rises west
        default: return {low, high, high, low};  // rises east
    }
}

// Used only when the game's table has not been read; reproduces the same
// families it builds at startup: 1-8 climb in two steps, 9-40 in eight,
// 41-44 in one, in the order north, south, west, east.
SlopeInfo DeriveSlope(int slopeType) {
    SlopeInfo slope;
    if (slopeType >= 1 && slopeType <= 8) {
        slope.direction = static_cast<uint8_t>((slopeType - 1) / 2 + 1);
        slope.steps = 2;
        slope.step = static_cast<uint8_t>((slopeType - 1) % 2);
    } else if (slopeType >= 9 && slopeType <= 40) {
        slope.direction = static_cast<uint8_t>((slopeType - 9) / 8 + 1);
        slope.steps = 8;
        slope.step = static_cast<uint8_t>((slopeType - 9) % 8);
    } else if (slopeType >= 41 && slopeType <= 44) {
        slope.direction = static_cast<uint8_t>(slopeType - 41 + 1);
        slope.steps = 1;
        slope.step = 0;
    }
    return slope;
}

CornerHeights LidHeights(const SlopeInfo& slope) { return RampHeights(slope); }

// Orientation tables copied verbatim out of gta2.exe. Each face type has its
// own: the game indexes them with the face word's bits 13-15 and passes the
// result to gbh_DrawTile as orientation flags. Walls therefore carry a built-in
// rotation relative to lids, and use flip bit 0x10 where lids use 0x08 - which
// is why applying the lid's rules to walls scrambles wall artwork even when the
// tile itself is correct.
//
// Addresses are for the default view-rotation state (0xFF); the other states
// also permute corner positions and are not handled here.
constexpr uint8_t kLidFlags[8] = {0x00, 0x08, 0x20, 0x28, 0x40, 0x48, 0x60, 0x68};     // 0x593154
constexpr uint8_t kLeftFlags[8] = {0x23, 0x33, 0x43, 0x53, 0x63, 0x73, 0x03, 0x13};    // 0x5930D4
constexpr uint8_t kRightFlags[8] = {0x64, 0x74, 0x04, 0x14, 0x24, 0x34, 0x44, 0x54};   // 0x5930F4
constexpr uint8_t kTopFlags[8] = {0x41, 0x49, 0x61, 0x69, 0x01, 0x09, 0x21, 0x29};     // 0x593114
constexpr uint8_t kBottomFlags[8] = {0x02, 0x0A, 0x22, 0x2A, 0x42, 0x4A, 0x62, 0x6A};  // 0x593134

// Diagonal blocks draw their lid as a triangle, and each of the four facings
// gets its own table (gta2.exe!FUN_0046dfe0, cases 0..3).
constexpr uint8_t kDiagLidDropNw[8] = {0x65, 0x75, 0x05, 0x15, 0x25, 0x35, 0x45, 0x55};  // 0x5931B4
constexpr uint8_t kDiagLidDropSe[8] = {0x25, 0x35, 0x45, 0x55, 0x65, 0x75, 0x05, 0x15};  // 0x593194
constexpr uint8_t kDiagLidDropNeSw[8] = {0x05, 0x0D, 0x25, 0x2D, 0x45, 0x4D, 0x65, 0x6D};  // 0x5931D4

// Maps a fraction of the tile onto the inset range, so a face covering only
// part of its cell shows only the matching part of the artwork.
float UvAt(float fraction) { return kUvMin + fraction * (kUvMax - kUvMin); }

// Reproduces gbh_DrawTile's orientation handling exactly: the flags permute the
// four corners' texture coordinates, and 180 degrees is expressed as "apply both
// flips" rather than as a rotation. Doing arithmetic on the coordinates instead
// gives the right artwork in the wrong orientation.
//
// Stated in its general form, starting from an arbitrary sub-rectangle of the
// tile rather than the whole of it: partial blocks need that - a slab a quarter
// of a cell deep shows a quarter of the tile, not the tile squeezed into a
// quarter - and the permutation afterwards is identical either way.
std::array<Uv, 4> FaceUvsRect(const Face& face, const uint8_t (&table)[8], float u0, float u1,
                              float v0, float v1) {
    std::array<Uv, 4> uv = {{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};

    unsigned flags = table[(face.raw >> 13) & 7];
    switch (flags & 0x60) {
        case 0x20: {  // one quarter turn: every corner takes the previous one's UV
            const Uv last = uv[3];
            uv[3] = uv[2];
            uv[2] = uv[1];
            uv[1] = uv[0];
            uv[0] = last;
            break;
        }
        case 0x40:  // half turn is expressed as both flips
            flags ^= 0x18;
            break;
        case 0x60: {  // three quarter turns: the same shift in reverse
            const Uv first = uv[0];
            uv[0] = uv[1];
            uv[1] = uv[2];
            uv[2] = uv[3];
            uv[3] = first;
            break;
        }
        default:
            break;
    }
    if (flags & 0x8) {
        std::swap(uv[0], uv[1]);
        std::swap(uv[2], uv[3]);
    }
    if (flags & 0x10) {
        std::swap(uv[0], uv[3]);
        std::swap(uv[1], uv[2]);
    }
    return uv;
}

std::array<Uv, 4> FaceUvs(const Face& face, const uint8_t (&table)[8]) {
    return FaceUvsRect(face, table, kUvMin, kUvMax, kUvMin, kUvMax);
}

struct Vec3 {
    float x, y, z;
};

class MeshBuilder {
public:
    explicit MeshBuilder(const Style& style) : style_(style) {
        perTile_.resize(style.TileCount());
        perTileTriangle_.resize(style.TileCount());
    }

    // Corners are given in the original renderer's own vertex-slot order for
    // that face, since the orientation tables are written against it.
    void AddFace(const Face& face, const std::array<Vec3, 4>& corners, Vec3 normal,
                 const uint8_t (&table)[8]) {
        AddFaceUv(face, corners, normal, FaceUvs(face, table));
    }

    // Ramps bypass the orientation tables entirely: the game writes texture
    // coordinates in by hand so a shortened wall shows the *bottom* of its tile
    // with the top cropped off, rather than the whole tile squashed into a
    // shorter quad (gta2.exe!FUN_0046c2c0, the DAT_006633b8 = 0x4005 path).
    void AddFaceUv(const Face& face, const std::array<Vec3, 4>& corners, Vec3 normal,
                   const std::array<Uv, 4>& uv) {
        const int tile = face.Tile();
        if (tile <= 0 || tile >= static_cast<int>(perTile_.size())) return;

        const float push = face.IsFlat() ? kFlatFaceOffset : 0.0f;

        // Slot order differs per face, so winding is derived from the geometry
        // rather than assumed: if the corners wind away from the outward normal,
        // the quad is emitted reversed.
        const Vec3 edge1{corners[1].x - corners[0].x, corners[1].y - corners[0].y,
                         corners[1].z - corners[0].z};
        const Vec3 edge2{corners[2].x - corners[0].x, corners[2].y - corners[0].y,
                         corners[2].z - corners[0].z};
        const Vec3 cross{edge1.y * edge2.z - edge1.z * edge2.y,
                         edge1.z * edge2.x - edge1.x * edge2.z,
                         edge1.x * edge2.y - edge1.y * edge2.x};
        const bool reversed =
            cross.x * normal.x + cross.y * normal.y + cross.z * normal.z < 0.0f;

        std::vector<Vertex>& out = perTile_[tile];
        static const int kForward[4] = {0, 1, 2, 3};
        static const int kReverse[4] = {0, 3, 2, 1};
        const int* order = reversed ? kReverse : kForward;
        for (int i = 0; i < 4; ++i) {
            const int slot = order[i];
            out.push_back(Vertex{corners[slot].x + normal.x * push,
                                 corners[slot].y + normal.y * push,
                                 corners[slot].z + normal.z * push,
                                 normal.x, normal.y, normal.z,
                                 uv[slot].u, uv[slot].v});
        }
    }

    // The cut face of a corner ramp is a genuine triangle, not a collapsed quad
    // (the game draws it through gbh_DrawTriangle, not gbh_DrawTile).
    void AddTriangle(const Face& face, const std::array<Vec3, 3>& corners,
                     const std::array<Uv, 3>& uv) {
        const int tile = face.Tile();
        if (tile <= 0 || tile >= static_cast<int>(perTile_.size())) return;

        const Vec3 edge1{corners[1].x - corners[0].x, corners[1].y - corners[0].y,
                         corners[1].z - corners[0].z};
        const Vec3 edge2{corners[2].x - corners[0].x, corners[2].y - corners[0].y,
                         corners[2].z - corners[0].z};
        Vec3 normal{edge1.y * edge2.z - edge1.z * edge2.y, edge1.z * edge2.x - edge1.x * edge2.z,
                    edge1.x * edge2.y - edge1.y * edge2.x};
        const float length =
            std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (length < 1e-6f) return;
        normal = {normal.x / length, normal.y / length, normal.z / length};
        // It is a ramp surface, so the outward side is the one facing up; that
        // fixes the winding without having to know the slot order.
        const bool flip = normal.y < 0.0f;
        if (flip) normal = {-normal.x, -normal.y, -normal.z};

        static const int kForward[3] = {0, 1, 2};
        static const int kReverse[3] = {0, 2, 1};
        const int* order = flip ? kReverse : kForward;
        // Kept apart from the quads so each can be indexed by its own stride.
        std::vector<Vertex>& out = perTileTriangle_[tile];
        for (int i = 0; i < 3; ++i) {
            const int slot = order[i];
            out.push_back(Vertex{corners[slot].x, corners[slot].y, corners[slot].z, normal.x,
                                 normal.y, normal.z, uv[slot].u, uv[slot].v});
        }
    }

    void Flatten(WorldMesh* out) const {
        for (size_t tile = 0; tile < perTile_.size(); ++tile) {
            const std::vector<Vertex>& quads = perTile_[tile];
            const std::vector<Vertex>& tris = perTileTriangle_[tile];
            if (quads.empty() && tris.empty()) continue;

            TileBatch batch;
            batch.tile = static_cast<int>(tile);
            batch.vertexStart = static_cast<uint32_t>(out->vertices.size());
            batch.vertexCount = static_cast<uint32_t>(quads.size() + tris.size());
            batch.indexStart = static_cast<uint32_t>(out->indices.size());
            batch.needsAlphaTest = style_.GetTile(static_cast<int>(tile)).hasTransparency;

            out->vertices.insert(out->vertices.end(), quads.begin(), quads.end());
            // Corners arrive clockwise seen from outside, which is front-facing
            // under D3DCULL_CCW once the viewport flips Y.
            for (uint32_t quad = 0; quad < quads.size(); quad += 4) {
                const uint32_t base = batch.vertexStart + quad;
                out->indices.insert(out->indices.end(),
                                    {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
            }

            const uint32_t triStart = batch.vertexStart + static_cast<uint32_t>(quads.size());
            out->vertices.insert(out->vertices.end(), tris.begin(), tris.end());
            for (uint32_t tri = 0; tri < tris.size(); tri += 3) {
                const uint32_t base = triStart + tri;
                out->indices.insert(out->indices.end(), {base + 0, base + 1, base + 2});
            }

            batch.indexCount = static_cast<uint32_t>(out->indices.size()) - batch.indexStart;
            out->batches.push_back(batch);
        }
    }

private:
    const Style& style_;
    std::vector<std::vector<Vertex>> perTile_;
    std::vector<std::vector<Vertex>> perTileTriangle_;
};

// Texture coordinates for a wall whose top edge has been cut down by a ramp.
// v is 0 at the top of the tile and 1 at its foot, so a top corner at height h
// starts the tile h of the way down: the artwork keeps its footing and loses
// its head, which is what the game does.
float CropV(float height) { return kUvMin + (1.0f - height) * (kUvMax - kUvMin); }

// Slot layouts differ per face, so each wall states which of its slots carry
// the top edge and what the two heights there are.
std::array<Uv, 4> SlopedWallUvs(const Face& face, const int (&topSlots)[2], const float (&topHeights)[2],
                                const int (&uOrder)[4]) {
    const bool flipped = face.IsFlipped();
    std::array<Uv, 4> uv;
    for (int i = 0; i < 4; ++i) {
        const int u = flipped ? 1 - uOrder[i] : uOrder[i];
        uv[i].u = u ? kUvMax : kUvMin;
        uv[i].v = kUvMax;
    }
    uv[topSlots[0]].v = CropV(topHeights[0]);
    uv[topSlots[1]].v = CropV(topHeights[1]);
    return uv;
}

// A diagonal block is a triangular prism: two of its four side walls stay put,
// the third spans the cut, and the lid becomes a triangle. Which is which comes
// straight from the game's dispatch (gta2.exe!FUN_0046edd0), and the lid corner
// it drops agrees independently with the slot rotation FUN_0046dfe0 applies.
void AddDiagonalBlock(MeshBuilder* builder, const Block& block, int slopeType, float xWest,
                      float xEast, float zNorth, float zSouth, float base) {
    const float top = base + 1.0f;
    const Vec3 nw{xWest, top, zNorth}, ne{xEast, top, zNorth};
    const Vec3 se{xEast, top, zSouth}, sw{xWest, top, zSouth};
    const Vec3 nwB{xWest, base, zNorth}, neB{xEast, base, zNorth};
    const Vec3 seB{xEast, base, zSouth}, swB{xWest, base, zSouth};

    constexpr float kDiag = 0.70710678f;

    // face  = the block word whose tile the diagonal wall wears
    // aTop/aBase, bTop/bBase = the two ends of the cut
    // leftOrder = the cut is drawn with the left wall's slot order, not the right's
    const Face* face = nullptr;
    Vec3 aTop{}, aBase{}, bTop{}, bBase{}, normal{};
    bool leftOrder = true;
    std::array<Vec3, 4> lid{};
    const uint8_t (*lidTable)[8] = &kDiagLidDropNw;

    switch (slopeType) {
        case 45:  // solid to the east and south, cut faces north-west
            face = &block.left;
            aTop = ne; aBase = neB; bTop = sw; bBase = swB;
            normal = {-kDiag, 0.0f, kDiag};
            leftOrder = true;
            lid = {ne, se, sw, ne};
            lidTable = &kDiagLidDropNw;
            if (block.right) builder->AddFace(block.right, {{ne, neB, seB, se}}, {1.0f, 0.0f, 0.0f}, kRightFlags);
            if (block.bottom) builder->AddFace(block.bottom, {{sw, se, seB, swB}}, {0.0f, 0.0f, -1.0f}, kBottomFlags);
            break;
        case 46:  // solid to the west and south, cut faces north-east
            face = &block.right;
            aTop = nw; aBase = nwB; bTop = se; bBase = seB;
            normal = {kDiag, 0.0f, kDiag};
            leftOrder = false;
            lid = {nw, se, se, sw};
            lidTable = &kDiagLidDropNeSw;
            if (block.left) builder->AddFace(block.left, {{nwB, nw, sw, swB}}, {-1.0f, 0.0f, 0.0f}, kLeftFlags);
            if (block.bottom) builder->AddFace(block.bottom, {{sw, se, seB, swB}}, {0.0f, 0.0f, -1.0f}, kBottomFlags);
            break;
        case 47:  // solid to the east and north, cut faces south-west
            face = &block.left;
            aTop = nw; aBase = nwB; bTop = se; bBase = seB;
            normal = {-kDiag, 0.0f, -kDiag};
            leftOrder = true;
            lid = {nw, ne, se, nw};
            lidTable = &kDiagLidDropNeSw;
            if (block.right) builder->AddFace(block.right, {{ne, neB, seB, se}}, {1.0f, 0.0f, 0.0f}, kRightFlags);
            if (block.top) builder->AddFace(block.top, {{nwB, neB, ne, nw}}, {0.0f, 0.0f, 1.0f}, kTopFlags);
            break;
        default:  // 48: solid to the west and north, cut faces south-east
            face = &block.right;
            aTop = ne; aBase = neB; bTop = sw; bBase = swB;
            normal = {kDiag, 0.0f, -kDiag};
            leftOrder = false;
            lid = {sw, nw, ne, sw};
            lidTable = &kDiagLidDropSe;
            if (block.left) builder->AddFace(block.left, {{nwB, nw, sw, swB}}, {-1.0f, 0.0f, 0.0f}, kLeftFlags);
            if (block.top) builder->AddFace(block.top, {{nwB, neB, ne, nw}}, {0.0f, 0.0f, 1.0f}, kTopFlags);
            break;
    }

    if (face && *face) {
        const std::array<Vec3, 4> corners =
            leftOrder ? std::array<Vec3, 4>{{aBase, aTop, bTop, bBase}}
                      : std::array<Vec3, 4>{{aTop, aBase, bBase, bTop}};
        builder->AddFace(*face, corners, normal, leftOrder ? kLeftFlags : kRightFlags);
    }
    if (block.lid) builder->AddFace(block.lid, lid, {0.0f, 1.0f, 0.0f}, *lidTable);
}

// Slope types 49-52: a corner ramp. Two sides stay solid full height, the lid
// covers three quarters of the cell as a triangle, and the fourth corner is
// chamfered away by a slanted triangle running from the diagonal down to that
// corner's foot (gta2.exe 0x0046E910 and its three siblings, reached through the
// dispatcher FUN_0046ee40).
//
// Decoded from 0x0046E910, which is type 49: the east and south walls are drawn
// whole, then the left face's tile is drawn as a triangle whose corners are the
// north-east corner at the top, the north-west corner at its foot, and the
// south-west corner at the top, with texture coordinates (0,0), (32,64) and
// (64,0). The lid uses view-rotation state 0, which drops the north-west corner.
// The other three types are the same shape turned round, and they pair with the
// diagonals exactly: 49 with 45, 50 with 46, 51 with 47, 52 with 48.
void AddCornerRampBlock(MeshBuilder* builder, const Block& block, int slopeType, float xWest,
                        float xEast, float zNorth, float zSouth, float base) {
    const float top = base + 1.0f;
    // Corner order is north-west, north-east, south-east, south-west.
    const Vec3 high[4] = {{xWest, top, zNorth},
                          {xEast, top, zNorth},
                          {xEast, top, zSouth},
                          {xWest, top, zSouth}};
    const Vec3 low[4] = {{xWest, base, zNorth},
                         {xEast, base, zNorth},
                         {xEast, base, zSouth},
                         {xWest, base, zSouth}};

    int cut = 0;                  // the corner that is chamfered away
    const Face* face = nullptr;   // whose tile the slanted triangle wears
    const uint8_t (*lidTable)[8] = &kDiagLidDropNw;
    std::array<Vec3, 4> lid{};

    switch (slopeType) {
        case 49:  // solid to the east and south, corner cut off to the north-west
            cut = 0;
            face = &block.left;
            lidTable = &kDiagLidDropNw;
            lid = {high[1], high[2], high[3], high[1]};
            if (block.right) builder->AddFace(block.right, {{high[1], low[1], low[2], high[2]}}, {1.0f, 0.0f, 0.0f}, kRightFlags);
            if (block.bottom) builder->AddFace(block.bottom, {{high[3], high[2], low[2], low[3]}}, {0.0f, 0.0f, -1.0f}, kBottomFlags);
            break;
        case 50:  // solid to the west and south, cut to the north-east
            cut = 1;
            face = &block.right;
            lidTable = &kDiagLidDropNeSw;
            lid = {high[0], high[2], high[2], high[3]};
            if (block.left) builder->AddFace(block.left, {{low[0], high[0], high[3], low[3]}}, {-1.0f, 0.0f, 0.0f}, kLeftFlags);
            if (block.bottom) builder->AddFace(block.bottom, {{high[3], high[2], low[2], low[3]}}, {0.0f, 0.0f, -1.0f}, kBottomFlags);
            break;
        case 51:  // solid to the east and north, cut to the south-west
            cut = 3;
            face = &block.left;
            lidTable = &kDiagLidDropNeSw;
            lid = {high[0], high[1], high[2], high[0]};
            if (block.right) builder->AddFace(block.right, {{high[1], low[1], low[2], high[2]}}, {1.0f, 0.0f, 0.0f}, kRightFlags);
            if (block.top) builder->AddFace(block.top, {{low[0], low[1], high[1], high[0]}}, {0.0f, 0.0f, 1.0f}, kTopFlags);
            break;
        default:  // 52: solid to the west and north, cut to the south-east
            cut = 2;
            face = &block.right;
            lidTable = &kDiagLidDropSe;
            lid = {high[3], high[0], high[1], high[3]};
            if (block.left) builder->AddFace(block.left, {{low[0], high[0], high[3], low[3]}}, {-1.0f, 0.0f, 0.0f}, kLeftFlags);
            if (block.top) builder->AddFace(block.top, {{low[0], low[1], high[1], high[0]}}, {0.0f, 0.0f, 1.0f}, kTopFlags);
            break;
    }
    // The two ends of the diagonal are the cut corner's neighbours; the apex is
    // the cut corner itself, at the block's foot.
    if (face && *face) {
        const std::array<Vec3, 3> corners = {high[(cut + 1) & 3], low[cut], high[(cut + 3) & 3]};
        const std::array<Uv, 3> uv = {
            Uv{UvAt(0.0f), UvAt(0.0f)}, Uv{UvAt(0.5f), UvAt(1.0f)}, Uv{UvAt(1.0f), UvAt(0.0f)}};
        builder->AddTriangle(*face, corners, uv);
    }
    if (block.lid) builder->AddFace(block.lid, lid, {0.0f, 1.0f, 0.0f}, *lidTable);
}

// Slope types 53-61: a plain box occupying only part of its cell. Which part is
// decided by two cut fractions the game holds at 0x006634FC and 0x006635B8, and
// FUN_00471c30 spells the footprints out by passing them as the wall extents -
// 53 to 56 are slabs against each edge, 57 to 60 are corner posts, 61 is a post
// in the middle. Each face shows the matching part of its tile rather than the
// whole of it squeezed down; that the game draws the *unshortened* walls with
// the ordinary full-tile routine is what confirms the reading.
void AddPartialBlock(MeshBuilder* builder, const Block& block, int slopeType,
                     const PartialCuts& cuts, float bx, float zNorth, float base) {
    const float lo = cuts.low;
    const float hi = cuts.high;
    float x0 = 0.0f, x1 = 1.0f, y0 = 0.0f, y1 = 1.0f;
    switch (slopeType) {
        case 53: x1 = lo; break;                        // slab against the west edge
        case 54: x0 = hi; break;                        // east
        case 55: y1 = lo; break;                        // north
        case 56: y0 = hi; break;                        // south
        case 57: x1 = lo; y1 = lo; break;               // north-west post
        case 58: x0 = hi; y1 = lo; break;               // north-east
        case 59: x0 = hi; y0 = hi; break;               // south-east
        case 60: x1 = lo; y0 = hi; break;               // south-west
        default: x0 = lo; x1 = hi; y0 = lo; y1 = hi; break;  // 61, centred post
    }

    const float xWest = bx + x0;
    const float xEast = bx + x1;
    // Map rows run north to south, so a larger y is further south, which is a
    // smaller z once the row is mirrored.
    const float zN = zNorth - y0;
    const float zS = zNorth - y1;
    const float top = base + 1.0f;

    if (block.lid) {
        builder->AddFaceUv(block.lid,
                           {{{xWest, top, zN}, {xEast, top, zN}, {xEast, top, zS}, {xWest, top, zS}}},
                           {0.0f, 1.0f, 0.0f},
                           FaceUvsRect(block.lid, kLidFlags, UvAt(x0), UvAt(x1), UvAt(y0), UvAt(y1)));
    }
    // Only the axis a face runs along is cropped; its height is always whole.
    if (block.left) {
        builder->AddFaceUv(block.left,
                           {{{xWest, base, zN}, {xWest, top, zN}, {xWest, top, zS}, {xWest, base, zS}}},
                           {-1.0f, 0.0f, 0.0f},
                           FaceUvsRect(block.left, kLeftFlags, UvAt(y0), UvAt(y1), kUvMin, kUvMax));
    }
    if (block.right) {
        builder->AddFaceUv(block.right,
                           {{{xEast, top, zN}, {xEast, base, zN}, {xEast, base, zS}, {xEast, top, zS}}},
                           {1.0f, 0.0f, 0.0f},
                           FaceUvsRect(block.right, kRightFlags, UvAt(y0), UvAt(y1), kUvMin, kUvMax));
    }
    if (block.top) {
        builder->AddFaceUv(block.top,
                           {{{xWest, base, zN}, {xEast, base, zN}, {xEast, top, zN}, {xWest, top, zN}}},
                           {0.0f, 0.0f, 1.0f},
                           FaceUvsRect(block.top, kTopFlags, UvAt(x0), UvAt(x1), kUvMin, kUvMax));
    }
    if (block.bottom) {
        builder->AddFaceUv(block.bottom,
                           {{{xWest, top, zS}, {xEast, top, zS}, {xEast, base, zS}, {xWest, base, zS}}},
                           {0.0f, 0.0f, -1.0f},
                           FaceUvsRect(block.bottom, kBottomFlags, UvAt(x0), UvAt(x1), kUvMin, kUvMax));
    }
}

void AddBlock(MeshBuilder* builder, const Block& block, const SlopeInfo& slope,
              const PartialCuts& cuts, int bx, int by, int level) {
    // Map rows run north to south, so the row index is mirrored into +Z-is-north.
    // Keeping north positive makes the world frame agree with the left-handed
    // view frame; sharing the sign would mirror the whole city.
    const float xWest = static_cast<float>(bx);
    const float xEast = xWest + 1.0f;
    const float zSouth = static_cast<float>(kMapHeight - by - 1);
    const float zNorth = zSouth + 1.0f;
    const float base = static_cast<float>(level);

    const int slopeType = block.SlopeType();
    if (slopeType >= 45 && slopeType <= 48) {
        AddDiagonalBlock(builder, block, slopeType, xWest, xEast, zNorth, zSouth, base);
        return;
    }
    if (slopeType >= 49 && slopeType <= 52) {
        AddCornerRampBlock(builder, block, slopeType, xWest, xEast, zNorth, zSouth, base);
        return;
    }
    if (slopeType >= 53 && slopeType <= 61) {
        AddPartialBlock(builder, block, slopeType, cuts, xWest, zNorth, base);
        return;
    }

    const CornerHeights h = LidHeights(slope);
    const float yNW = base + h[0];
    const float yNE = base + h[1];
    const float ySE = base + h[2];
    const float ySW = base + h[3];
    const bool ramp = slope.direction >= 1 && slope.direction <= 4 && slope.steps != 0;

    // A flat face is a zero-thickness panel rather than one side of a solid
    // block, so when one of an opposing pair is flat the game puts *both* tiles
    // on that one panel, one facing each way (FUN_00471d60 -> FUN_00470060 and
    // its three siblings). Drawing only the flat one leaves the panel blank from
    // behind, which is where the gaps in fences and hoardings came from.
    auto solo = [](const Face& face, const Face& opposite) {
        return static_cast<bool>(face) && (!opposite || !opposite.IsFlat() || face.IsFlat());
    };
    // Whenever the opposite face is flat, its panel also carries this face's
    // tile on its back. With both flat that gives four quads - two panels, each
    // double sided - which is exactly what the game emits.
    auto backOfPanel = [](const Face& face, const Face& opposite) {
        return static_cast<bool>(face) && static_cast<bool>(opposite) && opposite.IsFlat();
    };

    // Vertex slots below follow the original's own ordering per face, recovered
    // from which slot each projection call writes.
    if (block.lid) {
        builder->AddFace(block.lid,
                         {{{xWest, yNW, zNorth},    // v0 (x,   y)
                           {xEast, yNE, zNorth},    // v1 (x+1, y)
                           {xEast, ySE, zSouth},    // v2 (x+1, y+1)
                           {xWest, ySW, zSouth}}},  // v3 (x,   y+1)
                         {0.0f, 1.0f, 0.0f}, kLidFlags);
    }

    const std::array<Vec3, 4> leftCorners = {{{xWest, base, zNorth},   // v0 bottom (x, y)
                                              {xWest, yNW, zNorth},    // v1 top    (x, y)
                                              {xWest, ySW, zSouth},    // v2 top    (x, y+1)
                                              {xWest, base, zSouth}}}; // v3 bottom (x, y+1)
    const std::array<Vec3, 4> rightCorners = {{{xEast, yNE, zNorth},    // v0 top    (x+1, y)
                                               {xEast, base, zNorth},   // v1 bottom (x+1, y)
                                               {xEast, base, zSouth},   // v2 bottom (x+1, y+1)
                                               {xEast, ySE, zSouth}}};  // v3 top    (x+1, y+1)
    const std::array<Vec3, 4> topCorners = {{{xWest, base, zNorth},   // v0 bottom (x,   y)
                                             {xEast, base, zNorth},   // v1 bottom (x+1, y)
                                             {xEast, yNE, zNorth},    // v2 top    (x+1, y)
                                             {xWest, yNW, zNorth}}};  // v3 top    (x,   y)
    const std::array<Vec3, 4> bottomCorners = {{{xWest, ySW, zSouth},    // v0 top    (x,   y+1)
                                                {xEast, ySE, zSouth},    // v1 top    (x+1, y+1)
                                                {xEast, base, zSouth},   // v2 bottom (x+1, y+1)
                                                {xWest, base, zSouth}}}; // v3 bottom (x, y+1)

    constexpr Vec3 kWest{-1.0f, 0.0f, 0.0f};
    constexpr Vec3 kEast{1.0f, 0.0f, 0.0f};
    constexpr Vec3 kNorth{0.0f, 0.0f, 1.0f};
    constexpr Vec3 kSouth{0.0f, 0.0f, -1.0f};

    // Only a wall that a ramp actually shortened needs hand-written texture
    // coordinates; every full-height wall keeps the table-driven path, which is
    // verified against the original renderer.
    auto addWall = [&](const Face& face, const std::array<Vec3, 4>& corners, Vec3 normal,
                       const uint8_t (&table)[8], const int (&topSlots)[2],
                       const float (&topHeights)[2], const int (&uOrder)[4]) {
        if (ramp && (topHeights[0] < 1.0f || topHeights[1] < 1.0f)) {
            builder->AddFaceUv(face, corners, normal,
                               SlopedWallUvs(face, topSlots, topHeights, uOrder));
        } else {
            builder->AddFace(face, corners, normal, table);
        }
    };

    // Slot 1 and 2 hold the left wall's top edge; 0 and 3 the right wall's.
    const int leftTop[2] = {1, 2};
    const float leftHeights[2] = {h[0], h[3]};
    const int leftU[4] = {0, 0, 1, 1};
    const int rightTop[2] = {0, 3};
    const float rightHeights[2] = {h[1], h[2]};
    const int rightU[4] = {1, 1, 0, 0};
    const int northTop[2] = {2, 3};
    const float northHeights[2] = {h[1], h[0]};
    const int northU[4] = {0, 1, 1, 0};
    const int southTop[2] = {0, 1};
    const float southHeights[2] = {h[3], h[2]};
    const int southU[4] = {0, 1, 1, 0};

    if (solo(block.left, block.right)) {
        addWall(block.left, leftCorners, kWest, kLeftFlags, leftTop, leftHeights, leftU);
    }
    if (solo(block.right, block.left)) {
        addWall(block.right, rightCorners, kEast, kRightFlags, rightTop, rightHeights, rightU);
    }
    if (solo(block.top, block.bottom)) {
        addWall(block.top, topCorners, kNorth, kTopFlags, northTop, northHeights, northU);
    }
    if (solo(block.bottom, block.top)) {
        addWall(block.bottom, bottomCorners, kSouth, kBottomFlags, southTop, southHeights, southU);
    }

    // The far side of a flat panel: the opposite face's tile, on the panel's own
    // plane, facing the other way.
    //
    // The corners have to be in the slot order belonging to the face whose tile
    // is being drawn, not the one whose plane it borrows. Each orientation table
    // is written against its own face's slot order, and left and right number
    // their corners in opposite directions - left starts at the foot, right at
    // the top - so handing one the other's ordering turns the artwork upside
    // down. The game keeps them straight by giving each side its own helper:
    // FUN_00470060 draws the left tile in left order, FUN_00470250 the right
    // tile in right order.
    if (backOfPanel(block.right, block.left)) {
        builder->AddFace(block.right,
                         {{{xWest, yNW, zNorth},     // v0 top    (x, y)
                           {xWest, base, zNorth},    // v1 bottom (x, y)
                           {xWest, base, zSouth},    // v2 bottom (x, y+1)
                           {xWest, ySW, zSouth}}},   // v3 top    (x, y+1)
                         kEast, kRightFlags);
    }
    if (backOfPanel(block.left, block.right)) {
        builder->AddFace(block.left,
                         {{{xEast, base, zNorth},    // v0 bottom (x+1, y)
                           {xEast, yNE, zNorth},     // v1 top    (x+1, y)
                           {xEast, ySE, zSouth},     // v2 top    (x+1, y+1)
                           {xEast, base, zSouth}}},  // v3 bottom (x+1, y+1)
                         kWest, kLeftFlags);
    }
    if (backOfPanel(block.bottom, block.top)) {
        builder->AddFace(block.bottom,
                         {{{xWest, yNW, zNorth},     // v0 top    (x,   y)
                           {xEast, yNE, zNorth},     // v1 top    (x+1, y)
                           {xEast, base, zNorth},    // v2 bottom (x+1, y)
                           {xWest, base, zNorth}}},  // v3 bottom (x,   y)
                         kSouth, kBottomFlags);
    }
    if (backOfPanel(block.top, block.bottom)) {
        builder->AddFace(block.top,
                         {{{xWest, base, zSouth},    // v0 bottom (x,   y+1)
                           {xEast, base, zSouth},    // v1 bottom (x+1, y+1)
                           {xEast, ySE, zSouth},     // v2 top    (x+1, y+1)
                           {xWest, ySW, zSouth}}},   // v3 top    (x,   y+1)
                         kNorth, kTopFlags);
    }
}

}  // namespace

void BuildWorldMesh(const Map& map, const Style& style, const SlopeInfo* slopes,
                    const PartialCuts& cuts, WorldMesh* out) {
    // Prefer the game's own slope descriptors; fall back to deriving them when
    // they could not be read, so a map still builds without a live game.
    std::array<SlopeInfo, kSlopeTypeCount> resolved;
    for (int type = 0; type < kSlopeTypeCount; ++type) {
        const bool usable = slopes && slopes[type].direction >= 1 && slopes[type].direction <= 4 &&
                            slopes[type].steps != 0 && slopes[type].step < slopes[type].steps;
        resolved[type] = usable ? slopes[type] : DeriveSlope(type);
    }

    MeshBuilder builder(style);
    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            const Column& column = map.ColumnAt(x, y);
            for (size_t i = 0; i < column.blocks.size(); ++i) {
                const uint32_t index = column.blocks[i];
                if (index >= map.BlockCount()) continue;
                const Block& block = map.BlockAt(index);
                AddBlock(&builder, block, resolved[block.SlopeType() & (kSlopeTypeCount - 1)], cuts,
                         x, y, column.offset + static_cast<int>(i));
            }
        }
    }
    builder.Flatten(out);
}

}  // namespace gta2
