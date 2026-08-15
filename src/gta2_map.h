// Loader for GTA2 .gmp map files (GBMP v500), DMAP chunk.
//
// DMAP stores a 256x256 grid of offsets into a shared column pool; each column
// lists the block indices occupying levels [offset, height). Blocks are shared
// by index, so the same geometry definition is reused across the whole city.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gta2 {

constexpr int kMapWidth = 256;
constexpr int kMapHeight = 256;
constexpr int kMapLevels = 8;

// One face of a block. Tile 0 means "no face".
struct Face {
    uint16_t raw = 0;

    int Tile() const { return raw & 0x03FF; }
    bool IsFlat() const { return (raw & 0x1000) != 0; }
    bool IsFlipped() const { return (raw & 0x2000) != 0; }
    int Rotation() const { return (raw >> 14) & 3; }  // quarter turns
    explicit operator bool() const { return Tile() != 0; }
};

struct Block {
    Face left, right, top, bottom, lid;
    uint8_t arrows = 0;
    uint8_t slope = 0;

    int GroundType() const { return slope & 3; }
    int SlopeType() const { return slope >> 2; }
};

// A column of blocks at one (x, y) cell. Level `offset + i` holds blocks[i].
struct Column {
    uint8_t height = 0;
    uint8_t offset = 0;
    std::vector<uint32_t> blocks;
};

// Byte offsets of the DMAP arrays inside the game's in-memory map object. The
// running game keeps the map in exactly the file's layout, so both sources feed
// the same parser.
constexpr size_t kDmapBaseBytes = static_cast<size_t>(kMapWidth) * kMapHeight * 4;
constexpr size_t kMapObjectColumnsPtr = 0x40008;
constexpr size_t kMapObjectBlocksPtr = 0x4000C;

class Map {
public:
    bool Load(const std::string& path, std::string* error);

    // base: kMapWidth*kMapHeight uint32 column-word indices.
    // columns: the column pool. blocks: 12-byte block records.
    bool LoadFromDmap(const uint8_t* base, const uint8_t* columns, size_t columnBytes,
                      const uint8_t* blocks, size_t blockBytes, std::string* error);

    const Column& ColumnAt(int x, int y) const { return columns_[y * kMapWidth + x]; }
    const Block& BlockAt(uint32_t index) const { return blocks_[index]; }
    size_t BlockCount() const { return blocks_.size(); }
    bool Empty() const { return columns_.empty(); }

private:
    std::vector<Column> columns_;
    std::vector<Block> blocks_;
};

}  // namespace gta2
