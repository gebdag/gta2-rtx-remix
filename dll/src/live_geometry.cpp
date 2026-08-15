#include "live_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "../../src/gta2_map.h"
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

void LiveGeometry::BeginFrame() {
    sprites_.clear();
    spriteQuads_ = 0;
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
            return false;
        }
        if (world[0] < -kWorldMargin || world[0] > gta2::kMapWidth + kWorldMargin ||
            world[1] < -kWorldMargin || world[1] > gta2::kMapHeight + kWorldMargin ||
            world[2] < kMinLevel || world[2] > kMaxLevel) {
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
    if (length < 1e-9f) return false;  // degenerate, nothing to draw
    n = {n.x / length, n.y / length, n.z / length};
    for (int i = 0; i < corners; ++i) {
        out[i].nx = n.x;
        out[i].ny = n.y;
        out[i].nz = n.z;
    }
    return true;
}

void LiveGeometry::Emit(std::map<const void*, Batch>& into, const void* texture,
                        const Vertex* corners, int count) {
    Batch& batch = into[texture];
    batch.texture = texture;
    // The original draws these as a triangle fan; a list is the same geometry
    // and lets everything sharing a texture live in one draw call.
    for (int i = 1; i + 1 < count; ++i) {
        batch.vertices.push_back(corners[0]);
        batch.vertices.push_back(corners[i]);
        batch.vertices.push_back(corners[i + 1]);
    }
}

void LiveGeometry::AddSprite(unsigned flags, const void* texture, const float* vertices,
                             int corners) {
    if (!texture || !vertices) return;
    // With this flag only vertex 0 is filled in, so the other three shadow slots
    // still hold whatever the previous draw left there.
    if (flags & quad_flags::kExpandFromTexture) return;

    // The shadow slots only describe the game's *own* vertex array. FUN_004be060
    // also draws an object's drop shadow from a nudged stack copy of those
    // vertices, and pairing that copy with the unmoved world positions stretched
    // the sprite into a second, wrong polygon - the extra "sprouting" panels on
    // trees. Only the real array can be trusted here.
    if (reinterpret_cast<uintptr_t>(vertices) != game::kSpriteVertexArray) return;

    Vertex out[4];
    if (!ReadCorners(vertices, corners, texture, game::kSpriteVertexArray, out)) return;
    Emit(sprites_, texture, out, corners);
    ++spriteQuads_;

    if (!loggedFirstSprite_) {
        loggedFirstSprite_ = true;
        Log("sprite in world space at %.2f, %.2f, %.2f", out[0].x, out[0].y, out[0].z);
    }
}

void LiveGeometry::Draw(IDirect3DDevice9* device, const gta2::Camera& camera, int width,
                        int height) {
    if (!device) return;
    if (sprites_.empty()) return;

    D3DMATRIX matrix;
    const gta2::Mat4 world = gta2::Identity();
    memcpy(&matrix, world.m, sizeof(matrix));
    device->SetTransform(D3DTS_WORLD, &matrix);
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
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    // An alpha test rather than blending: it keeps the geometry opaque to the
    // path tracer, which behaves far better than a blended surface under Remix.
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device->SetRenderState(D3DRS_ALPHAREF, 128);

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

    for (const auto& entry : sprites_) {
        {
            const Batch& batch = entry.second;
            if (batch.vertices.empty()) continue;
            IDirect3DTexture9* texture = DeviceTextureFor(device, batch.texture);
            if (!texture) continue;
            device->SetTexture(0, texture);
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
                                    static_cast<UINT>(batch.vertices.size() / 3),
                                    batch.vertices.data(), sizeof(Vertex));
        }
    }

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

void LiveGeometry::ReleaseResources() {
    sprites_.clear();
    spriteQuads_ = 0;
}

}  // namespace gta2dx9
