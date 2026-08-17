#include "texture_store.h"

#include "../../src/alpha_bleed.h"

#include <cstring>
#include <map>
#include <vector>

#include "game_access.h"

namespace gta2dx9 {
namespace {

constexpr int kPaletteEntries = 256;
constexpr int kPaletteStride = 64;  // in dwords, matching the style file layout
constexpr int kTileSize = 64;

// Tiles are not tightly packed: they live inside 256x256 pages holding a 4x4
// grid of them, and the game registers each tile as a pointer to its top-left
// pixel within that page. Rows are therefore a page apart, not a tile apart.
// Reading them contiguously produces interleaved strips rather than a tile.
constexpr int kPageStride = 256;

// Where each palette lives inside the game's own palette pages, not a copy of
// it. GTA2 registers a palette before it has filled the colours in - 74 of them
// came back entirely black across a single level load - so a snapshot taken at
// registration freezes whatever happened to be there at that instant. That is
// what left cars flat black until something else forced their texture to
// rebuild. Holding the pointer and reading the colours when a texture is
// actually built means we always see what the game currently has.
std::map<int, const uint32_t*> g_palettes;

// Expanded on demand into one shared buffer; a texture build reads it once.
uint32_t g_expanded[kPaletteEntries];

}  // namespace

TextureTrouble g_trouble;

void StorePalette(int index, const uint32_t* source) {
    if (!source || index < 0) return;
    g_palettes[index] = source;
}

void ForgetPalette(int index) { g_palettes.erase(index); }

int PaletteCount() { return static_cast<int>(g_palettes.size()); }

// hasColour reports whether the game has actually filled this palette in yet;
// entry 0 is the transparency key and is ignored for that.
const uint32_t* PaletteColours(int index, bool* hasColour) {
    const auto found = g_palettes.find(index);
    if (found == g_palettes.end() || !found->second) {
        if (hasColour) *hasColour = false;
        return nullptr;
    }
    const uint32_t* page = found->second;
    bool any = false;
    for (int i = 0; i < kPaletteEntries; ++i) {
        g_expanded[i] = page[static_cast<size_t>(i) * kPaletteStride];
        if (i > 0 && (g_expanded[i] & 0x00FFFFFFu) != 0) any = true;
    }
    if (hasColour) *hasColour = any;
    if (!any) ++g_trouble.paletteAllBlack;
    return g_expanded;
}

namespace {

struct CachedTexture {
    IDirect3DTexture9* texture = nullptr;
    uint16_t width = 0, height = 0;
    uint16_t palette = 0xFFFF;
    uint16_t revision = 0xFFFF;
    const void* pixels = nullptr;
    bool provisional = false;  // built before the game had filled the palette in
};

std::map<const void*, CachedTexture> g_deviceTextures;

}  // namespace

IDirect3DTexture9* DeviceTextureFor(IDirect3DDevice9* device, const void* handle) {
    const TextureRecord* record = static_cast<const TextureRecord*>(handle);
    if (!device || !record || !record->pixels || !record->width || !record->height) return nullptr;

    CachedTexture& cached = g_deviceTextures[handle];
    // A texture built from a palette the game had not filled in yet is kept but
    // not trusted, so the next frame builds it again rather than leaving it
    // black for good.
    if (cached.texture && !cached.provisional && cached.width == record->width &&
        cached.height == record->height && cached.palette == record->palette &&
        cached.revision == record->revision) {
        // The four fields the original cache keyed on can all be unchanged while
        // the record has been pointed at different artwork: GTA2 recycles its
        // texture records, and a record reused for another sprite of the same
        // size and palette looks identical here. Counted rather than acted on
        // for now, to find out whether it happens at all.
        if (cached.pixels != record->pixels) ++g_trouble.pixelsMovedSilently;
        return cached.texture;
    }

    if (cached.texture && (cached.width != record->width || cached.height != record->height)) {
        cached.texture->Release();
        cached.texture = nullptr;
    }
    if (!cached.texture &&
        FAILED(device->CreateTexture(record->width, record->height, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &cached.texture, nullptr))) {
        cached.texture = nullptr;
        return nullptr;
    }

    bool paletteHasColour = false;
    const uint32_t* palette = PaletteColours(record->palette, &paletteHasColour);
    if (!palette) {
        // Nothing to build from, so the caller draws nothing this frame: one
        // frame of a missing sprite.
        ++g_trouble.paletteMissing;
        return nullptr;
    }
    if (record->flags & 1) ++g_trouble.builtWhileLocked;  // game is mid-rewrite

    D3DLOCKED_RECT locked;
    if (FAILED(cached.texture->LockRect(0, &locked, nullptr, 0))) {
        ++g_trouble.lockFailed;
        return nullptr;
    }
    const uint8_t* indices = static_cast<const uint8_t*>(record->pixels);
    std::vector<uint32_t> image(static_cast<size_t>(record->width) * record->height);
    for (int y = 0; y < record->height; ++y) {
        const uint8_t* row = indices + static_cast<size_t>(y) * kPageStride;
        uint32_t* out = image.data() + static_cast<size_t>(y) * record->width;
        for (int x = 0; x < record->width; ++x) {
            const uint8_t index = row[x];
            out[x] = index ? (palette[index] | 0xFF000000u) : 0u;
        }
    }
    // Keyed texels are transparent *black*, and every filter that touches this
    // texture - ours, and the mipmaps Remix builds for its own materials -
    // averages that black into the neighbouring colour. That is the hard black
    // rim around cutouts. Give the invisible texels a colour and it goes away.
    gta2::BleedTransparentEdges(image.data(), record->width, record->height);
    for (int y = 0; y < record->height; ++y) {
        memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch,
               image.data() + static_cast<size_t>(y) * record->width,
               static_cast<size_t>(record->width) * 4);
    }
    cached.texture->UnlockRect(0);

    cached.width = record->width;
    cached.height = record->height;
    cached.palette = record->palette;
    cached.revision = record->revision;
    cached.pixels = record->pixels;
    cached.provisional = !paletteHasColour;
    ++g_trouble.built;
    return cached.texture;
}

void ForgetDeviceTexture(const void* handle) {
    const auto found = g_deviceTextures.find(handle);
    if (found == g_deviceTextures.end()) return;
    if (found->second.texture) found->second.texture->Release();
    g_deviceTextures.erase(found);
}

void ReleaseDeviceTextures() {
    for (auto& entry : g_deviceTextures) {
        if (entry.second.texture) entry.second.texture->Release();
    }
    g_deviceTextures.clear();
}

bool ResolveTileImage(int tileNumber, uint32_t* out) {
    const TextureRecord* record = static_cast<const TextureRecord*>(game::TextureForTile(tileNumber));
    if (!record || !record->pixels) return false;
    if (record->width != kTileSize || record->height != kTileSize) return false;

    const uint32_t* colours = PaletteColours(record->palette);
    if (!colours) return false;

    const uint8_t* indices = static_cast<const uint8_t*>(record->pixels);
    for (int y = 0; y < kTileSize; ++y) {
        const uint8_t* row = indices + static_cast<size_t>(y) * kPageStride;
        for (int x = 0; x < kTileSize; ++x) {
            const uint8_t index = row[x];
            out[y * kTileSize + x] = index ? (colours[index] | 0xFF000000u) : 0u;
        }
    }
    gta2::BleedTransparentEdges(out, kTileSize, kTileSize);
    return true;
}

}  // namespace gta2dx9
