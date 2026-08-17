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

class LiveGeometry {
public:
    void BeginFrame();

    // A sprite quad or triangle. `vertices` is the game's screen-space array;
    // the world positions are read from its shadow slots.
    void AddSprite(unsigned flags, const void* texture, const float* vertices, int corners);

    void Draw(IDirect3DDevice9* device, const gta2::Camera& camera, int width, int height);
    void ReleaseResources();

    int SpriteQuads() const { return spriteQuads_; }

private:
    struct Vertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
    };

    struct Batch {
        const void* texture;
        std::vector<Vertex> vertices;  // triangle list
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
    void Emit(std::map<const void*, Batch>& into, const void* texture, const Vertex* corners,
              int count);

    std::map<const void*, Batch> sprites_;
    int spriteQuads_ = 0;
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
