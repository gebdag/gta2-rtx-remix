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
    sprites_.clear();
    spriteGraphics_.clear();
    deltas_.clear();
    deltaStore_.clear();
    palettes_.clear();
    paletteIndex_.clear();
    tilePalettes_ = 0;
    size_t delxOff = 0, delxSize = 0;
    memset(spriteBases_, 0, sizeof(spriteBases_));
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
            palettes_.assign(data.begin() + body, data.begin() + body + size);
        } else if (!strcmp(tag, "PALX")) {
            palxOff = body;
            palxSize = size;
            for (size_t at = body; at + 2 <= body + size; at += 2) {
                paletteIndex_.push_back(ReadU16(&data[at]));
            }
        } else if (!strcmp(tag, "PALB") && size >= 2) {
            tilePalettes_ = ReadU16(&data[body]);
        } else if (!strcmp(tag, "SPRG")) {
            spriteGraphics_.assign(data.begin() + body, data.begin() + body + size);
        } else if (!strcmp(tag, "SPRX")) {
            for (size_t at = body; at + 8 <= body + size; at += 8) {
                sprites_.push_back({ReadU32(&data[at]), data[at + 4], data[at + 5]});
            }
        } else if (!strcmp(tag, "SPRB") && size >= sizeof(spriteBases_)) {
            for (int i = 0; i < 6; ++i) spriteBases_[i] = ReadU16(&data[body + i * 2]);
        } else if (!strcmp(tag, "DELX")) {
            delxOff = body;
            delxSize = size;
        } else if (!strcmp(tag, "DELS")) {
            deltaStore_.assign(data.begin() + body, data.begin() + body + size);
        }
        offset = body + size;
    }

    if (!tileOff || !palOff || !palxOff) {
        *error = "style file missing TILE/PPAL/PALX chunks";
        return false;
    }

    DecodeTiles(data.data(), tileOff, tileSize, palOff, palxOff, palxSize);

    // DELX: per sprite, its number, how many deltas it has and each one's size in
    // DELS, whose data runs on in the same order.
    uint32_t storeAt = 0;
    for (size_t at = delxOff; delxOff && at + 4 <= delxOff + delxSize;) {
        const int sprite = ReadU16(&data[at]);
        const int count = data[at + 2];
        at += 4;
        if (at + static_cast<size_t>(count) * 2 > delxOff + delxSize) break;
        DeltaSet set;
        for (int i = 0; i < count; ++i) {
            const uint16_t size = ReadU16(&data[at + i * 2]);
            set.offsets.push_back(storeAt);
            set.sizes.push_back(size);
            storeAt += size;
        }
        at += static_cast<size_t>(count) * 2;
        deltas_.emplace_back(sprite, std::move(set));
    }
    return true;
}

// A delta is a list of runs: a 16-bit skip from where the last run ended, a
// length, and that many palette indices. Positions are in the sprite's page, so
// a row is 256 bytes whatever the sprite's own width.
bool Style::DeltaPixels(SpriteBase base, int index, int delta, std::vector<DeltaPixel>* out) const {
    const int which = static_cast<int>(base);
    if (index < 0 || index >= spriteBases_[which] || delta < 0) return false;
    int first = 0;
    for (int i = 0; i < which; ++i) first += spriteBases_[i];
    const int sprite = first + index;
    for (const auto& entry : deltas_) {
        if (entry.first != sprite) continue;
        const DeltaSet& set = entry.second;
        if (delta >= static_cast<int>(set.sizes.size())) return false;
        const size_t begin = set.offsets[delta];
        const size_t end = begin + set.sizes[delta];
        if (end > deltaStore_.size()) return false;
        out->clear();
        uint32_t position = 0;
        for (size_t at = begin; at + 3 <= end;) {
            position += ReadU16(&deltaStore_[at]);
            const int length = deltaStore_[at + 2];
            at += 3;
            for (int i = 0; i < length && at + i < end; ++i) {
                out->push_back({static_cast<int>((position + i) % kPageStride),
                                static_cast<int>((position + i) / kPageStride),
                                deltaStore_[at + i]});
            }
            position += length;
            at += length;
        }
        return !out->empty();
    }
    return false;
}

bool Style::SpriteDeltaCentre(SpriteBase base, int index, int delta, float* x, float* y) const {
    std::vector<DeltaPixel> pixels;
    if (!DeltaPixels(base, index, delta, &pixels)) return false;
    double sumX = 0.0, sumY = 0.0;
    for (const DeltaPixel& p : pixels) {
        sumX += p.x;
        sumY += p.y;
    }
    // Pixel centres, so a one-pixel delta at column 0 is at 0.5.
    *x = static_cast<float>(sumX / pixels.size()) + 0.5f;
    *y = static_cast<float>(sumY / pixels.size()) + 0.5f;
    return true;
}

bool Style::SpriteDeltaBounds(SpriteBase base, int index, int delta, int* x0, int* y0, int* x1,
                              int* y1) const {
    std::vector<DeltaPixel> pixels;
    if (!DeltaPixels(base, index, delta, &pixels)) return false;
    *x0 = *y0 = 1 << 30;
    *x1 = *y1 = -1;
    for (const DeltaPixel& p : pixels) {
        if (p.x < *x0) *x0 = p.x;
        if (p.y < *y0) *y0 = p.y;
        if (p.x + 1 > *x1) *x1 = p.x + 1;
        if (p.y + 1 > *y1) *y1 = p.y + 1;
    }
    return true;
}

// PPAL keeps 64 palettes to a 64K page, interleaved: entry i of palette p is the
// dword at (p / 64) * 64K + i * 256 + (p % 64) * 4, as B, G, R. A sprite's own
// palette is the PALX slot after the tiles' ones, at its number across all bases.
bool Style::SpriteDeltaColour(SpriteBase base, int index, int delta, float* rgb) const {
    std::vector<DeltaPixel> pixels;
    if (!DeltaPixels(base, index, delta, &pixels)) return false;
    const int which = static_cast<int>(base);
    int first = 0;
    for (int i = 0; i < which; ++i) first += spriteBases_[i];
    const size_t slot = static_cast<size_t>(tilePalettes_) + first + index;
    if (slot >= paletteIndex_.size()) return false;
    const uint16_t palette = paletteIndex_[slot];
    const size_t paletteBase = static_cast<size_t>(palette / kPalettesPerPage) * kPalettePageBytes +
                               static_cast<size_t>(palette % kPalettesPerPage) * 4;
    double sum[3] = {};
    int counted = 0;
    for (const DeltaPixel& p : pixels) {
        if (p.index == 0) continue;
        const size_t at = paletteBase + static_cast<size_t>(p.index) * 256;
        if (at + 3 > palettes_.size()) continue;
        sum[0] += palettes_[at + 2];
        sum[1] += palettes_[at + 1];
        sum[2] += palettes_[at];
        ++counted;
    }
    if (!counted) return false;
    for (int c = 0; c < 3; ++c) rgb[c] = static_cast<float>(sum[c] / counted / 255.0);
    return true;
}

bool Style::SpriteArtwork(SpriteBase base, int index, SpriteIndices* out) const {
    const int which = static_cast<int>(base);
    if (!out || index < 0 || index >= spriteBases_[which]) return false;
    int first = 0;
    for (int i = 0; i < which; ++i) first += spriteBases_[i];
    const size_t n = static_cast<size_t>(first + index);
    if (n >= sprites_.size()) return false;
    const SpriteEntry& e = sprites_[n];
    const size_t page = e.offset / kPageBytes;
    const size_t x = e.offset % kPageStride;
    const size_t y = (e.offset % kPageBytes) / kPageStride;
    if (x + e.width > kPageStride || y + e.height > kPageStride ||
        (page + 1) * kPageBytes > spriteGraphics_.size()) {
        return false;
    }
    out->width = e.width;
    out->height = e.height;
    out->indices.resize(static_cast<size_t>(e.width) * e.height);
    for (int row = 0; row < e.height; ++row) {
        memcpy(&out->indices[static_cast<size_t>(row) * e.width],
               &spriteGraphics_[page * kPageBytes + (y + row) * kPageStride + x], e.width);
    }
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
