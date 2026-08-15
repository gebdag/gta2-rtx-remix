#include "texture_store.h"

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

std::map<int, std::vector<uint32_t>> g_palettes;

}  // namespace

void StorePalette(int index, const uint32_t* source) {
    if (!source || index < 0) return;
    std::vector<uint32_t>& palette = g_palettes[index];
    palette.resize(kPaletteEntries);
    for (int i = 0; i < kPaletteEntries; ++i) {
        palette[i] = source[static_cast<size_t>(i) * kPaletteStride];
    }
}

int PaletteCount() { return static_cast<int>(g_palettes.size()); }

const uint32_t* PaletteColours(int index) {
    const auto found = g_palettes.find(index);
    return found == g_palettes.end() ? nullptr : found->second.data();
}

namespace {

struct CachedTexture {
    IDirect3DTexture9* texture = nullptr;
    uint16_t width = 0, height = 0;
    uint16_t palette = 0xFFFF;
    uint16_t revision = 0xFFFF;
};

std::map<const void*, CachedTexture> g_deviceTextures;

}  // namespace

IDirect3DTexture9* DeviceTextureFor(IDirect3DDevice9* device, const void* handle) {
    const TextureRecord* record = static_cast<const TextureRecord*>(handle);
    if (!device || !record || !record->pixels || !record->width || !record->height) return nullptr;

    CachedTexture& cached = g_deviceTextures[handle];
    if (cached.texture && cached.width == record->width && cached.height == record->height &&
        cached.palette == record->palette && cached.revision == record->revision) {
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

    const uint32_t* palette = PaletteColours(record->palette);
    if (!palette) return nullptr;  // Retry once the game registers it.

    D3DLOCKED_RECT locked;
    if (FAILED(cached.texture->LockRect(0, &locked, nullptr, 0))) return nullptr;
    const uint8_t* indices = static_cast<const uint8_t*>(record->pixels);
    for (int y = 0; y < record->height; ++y) {
        uint32_t* out =
            reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
        const uint8_t* row = indices + static_cast<size_t>(y) * kPageStride;
        for (int x = 0; x < record->width; ++x) {
            const uint8_t index = row[x];
            out[x] = index ? (palette[index] | 0xFF000000u) : 0u;
        }
    }
    cached.texture->UnlockRect(0);

    cached.width = record->width;
    cached.height = record->height;
    cached.palette = record->palette;
    cached.revision = record->revision;
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

    const auto palette = g_palettes.find(record->palette);
    if (palette == g_palettes.end()) return false;

    const uint8_t* indices = static_cast<const uint8_t*>(record->pixels);
    const uint32_t* colours = palette->second.data();
    for (int y = 0; y < kTileSize; ++y) {
        const uint8_t* row = indices + static_cast<size_t>(y) * kPageStride;
        for (int x = 0; x < kTileSize; ++x) {
            const uint8_t index = row[x];
            out[y * kTileSize + x] = index ? (colours[index] | 0xFF000000u) : 0u;
        }
    }
    return true;
}

}  // namespace gta2dx9
