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

namespace gta2dx9 {

// How far a sprite is lifted off the floor it stands on, in map blocks. See the
// long note in live_geometry.cpp for why any lift is needed at all and how the
// default was arrived at. Sprites are rebuilt from the game's stream every
// frame, so a change takes effect on the next one.
constexpr float kDefaultSpriteLift = 0.075f;

void SetSpriteLift(float blocks);

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

    int StackLayerFor(float cx, float cz);

    bool ReadCorners(const float* vertices, int corners, const void* texture, uintptr_t shadowBase,
                     Vertex* out) const;
    // Splits world-space corners into a canonical object-space quad and the
    // transform that places it. False if the quad has no area to build a frame
    // from.
    bool Place(const Vertex* world, int corners, Sprite* out) const;

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

private:
    mutable Drops drops_;
};

}  // namespace gta2dx9
