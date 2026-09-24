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

// Set from the `lighting` config option by FUN_004cb1d0, default on. While it is
// clear the game never calls gbh_ResetLights or gbh_AddLight, so the Remix light
// injection has nothing to work with and the menu says so rather than looking
// broken.
constexpr uintptr_t kLightingEnabledFlag = 0x00595011;

inline bool LightingEnabled() {
    return *reinterpret_cast<const uint8_t*>(kLightingEnabledFlag) != 0;
}

// GTA2's blood switch, one of the thirty-odd debug flags FUN_00451930 reads at
// startup. The ped death handler, FUN_004411b0, ends by putting a blood pool
// under the body (FUN_0048cc50, a particle-manager object that grows through
// code_obj 497-502) - but only while this byte is set, and the retail game sets
// it only if a value named do_blood exists under
// HKLM\SOFTWARE\DMA Design Ltd\GTA2\Debug, or through a pair of debug cheats in
// FUN_004590f0. So out of the box nobody bleeds. Plain .data, writable as is.
constexpr uintptr_t kDoBloodFlag = 0x005EAD51;

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

// Where GTA2 decides how much of the world is on screen, and the one place its
// 4:3 assumption lives.
//
// FUN_0041E7A0 (sole caller FUN_0041F2F0, the per-frame camera update) builds
// both rectangles a camera carries, from nothing but the camera's position and
// zoom - the screen's pixel size is not an input:
//
//     half   = (DAT_005E3D18 + cam[0xA0]) * (DAT_005E3D64 / cam[0xA4])
//              * DAT_005E3F54                              ; 0.5
//     view.x = cam[0x98] -/+ half        -> cam+0x78, +0x7C   (clamped to the map)
//     half  *= DAT_005E3F34                                ; 0.75  <- the 4:3
//     view.y = cam[0x9C] -/+ half        -> cam+0x80, +0x84
//     seen   = view padded by DAT_005E3D98 (0.75 tiles) -> cam+0x20..+0x2C
//
// Every spatial question the population asks goes to one of those two:
//
//   - traffic is created where FUN_0045AF40 says `seen` does not reach
//     (FUN_004B34E0 -> FUN_0045BC90), and recycled by FUN_0045AEA0 on `seen`;
//   - pedestrians are put on a ring around `view` by FUN_00440CC0 and rejected
//     by FUN_0040CF60 against `view`;
//   - FUN_00447390 walks the object grids over `view` to draw them.
//
// So a frame wider than 4:3 shows ground the game believes is off screen, and
// things are created and destroyed there in plain sight. The cure is to make the
// rectangle the shape of the frame at its source. Both constants are static
// Fixed objects built at startup (0x004FCD80 and 0x004FD0B0), and DAT_005E3F54
// has other readers, so neither is written: the operand of each PUSH is
// repointed at a Fixed the renderer owns. Scaling the first by w and the second
// by 1/w widens x and leaves y exactly as it was.
//
// What this replaced, for the record: a detour of FUN_0045AF40, a widened
// pedestrian ring, padded grid-walk operands, a per-frame pad written into
// `seen`, and DAT_005E4CB8 (FUN_0045AEA0's margin). Each moved one consumer
// without the others, and a spawn point that is further out than the recycler's
// idea of visible is destroyed before it walks in - the empty streets.
constexpr uintptr_t kViewHalfScale = 0x005E3F54;
constexpr uintptr_t kViewAspect = 0x005E3F34;
// The operand of each PUSH, one past its 0x68 opcode.
constexpr uintptr_t kViewHalfScaleSite = 0x0041E7F0;
constexpr uintptr_t kViewAspectSite = 0x0041E8E4;
// What the game constructs those two as, in 16.14 fixed point.
constexpr int32_t kViewHalfScaleStock = 0x2000;  // 0.5
constexpr int32_t kViewAspectStock = 0x3000;     // 0.75
constexpr float kViewAspectRatioStock = 4.0f / 3.0f;

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

// ---------------------------------------------------------------------------
// Live effect state, for the lights GTA2 does not emit itself.
//
// Particles, objects and vehicles all carry the same placement block, because
// they all go through FUN_00420600 (position) and FUN_00420690 (heading).
// ---------------------------------------------------------------------------

// The placement block itself, allocated by FUN_00421000 and written by
// FUN_00420600 (position) and FUN_00420690 (heading). Entities do not embed it:
// each holds a *pointer* to one, at an offset that differs per entity type.
// Reading these straight off the entity - which is what the first attempt did -
// puts every effect at the corner of the map.
constexpr uintptr_t kPlaceHeading = 0x00;  // uint16, indexes the trig tables below
constexpr uintptr_t kPlaceX = 0x14;        // 16.14 fixed, tiles east
constexpr uintptr_t kPlaceY = 0x18;        // 16.14 fixed, tiles south
constexpr uintptr_t kPlaceZ = 0x1C;        // 16.14 fixed, map level

// Where each entity keeps that pointer. Both were found by the structure probe
// in synthetic_lights.cpp rather than read off a decompiler: it snapshots the
// lists a second apart and looks for the field that is always a map coordinate
// and moves by a fraction of a tile, which nothing else does.
constexpr uintptr_t kVehiclePlacementPtr = 0x50;
constexpr uintptr_t kParticlePlacementPtr = 0x30;

// particle.cpp. The manager (FUN_00491B90 allocates it, 0x947C bytes) is also
// the pool: free list at +0, live list at +4, walked through +0x3C.
constexpr uintptr_t kParticleManagerPtr = 0x00669E70;
constexpr uintptr_t kParticleLiveHead = 0x04;
constexpr uintptr_t kParticleNext = 0x3C;
constexpr uintptr_t kParticleLife = 0x2C;  // int16, frames remaining
constexpr uintptr_t kParticleType = 0x38;  // int32

// Same shape for vehicles: FUN_004254A0 appends, FUN_00425480 prepends.
constexpr uintptr_t kVehiclePoolPtr = 0x005E4CA0;
constexpr uintptr_t kVehicleLiveHead = 0x04;
constexpr uintptr_t kVehicleNext = 0x4C;
constexpr uintptr_t kVehicleModel = 0x84;   // int32, car model id
// The occupant. Found by the structure probe correlating pointer fields against
// which cars actually moved: this one is a pointer on 100% of moving cars and on
// 0% of parked ones, and it points into the entity heap rather than the
// placement heap - it is a ped sitting in the car. +0x58 behaves the same way
// and is presumably a passenger.
constexpr uintptr_t kVehicleDriver = 0x54;


// Sine and cosine of a heading, in 16.14 fixed, indexed by the raw heading with
// no masking - which is how FUN_0040F500 and FUN_0040F520 do it, so any heading
// the game stores is already in range. Like the slope table these are built at
// startup, so they read as zeroes in the image and only exist in the process.
constexpr uintptr_t kSinTablePtr = 0x005D3938;
constexpr uintptr_t kCosTablePtr = 0x005D6718;
constexpr int kTrigTableEntries = (kCosTablePtr - kSinTablePtr) / 4;

// A linked list read out of another process's heap is one bad pointer away from
// taking the game down with it, and these lists are walked every frame.
inline bool PlausiblePointer(const void* p) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    return address >= 0x00010000 && address < 0x7FFE0000 && (address & 3) == 0;
}

inline uint8_t* ParticleListHead() {
    uint8_t* manager = *reinterpret_cast<uint8_t**>(kParticleManagerPtr);
    if (!PlausiblePointer(manager)) return nullptr;
    uint8_t* head = *reinterpret_cast<uint8_t**>(manager + kParticleLiveHead);
    return PlausiblePointer(head) ? head : nullptr;
}

inline uint8_t* VehicleListHead() {
    uint8_t* pool = *reinterpret_cast<uint8_t**>(kVehiclePoolPtr);
    if (!PlausiblePointer(pool)) return nullptr;
    uint8_t* head = *reinterpret_cast<uint8_t**>(pool + kVehicleLiveHead);
    return PlausiblePointer(head) ? head : nullptr;
}

// Someone in the driving seat, which is what makes a car's headlights its own
// rather than a parked car's.
inline bool VehicleHasDriver(const uint8_t* vehicle) {
    return PlausiblePointer(*reinterpret_cast<const void* const*>(vehicle + kVehicleDriver));
}

inline uint8_t* NextInList(const uint8_t* entry, uintptr_t nextOffset) {
    uint8_t* next = *reinterpret_cast<uint8_t* const*>(entry + nextOffset);
    return PlausiblePointer(next) ? next : nullptr;
}

inline const uint8_t* Placement(const uint8_t* entity, uintptr_t placementPtrOffset) {
    const uint8_t* place = *reinterpret_cast<const uint8_t* const*>(entity + placementPtrOffset);
    return PlausiblePointer(place) ? place : nullptr;
}

// x east, y south, z the map level - the game's own frame, not the renderer's.
inline bool ReadPlacement(const uint8_t* entity, uintptr_t placementPtrOffset, float* x, float* y,
                          float* z) {
    const uint8_t* place = Placement(entity, placementPtrOffset);
    if (!place) return false;
    *x = *reinterpret_cast<const int32_t*>(place + kPlaceX) * kFixedScale;
    *y = *reinterpret_cast<const int32_t*>(place + kPlaceY) * kFixedScale;
    *z = *reinterpret_cast<const int32_t*>(place + kPlaceZ) * kFixedScale;
    return true;
}

// Forward direction in the game's frame, as the game computes it
// (FUN_0041FC20: x = sin(heading), y = cos(heading)). False when the trig
// tables have not been built yet, which is the case in the menus.
inline bool ReadFacing(const uint8_t* entity, uintptr_t placementPtrOffset, float* dirX,
                       float* dirY) {
    const uint8_t* place = Placement(entity, placementPtrOffset);
    if (!place) return false;
    const uint16_t heading = *reinterpret_cast<const uint16_t*>(place + kPlaceHeading);
    if (heading >= kTrigTableEntries) return false;
    const int32_t s = *reinterpret_cast<const int32_t*>(kSinTablePtr + heading * 4u);
    const int32_t c = *reinterpret_cast<const int32_t*>(kCosTablePtr + heading * 4u);
    if (s == 0 && c == 0) return false;   // table not built
    *dirX = s * kFixedScale;
    *dirY = c * kFixedScale;
    return true;
}

inline bool CameraPosition(float* x, float* y) {
    uint8_t* camera = CameraStruct();
    if (!camera) return false;
    *x = *reinterpret_cast<int32_t*>(camera + kCameraXOffset) * kFixedScale;
    *y = *reinterpret_cast<int32_t*>(camera + kCameraYOffset) * kFixedScale;
    return true;
}

}  // namespace game
