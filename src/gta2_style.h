// Loader for GTA2 .sty style files (GBST v700).
//
// Only the tile graphics needed for world geometry are decoded here: the TILE
// chunk holds 64x64 paletted tiles packed into 256x256 pages, PALX maps a tile
// to a physical palette, and PPAL stores those palettes interleaved 64-per-page.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gta2 {

constexpr int kTileSize = 64;
constexpr int kTilePixels = kTileSize * kTileSize;

// A sprite's palette indices as the style file stores them, row by row, before
// any palette is applied. Index 0 is the transparency key.
struct SpriteIndices {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> indices;  // width * height
};

// The sprite bases, in the order SPRB lists them.
enum class SpriteBase { Car, Ped, CodeObj, MapObj, User, Font };

// Palette entry 0 is the transparency key for every tile.
struct Tile {
    std::vector<uint32_t> pixels;  // kTilePixels, A8R8G8B8
    bool hasTransparency = false;
};

class Style {
public:
    bool Load(const std::string& path, std::string* error);

    int TileCount() const { return static_cast<int>(tiles_.size()); }
    const Tile& GetTile(int index) const { return tiles_[index]; }

    // Replaces a tile's pixels with artwork captured from the running game,
    // which is authoritative: the game resolves tile numbers through its own
    // remap table, so a direct index into the style file can disagree.
    void OverrideTile(int index, const uint32_t* pixels);

    // A sprite's artwork by base and number within the base, from the SPRX,
    // SPRB and SPRG chunks. False when the style has none, or no such sprite.
    bool SpriteArtwork(SpriteBase base, int index, SpriteIndices* out) const;

private:
    void DecodeTiles(const uint8_t* data, size_t tileOffset, size_t tileSize,
                     size_t palOffset, size_t palIndexOffset, size_t palIndexSize);

    std::vector<Tile> tiles_;

    struct SpriteEntry {
        uint32_t offset;   // into spriteGraphics_, which is 256-wide pages
        uint8_t width, height;
    };
    std::vector<SpriteEntry> sprites_;
    std::vector<uint8_t> spriteGraphics_;
    uint16_t spriteBases_[6] = {};
};

}  // namespace gta2
