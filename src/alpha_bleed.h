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

// Close the holes GTA2's artwork never meant to be holes.
//
// Palette entry 0 is the colour key, and the artwork uses it for two different
// things. Around the outside of a cutout it means "this is not part of the
// picture" - the sky around an arch, the gap a fence is not - and that has to
// stay a hole. Inside the artwork it is just the colour the artist drew an
// outline with, and there it is a slit straight through a solid surface.
//
// GTA2's own renderers never showed the difference. A slit in a wall showed the
// black background behind it and read as a dark outline, which is presumably
// what it was drawn to be. A path tracer shows what is really behind, and at
// night that is the sky: every one of these becomes a bright bluish line, and
// lets light through besides.
//
// A transparent island that reaches no edge of the tile is enclosed by artwork,
// and is a candidate. That alone is not enough - a window in a wall and the gaps
// in a grille are enclosed too, and are genuinely see-through - so the test is
// how *narrow* it is: an island with no texel whose whole 3x3 neighbourhood is
// also transparent is at most two texels across anywhere. Nothing that reads as
// a window is that thin.
//
// Measured over a Remix capture of the shipped districts (1789 textures, 528
// with transparency, 10133 enclosed islands): 9017 of those islands are two
// texels across or less, and they are the speckles and the outline slits. The
// other 1116 include every window and grille, and are left alone.
//
// The honest limit: a *long* slit can be three texels across where it bends, and
// then it is the same shape as a small window pane - a 30-texel island of one
// and a 30-texel island of the other are not distinguishable by shape. Those are
// left open, because closing a window is a visible mistake and leaving a slit
// open is only the dark line the original had. maxIslandTexels is the way to go
// further: it closes any enclosed island of at most that many texels whatever
// its shape. 0 leaves the rule to thinness alone, which is the safe default.
//
// Run *after* BleedTransparentEdges. Every texel being closed is within a texel
// or two of the artwork, so the bleed has already given it the neighbouring
// colour and there is nothing to do here but the alpha. Returns how many texels
// it closed.
inline int CloseArtworkHoles(uint32_t* pixels, int width, int height,
                             int maxIslandTexels = 0) {
    if (!pixels || width <= 2 || height <= 2) return 0;
    const int count = width * height;

    // 0 = artwork, 1 = transparent and not yet reached, 2 = transparent and
    // connected to the edge of the tile, so part of a real cutout.
    std::vector<uint8_t> state(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) state[i] = (pixels[i] >> 24) ? 0u : 1u;

    std::vector<int> stack;
    stack.reserve(static_cast<size_t>(count));
    auto reach = [&](int i) {
        if (state[i] == 1u) {
            state[i] = 2u;
            stack.push_back(i);
        }
    };
    for (int x = 0; x < width; ++x) {
        reach(x);
        reach((height - 1) * width + x);
    }
    for (int y = 0; y < height; ++y) {
        reach(y * width);
        reach(y * width + width - 1);
    }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % width, y = i / width;
        if (x > 0) reach(i - 1);
        if (x < width - 1) reach(i + 1);
        if (y > 0) reach(i - width);
        if (y < height - 1) reach(i + width);
    }

    // Whatever is still 1 is enclosed. Take one island at a time.
    int closed = 0;
    std::vector<int> island;
    for (int seed = 0; seed < count; ++seed) {
        if (state[seed] != 1u) continue;
        island.clear();
        stack.clear();
        stack.push_back(seed);
        state[seed] = 3u;   // claimed by this island
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            island.push_back(i);
            // No bounds test: an enclosed island holds no border texel by
            // construction, so all four neighbours are real and i-1 / i+1
            // cannot wrap onto the row next door.
            const int n[4] = {i - 1, i + 1, i - width, i + width};
            for (int k = 0; k < 4; ++k) {
                if (state[n[k]] == 1u) {
                    state[n[k]] = 3u;
                    stack.push_back(n[k]);
                }
            }
        }

        bool wide = false;
        if (static_cast<int>(island.size()) > maxIslandTexels) {
            // Thick anywhere means see-through on purpose. The 3x3 is what makes
            // this test survive a diagonal: a staircase of single texels has all
            // four orthogonal neighbours inside the island at every step, and
            // would read as solid to a four-neighbour test.
            for (int i : island) {
                const int x = i % width, y = i / width;
                bool solid = true;
                for (int dy = -1; dy <= 1 && solid; ++dy) {
                    for (int dx = -1; dx <= 1 && solid; ++dx) {
                        if (!dx && !dy) continue;
                        const int nx = x + dx, ny = y + dy;
                        // Off the tile counts as artwork, which is right: an
                        // island touching the edge is not enclosed anyway.
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                            solid = false;
                        } else if (state[ny * width + nx] == 0u) {
                            solid = false;
                        }
                    }
                }
                if (solid) {
                    wide = true;
                    break;
                }
            }
        }
        if (wide) continue;
        for (int i : island) {
            pixels[i] |= 0xFF000000u;
            ++closed;
        }
    }
    return closed;
}

// Soften a cutout's edge.
//
// Bleeding fixed the colour, and the edge is still hard, because the alpha was
// never anything but 0 or 255: GTA2's artwork is palettised with entry 0 as a
// colour key and there is no gradient anywhere in it. A fence or a tree wants
// exactly that, but an explosion is a soft glow and reads wrong as a stencil.
//
// There is nothing to recover, so this invents it: every visible texel is dimmed
// in proportion to how much of its neighbourhood is transparent, which turns the
// one-texel step into a ramp. Strength 0 leaves the artwork alone.
//
// Run *after* BleedTransparentEdges - that one keys off alpha being exactly 0 to
// decide what to fill, and this puts values in between.
inline void FeatherAlpha(uint32_t* pixels, int width, int height, float strength) {
    if (!pixels || strength <= 0.0f || width <= 2 || height <= 2) return;
    const size_t count = static_cast<size_t>(width) * height;
    std::vector<uint32_t> src(pixels, pixels + count);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t here = static_cast<size_t>(y) * width + x;
            const uint32_t alpha = src[here] >> 24;
            if (!alpha) continue;   // already a hole; leave it one

            int clear = 0, total = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    ++total;
                    // Off the edge of the sprite counts as transparent, so a
                    // flame running to the border fades rather than being cut.
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        ++clear;
                        continue;
                    }
                    if ((src[static_cast<size_t>(ny) * width + nx] >> 24) == 0) ++clear;
                }
            }
            if (!clear) continue;

            const float open = static_cast<float>(clear) / static_cast<float>(total);
            float scale = 1.0f - open * strength;
            if (scale < 0.0f) scale = 0.0f;
            const uint32_t faded = static_cast<uint32_t>(alpha * scale + 0.5f);
            pixels[here] = (faded << 24) | (src[here] & 0x00FFFFFFu);
        }
    }
}

}  // namespace gta2
