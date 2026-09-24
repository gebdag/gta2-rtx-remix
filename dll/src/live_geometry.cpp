#include "live_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "../../src/gta2_map.h"
#include "../../src/renderer.h"
#include "game_access.h"
#include "log.h"
#include "overlay.h"
#include "texture_store.h"

namespace gta2dx9 {
namespace {

constexpr DWORD kWorldFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
constexpr int kGameVertexFloats = 8;
constexpr int kUvU = 6;
constexpr int kUvV = 7;

// How far a sprite is lifted off the floor it stands on.
//
// GTA2 itself lifts them by nothing at all. Its object pass puts all four
// corners of the quad at exactly the level of the block underneath - read back
// out of the game's own vertex shadow, a pedestrian at cell (158,138) comes out
// as (158.2656 138.7500 2.0000) (158.2656 138.2500 2.0000) (158.7344 138.2500
// 2.0000) (158.7344 138.7500 2.0000), all four at 2.0000, while the floor there
// is the lid at 2.0. Perfectly coplanar.
//
// It gets away with that because its renderers never depth test: 3dfx.dll calls
// grDepthBufferFunction(GR_CMP_ALWAYS) and imports no grDepthMask or
// grDepthBufferMode at all, and d3ddll.dll never sets ZENABLE, ZWRITEENABLE or
// ZFUNC. Both are pure painter's algorithm - the map is drawn level by level and
// the object pass paints over the floor it just laid down. That is also why the
// screen-space overlay pass never had this problem: it replays submission order
// with no depth test, which is the original behaviour exactly.
//
// Turning a sprite into real 3D geometry is what breaks it. Coplanar surfaces
// have no defined order in 3D: the depth test drops them, and under Remix the
// ray hit is ambiguous however the raster state is set, so ZFUNC_LESSEQUAL would
// fix the raster view and still leave the path tracer fighting itself.
//
// So the lift is ours and has to be justified rather than guessed.
//
// A quarter of a ground texel - 1/256 of a block, the first value used here -
// is enough to break the coplanarity and nothing more. That settles flat
// ground, and does nothing at all for a slope: GTA2 gives all four corners of
// the quad the object's single level, so on a ramp a horizontal quad cuts
// straight through the lid and the uphill half of the sprite disappears into
// it. A sprite of length L on a gradient g sinks by up to g*L/2.
//
// Measured over the shipped districts, of the surfaces a sprite can actually
// stand on (ground type road or pavement, 16419 of them):
//
//     flat                                  72.06%
//     7 degrees   (8 steps, g = 0.125)      21.88%
//     26 degrees  (2 steps, g = 0.5)         5.92%
//     45 degrees  (1 step,  g = 1.0)         0.14%
//
// A pedestrian quad is 0.47 blocks across (measured off the game's own shadow
// slots), a car about 2. So 1/8 of a block clears 99.5% of the sloped surfaces
// a pedestrian can reach and 78% of those a car can, which is every 7 degree
// ramp - the common case by a wide margin. Doubling it to 1/4 buys nothing more
// for cars, and it takes a full 1/2 block to cover them on 26 degree ramps,
// which floats every sprite in the city half a block to fix 6% of surfaces.
//
// No single number does both jobs, which is why this is now the fallback rather
// than the mechanism. It is used where there is no floor to measure - off the
// map, over a hole, or with conforming switched off - and as the ceiling on the
// extra clearance a footprint straddling a kerb is given.
float g_spriteLift = kDefaultSpriteLift;

// --- Conforming -----------------------------------------------------------
//
// The real answer, and what runs by default: sample the lid under the sprite's
// four corners, fit a plane to it, and make that plane the sprite's transform.
// The quad is then parallel to what it is standing on, so the whole slope term
// above disappears and the clearance collapses to kConformClearance - a quarter
// of a ground texel, enough to break coplanarity and nothing more.
//
// The part that matters for Remix: **the object-space quad does not change.**
// Baking per-corner heights into the vertices is the obvious way to tilt a
// sprite and it is the wrong one - local[] would become ground-dependent, so it
// would change continuously as a car climbed a ramp, CanonicalShape's eight-shape
// cache would fill and then dither, and the topological hash would churn exactly
// the way the note above Place explains it must not. So the shape is still
// measured from the flat corners the game gave, bit for bit, and every bit of
// the ground adaptation lives in objectToWorld, which stays a pure rotation and
// translation. That is the one form of this that Remix is happy with, and it
// pays twice over: the normal handed to the path tracer becomes the ground
// normal, so a car on a ramp is lit as one, and a replacement mesh placed by
// this matrix inherits the bank for free.
//
// Two rules keep it out of trouble without having to recognise anything:
//
//   * A sprite is never moved *down*. Where the game's own level is above the
//     floor the map reports, the game's wins - which leaves a jumping car, a
//     helicopter and a bullet exactly where they were put, with no notion of
//     what any of them are.
//   * The tilt fades out over the same gap, so something leaving the ground
//     rotates level again smoothly instead of snapping when it crosses a
//     threshold.
bool g_spriteConform = true;

// How high the object sits above the road, as opposed to how far the quad is
// held off it for the depth test. See kDefaultSpriteHeight: this is the one that
// decides whether a car casts a shadow, and it was the thing the old fixed lift
// was accidentally providing a little of while trying to do something else.
float g_spriteHeight = kDefaultSpriteHeight;

float Smoothstep(float edge0, float edge1, float x) {
    if (x <= edge0) return 0.0f;
    if (x >= edge1) return 1.0f;
    const float t = (x - edge0) / (edge1 - edge0);
    return t * t * (3.0f - 2.0f * t);
}

// Where the tilt starts to fade and where it is gone. The near edge is well
// above any step a sprite stands on and well below a real jump; the far edge is
// a block, at which point the game has plainly taken the object off the floor.
constexpr float kTiltFadeNear = 0.25f;
constexpr float kTiltFadeFar = 1.0f;

// The tilt cap as the sine of the angle, which is what the normal is clamped
// against: the horizontal part of a unit normal is exactly sin of the slope.
const float kMaxTiltTan = std::tan(kMaxTiltDegrees * 3.14159265358979f / 180.0f);

// How much a sprite keeps of a sideways tilt. See kDefaultSpriteRoll.
float g_spriteRoll = kDefaultSpriteRoll;

// How far a footprint may depart from the plane fitted through it before the
// tilt stops being believed. Below the first figure the ground really is one
// plane and the fit is worth following whole; past the second it is a step or a
// block edge, where a confident tilt is worse than none.
constexpr float kFitGood = 0.05f;
constexpr float kFitPoor = 0.30f;

// How much of the way to this frame's answer a sprite moves. The game is paced
// at 30 fps, so a quarter converges in about a third of a second: fast enough to
// follow a car up a ramp, slow enough that a footprint crossing a kerb eases
// rather than steps.
constexpr float kSmoothing = 0.25f;

// Wider than anything can travel between two frames - a car at GTA2's top speed
// covers about a third of this - and narrower than the gap to the next sprite.
constexpr float kTrackRadius = 0.75f;

// Anything outside this is stale data left in the shadow slots by an earlier
// draw rather than a position the game just computed.
constexpr float kWorldMargin = 8.0f;
constexpr float kMinLevel = -4.0f;
constexpr float kMaxLevel = 16.0f;

struct Vec {
    float x, y, z;
};

Vec Cross(const Vec& a, const Vec& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

}  // namespace

void SetSpriteLift(float blocks) {
    // Negative would push sprites into the floor, which is the bug this exists
    // to avoid; the ceiling is a whole block, past which a sprite is standing on
    // the storey above.
    g_spriteLift = blocks < 0.0f ? 0.0f : (blocks > 1.0f ? 1.0f : blocks);
}

float SpriteLift() { return g_spriteLift; }

void SetSpriteConform(bool on) { g_spriteConform = on; }
bool SpriteConform() { return g_spriteConform; }

void SetSpriteRoll(float amount) {
    g_spriteRoll = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

float SpriteRoll() { return g_spriteRoll; }

void SetSpriteHeight(float blocks) {
    // Negative would bury the sprite, which is the bug all of this exists to
    // avoid. The ceiling is a whole block: past that a sprite is standing at the
    // height of the storey above the one the game put it on.
    g_spriteHeight = blocks < 0.0f ? 0.0f : (blocks > 1.0f ? 1.0f : blocks);
}

float SpriteHeight() { return g_spriteHeight; }

float g_spriteFeather = 0.0f;

void SetSpriteFeather(float strength) {
    g_spriteFeather = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
}

float SpriteFeather() { return g_spriteFeather; }

// A hundredth of a block. Far below anything visible at GTA2's near-top-down
// camera, and comfortably above the depth buffer's resolving power at the ten to
// twenty units the camera actually sits at.
float g_spriteStackStep = 0.01f;

void SetSpriteStackStep(float blocks) {
    g_spriteStackStep = blocks < 0.0f ? 0.0f : (blocks > 0.25f ? 0.25f : blocks);
}

float SpriteStackStep() { return g_spriteStackStep; }

int g_effectBatches = 0;
int EffectBatchesDrawn() { return g_effectBatches; }

// The last complete frame's conform counts. The menu has no handle on the
// LiveGeometry instance - the same reason EffectBatchesDrawn is a free function.
LiveGeometry::Conform g_lastConform;
const LiveGeometry::Conform& SpriteConformCounts() { return g_lastConform; }

void LiveGeometry::BeginFrame() {
    sprites_.clear();
    stacks_.clear();
    spriteQuads_ = 0;
    effectQuads_ = 0;
    g_lastConform = conform_;
    conform_ = Conform{};
    // This frame's placements become next frame's history.
    tracks_.swap(tracksNext_);
    tracksNext_.clear();
}

const LiveGeometry::Track* LiveGeometry::FindTrack(const void* texture, float x, float z) const {
    const Track* best = nullptr;
    float bestDistance = kTrackRadius * kTrackRadius;
    for (const Track& track : tracks_) {
        // The texture has to match: two sprites can share a spot - a car and its
        // own headlight quad - and easing one toward the other's answer is the
        // pop this is meant to remove, not a smaller one.
        if (track.texture != texture) continue;
        const float dx = track.x - x, dz = track.z - z;
        const float distance = dx * dx + dz * dz;
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = &track;
        }
    }
    return best;
}

// Which layer of a stack this sprite belongs to.
//
// Two quads count as stacked when their centres are within about a third of a
// tile, which is well inside a car and well outside the gap to the next one. The
// search is linear over the sprites already taken this frame, which is tens of
// entries - a car's worth of parts, a few pedestrians - so it costs nothing
// worth indexing away.
//
// Only a sprite that is not much smaller counts as something to stack on. The
// stack is for a car's lights, logo and roof light, which are drawn after the
// body and are its own size or smaller, so they still climb onto it. It is not
// for the burst of exhaust smoke a car puffs out under itself as it pulls away:
// six small particles drawn *before* the body counted as a pile six deep and
// lifted the car by six steps, three times its ride height, which in Remix
// tears its shadow off - and then it eased back down as the smoke died. That
// was the hop when a car starts from standstill, and whatever small thing sat
// under a parked car held it up the same way.
int LiveGeometry::StackLayerFor(float cx, float cz, float area) {
    const float kSameSpot = 0.33f * 0.33f;
    // A quarter of the area: a car's roof light still counts against a car,
    // a smoke puff or a pedestrian does not.
    const float kComparable = 0.25f;
    int layer = 0;
    for (const Stack& s : stacks_) {
        const float dx = s.x - cx, dz = s.z - cz;
        if (dx * dx + dz * dz > kSameSpot || s.layer < layer) continue;
        if (s.area < area * kComparable) continue;
        layer = s.layer + 1;
    }
    // A car with a light and a logo is three deep; anything claiming more than
    // this is sprites that happen to share a spot rather than a real stack, and
    // letting it climb would float them.
    const int kMaxLayers = 6;
    if (layer > kMaxLayers) layer = kMaxLayers;
    stacks_.push_back({cx, cz, area, layer});
    return layer;
}

// The game's world frame is x east, y south, level up. Ours is x east, y up,
// z north, so the row is mirrored exactly the way the static mesh builder
// mirrors it - sharing the sign would put sprites on the wrong side of town.
bool LiveGeometry::ReadCorners(const float* vertices, int corners, const void* texture,
                              uintptr_t shadowBase, Vertex* out) const {
    const TextureRecord* record = static_cast<const TextureRecord*>(texture);
    const float invU = record && record->width ? 1.0f / record->width : 1.0f / 64.0f;
    const float invV = record && record->height ? 1.0f / record->height : 1.0f / 64.0f;

    for (int i = 0; i < corners; ++i) {
        const float* world = reinterpret_cast<const float*>(
            shadowBase + static_cast<uintptr_t>(i + game::kWorldShadowSlots) * game::kVertexStride);
        if (!std::isfinite(world[0]) || !std::isfinite(world[1]) || !std::isfinite(world[2])) {
            ++drops_.notFinite;
            return false;
        }
        if (world[0] < -kWorldMargin || world[0] > gta2::kMapWidth + kWorldMargin ||
            world[1] < -kWorldMargin || world[1] > gta2::kMapHeight + kWorldMargin ||
            world[2] < kMinLevel || world[2] > kMaxLevel) {
            ++drops_.outOfWorld;
            return false;
        }
        out[i].x = world[0];
        out[i].y = world[2];
        out[i].z = static_cast<float>(gta2::kMapHeight) - world[1];
        out[i].u = vertices[i * kGameVertexFloats + kUvU] * invU;
        out[i].v = vertices[i * kGameVertexFloats + kUvV] * invV;
    }

    // A real normal rather than a fixed "up": GTA2 sprites lie flat but the odd
    // block faces are vertical, and Remix uses the normal.
    const Vec e1{out[1].x - out[0].x, out[1].y - out[0].y, out[1].z - out[0].z};
    const Vec e2{out[2].x - out[0].x, out[2].y - out[0].y, out[2].z - out[0].z};
    Vec n = Cross(e1, e2);
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length < 1e-9f) {
        ++drops_.degenerate;  // no area, nothing to draw
        return false;
    }
    n = {n.x / length, n.y / length, n.z / length};
    for (int i = 0; i < corners; ++i) {
        out[i].nx = n.x;
        out[i].ny = n.y;
        out[i].nz = n.z;
    }
    return true;
}

namespace {

// The quad a sprite has always had, rather than the one this frame worked out.
//
// Quantising the measured quad is not quite enough. The corners arrive from the
// game's fixed point through a normalise, so the last mantissa bits move about
// even for a parked car, and a value that happens to sit on a rounding boundary
// dithers between two quantised results - which is precisely the vertex-hash
// churn this whole exercise exists to remove.
//
// So the first quad seen for a sprite is quantised and kept, and every later one
// that is the same shape to within a coarse tolerance is answered with the
// stored copy, bit for bit. A sprite genuinely drawn at a second size gets a
// second entry; the cap is there so that something continuously scaled cannot
// grow this without bound.
struct Shape {
    LiveGeometry::Vertex local[4];
};
std::map<const void*, std::vector<Shape>> g_shapes;
constexpr size_t kMaxShapesPerTexture = 8;

void CanonicalShape(const void* texture, const LiveGeometry::Vertex* measured, int corners,
                    LiveGeometry::Vertex* out) {
    // A 256th of a tile, about 8 mm at GTA2's scale: far above the float noise
    // and far below any real difference in sprite size.
    const float kTolerance = 1.0f / 256.0f;

    std::vector<Shape>& known = g_shapes[texture];
    for (const Shape& shape : known) {
        bool same = true;
        for (int i = 0; i < corners && same; ++i) {
            same = std::fabs(shape.local[i].x - measured[i].x) < kTolerance &&
                   std::fabs(shape.local[i].y - measured[i].y) < kTolerance &&
                   std::fabs(shape.local[i].z - measured[i].z) < kTolerance &&
                   std::fabs(shape.local[i].u - measured[i].u) < kTolerance &&
                   std::fabs(shape.local[i].v - measured[i].v) < kTolerance;
        }
        if (same) {
            memcpy(out, shape.local, sizeof(Shape::local[0]) * corners);
            return;
        }
    }

    Shape shape{};
    auto quantise = [](float value) { return std::floor(value * 4096.0f + 0.5f) / 4096.0f; };
    for (int i = 0; i < corners; ++i) {
        shape.local[i] = measured[i];
        shape.local[i].x = quantise(measured[i].x);
        shape.local[i].y = quantise(measured[i].y);
        shape.local[i].z = quantise(measured[i].z);
    }
    memcpy(out, shape.local, sizeof(Shape::local[0]) * corners);
    if (known.size() < kMaxShapesPerTexture) known.push_back(shape);
}

}  // namespace

void SpriteShapeCounts(int* shapes, int* textures) {
    int total = 0;
    for (const auto& entry : g_shapes) total += static_cast<int>(entry.second.size());
    if (shapes) *shapes = total;
    if (textures) *textures = static_cast<int>(g_shapes.size());
}

// Take the quad apart into a shape and a placement.
//
// The shape is the quad measured in its own frame: origin at its centre, one
// axis along its first edge, one along its normal. A GTA2 sprite is rigid, so
// for a given sprite that measurement is the same numbers wherever it stands and
// however it is turned - which is exactly the property Remix needs to recognise
// it again next frame. The placement is that frame's position in the world, and
// it is what Remix differentiates to get the motion vector.
//
// The measured shape goes through CanonicalShape above, which is what makes it
// bit-identical rather than merely close.
bool LiveGeometry::Place(const Vertex* world, int corners, const Vec3& centre,
                         const Vec3* groundUp, Sprite* out) const {
    // The corners' own centroid, which is what the shape is measured about. It is
    // deliberately not `centre`: the shape has to come out the same numbers
    // wherever the sprite stands, so the placement can never be allowed to leak
    // into it.
    const Vec measured{
        (world[0].x + world[1].x + world[2].x + world[3].x) * 0.25f,
        (world[0].y + world[1].y + world[2].y + world[3].y) * 0.25f,
        (world[0].z + world[1].z + world[2].z + world[3].z) * 0.25f,
    };

    // The quad's own frame: right along its first edge, up along its normal,
    // forward completing the set. Orthonormal by construction, so the matrix is a
    // rotation and a translation and nothing else - normals survive it unchanged
    // and Remix's inverse of it is exact.
    const Vec edge{world[1].x - world[0].x, world[1].y - world[0].y, world[1].z - world[0].z};
    const Vec up{world[0].nx, world[0].ny, world[0].nz};   // ReadCorners already normalised this

    const float edgeLength = std::sqrt(edge.x * edge.x + edge.y * edge.y + edge.z * edge.z);
    if (edgeLength < 1e-6f) {
        ++drops_.degenerate;
        return false;
    }
    const Vec right{edge.x / edgeLength, edge.y / edgeLength, edge.z / edgeLength};
    // Cross(right, up) rather than Cross(up, right): the three axes have to come
    // out right handed or the matrix is a reflection, which flips the winding and
    // has Remix guessing at which way the face points.
    const Vec forward = Cross(right, up);

    Vertex local[4];
    for (int i = 0; i < corners; ++i) {
        const Vec offset{world[i].x - measured.x, world[i].y - measured.y, world[i].z - measured.z};
        local[i].x = offset.x * right.x + offset.y * right.y + offset.z * right.z;
        local[i].y = offset.x * up.x + offset.y * up.y + offset.z * up.z;
        local[i].z = offset.x * forward.x + offset.y * forward.y + offset.z * forward.z;
        // The normal is the frame's own up, so in object space it is exactly that
        // for every sprite - one more thing that cannot drift.
        local[i].nx = 0.0f;
        local[i].ny = 1.0f;
        local[i].nz = 0.0f;
        local[i].u = world[i].u;
        local[i].v = world[i].v;
    }
    CanonicalShape(out->texture, local, corners, out->local);
    out->corners = corners;

    // Now the same frame rotated onto the ground. The shape above was measured
    // against the quad's own up; rotating both axes together is a rigid motion of
    // that frame, so local[] still describes the same sprite - which is precisely
    // why the tilt goes here and not into the vertices.
    Vec rightOut = right;
    Vec upOut = up;
    Vec forwardOut = forward;
    if (groundUp) {
        // The quad's normal can point either way depending on how the game wound
        // it, and the ground normal only ever points up. Matching the sign keeps
        // the frame's handedness, which local[] was built against; taking it
        // straight would mirror every sprite whose quad happened to face down.
        const float sign = up.y >= 0.0f ? 1.0f : -1.0f;
        const Vec ground{groundUp->x * sign, groundUp->y * sign, groundUp->z * sign};
        // The heading, re-squared against the new up. A GTA2 sprite's first edge
        // is horizontal, so this is a small rotation about it and nothing else.
        const float along = right.x * ground.x + right.y * ground.y + right.z * ground.z;
        const Vec projected{right.x - ground.x * along, right.y - ground.y * along,
                            right.z - ground.z * along};
        const float length = std::sqrt(projected.x * projected.x + projected.y * projected.y +
                                       projected.z * projected.z);
        // Only a quad standing on its edge could line up with the ground normal,
        // and there is no heading to recover from one. Those keep their own frame.
        if (length > 1e-4f) {
            rightOut = {projected.x / length, projected.y / length, projected.z / length};
            upOut = ground;
            forwardOut = Cross(rightOut, upOut);
        }
    }

    // D3D9 multiplies row vectors on the left, so the basis goes in the rows and
    // the translation in the last one.
    gta2::Mat4& m = out->objectToWorld;
    m = gta2::Mat4{};
    m.m[0][0] = rightOut.x;   m.m[0][1] = rightOut.y;   m.m[0][2] = rightOut.z;
    m.m[1][0] = upOut.x;      m.m[1][1] = upOut.y;      m.m[1][2] = upOut.z;
    m.m[2][0] = forwardOut.x; m.m[2][1] = forwardOut.y; m.m[2][2] = forwardOut.z;
    m.m[3][0] = centre.x;     m.m[3][1] = centre.y;     m.m[3][2] = centre.z;
    m.m[3][3] = 1.0f;
    return true;
}

void LiveGeometry::AddSprite(unsigned flags, const void* texture, const float* vertices,
                             int corners) {
    if (!texture || !vertices) return;
    // With this flag only vertex 0 is filled in, so the other three shadow slots
    // still hold whatever the previous draw left there.
    if (flags & quad_flags::kExpandFromTexture) {
        ++drops_.expandFlag;
        return;
    }

    // The shadow slots only describe the game's *own* vertex array. FUN_004be060
    // also draws an object's drop shadow from a nudged stack copy of those
    // vertices, and pairing that copy with the unmoved world positions stretched
    // the sprite into a second, wrong polygon - the extra "sprouting" panels on
    // trees. Only the real array can be trusted here.
    if (reinterpret_cast<uintptr_t>(vertices) != game::kSpriteVertexArray) {
        ++drops_.notTheSpriteArray;
        return;
    }

    Vertex out[4];
    if (!ReadCorners(vertices, corners, texture, game::kSpriteVertexArray, out)) return;

    // Only quads come through here - gbh_DrawQuad and gbh_DrawQuadClipped are the
    // only callers - and Place measures the frame from four corners.
    if (corners != 4) {
        ++drops_.degenerate;
        return;
    }

    // Where the game put the sprite. All four corners carry the single level of
    // the object, so this is that level and the footprint it covers.
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (int i = 0; i < corners; ++i) {
        cx += out[i].x;
        cy += out[i].y;
        cz += out[i].z;
    }
    const float inv = 1.0f / static_cast<float>(corners);
    cx *= inv;
    cy *= inv;
    cz *= inv;

    // The stack layer is decided from where the sprite sits, before it is
    // lifted, so a car's lights and logo land on the body rather than on each
    // other's raised copies.
    // A quad's corners go round its edge in order, so two adjacent sides give
    // its footprint.
    const float sideAx = out[1].x - out[0].x, sideAz = out[1].z - out[0].z;
    const float sideBx = out[2].x - out[1].x, sideBz = out[2].z - out[1].z;
    const float area = std::sqrt(sideAx * sideAx + sideAz * sideAz) *
                       std::sqrt(sideBx * sideBx + sideBz * sideBz);
    // Skid marks, blood trails and blood pools lie on the road rather than
    // standing on it: no ride height, and none of the clearance a car is given.
    // They still follow the ground's tilt, keep the quarter-texel gap that
    // breaks coplanarity with the road, and stack against each other - but by
    // a sliver per layer, because a trail is dozens of overlapping strips and a
    // car's worth of stack step each piled them into the air.
    const bool decal = IsGroundDecal(texture);
    const float stackStep = decal ? kConformClearance * 0.25f : g_spriteStackStep;
    const float stacked = stackStep > 0.0f ? StackLayerFor(cx, cz, area) * stackStep : 0.0f;

    // Stand it on the floor the map says is under it. See the note on
    // g_spriteConform: the quad itself is not touched, only the frame it is
    // placed in.
    Vec3 groundUp{0.0f, 1.0f, 0.0f};
    bool tilted = false;
    float baseY = cy;
    float clearance = g_spriteLift;
    // The real floor under the corners and the middle, kept for the guard after
    // smoothing. A sample with no floor under it is left out.
    float guardX[5], guardZ[5], guardFloor[5];
    int guardSamples = 0;

    // A quad standing on its edge has no floor under it in any useful sense.
    // Nothing on this path should be one, but the guard is a line and the
    // alternative is a sprite laid flat on the road.
    const bool flatQuad = std::fabs(out[0].ny) > 0.5f;
    if (g_spriteConform && ground_ && ground_->Ready() && flatQuad) {
        // The corners plus the middle. A car parked across the ridge where a ramp
        // meets the flat has all four corners low and the crest under its belly,
        // which four samples cannot see at all.
        const float xs[5] = {out[0].x, out[1].x, out[2].x, out[3].x, cx};
        const float zs[5] = {out[0].z, out[1].z, out[2].z, out[3].z, cz};
        const gta2dx9::GroundPlane plane =
            ground_->Fit(xs, zs, corners + 1, cy + GroundSampler::kCeilingHeadroom, cx, cz);
        if (plane.valid) {
            for (int i = 0; i < corners + 1; ++i) {
                float floorHere = 0.0f;
                if (!ground_->SurfaceAt(xs[i], zs[i], cy + GroundSampler::kCeilingHeadroom,
                                        &floorHere)) {
                    continue;
                }
                guardX[guardSamples] = xs[i];
                guardZ[guardSamples] = zs[i];
                guardFloor[guardSamples] = floorHere;
                ++guardSamples;
            }
        }
        if (!plane.valid) {
            ++conform_.noGround;
        } else {
            const float gap = cy - plane.y;
            if (gap > conform_.maxGap) conform_.maxGap = gap;
            if (plane.residual > conform_.maxResidual) conform_.maxResidual = plane.residual;

            // Never downward: above its floor, the game's own height wins.
            baseY = gap > 0.0f ? cy : plane.y;

            // The gradient the ground actually has. Working in gradient rather
            // than in the normal keeps every adjustment below a plain scaling of
            // two numbers, and the normal falls out at the end.
            const float rawGx = -plane.nx / plane.ny;
            const float rawGz = -plane.nz / plane.ny;

            // Two reasons to use less of it than all. A footprint that is not one
            // plane has a fit that means little, so a large residual damps the
            // tilt instead of steering it - that is a corner over a kerb, which
            // is exactly the case that used to lift one corner of a car. And a
            // sprite off the ground levels out over the same gap the lift fades
            // across.
            const float trust = 1.0f - Smoothstep(kFitGood, kFitPoor, plane.residual);
            const float fade = 1.0f - Smoothstep(kTiltFadeNear, kTiltFadeFar, gap);
            float gx = rawGx * trust * fade;
            float gz = rawGz * trust * fade;

            // Pitch is kept, roll is damped. The long side of the quad is the
            // sprite's own direction of travel - two blocks against one for a
            // car - so a gradient along it tips the nose, which is what driving
            // up a ramp looks like, and a gradient across it lifts one side,
            // which a car on wheels does not do. See kDefaultSpriteRoll.
            float axisX = out[1].x - out[0].x, axisZ = out[1].z - out[0].z;
            const float sideA = std::sqrt(axisX * axisX + axisZ * axisZ);
            const float otherX = out[2].x - out[1].x, otherZ = out[2].z - out[1].z;
            const float sideB = std::sqrt(otherX * otherX + otherZ * otherZ);
            if (sideB > sideA) {
                axisX = otherX;
                axisZ = otherZ;
            }
            const float axisLength = (std::max)(sideA, sideB);
            if (axisLength > 1e-6f && g_spriteRoll < 1.0f) {
                axisX /= axisLength;
                axisZ /= axisLength;
                const float along = gx * axisX + gz * axisZ;   // the pitch part
                const float acrossX = gx - axisX * along;      // the roll part
                const float acrossZ = gz - axisZ * along;
                gx = axisX * along + acrossX * g_spriteRoll;
                gz = axisZ * along + acrossZ * g_spriteRoll;
            }

            const float slope = std::sqrt(gx * gx + gz * gz);
            if (slope > kMaxTiltTan) {
                const float scale = kMaxTiltTan / slope;
                gx *= scale;
                gz *= scale;
                ++conform_.capped;
            }
            if (slope > 1e-6f) {
                const float inv = 1.0f / std::sqrt(gx * gx + 1.0f + gz * gz);
                groundUp = {-gx * inv, inv, -gz * inv};
                tilted = true;
            }

            // Clearance is measured against the tilt the sprite actually ended up
            // with, not the one the ground has - so everything given away above,
            // to the roll damping, the trust and the cap, comes back here as
            // height instead of as a sprite cutting into the road.
            float needed = 0.0f;
            if (gap > kConformClearance) {
                // The game has it above the plane fitted under it, and that
                // plane is then not the floor. The case is a car on the upper
                // of two floors with part of its footprint over the lower one -
                // a kerb, a block edge: the fit runs a slope between the two,
                // and its residual measures the step, not anything under the
                // car. Adding that back as clearance floated the car by up to
                // half a block, and eased it up over a few frames as it started
                // to move onto an edge - the hop - or held it up while it stood
                // there and dropped it as it drove off.
                //
                // The samples themselves say what is actually under it: clear
                // whatever real floor rises above the quad, and nothing else.
                // The lower floor it overhangs contributes nothing, and the one
                // it stands on only what its own tilt dips into.
                const float ceiling = cy + GroundSampler::kCeilingHeadroom;
                for (int i = 0; i < corners + 1; ++i) {
                    float floorHere = 0.0f;
                    if (!ground_->SurfaceAt(xs[i], zs[i], ceiling, &floorHere)) continue;
                    const float quadHere = baseY + gx * (xs[i] - cx) + gz * (zs[i] - cz);
                    if (floorHere - quadHere > needed) needed = floorHere - quadHere;
                }
            } else {
                // On the fitted floor. The residual is how far the highest sample
                // pokes above the plane, so the quad has to clear that plus
                // whatever the plane itself rises above the quad at a corner -
                // which is where the gap between two flat things is widest.
                float worst = 0.0f;
                for (int i = 0; i < corners; ++i) {
                    const float dx = out[i].x - cx;
                    const float dz = out[i].z - cz;
                    const float floorHere = plane.y + rawGx * dx + rawGz * dz;
                    const float quadHere = baseY + gx * dx + gz * dz;
                    if (floorHere - quadHere > worst) worst = floorHere - quadHere;
                }
                needed = worst + plane.residual;
            }
            const float owed = (needed - g_spriteHeight * groundUp.y) / groundUp.y;
            const float extra = owed > 0.0f ? (std::min)(owed, kMaxExtraClearance) : 0.0f;
            clearance = kConformClearance + extra;
            if (needed > kConformClearance) ++conform_.straddled;
            if (fade < 1.0f) {
                ++conform_.airborne;
            } else {
                ++conform_.grounded;
            }
        }
    }

    // Ease toward this frame's answer rather than snapping to it. Only the lift
    // and the tilt are smoothed: baseY follows the ground directly, because a car
    // driving up a ramp *should* rise with it and lagging that would be the
    // strange movement rather than the fix for it. See the Track note.
    float offset = decal ? kConformClearance + stacked : g_spriteHeight + clearance + stacked;
    if (const Track* previous = FindTrack(texture, cx, cz)) {
        offset = previous->offset + (offset - previous->offset) * kSmoothing;
        Vec3 eased{previous->nx + (groundUp.x - previous->nx) * kSmoothing,
                   previous->ny + (groundUp.y - previous->ny) * kSmoothing,
                   previous->nz + (groundUp.z - previous->nz) * kSmoothing};
        const float length =
            std::sqrt(eased.x * eased.x + eased.y * eased.y + eased.z * eased.z);
        if (length > 1e-6f) {
            groundUp = {eased.x / length, eased.y / length, eased.z / length};
            tilted = std::fabs(groundUp.x) > 1e-5f || std::fabs(groundUp.z) > 1e-5f;
        }
    }

    // The floor guard, after the smoothing and against what is actually drawn.
    //
    // Clearance above is worked out for the tilt and height the sprite is
    // heading for, but both are then eased there over a few frames. Driving onto
    // a ramp that lag is the clip: the target tips the nose up and adds the
    // clearance the kink needs, the eased sprite is still nearly level and
    // nearly as low, and for a few frames the front corner is inside the slope.
    // So measure the real floor at the corners and the middle against the tilt
    // this frame really has, and if any of them would be under it, raise the
    // sprite to just clear it - now, not eased. Only ever up, and only as far as
    // the floor demands, so nothing already clear moves: easing still decides
    // everything else, and lowering stays smooth.
    if (guardSamples > 0 && groundUp.y > 1e-3f) {
        const float gx = -groundUp.x / groundUp.y;
        const float gz = -groundUp.z / groundUp.y;
        float least = offset;
        for (int i = 0; i < guardSamples; ++i) {
            const float under = baseY + gx * (guardX[i] - cx) + gz * (guardZ[i] - cz);
            const float need = (guardFloor[i] + kConformClearance - under) / groundUp.y;
            if (need > least) least = need;
        }
        offset = least;
    }
    tracksNext_.push_back({texture, cx, cz, offset, groundUp.x, groundUp.y, groundUp.z});

    // Along the ground normal rather than straight up: on a ramp those are not
    // the same direction, and clearance measured the wrong way is not clearance.
    // The ride height goes the same way, so a car on a ramp rides above the ramp
    // rather than leaning out of it.
    const Vec3 centre{cx + groundUp.x * offset, baseY + groundUp.y * offset,
                      cz + groundUp.z * offset};

    Sprite placed;
    placed.texture = texture;
    if (!Place(out, corners, centre, tilted ? &groundUp : nullptr, &placed)) return;
    sprites_.push_back(placed);
    ++spriteQuads_;
    ++drops_.accepted;

    // The game's own corner positions, straight out of the shadow slots and
    // before our axis swap, so the question "does GTA2 lift a sprite off the
    // floor it stands on?" can be answered from its numbers rather than ours.
    if (spriteLogs_ < 8) {
        ++spriteLogs_;
        char line[320];
        int used = snprintf(line, sizeof(line), "sprite corners (game frame x,y,level):");
        for (int i = 0; i < corners && used > 0 && used < static_cast<int>(sizeof(line)) - 40; ++i) {
            const float* world = reinterpret_cast<const float*>(
                game::kSpriteVertexArray +
                static_cast<uintptr_t>(i + game::kWorldShadowSlots) * game::kVertexStride);
            used += snprintf(line + used, sizeof(line) - used, "  (%.4f %.4f %.4f)", world[0],
                             world[1], world[2]);
        }
        Log("%s", line);
    }
}

void LiveGeometry::Draw(IDirect3DDevice9* device, const gta2::Camera& camera, int width,
                        int height) {
    if (!device) return;
    if (sprites_.empty()) return;

    D3DMATRIX matrix;
    // No world matrix here: each sprite sets its own, which is the whole point.
    // See the note in live_geometry.h.
    const gta2::Mat4 view = camera.ViewMatrix();
    memcpy(&matrix, view.m, sizeof(matrix));
    device->SetTransform(D3DTS_VIEW, &matrix);
    const gta2::Mat4 projection =
        camera.ProjectionMatrix(static_cast<float>(width) / static_cast<float>(height));
    memcpy(&matrix, projection.m, sizeof(matrix));
    device->SetTransform(D3DTS_PROJECTION, &matrix);

    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    // Sprites are flat quads and the odd block faces come in either winding, so
    // culling is off rather than guessed at.
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    const bool blending = gta2::GetAlphaMode() == gta2::AlphaMode::Blend;
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, blending ? TRUE : FALSE);
    // An alpha test rather than blending: it keeps the geometry opaque to the
    // path tracer, which behaves far better than a blended surface under Remix.
    // Sprites were hardcoded to alpha test, which is why the mode switch appeared
    // to do nothing to them: it only ever reached the static world mesh. An
    // explosion is exactly the case that wants blending - it is a soft glow, not
    // a cutout like a fence or a tree.
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    // In blend mode the test drops to 1, which still discards the fully
    // transparent texels so they do not write depth over what is behind them.
    device->SetRenderState(D3DRS_ALPHAREF, blending ? 1u
                                                    : static_cast<DWORD>(gta2::GetAlphaRef()));
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

    device->SetFVF(kWorldFvf);

    // Effect artwork - fire, explosions, muzzle flashes - is drawn in a second
    // pass, so the texture has to be resolved before the pass is chosen. See
    // texture_store.h for what makes a sprite an effect and why it matters.
    struct Drawable {
        const Sprite* sprite;
        IDirect3DTexture9* texture;
    };
    std::vector<Drawable> plain, effects;
    plain.reserve(sprites_.size());
    for (const Sprite& sprite : sprites_) {
        bool effect = false;
        IDirect3DTexture9* texture = DeviceTextureFor(device, sprite.texture, &effect);
        if (!texture) {
            // The sprite was accepted and then had no artwork to draw with:
            // this is a sprite that vanishes for exactly one frame.
            ++drops_.noTexture;
            continue;
        }
        // Recorded so the texture report can tell a fireball from a road tile
        // that happens to have a hole in it. See texture_store.h.
        NoteSpriteTexture(sprite.texture);
        (effect ? effects : plain).push_back({&sprite, texture});
    }

    // Two triangles from four corners, the same two every time. Indexed rather
    // than a six-vertex list so the index buffer is part of the topological hash
    // Remix buckets on, and so the quad really is four vertices rather than four
    // with two duplicated.
    static const uint16_t kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    // The device is a state machine and the texture rarely changes between
    // neighbouring sprites, so the redundant SetTexture calls are skipped. The
    // world matrix genuinely changes every time and cannot be.
    IDirect3DTexture9* bound = nullptr;
    auto issue = [&](const std::vector<Drawable>& list) {
        for (const Drawable& d : list) {
            D3DMATRIX world;
            memcpy(&world, d.sprite->objectToWorld.m, sizeof(world));
            device->SetTransform(D3DTS_WORLD, &world);
            if (d.texture != bound) {
                device->SetTexture(0, d.texture);
                bound = d.texture;
            }
            device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, kQuadIndices,
                                           D3DFMT_INDEX16, d.sprite->local, sizeof(Vertex));
            ++drops_.drawn;
        }
    };

    issue(plain);

    if (!effects.empty()) {
        // Additive, which is what this artwork was drawn for: it fades to black
        // on its way out, and black adds nothing. The rim disappears because it
        // stops being painted at all rather than because anything was cut out of
        // it, and the fireball ends up a glow over the scene the way it should.
        //
        // Remix treats additively blended draws as emissive particles, so this is
        // also the classification that gets a fireball to light the street rather
        // than sit on it as a flat decal.
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        // Still depth *tested*, so a fire behind a wall stays behind it, but not
        // depth written: a glow must not stop what is drawn after it.
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        issue(effects);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    effectQuads_ = static_cast<int>(effects.size());
    g_effectBatches = effectQuads_;

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    // Left as we found it, so the next pass that assumes a world matrix - and the
    // static mesh does - is not drawing at the last sprite's position.
    const gta2::Mat4 identity = gta2::Identity();
    memcpy(&matrix, identity.m, sizeof(matrix));
    device->SetTransform(D3DTS_WORLD, &matrix);
}

void LiveGeometry::ReleaseResources() {
    sprites_.clear();
    spriteQuads_ = 0;
    effectQuads_ = 0;
    // The shapes are keyed by texture record, and a new level hands out new ones.
    g_shapes.clear();
}

}  // namespace gta2dx9
