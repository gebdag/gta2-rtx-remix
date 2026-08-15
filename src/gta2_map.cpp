#include "gta2_map.h"

#include <cstdio>
#include <cstring>

namespace gta2 {
namespace {

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

bool Map::Load(const std::string& path, std::string* error) {
    std::vector<uint8_t> data;
    if (!ReadFile(path, &data)) {
        *error = "cannot read map file: " + path;
        return false;
    }
    if (data.size() < 6 || memcmp(data.data(), "GBMP", 4) != 0) {
        *error = "not a GBMP map file: " + path;
        return false;
    }

    size_t dmap = 0;
    size_t dmapSize = 0;
    size_t offset = 6;
    while (offset + 8 <= data.size()) {
        char tag[5] = {};
        memcpy(tag, &data[offset], 4);
        uint32_t size = ReadU32(&data[offset + 4]);
        size_t body = offset + 8;
        if (body + size > data.size()) break;
        if (!strcmp(tag, "DMAP")) {
            dmap = body;
            dmapSize = size;
        }
        offset = body + size;
    }
    if (!dmap) {
        *error = "map file has no DMAP chunk";
        return false;
    }

    const size_t columnPool = dmap + kDmapBaseBytes;
    const uint32_t columnWords = ReadU32(&data[columnPool]);
    const size_t columnData = columnPool + 4;
    const size_t blockCountOffset = columnData + static_cast<size_t>(columnWords) * 4;
    const uint32_t blockCount = ReadU32(&data[blockCountOffset]);
    const size_t blockData = blockCountOffset + 4;

    if (blockData + static_cast<size_t>(blockCount) * 12 > dmap + dmapSize) {
        *error = "DMAP chunk is truncated";
        return false;
    }

    return LoadFromDmap(&data[dmap], &data[columnData],
                        static_cast<size_t>(columnWords) * 4, &data[blockData],
                        static_cast<size_t>(blockCount) * 12, error);
}

bool Map::LoadFromDmap(const uint8_t* base, const uint8_t* columns, size_t columnBytes,
                       const uint8_t* blocks, size_t blockBytes, std::string* error) {
    const size_t blockCount = blockBytes / 12;
    if (blockCount == 0) {
        *error = "DMAP has no blocks";
        return false;
    }

    blocks_.resize(blockCount);
    for (size_t i = 0; i < blockCount; ++i) {
        const uint8_t* p = blocks + i * 12;
        Block& b = blocks_[i];
        b.left.raw = ReadU16(p + 0);
        b.right.raw = ReadU16(p + 2);
        b.top.raw = ReadU16(p + 4);
        b.bottom.raw = ReadU16(p + 6);
        b.lid.raw = ReadU16(p + 8);
        b.arrows = p[10];
        b.slope = p[11];
    }

    columns_.assign(static_cast<size_t>(kMapWidth) * kMapHeight, Column{});
    for (size_t cell = 0; cell < columns_.size(); ++cell) {
        const uint32_t wordIndex = ReadU32(base + cell * 4);
        if (static_cast<size_t>(wordIndex) * 4 + 4 > columnBytes) continue;

        const uint8_t* p = columns + static_cast<size_t>(wordIndex) * 4;
        Column& column = columns_[cell];
        column.height = p[0];
        column.offset = p[1];
        const int count = column.height - column.offset;
        if (count <= 0) continue;
        if ((static_cast<size_t>(wordIndex) + 1 + count) * 4 > columnBytes) continue;

        column.blocks.resize(count);
        for (int i = 0; i < count; ++i) {
            const uint32_t index = ReadU32(p + 4 + static_cast<size_t>(i) * 4);
            column.blocks[i] = index < blockCount ? index : 0;
        }
    }
    return true;
}

}  // namespace gta2
