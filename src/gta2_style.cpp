#include "gta2_style.h"

#include "alpha_bleed.h"

#include <cstdio>
#include <cstring>

namespace gta2 {
namespace {

constexpr int kTilesPerPage = 16;      // 4x4 grid of 64x64 tiles in a 256x256 page
constexpr int kPageStride = 256;
constexpr int kPageBytes = kPageStride * kPageStride;
constexpr int kPalettesPerPage = 64;
constexpr int kPalettePageBytes = 256 * kPalettesPerPage * 4;

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(static_cast<size_t>(size));
    bool ok = size > 0 && fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

}  // namespace

bool Style::Load(const std::string& path, std::string* error) {
    std::vector<uint8_t> data;
    if (!ReadFile(path, &data)) {
        *error = "cannot read style file: " + path;
        return false;
    }
    if (data.size() < 6 || memcmp(data.data(), "GBST", 4) != 0) {
        *error = "not a GBST style file: " + path;
        return false;
    }

    size_t tileOff = 0, tileSize = 0, palOff = 0, palxOff = 0, palxSize = 0;
    size_t offset = 6;
    while (offset + 8 <= data.size()) {
        char tag[5] = {};
        memcpy(tag, &data[offset], 4);
        uint32_t size = ReadU32(&data[offset + 4]);
        size_t body = offset + 8;
        if (body + size > data.size()) break;

        if (!strcmp(tag, "TILE")) {
            tileOff = body;
            tileSize = size;
        } else if (!strcmp(tag, "PPAL")) {
            palOff = body;
        } else if (!strcmp(tag, "PALX")) {
            palxOff = body;
            palxSize = size;
        }
        offset = body + size;
    }

    if (!tileOff || !palOff || !palxOff) {
        *error = "style file missing TILE/PPAL/PALX chunks";
        return false;
    }

    DecodeTiles(data.data(), tileOff, tileSize, palOff, palxOff, palxSize);
    return true;
}

void Style::OverrideTile(int index, const uint32_t* pixels) {
    if (index < 0 || index >= static_cast<int>(tiles_.size()) || !pixels) return;
    Tile& tile = tiles_[index];
    memcpy(tile.pixels.data(), pixels, kTilePixels * sizeof(uint32_t));
    tile.hasTransparency = false;
    for (uint32_t pixel : tile.pixels) {
        if ((pixel & 0xFF000000u) == 0) {
            tile.hasTransparency = true;
            break;
        }
    }
}

void Style::DecodeTiles(const uint8_t* data, size_t tileOffset, size_t tileSize,
                        size_t palOffset, size_t palIndexOffset, size_t palIndexSize) {
    const int pageCount = static_cast<int>(tileSize / kPageBytes);
    const int tileCount = pageCount * kTilesPerPage;
    const int paletteEntries = static_cast<int>(palIndexSize / 2);

    tiles_.resize(tileCount);
    for (int index = 0; index < tileCount; ++index) {
        Tile& tile = tiles_[index];
        tile.pixels.resize(kTilePixels);

        const int page = index / kTilesPerPage;
        const int slot = index % kTilesPerPage;
        const size_t pixelBase = tileOffset + static_cast<size_t>(page) * kPageBytes +
                                 static_cast<size_t>(slot / 4) * kTileSize * kPageStride +
                                 static_cast<size_t>(slot % 4) * kTileSize;

        const uint16_t palette =
            index < paletteEntries ? ReadU16(&data[palIndexOffset + index * 2]) : 0;
        const size_t paletteBase = palOffset +
                                   static_cast<size_t>(palette / kPalettesPerPage) * kPalettePageBytes +
                                   static_cast<size_t>(palette % kPalettesPerPage) * 4;

        for (int y = 0; y < kTileSize; ++y) {
            const uint8_t* row = &data[pixelBase + static_cast<size_t>(y) * kPageStride];
            for (int x = 0; x < kTileSize; ++x) {
                const uint8_t colorIndex = row[x];
                if (colorIndex == 0) {
                    tile.pixels[y * kTileSize + x] = 0;
                    tile.hasTransparency = true;
                    continue;
                }
                const uint8_t* c = &data[paletteBase + static_cast<size_t>(colorIndex) * 256];
                tile.pixels[y * kTileSize + x] =
                    0xFF000000u | (static_cast<uint32_t>(c[2]) << 16) |
                    (static_cast<uint32_t>(c[1]) << 8) | c[0];
            }
        }
        // See alpha_bleed.h: transparent black is what makes a cutout's edge
        // read as a hard black line once anything filters it.
        if (tile.hasTransparency) {
            BleedTransparentEdges(tile.pixels.data(), kTileSize, kTileSize);
            // And close the ones that are not really holes at all - see
            // CloseArtworkHoles. The live path in texture_store.cpp does the
            // same; this is the fallback that parses the .sty itself.
            CloseArtworkHoles(tile.pixels.data(), kTileSize, kTileSize);
        }
    }
}

}  // namespace gta2
