// Colour for the texels nobody can see.
//
// GTA2's artwork is 8-bit palettised with entry 0 as a colour key, so there is
// no alpha channel to blend and every edge is binary. Building an RGBA texture
// from that, the obvious thing is to write 0 for a keyed texel - transparent
// black - and that is what produces the hard black rim around every cutout.
//
// Nothing samples a texel's alpha in isolation. Bilinear filtering, and above
// all the mipmaps RTX Remix builds for its own materials, average *all four*
// channels across neighbouring texels. An opaque orange texel next to a
// transparent black one averages to a dark muddy orange at half alpha, and
// whether that survives the alpha test or gets blended, it is visibly black at
// the edge. The alpha is doing its job; the colour underneath it is wrong.
//
// So the invisible texels get a colour anyway: repeatedly copy the average of
// each transparent texel's visible neighbours inwards, leaving alpha at zero
// throughout. Filtering then blends orange with orange and only the alpha falls
// off, which is the smooth edge that was wanted. This is the standard fix and
// it is usually called alpha bleeding or dilation.
//
// Alpha stays exactly 0 or 255, so this changes nothing about which texels are
// transparent - only what colour they are while being transparent. Existing
// alpha-test behaviour is unaffected.
#pragma once

#include <cstdint>
#include <vector>

namespace gta2 {

// `pixels` is width*height of 0xAARRGGBB. Four passes reach four texels in from
// any edge, which is past anything a mip chain will pull from on a 64x64 tile.
inline void BleedTransparentEdges(uint32_t* pixels, int width, int height, int passes = 4) {
    if (!pixels || width <= 1 || height <= 1) return;

    const size_t count = static_cast<size_t>(width) * height;
    // Which texels have a colour worth copying: opaque to begin with, then
    // whatever previous passes have filled in. Kept separate from alpha, because
    // alpha must not move.
    std::vector<uint8_t> seeded(count);
    bool any = false;
    bool anyHole = false;
    for (size_t i = 0; i < count; ++i) {
        seeded[i] = (pixels[i] >> 24) != 0 ? 1u : 0u;
        any = any || seeded[i];
        anyHole = anyHole || !seeded[i];
    }
    // A fully opaque tile has no edge to soften, and a fully transparent one has
    // no colour to spread. Both are common enough to be worth skipping.
    if (!any || !anyHole) return;

    std::vector<uint32_t> colour(pixels, pixels + count);
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<uint8_t> grown = seeded;
        bool spread = false;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t here = static_cast<size_t>(y) * width + x;
                if (seeded[here]) continue;

                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= width ||
                            ny >= height) {
                            continue;
                        }
                        const size_t there = static_cast<size_t>(ny) * width + nx;
                        if (!seeded[there]) continue;
                        const uint32_t c = colour[there];
                        r += (c >> 16) & 0xFF;
                        g += (c >> 8) & 0xFF;
                        b += c & 0xFF;
                        ++n;
                    }
                }
                if (!n) continue;
                // Alpha deliberately left at 0: this texel stays invisible, it
                // just stops being black.
                colour[here] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
                grown[here] = 1u;
                spread = true;
            }
        }
        seeded.swap(grown);
        if (!spread) break;   // nothing left within reach
    }

    for (size_t i = 0; i < count; ++i) {
        // Only the invisible texels are touched; the visible ones are already
        // exactly what the palette said.
        if ((pixels[i] >> 24) == 0) pixels[i] = colour[i] & 0x00FFFFFFu;
    }
}

}  // namespace gta2
