// Textures and palettes captured from the game.
//
// GTA2 hands the renderer every bitmap it uses: gbh_RegisterTexture supplies
// 8-bit indexed pixels and gbh_RegisterPalette the colours. Taking them from
// there means tile artwork comes from the game rather than from a second,
// independently guessed parse of the style file.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <d3d9.h>

namespace gta2dx9 {

// Mirrors the original renderer's 0x20-byte record, because the game reads the
// sprite dimensions back out of it. Only the first 0x14 bytes have to match;
// the trailing padding is ours, which is where the full palette index lives -
// GTA2 registers well over 256 palettes, so the original's byte-wide field at
// +0x12 truncates and would recolour every sprite that uses a high palette.
struct TextureRecord {
    uint8_t pad0[0x0E];
    uint16_t width;      // +0x0E
    uint16_t height;     // +0x10
    uint8_t paletteLow;  // +0x12, kept where the original put it
    uint8_t flags;       // +0x13
    void* pixels;        // +0x14
    uint16_t palette;    // +0x18, ours: the untruncated palette index
    uint16_t revision;   // +0x1A, bumped on unlock so cached textures rebuild
    uint8_t pad1[0x04];
};
static_assert(sizeof(TextureRecord) == 0x20, "texture record must match the original layout");

// Why a sprite came out black or missed a frame. Counted rather than logged per
// event, because the interesting cases happen a few times a second in traffic.
struct TextureTrouble {
    int built = 0;                 // textures (re)built from the game's pixels
    int paletteMissing = 0;        // asked for before the game registered it
    int paletteAllBlack = 0;       // registered before the game filled it in
    int pixelsMovedSilently = 0;   // record reused, cache key could not tell
    int builtWhileLocked = 0;      // built while the game was rewriting it
    int lockFailed = 0;
    bool Quiet() const {
        return !paletteMissing && !paletteAllBlack && !pixelsMovedSilently && !builtWhileLocked &&
               !lockFailed;
    }
};
extern TextureTrouble g_trouble;

// Remembers where a palette lives rather than copying it: colour i is at
// source[i * 64], the interleaved layout the style file stores palettes in, and
// GTA2 fills those pages in after registering them.
void StorePalette(int index, const uint32_t* source);
void ForgetPalette(int index);

// Expands a tile to 64x64 A8R8G8B8 using the game's own tile->texture mapping.
// Palette entry 0 is the transparency key. False if the game has not registered
// that tile yet, in which case the caller keeps whatever it already had.
bool ResolveTileImage(int tileNumber, uint32_t* out);

int PaletteCount();

// The 256 expanded colours of a registered palette, read from the game's page as
// it stands right now, or null if the game has not supplied that one yet.
// hasColour reports whether it has been filled in beyond the transparency key.
// The buffer is shared and only valid until the next call.
const uint32_t* PaletteColours(int index, bool* hasColour = nullptr);

// --- Effect sprites -------------------------------------------------------
//
// Fire, explosions and muzzle flashes are drawn from artwork that fades to
// *black* on its way out to the cutout edge, and those black texels are opaque:
// alpha 255, colour (8,0,0). That is the black rim, and it is why bleeding the
// transparent texels never touched these sprites - the black is in the visible
// part of the picture, not behind the alpha.
//
// Art like that only makes sense drawn additively, where black adds nothing.
// GTA2's own renderers never blended at all (d3ddll.dll sets no blend state; the
// 3dfx one draws colour-keyed), so on a 1999 CRT the rim was simply accepted.
// Under a path tracer an opaque black surface is as black as black gets and it
// reads as a hole punched round the fireball.
//
// Which sprites those are is decided from the artwork rather than from a list:
// a black outline is a thing no ordinary cutout sprite has. Measured over a
// capture of the shipped districts, the fire and explosion frames come out at
// edge median luminance ~5, while the darkest sprite that is not an effect - a
// pedestrian in shadow - sits at 23. The default falls in that gap.
//
// This deliberately does *not* also require a bright core. It used to, and that
// was wrong: an explosion's last frames are embers and smoke, dark all over, and
// the core test threw exactly those out - so the animation went from a glow to
// an opaque black blob on its final frame, which is worse than never having
// fixed it. A dying fire is still a fire, and additively it fades to nothing,
// which is what it should do.
enum class EffectSpriteMode {
    Off,       // treat them like everything else, black rim and all
    Cutout,    // turn the dark fringe into alpha, keep the alpha test
    Additive,  // draw them additively, which is what the artwork was drawn for
};

struct EffectSpriteSettings {
    EffectSpriteMode mode = EffectSpriteMode::Additive;
    // A texel on the cutout boundary must be darker than this, at the median,
    // for the sprite to count as an effect.
    float edgeLuma = 16.0f;
    // ...or the whole sprite is darker than this, brightest texel and all, which
    // is the tail of an animation whose outline has stopped being crisp. Nothing
    // in the capture that is not an effect comes anywhere near: the darkest is a
    // pedestrian peaking at 105. 0 disables this second route.
    float faintLuma = 40.0f;
    // Cutout mode only: alpha = luminance * gain, so the fringe fades out
    // instead of being painted.
    float cutoutGain = 3.0f;
};

EffectSpriteSettings& EffectSprites();

// How many distinct frames of artwork have been classified as effects, and how
// many have been built at all, so the menu can say whether the thresholds are
// picking anything up.
int EffectSpriteFrames();
int ClassifiedFrames();

// A D3D texture for one of the game's records, built on first use and rebuilt
// when the record's palette or pixels change. Shared by the screen-space pass
// and the world-space sprite pass, which draw from the same set of bitmaps.
//
// `effect` reports whether this frame was classified as effect artwork, so the
// sprite pass can draw it additively. It is answered from the cache too, not
// only on the frame the texture is built.
IDirect3DTexture9* DeviceTextureFor(IDirect3DDevice9* device, const void* record,
                                    bool* effect = nullptr);
void ForgetDeviceTexture(const void* record);
void ReleaseDeviceTextures();

// Drops every built texture so the next frame rebuilds them. The classification
// and the pixel work both happen at build time, so a threshold change has no
// effect on artwork that is already cached; this is what makes the sliders live.
void RebuildDeviceTextures();

// --- The texture report ---------------------------------------------------
//
// The classification is a threshold on a measurement, and a threshold is only as
// good as the cases it was set from. So every distinct frame the session builds
// keeps the numbers it was judged on, and the world-space sprite pass says which
// frames it actually drew - that is what separates a fireball from a road tile
// that happens to have a hole in it.
//
// Written beside the game as gta2dx9_textures.csv on shutdown, alongside a
// folder of the frames themselves as 32-bit TGAs. A sprite that still has a
// black rim is then a row to look up rather than a guess: its edge_median and
// peak say exactly how far off the thresholds it fell.

// Called by the sprite pass for each batch it draws, so the report can tell
// sprites from tiles and from the HUD.
void NoteSpriteTexture(const void* record);

// Artwork that lies on the road rather than standing on it: skid marks and the
// blood trails drawn from the same strips. Registered by content - the sprite's
// palette indices - because the game draws those strips in whatever colour the
// moment needs, grey for rubber and red for blood, so neither the palette nor
// the texture record identifies them. See LiveGeometry::AddSprite.
void ClearGroundDecalArtwork();
void AddGroundDecalArtwork(int width, int height, const uint8_t* indices, int stride);
int GroundDecalArtworkCount();
bool IsGroundDecal(const void* record);

// Writes the CSV and returns how many rows it held. Called on shutdown and from
// the menu button.
int WriteTextureReport();

void        SetTextureDumping(bool on);
bool        TextureDumping();
const char* TextureDumpDir();
int         TextureFramesDumped();

// Close the transparent slits and speckles that GTA2's artists drew in the
// colour-key index, which the original renderers showed as a dark outline and a
// path tracer shows as a hole with the sky behind it. See CloseArtworkHoles in
// alpha_bleed.h for what is and is not treated as one. On by default; off is
// there to see what it was doing. Takes effect on textures built after the
// change, so it wants a rebuild.
void SetCloseHoles(bool on);
bool CloseHoles();

// Also close any enclosed island of at most this many texels, whatever its
// shape. 0 leaves the rule to thinness alone, which is the safe default: a long
// slit and a small window pane are the same shape, and closing a window is a
// visible mistake where leaving a slit is only the dark line the original had.
void SetCloseHoleMax(int texels);
int  CloseHoleMax();

// How many texels it has closed this session, for the readout in the menu.
int HolesClosed();

}  // namespace gta2dx9
