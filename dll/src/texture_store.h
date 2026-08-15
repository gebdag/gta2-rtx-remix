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

// colour i of a palette lives at source[i * 64]; that stride is the interleaved
// layout the style file stores palettes in, passed through unchanged.
void StorePalette(int index, const uint32_t* source);

// Expands a tile to 64x64 A8R8G8B8 using the game's own tile->texture mapping.
// Palette entry 0 is the transparency key. False if the game has not registered
// that tile yet, in which case the caller keeps whatever it already had.
bool ResolveTileImage(int tileNumber, uint32_t* out);

int PaletteCount();

// The 256 expanded colours of a registered palette, or null if the game has not
// supplied that one yet.
const uint32_t* PaletteColours(int index);

// A D3D texture for one of the game's records, built on first use and rebuilt
// when the record's palette or pixels change. Shared by the screen-space pass
// and the world-space sprite pass, which draw from the same set of bitmaps.
IDirect3DTexture9* DeviceTextureFor(IDirect3DDevice9* device, const void* record);
void ForgetDeviceTexture(const void* record);
void ReleaseDeviceTextures();

}  // namespace gta2dx9
