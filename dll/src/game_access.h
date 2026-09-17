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

// The viewports, and the rectangle each of them counts an object as inside.
//
// gta2.exe!0x0045BD53 loads the manager as the `this` for the on-screen test:
//
//     manager     = *(void**)0x005EB4FC
//     count       = *(uint8_t*)(manager + 0x23)
//     viewport[i] = *(void**)(manager + 4 + i*4)
//
// FUN_0045AEA0 is the test - a point against that viewport's rectangle, widened
// by a margin passed in:
//
//     pos.x inside [ vp+0x20 - m , vp+0x24 + m ]
//     pos.y inside [ vp+0x28 - m , vp+0x2C + m ]
//
// Six viewports exist for split screen; single player uses one. Reached through
// FUN_0045BBA0, which is what the recycle test and several other systems ask
// "can anything see this".
// GTA2's hardcoded 4:3 aspect ratio, as 12288 = 0.75 in 16.14 fixed point.
//
// gta2.exe!SetupCameraView (0x00472110) builds the tile rectangle it walks, and
// hands to gbh_SetCamera, like this:
//
//     halfW = (fixed(8 - storey) + camera[0xA0]) / camera[0xA4]
//     halfH = halfW * kMapAspect                       <- this value
//     minX  = (camera[0x98] - halfW / 2) >> 14         and so on for max, and Y
//
// So the vertical extent is derived from the horizontal one by multiplying by
// 3/4. Scaling it is measurable and does work - logged at 294% the rectangle
// went from 8 tiles tall to 22 - but it is the *map tile walk*, not what gates
// objects: over that same range the span of sprites the game handed us grew by
// two tiles and stopped. Kept because it identifies the constant, not because
// it is a lever worth pulling. The lever is kVisibilityMarginPtr below.
constexpr uintptr_t kMapAspectPtr = 0x006636C8;
inline int32_t MapAspect() { return *reinterpret_cast<int32_t*>(kMapAspectPtr); }

// How far outside a viewport an object still counts as visible, in 16.14 fixed
// point - the margin FUN_0045AEA0 pads the rectangle with:
//
//     pos.x > viewport[0x20] - margin  &&  pos.x < viewport[0x24] + margin
//     pos.y > viewport[0x28] - margin  &&  pos.y < viewport[0x2C] + margin
//
// Every visibility query in the game passes this one global as that margin -
// FUN_0041F940, FUN_0041F960, and the per-viewport loop in FUN_00424090 - and
// the only write to it, at 0x004FFAA5, copies a zero Fixed in at startup.
// Nothing writes it again. So the margin is zero for the life of the process
// and an object a step outside GTA2's 4:3 viewport is simply out of view: it
// stops being drawn, its unseen-frame count at obj+0x76 climbs, and past 0x81
// of them the recycler destroys it. That is the popping, and this is the one
// number that moves the boundary for all of it at once.
// How far the object grid is walked around the camera, and where to change it.
//
// This is what actually decides whether a car or a pedestrian is drawn. Per
// frame, FUN_0045A5A0 calls FUN_00447390 three times - one grid each for
// pedestrians, cars and objects - and each call sizes its rectangle like this
// (gta2.exe 0x00447390, camera = *(0x005E3CC4)):
//
//     minCol = (camera[0x78] - DAT_005E672C) >> 14   clamped to 0..255
//     maxCol = (camera[0x7C] + DAT_005E6944) >> 14
//     minRow = (camera[0x80] - DAT_005E672C) >> 14
//     maxRow = (camera[0x84] + DAT_005E6944) >> 14
//     FUN_00446D60(minCol, maxCol, minRow, maxRow)
//
// FUN_00446D60 then walks exactly those grid cells and submits what it finds to
// the draw tree. So the rectangle is the camera's own world-space box padded by
// two globals - and **the same pair pads both axes**, with no aspect ratio
// anywhere in it. It is a square box around a 4:3 camera, which is why a 16:9
// frame pokes out of it at the left and right and nowhere else.
//
// Both globals are zero, so there is no padding at all. Neither is writable in
// place: DAT_005E672C is a shared Fixed used elsewhere in the game, and writing
// it would move far more than this rectangle. What is safe is to repoint the
// four PUSH operands at Fixed values we own, which is what kObjectRangeSites
// does - it changes only these three calls and leaves the game's own constants
// untouched. camera+0x78..0x84 itself is not a lever: the game rebuilds that
// box from the camera every frame, which is why writing it read as inert.
constexpr uintptr_t kObjectRangeMinPad = 0x005E672C;
constexpr uintptr_t kObjectRangeMaxPad = 0x005E6944;
// The operand of each PUSH, one past its 0x68 opcode.
constexpr uintptr_t kObjectRangeSites[4] = {0x004473A6, 0x004473D6, 0x00447406,
                                            0x00447439};

constexpr uintptr_t kVisibilityMarginPtr = 0x005E4CB8;
inline int32_t VisibilityMargin() {
    return *reinterpret_cast<int32_t*>(kVisibilityMarginPtr);
}
inline void SetVisibilityMargin(int32_t fixed) {
    *reinterpret_cast<int32_t*>(kVisibilityMarginPtr) = fixed;
}

// The test that decides where GTA2 is allowed to *create* something, and the
// reason four widened rectangles changed nothing.
//
// Drawing is not gated. FUN_0045A5A0 walks three object grids (FUN_00447390 ->
// FUN_00446D60), and the only per-object test on that path is FUN_00446950,
// `obj[0x30] > 1` - not spatial. Everything in the walked cells reaches the draw
// tree and is drawn. Measured too: over 330k car-frames, not one car alive
// inside the camera's own view went undrawn.
//
// Creation is gated, and by a different rectangle test from the one every
// earlier attempt moved:
//
//   FUN_0042A4C0  per-frame vehicle manager (destroy pass, then populate)
//    -> FUN_004B4E60   for each viewport rectangle, gated on `skip_recycling`
//     -> FUN_004B4A60  try each of the four edges
//      -> FUN_004B34E0 the traffic spawner. At 0x004B47A0:
//
//              MOV  EAX, [ESI+0x4]        ; the candidate position
//              MOV  ECX, [0x005EB4FC]     ; the viewport manager
//              PUSH EAX
//              CALL 0x0045BC90            ; can anything see it?
//              TEST AL, AL
//              JNZ  skip                  ; visible -> do not create
//
//   FUN_0045BC90 walks the viewports and calls FUN_0045AF40 on each rectangle,
//   at viewport+0x90 and viewport+0x208.
//
// **FUN_0045AF40 takes no margin.** It is four plain Fixed comparisons against
// rect+0x20..0x2C:
//
//     visible = pos.y <= maxY && pos.y >= minY && pos.x <= maxX && pos.x >= minX
//
// That is the whole spawn boundary. FUN_0045AEA0 - the *other* rectangle test,
// the one that does take a margin and is where kVisibilityMarginPtr is read -
// is never called on this path. So widening that global moved recycling and
// nothing else, which is exactly what was observed.
//
// FUN_0045AF40 is called from FUN_0045BC90 alone, and FUN_0045BC90 only from
// the three vehicle-creation gates (FUN_004B34E0, FUN_00427D60, FUN_00428540)
// and the light cull at FUN_004690B0. Detouring it therefore moves where the
// game is willing to put new objects and touches nothing else.
constexpr uintptr_t kSpawnVisibleTest = 0x0045AF40;
// SUB ESP,0xc / PUSH ESI / MOV ESI,ECX - the first five bytes, checked before
// anything is written so a different build of gta2.exe is left alone.
constexpr uint8_t kSpawnVisiblePrologue[5] = {0x83, 0xEC, 0x0C, 0x56, 0x8B};

// Pedestrians are populated by their own spawner, and it wants a different
// lever from the cars.
//
// FUN_00440CC0 is the only pedestrian creator - FUN_004404F0, which allocates
// the ped and picks its type, has exactly one caller. It is handed a viewport
// rectangle (FUN_004415E0 does MOV ECX,[0x005E5BC0], the population object, then
// PUSH ESI, ESI coming from the FUN_0045A800/FUN_0045A850 rectangle iterator),
// builds a ring around that rectangle's *second* bounds pair, walks the four
// edges, and puts a pedestrian on the ring facing inwards:
//
//     00440CE1  PUSH 0x005E5E74        ; ring width
//     00440CE7  LEA  ECX, [ESI+0x78]   ; rect2.minX, so viewport+0x108
//     00440CEE  CALL 0x00401B40        ; minus -> the ring's left edge
//     00440CFE / 00440D04   +0x7C, plus
//     00440D1D / 00440D23   +0x80, minus
//     00440D39 / 00440D41   +0x84, plus
//
//     004410E3  MOV  ECX, [0x005EB4FC] ; the viewport manager
//     004410EA  CALL 0x0045BC10        ; is the point on screen?  (no margin)
//     004410F1  JNZ  skip              ; yes -> do not create
//
// Two things follow, and both were learned by getting them wrong first.
//
// **Do not widen the test at 0x004410EA.** No pedestrians spawn at all: the
// ring's inner edge *is* the rectangle the test rejects against, and the four
// edge points are fixed rather than a search, so a widened test swallows every
// candidate. The car spawner survives the same treatment only because it walks
// further along the road and asks again.
//
// **Widen the ring, but only by what the frame actually overhangs, and only on
// x.** A pedestrian spawned on the ring walks inwards from it, and the recycler
// destroys anything unseen for 0x81 frames - so a ring pushed out by more than
// the frame needs means they are killed on the way in and the pavements empty.
// That is what "far fewer pedestrians" was. The frames match vertically, so the
// two y operands are left exactly as the game had them and vertical population
// is untouched; only the two x operands move.
//
// Each edge has its own PUSH, which is what makes the x/y split possible.
// DAT_005E5E74 is a shared Fixed with forty-odd readers, so it is never written
// - the operands are repointed, as kObjectRangeSites does.
constexpr uintptr_t kPedRingWidth = 0x005E5E74;
// The operand of each PUSH, one past its 0x68 opcode: minX, maxX, then minY,
// maxY. The first two are the ones that move.
constexpr uintptr_t kPedRingSitesX[2] = {0x00440CE2, 0x00440CFF};
constexpr uintptr_t kPedRingSitesY[2] = {0x00440D1E, 0x00440D3A};

// The second rectangle on the same object, the one FUN_0040CF60 tests and
// FUN_00440CC0 builds its ring from - viewport+0x108..0x114 for the first rect
// base. Not the +0x20..0x2C pair every other test reads.
constexpr uintptr_t kRect2MinXOffset = 0x78;
constexpr uintptr_t kRect2MaxXOffset = 0x7C;
constexpr uintptr_t kRect2MinYOffset = 0x80;
constexpr uintptr_t kRect2MaxYOffset = 0x84;

constexpr uintptr_t kViewportManagerPtr = 0x005EB4FC;
constexpr uintptr_t kViewportArrayOffset = 0x04;
// Whether a viewport slot is in use. The game does not keep a count: every
// visibility loop runs all six slots and tests this flag on each.
constexpr uintptr_t kViewportEnabledOffset = 0x8E;

// The rectangle every visibility test measures against, and the second one that
// only counts while viewport[0x2D0] is set:
//
//     0045BBC8  LEA ECX, [ESI + 0x90]     <- FUN_0045AEA0's `this`
//     0045BBE3  LEA ECX, [ESI + 0x208]    <- and again for the second
//
// FUN_0045AEA0 then reads +0x20/+0x24 (x) and +0x28/+0x2C (y) *from that*, so
// the fields are at viewport+0xB0..0xBC and +0x228..0x234. Written down as
// +0x20..+0x2C for most of this investigation, which is an unrelated zeroed
// field - every reading taken of "the viewport rectangle", and the one attempt
// at widening it, went to the wrong address and proved nothing.
constexpr uintptr_t kViewportRectBases[2] = {0x90, 0x208};
constexpr uintptr_t kViewportSecondRectFlag = 0x2D0;
constexpr uintptr_t kRectMinXOffset = 0x20;
constexpr uintptr_t kRectMaxXOffset = 0x24;
constexpr uintptr_t kRectMinYOffset = 0x28;
constexpr uintptr_t kRectMaxYOffset = 0x2C;
constexpr int kMaxViewports = 6;

inline uint8_t* ViewportManager() {
    return *reinterpret_cast<uint8_t**>(kViewportManagerPtr);
}

inline uint8_t* Viewport(int index) {
    uint8_t* manager = ViewportManager();
    if (!manager || index < 0 || index >= kMaxViewports) return nullptr;
    return *reinterpret_cast<uint8_t**>(manager + kViewportArrayOffset + index * 4);
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
