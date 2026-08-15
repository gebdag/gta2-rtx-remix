// Direct in-process access to gta2.exe's world state.
//
// Same globals the out-of-process probe used, but as plain pointers now that we
// run inside the game. Addresses were recovered from the world render loop
// (FUN_00472110) and the per-cell block draw (FUN_00471f20); image base 0x400000.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

namespace game {

constexpr uintptr_t kCameraStructPtr = 0x005E3CC4;
constexpr uintptr_t kMapHolderPtr = 0x00662C08;
constexpr uintptr_t kMainWindowPtr = 0x00673D18;

// World coordinates are 16.14 fixed point, one unit per map block.
constexpr float kFixedScale = 1.0f / (1 << 14);

// Camera struct layout, as used by the game's own projection (FUN_0046bd40):
//   screen = camera[+0x70/+0x74] + scale * project(world - camera[+0x98/+0x9C])
//
// All of these are 16.14 fixed point except the two screen fields, which are
// plain pixel integers.
constexpr uintptr_t kCameraXOffset = 0x98;
constexpr uintptr_t kCameraYOffset = 0x9C;
constexpr uintptr_t kScreenWidthOffset = 0x68;    // pixels, integer
constexpr uintptr_t kScreenHeightOffset = 0x6C;
constexpr uintptr_t kScreenCentreXOffset = 0x70;
constexpr uintptr_t kScreenCentreYOffset = 0x74;

// The visible extent, in tiles. This is the game's real zoom: the rectangle
// passed to gbh_SetCamera is the same window rounded out to whole tiles, so it
// re-snaps as the camera moves sub-tile and is useless as a zoom source.
constexpr uintptr_t kViewMinXOffset = 0x78;
constexpr uintptr_t kViewMaxXOffset = 0x7C;
constexpr uintptr_t kViewMinYOffset = 0x80;
constexpr uintptr_t kViewMaxYOffset = 0x84;

// Vertical squash the game applies to the world; 0.8875 in practice.
constexpr uintptr_t kVerticalScaleOffset = 0x94;

struct ViewExtent {
    float minX, maxX, minY, maxY;
    float Width() const { return maxX - minX; }
    float Height() const { return maxY - minY; }
    bool Valid() const { return Width() > 0.01f && Height() > 0.01f; }
};

// Tile number -> texture, exactly the way the game resolves it
// (FUN_004bf6b0 then FUN_0046bb50).
constexpr uintptr_t kStyleObjectPtr = 0x00670684;
constexpr uintptr_t kTextureArrayPtr = 0x00671978;
constexpr uintptr_t kStyleTileRemapOffset = 0x40;

inline void* TextureForTile(int tileNumber) {
    uint8_t* style = *reinterpret_cast<uint8_t**>(kStyleObjectPtr);
    void** textures = *reinterpret_cast<void***>(kTextureArrayPtr);
    if (!style || !textures) return nullptr;
    const uint16_t* remap = *reinterpret_cast<const uint16_t* const*>(style + kStyleTileRemapOffset);
    if (!remap) return nullptr;
    const uint16_t id = remap[tileNumber & 0x3FF];
    return id ? textures[id] : nullptr;
}

inline uint8_t* CameraStruct() {
    return *reinterpret_cast<uint8_t**>(kCameraStructPtr);
}

// Null while the game is in a menu with no world loaded.
inline uint8_t* MapObject() {
    uint8_t* holder = *reinterpret_cast<uint8_t**>(kMapHolderPtr);
    return holder ? *reinterpret_cast<uint8_t**>(holder) : nullptr;
}

inline HWND MainWindow() {
    return *reinterpret_cast<HWND*>(kMainWindowPtr);
}

inline bool VisibleExtent(ViewExtent* out) {
    const uint8_t* camera = CameraStruct();
    if (!camera) return false;
    auto fixed = [camera](uintptr_t offset) {
        return *reinterpret_cast<const int32_t*>(camera + offset) * kFixedScale;
    };
    out->minX = fixed(kViewMinXOffset);
    out->maxX = fixed(kViewMaxXOffset);
    out->minY = fixed(kViewMinYOffset);
    out->maxY = fixed(kViewMaxYOffset);
    return out->Valid();
}

// Every vertex the game projects gets its **absolute** world position written
// four slots further along the same array: FUN_0046bbf0 does it for map faces
// (adding the camera position back in first, which is what makes it absolute),
// and FUN_004b9990 for sprites. x is east in tiles, y is south in tiles, z is
// the level. This is the input to the game's projection, not its output, so
// reading it is not an unprojection.
constexpr uintptr_t kTileVertexArray = 0x006632A0;
constexpr uintptr_t kSpriteVertexArray = 0x0066FE18;
constexpr size_t kVertexStride = 0x20;
constexpr int kWorldShadowSlots = 4;

// The object draw (FUN_004be060) is the only producer of world sprites; it
// reaches gbh_DrawQuad from three call sites inside itself. Everything else on
// that entry point is genuine 2D and belongs in the screen-space pass.
constexpr uintptr_t kObjectDrawBegin = 0x004BE060;
constexpr uintptr_t kObjectDrawEnd = 0x004BE600;

inline bool IsObjectDraw(const void* returnAddress) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(returnAddress);
    return address >= kObjectDrawBegin && address < kObjectDrawEnd;
}

// The block the world loop is drawing right now, set by FUN_00471f20. Byte 0x0B
// holds the slope type in its top six bits.
constexpr uintptr_t kCurrentBlockPtr = 0x00663298;

inline int CurrentBlockSlopeType() {
    const uint8_t* block = *reinterpret_cast<uint8_t* const*>(kCurrentBlockPtr);
    return block ? (block[0x0B] >> 2) : 0;
}

// The cell fractions the partial and corner blocks are cut at (slope types
// 53-61). The game passes these four as the wall extents in FUN_00471c30:
// 0x006634B4 is 0, 0x00663450 is a whole cell, and the other two are where the
// cut falls. Like the slope table they are built at startup, so they only exist
// in the running process.
constexpr uintptr_t kCellZeroPtr = 0x006634B4;
constexpr uintptr_t kCellOnePtr = 0x00663450;
constexpr uintptr_t kCellLowPtr = 0x006634FC;
constexpr uintptr_t kCellHighPtr = 0x006635B8;

inline float CellFraction(uintptr_t address) {
    return *reinterpret_cast<const int32_t*>(address) * kFixedScale;
}

// Slope descriptors, twelve bytes per slope type: direction at +0, the number
// of blocks the whole ramp climbs over at +1, and which of them this type is at
// +2 (gta2.exe!FUN_00471ce0). The table is built at startup rather than stored
// in the image, so it only exists in the running process.
constexpr uintptr_t kSlopeTablePtr = 0x00662DB0;
constexpr size_t kSlopeTableStride = 12;

inline void ReadSlopeTable(uint8_t* directions, uint8_t* steps, uint8_t* stepIndices, int count) {
    const uint8_t* table = reinterpret_cast<const uint8_t*>(kSlopeTablePtr);
    for (int type = 0; type < count; ++type) {
        const uint8_t* entry = table + static_cast<size_t>(type) * kSlopeTableStride;
        directions[type] = entry[0];
        steps[type] = entry[1];
        stepIndices[type] = entry[2];
    }
}

// Global view-rotation state. At its default 0xFF the game applies no view
// rotation, which is the case the mesh builder's tile orientation is written
// against; any other value also permutes the quad corners.
constexpr uintptr_t kViewRotationPtr = 0x005930D0;

inline uint8_t ViewRotation() { return *reinterpret_cast<const uint8_t*>(kViewRotationPtr); }

// Set per cell by the game's block draw (FUN_00471f20) just before it calls
// gbh_DrawTile: the cell's position relative to the camera, in 16.14 fixed
// point, plus the level currently being drawn.
constexpr uintptr_t kCellRelXPtr = 0x00663600;
constexpr uintptr_t kCellRelYPtr = 0x006636F0;
constexpr uintptr_t kCurrentLevelPtr = 0x006633A0;

// Recovers which cell the game is drawing right now, for cross-checking our own
// map walk against the game's.
inline bool CurrentCell(float* cellX, float* cellY, int* level) {
    const uint8_t* camera = CameraStruct();
    if (!camera) return false;
    const float relX = *reinterpret_cast<const int32_t*>(kCellRelXPtr) * kFixedScale;
    const float relY = *reinterpret_cast<const int32_t*>(kCellRelYPtr) * kFixedScale;
    *cellX = relX + *reinterpret_cast<const int32_t*>(camera + kCameraXOffset) * kFixedScale;
    *cellY = relY + *reinterpret_cast<const int32_t*>(camera + kCameraYOffset) * kFixedScale;
    *level = *reinterpret_cast<const int32_t*>(kCurrentLevelPtr);
    return true;
}

// Which tile number a texture record belongs to, by inverting the game's own
// tile -> texture mapping.
inline int TileForTexture(const void* record) {
    if (!record) return -1;
    for (int tile = 1; tile < 1024; ++tile) {
        if (TextureForTile(tile) == record) return tile;
    }
    return -1;
}

inline bool CameraPosition(float* x, float* y) {
    uint8_t* camera = CameraStruct();
    if (!camera) return false;
    *x = *reinterpret_cast<int32_t*>(camera + kCameraXOffset) * kFixedScale;
    *y = *reinterpret_cast<int32_t*>(camera + kCameraYOffset) * kFixedScale;
    return true;
}

}  // namespace game
