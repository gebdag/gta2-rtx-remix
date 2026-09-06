// World-space geometry recovered from the game's own draw calls.
//
// GTA2 hands the renderer screen-space vertices, but it does not throw the
// world-space ones away: every vertex it projects also has its **absolute**
// world position written four slots further along the same array, by
// FUN_0046bbf0 for map faces and FUN_004b9990 for sprites. FUN_0046bbf0 adds
// the camera position back in before storing, so these are absolute map
// coordinates, not camera-relative ones.
//
// That makes it possible to place sprites and the odd block shapes as real 3D
// geometry without ever inverting a projection: the numbers here are the input
// to the game's transform, not the output.
//
// Used for sprites - cars, pedestrians, powerups - which are rebuilt each frame
// so they can move. Map geometry is deliberately *not* sourced this way: the
// game only draws what is currently visible, so anything built from its stream
// would pop into existence as the camera reached it.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <d3d9.h>
#include <map>
#include <vector>

#include "../../src/camera.h"
#include "../../src/math3d.h"
#include "ground.h"

namespace gta2dx9 {

// How far a sprite is lifted off the floor when there is no floor to measure -
// off the edge of the map, over a hole, or with conforming switched off. See
// the long note in live_geometry.cpp for why any lift is needed at all and how
// this number was arrived at. It doubles as the ceiling on the extra clearance
// a footprint straddling a kerb is given, so one knob still bounds how far a
// sprite can ever be pushed off the ground. Sprites are rebuilt from the game's
// stream every frame, so a change takes effect on the next one.
constexpr float kDefaultSpriteLift = 0.075f;

void SetSpriteLift(float blocks);

// Stand each sprite on the ground plane under it rather than at a fixed height
// above its own level: the lid is sampled under the four corners, a plane is
// fitted to it, and that plane becomes the sprite's transform. On when the map
// can be read; off restores the fixed lift exactly.
void SetSpriteConform(bool on);
bool SpriteConform();

// How far a sprite is held off the ground once it is conformed to it. A quarter
// of a ground texel: enough to break the coplanarity that has no defined answer
// in a path tracer, and nothing more. The fixed lift had to be twenty times this
// only because a horizontal quad on a ramp needed the headroom.
//
// This is a depth-buffer number, not a visual one. How high the sprite *looks*
// is kDefaultSpriteHeight below, which is a completely different question.
constexpr float kConformClearance = 1.0f / 256.0f;

// How high the object a sprite stands in for sits above the road.
//
// Separate from the clearance above: clearance answers "which surface does the
// ray hit", this answers "is there a car here, or a picture of one painted on
// the road".
//
// This was 0.3 - the mid-height of a car body at GTA2's 2 m block - on the
// reasoning that a flat quad lying on the ground casts its shadow exactly where
// it already is and so shows nothing. That reasoning is right about shadows and
// wrong about the result: at a near-top-down camera the visible effect of ride
// height is not the shadow, it is the parallax, and 60 cm of it reads as the
// sprite hovering rather than as the sprite having depth. Judged by eye at both,
// a tenth of that sits on the road.
//
// So the height is small, and the *clipping* is handled where it belongs - by
// the conform, and by clearance that is computed per sprite rather than budgeted
// for in advance.
constexpr float kDefaultSpriteHeight = 0.03f;

void SetSpriteHeight(float blocks);
float SpriteHeight();

// The most extra clearance a sprite can be given on top of its ride height, for
// ground the fitted plane could not describe. Geometry bounds the real figure
// well below this - the worst case is a footprint across a whole block step -
// so anything reaching the cap is a bad sample rather than a steep street, and
// the cap is what stops it launching a car into the air.
constexpr float kMaxExtraClearance = 1.0f;

// How much of a sideways tilt a sprite keeps, 0..1.
//
// Fitting a plane to the ground and using the whole of it is right for a rigid
// body and wrong for a car. A car crossing a ramp at an angle stands on a plane
// tilted along both of its own axes, so the body rolls as well as pitches and a
// corner lifts - which is not something a car on wheels does. Pitching along the
// direction of travel is the part that looks right, and that part is kept whole.
//
// The sprite's long axis is taken as its direction of travel, which for a car
// quad is the side measuring about two blocks against the other's one. Rotation
// about that axis is the roll, and this scales it. 0 keeps sprites level side to
// side, 1 is the full fitted plane again.
constexpr float kDefaultSpriteRoll = 0.25f;

void SetSpriteRoll(float amount);
float SpriteRoll();

// The steepest ground a sprite is rotated onto. GTA2's 26 degree ramps come in
// under it; its 45 degree blocks, 0.14% of the surfaces a sprite can stand on,
// are clamped here because standing top-down artwork on its edge looks far worse
// than the clipping it would fix. What the clamp leaves behind is picked up as
// clearance.
constexpr float kMaxTiltDegrees = 30.0f;

// How much to soften a sprite's cutout edge, 0..1. GTA2's alpha is 1-bit, so
// this is invented rather than recovered - see FeatherAlpha in alpha_bleed.h.
// Sprites only: the world mesh keeps its hard edges, which is what a fence or a
// wall wants.
void SetSpriteFeather(float strength);
float SpriteFeather();

// How far apart to hold sprites that are stacked on the same spot.
//
// A GTA2 car is not one sprite: the body is drawn, then its lights, then any
// logo, each a separate quad at exactly the same height. The game got away with
// that because it never depth tested - it just painted them in order - but as
// real geometry they are coplanar and the depth test picks a winner per pixel,
// which is the flicker and the dropouts. Each sprite landing on a spot another
// has already taken this frame is lifted one step higher than the last, in the
// order the game drew them, so the painter's order becomes a real stacking
// order. 0 disables it.
void SetSpriteStackStep(float blocks);
float SpriteStackStep();
float SpriteLift();

// How many sprites were drawn additively last frame - the fire and explosion
// artwork. A free function because the menu has no handle on the LiveGeometry
// instance. See EffectSpriteSettings in texture_store.h.
int EffectBatchesDrawn();

// How many distinct object-space quads the session has had to keep, and for how
// many sprite textures. This is the health check on the whole scheme below: a
// count that settles is geometry Remix can recognise frame to frame, and one
// that keeps climbing means something is dithering and the motion vectors will
// be no better than they were.
void SpriteShapeCounts(int* shapes, int* textures);

// --- Why each sprite is its own draw call ---------------------------------
//
// Because RTX Remix has to recognise a sprite as the same sprite it saw last
// frame, and it can only do that from what the draw call looks like.
//
// This pass used to hand Remix, once per texture, a bag of world-space vertices
// for every sprite sharing that texture, drawn with an identity world matrix.
// Reading dxvk-remix, that is close to the worst thing to give it:
//
//   * DrawCallCache buckets geometry by its *topological* hash, and
//     hashGeometryDescriptor (rtx_hashing.cpp) mixes in the **vertex count**. A
//     batch grows and shrinks as cars and pedestrians come into view, so its
//     bucket key changed constantly and every such frame allocated a fresh
//     BlasEntry with no history at all.
//
//   * Previous-frame vertex positions only exist when the *same* BlasEntry is
//     matched again (SceneManager::processGeometryInfo keeps them in
//     historyBuffer on kUpdateBVH). Inside a batch, vertex i belongs to whichever
//     sprite the game happened to submit i'th that frame. Two cars swapping draw
//     order silently repointed every vertex at a different object, so the
//     previous positions Remix did have described the wrong sprite - a motion
//     vector from one car to another. That is the flickering copies.
//
//   * With an identity world matrix, InstanceManager::updateInstance sees
//     hasTransformChanged == false; with no usable history hasPreviousPositions
//     is false too, and surface.isStatic is then set, which tells the renderer to
//     skip motion vector calculation for a surface that is visibly moving.
//
// So the geometry has to be the thing that stays still and the transform the
// thing that moves - which is what every other game gives Remix. Each sprite is
// now one draw call of a canonical quad in its own object space, quantised so it
// is bit-identical frame to frame, with the placement in D3DTS_WORLD. Remix then
// matches it by topological hash plus proximity (DrawCallTracker's spatial map)
// and takes the motion straight from objectToWorld against prevObjectToWorld.
//
// The cost is one draw call per sprite instead of per texture - a few hundred
// two-triangle draws, which is nothing for either D3D9 or Remix, and it lets
// every sprite of a kind share one BLAS instead of rebuilding a merged one.

class LiveGeometry {
public:
    // Public only so the object-space shape cache in live_geometry.cpp can name
    // it; nothing outside this pass has any use for it.
    struct Vertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
    };

    void BeginFrame();

    // The floor sprites are stood on. Held, not copied; WorldView owns both this
    // and the sampler, so the pointer stays good for the process. Null, or a
    // sampler with no map in it yet, falls back to the fixed lift.
    void SetGround(const GroundSampler* ground) { ground_ = ground; }

    // A sprite quad or triangle. `vertices` is the game's screen-space array;
    // the world positions are read from its shadow slots.
    void AddSprite(unsigned flags, const void* texture, const float* vertices, int corners);

    void Draw(IDirect3DDevice9* device, const gta2::Camera& camera, int width, int height);
    void ReleaseResources();

    int SpriteQuads() const { return spriteQuads_; }
    // Sprites drawn additively last frame - the fire and explosion artwork. See
    // EffectSpriteSettings in texture_store.h.
    int EffectBatches() const { return effectQuads_; }

private:
    // One sprite: a quad in its own object space, and where that space sits in
    // the world. The vertices are the part Remix hashes, so they are quantised
    // and carry no trace of where the sprite is; the matrix is the part Remix
    // differentiates for motion, so it carries all of it.
    struct Sprite {
        const void* texture;
        Vertex local[4];
        int corners;
        gta2::Mat4 objectToWorld;
    };

    // One entry per sprite already placed this frame, for spotting a stack.
    struct Stack {
        float x, z;
        int layer;
    };
    std::vector<Stack> stacks_;

    // What a sprite was given last frame, so this frame can move toward its
    // answer instead of jumping to it.
    //
    // The ground under a footprint is not a continuous function of where that
    // footprint is: a corner crossing a kerb, or the crest where a ramp meets
    // flat, changes the fitted plane in one step. Applied directly that is a
    // sprite which twitches as it drives, so both the lift and the tilt are
    // eased toward the answer instead of being set to it.
    //
    // Sprites have no identity in this stream - they are rebuilt from scratch
    // every frame - so the match is the same trick the stack finder uses:
    // nearest entry from last frame with the same texture, within a radius wider
    // than anything can travel in one frame. No match means the sprite is new
    // and gets its answer immediately; easing one in from nothing would be the
    // visible pop this exists to remove.
    struct Track {
        const void* texture;
        float x, z;
        float offset;
        float nx, ny, nz;
    };
    std::vector<Track> tracks_;      // last frame
    std::vector<Track> tracksNext_;  // being built this frame

    // Nearest match from last frame, or null.
    const Track* FindTrack(const void* texture, float x, float z) const;

    int StackLayerFor(float cx, float cz);

    bool ReadCorners(const float* vertices, int corners, const void* texture, uintptr_t shadowBase,
                     Vertex* out) const;
    // Splits world-space corners into a canonical object-space quad and the
    // transform that places it. False if the quad has no area to build a frame
    // from.
    //
    // `centre` is where the sprite ends up, which is not the centroid of the
    // corners: the corners are the shape, the centre is the placement. `groundUp`
    // rotates the sprite onto the floor - null, or an exactly vertical one,
    // leaves the frame the corners themselves describe.
    struct Vec3 {
        float x, y, z;
    };
    bool Place(const Vertex* world, int corners, const Vec3& centre, const Vec3* groundUp,
               Sprite* out) const;

    const GroundSampler* ground_ = nullptr;
    std::vector<Sprite> sprites_;
    int spriteQuads_ = 0;
    int effectQuads_ = 0;
    int spriteLogs_ = 0;

public:
    // Why sprites the game asked for did not reach the screen. Every one of
    // these is a frame where something the player can see is simply absent.
    struct Drops {
        int accepted = 0;  // quads we took from the game
        int drawn = 0;     // quads that actually reached the device
        int expandFlag = 0;
        int notTheSpriteArray = 0;
        int notFinite = 0;
        int outOfWorld = 0;
        int degenerate = 0;
        int noTexture = 0;
        bool Quiet() const {
            return !expandFlag && !notTheSpriteArray && !notFinite && !outOfWorld && !degenerate &&
                   !noTexture;
        }
    };
    const Drops& DropCounts() const { return drops_; }
    void ClearDropCounts() { drops_ = Drops{}; }

    // What the conform actually did, per frame. This is the health check on the
    // whole scheme: `grounded` should be nearly every sprite in a normal street,
    // and a large `maxGap` on a frame with nothing in the air means the game's
    // own level for a sprite disagrees with the lid the map says is under it -
    // which is the one thing that would make conforming pointless rather than
    // wrong, and it is measured here rather than assumed.
    struct Conform {
        int grounded = 0;    // stood and rotated onto a fitted plane
        int airborne = 0;    // above its floor, so left where the game put it
        int noGround = 0;    // nothing under the footprint; fixed lift used
        int capped = 0;      // ground steeper than kMaxTiltDegrees
        int straddled = 0;   // footprint over a step, given extra clearance
        float maxGap = 0.0f;       // furthest any sprite sat above its own floor
        float maxResidual = 0.0f;  // worst a footprint failed to be one plane
    };
    const Conform& ConformCounts() const { return conform_; }
    void ClearConformCounts() { conform_ = Conform{}; }

private:
    mutable Drops drops_;
    mutable Conform conform_;
};

// The last complete frame's conform counts, for the menu. A free function for
// the same reason EffectBatchesDrawn is one: nothing outside WorldView holds the
// LiveGeometry instance.
const LiveGeometry::Conform& SpriteConformCounts();

}  // namespace gta2dx9
