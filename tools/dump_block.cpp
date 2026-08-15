// Prints the geometry the mesh builder produces for a single block of a given
// slope type, so a shape can be checked against the game without launching it.
//
//   dump_block.exe <slope type> <path to a .sty>
//
// The block sits at cell (0, 0), so every coordinate should land inside
// x 0..1, z 255..256, y 0..1.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/gta2_map.h"
#include "../src/gta2_style.h"
#include "../src/world_mesh.h"

// No block may reach outside its own cell, so a triangle wider than one unit is
// proof of a displaced vertex wherever it came from.
static int AuditWholeMap(const char* mapPath, const char* stylePath) {
    std::string error;
    gta2::Map map;
    gta2::Style style;
    if (!map.Load(mapPath, &error) || !style.Load(stylePath, &error)) {
        printf("load: %s\n", error.c_str());
        return 1;
    }
    gta2::PartialCuts cuts;
    gta2::WorldMesh mesh;
    gta2::BuildWorldMesh(map, style, nullptr, cuts, &mesh);

    int oversized = 0, degenerate = 0, shown = 0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const gta2::Vertex& a = mesh.vertices[mesh.indices[i]];
        const gta2::Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        const gta2::Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        const float spanX = (std::max)((std::max)(a.x, b.x), c.x) - (std::min)((std::min)(a.x, b.x), c.x);
        const float spanZ = (std::max)((std::max)(a.z, b.z), c.z) - (std::min)((std::min)(a.z, b.z), c.z);
        const float spanY = (std::max)((std::max)(a.y, b.y), c.y) - (std::min)((std::min)(a.y, b.y), c.y);
        const bool same = (a.x == b.x && a.y == b.y && a.z == b.z) ||
                          (a.x == c.x && a.y == c.y && a.z == c.z) ||
                          (b.x == c.x && b.y == c.y && b.z == c.z);
        if (same) ++degenerate;
        if (spanX > 1.01f || spanZ > 1.01f || spanY > 1.01f) {
            ++oversized;
            if (shown++ < 8) {
                printf("  oversized: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)\n", a.x, a.y,
                       a.z, b.x, b.y, b.z, c.x, c.y, c.z);
            }
        }
    }
    printf("%s: %zu triangles, %d oversized, %d degenerate\n", mapPath, mesh.indices.size() / 3,
           oversized, degenerate);
    return oversized ? 2 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "audit") == 0) {
        return AuditWholeMap(argc > 2 ? argv[2] : "<GTA2 folder>\\data\\wil.gmp",
                             argc > 3 ? argv[3] : "<GTA2 folder>\\data\\wil.sty");
    }
    const int type = argc > 1 ? atoi(argv[1]) : 49;
    const char* stylePath = argc > 2 ? argv[2] : "<GTA2 folder>\\data\\wil.sty";

    // One block at cell (0,0); every other cell points at an empty column.
    std::vector<uint8_t> columns(64, 0);
    columns[0] = 1;  // height
    columns[1] = 0;  // offset
    // columns[4..7] is the block index, which is 0.
    columns[16] = 0;  // the empty column: height 0
    columns[17] = 0;

    std::vector<uint8_t> base(256 * 256 * 4, 0);
    for (size_t cell = 0; cell < 256u * 256u; ++cell) {
        const uint32_t word = cell == 0 ? 0u : 4u;
        memcpy(&base[cell * 4], &word, 4);
    }

    std::vector<uint8_t> blocks(24, 0);
    const uint16_t faces[5] = {1, 2, 3, 4, 5};  // left, right, top, bottom, lid
    memcpy(&blocks[0], faces, sizeof(faces));
    blocks[11] = static_cast<uint8_t>(type << 2);

    std::string error;
    gta2::Map map;
    if (!map.LoadFromDmap(base.data(), columns.data(), columns.size(), blocks.data(), blocks.size(),
                          &error)) {
        printf("map: %s\n", error.c_str());
        return 1;
    }
    gta2::Style style;
    if (!style.Load(stylePath, &error)) {
        printf("style: %s\n", error.c_str());
        return 1;
    }

    gta2::PartialCuts cuts;
    gta2::WorldMesh mesh;
    gta2::BuildWorldMesh(map, style, nullptr, cuts, &mesh);

    const char* names[6] = {"?", "left", "right", "top", "bottom", "lid"};
    printf("slope type %d: %zu vertices, %zu triangles, %zu batches\n", type, mesh.vertices.size(),
           mesh.indices.size() / 3, mesh.batches.size());
    for (const gta2::TileBatch& batch : mesh.batches) {
        printf("  face %-6s (tile %d)\n", batch.tile < 6 ? names[batch.tile] : "?", batch.tile);
        for (uint32_t i = batch.indexStart; i < batch.indexStart + batch.indexCount; i += 3) {
            printf("    tri");
            for (int k = 0; k < 3; ++k) {
                const gta2::Vertex& v = mesh.vertices[mesh.indices[i + k]];
                printf("  (%.2f %.2f %.2f uv %.2f %.2f)", v.x, v.y, v.z, v.u, v.v);
            }
            const gta2::Vertex& n = mesh.vertices[mesh.indices[i]];
            printf("  n=(%.2f %.2f %.2f)\n", n.nx, n.ny, n.nz);
        }
    }
    return 0;
}
