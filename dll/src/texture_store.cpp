#include "texture_store.h"

#include "log.h"

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

// Every distinct frame of artwork ever seen, keyed by its content, and the owner
// of all the D3D textures.
//
// GTA2 animates a tile by rewriting the pixels behind the same texture record -
// ceiling fans, screens, water, traffic lights. Filling those new pixels back
// into the same IDirect3DTexture9, which is what this used to do, gives RTX
// Remix one texture whose content changes: it appears once in the texture picker
// and animates there, and there is no way to replace a single frame of it.
//
// Giving each distinct frame its own texture object gives each its own stable
// Remix hash, so every frame shows up separately and can be replaced on its own.
// The cost is one texture per frame rather than per record, which is bounded by
// how much animation the artwork actually has - a few hundred at most, and
// identical frames still share one entry because the key is the content.
std::map<uint64_t, IDirect3DTexture9*> g_frames;

// Hashed off the palette indices rather than the finished RGBA, because that is
// the smaller buffer and it is what actually changed.
uint64_t FrameKey(const TextureRecord* record) {
    uint64_t h = 0xCBF29CE484222325ULL;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 0x100000001B3ULL;
    };
    mix(record->width);
    mix(record->height);
    // Two sprites with the same indices and different palettes are different
    // pictures, so the palette is part of the identity.
    mix(record->palette);
    const uint8_t* indices = static_cast<const uint8_t*>(record->pixels);
    for (int y = 0; y < record->height; ++y) {
        const uint8_t* row = indices + static_cast<size_t>(y) * kPageStride;
        for (int x = 0; x < record->width; ++x) mix(row[x]);
    }
    return h;
}

// A hash that turned out not to be stable would otherwise grow this without
// limit and exhaust video memory in a long session.
const size_t kMaxFrames = 4096;
bool g_frameLimitLogged = false;

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

    // The content decides which texture this is. An animated tile cycling back
    // to a frame it has already shown gets the same object again, and with it the
    // same Remix hash, which is the whole point.
    const uint64_t key = FrameKey(record);
    const auto existing = g_frames.find(key);
    if (existing != g_frames.end()) {
        cached.texture = existing->second;
        cached.width = record->width;
        cached.height = record->height;
        cached.palette = record->palette;
        cached.revision = record->revision;
        cached.pixels = record->pixels;
        cached.provisional = false;
        return cached.texture;
    }

    if (g_frames.size() >= kMaxFrames) {
        if (!g_frameLimitLogged) {
            g_frameLimitLogged = true;
            Log("texture cache: %u distinct frames, refusing more. Either this district has "
                "extraordinary amounts of animation or the frame hash is not stable.",
                static_cast<unsigned>(g_frames.size()));
        }
        return cached.texture;   // whatever we had last, rather than nothing
    }

    // Never reused: each frame owns its texture for the life of the process, so
    // that its Remix hash stays put.
    cached.texture = nullptr;
    if (FAILED(device->CreateTexture(record->width, record->height, 1, 0, D3DFMT_A8R8G8B8,
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
    // A texture built before the game filled its palette in is the wrong picture,
    // so it is not published under this content key - the next frame builds it
    // again and that one gets remembered.
    if (!cached.provisional) g_frames[key] = cached.texture;
    ++g_trouble.built;
    return cached.texture;
}

void ForgetDeviceTexture(const void* handle) {
    // Only the record's bookkeeping goes: the texture itself belongs to g_frames
    // and may well be shared with another record showing the same artwork.
    g_deviceTextures.erase(handle);
}

void ReleaseDeviceTextures() {
    g_deviceTextures.clear();
    for (auto& entry : g_frames) {
        if (entry.second) entry.second->Release();
    }
    g_frames.clear();
    g_frameLimitLogged = false;
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
