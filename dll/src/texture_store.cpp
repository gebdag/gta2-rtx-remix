#include "texture_store.h"

#include "live_geometry.h"
#include "log.h"

#include "../../src/alpha_bleed.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
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
    bool effect = false;       // glow artwork rather than a cutout; see the header
    uint64_t key = 0;          // which frame of artwork this record is showing
};

std::map<const void*, CachedTexture> g_deviceTextures;

EffectSpriteSettings g_effects;
int g_effectFrames = 0;
int g_classifiedFrames = 0;

float Luminance(uint32_t argb) {
    // Rec.601, which is what "how bright does this look" means here; the exact
    // weights matter far less than the fringe being near zero and the core not.
    const float r = static_cast<float>((argb >> 16) & 0xFF);
    const float g = static_cast<float>((argb >> 8) & 0xFF);
    const float b = static_cast<float>(argb & 0xFF);
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// Everything the classification is decided from, kept rather than reduced to a
// yes/no, because the interesting question is always "how close was it?" - a
// sprite with a black rim that did not qualify is a threshold to move, and the
// only way to know by how much is to have the numbers.
struct EffectMeasure {
    bool  hasCutout = false;
    int   fringeTexels = 0;
    int   opaqueTexels = 0;
    float fringeMedian = 0.0f;   // median luminance of the opaque texels on the boundary
    float fringeMean = 0.0f;
    float fringeDarkFraction = 0.0f;  // share of the boundary below 40, for context
    float peak = 0.0f;                // brightest texel anywhere in the sprite
};

// Is this frame of artwork a glow rather than a cutout?
//
// The test is on the *opaque* texels that touch the transparent region: for a
// fence, a tree or a car those are ordinary picture, whatever colour the artist
// used; for a fireball they are the black the glow fades out into. The median
// rather than the mean, so one stray dark pixel on a car cannot swing it.
EffectMeasure MeasureEffect(const uint32_t* image, int width, int height) {
    EffectMeasure m;
    if (!image || width < 3 || height < 3) return m;

    std::vector<float> fringe;
    fringe.reserve(static_cast<size_t>(width) * 2 + height * 2);

    auto clearAt = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return true;  // past the edge
        return (image[static_cast<size_t>(y) * width + x] >> 24) == 0;
    };

    double sum = 0.0;
    int dark = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint32_t texel = image[static_cast<size_t>(y) * width + x];
            if ((texel >> 24) == 0) {
                m.hasCutout = true;
                continue;
            }
            ++m.opaqueTexels;
            const float luma = Luminance(texel);
            if (luma > m.peak) m.peak = luma;
            if (clearAt(x - 1, y) || clearAt(x + 1, y) || clearAt(x, y - 1) || clearAt(x, y + 1)) {
                fringe.push_back(luma);
                sum += luma;
                if (luma < 40.0f) ++dark;
            }
        }
    }
    m.fringeTexels = static_cast<int>(fringe.size());
    if (fringe.empty()) return m;

    std::nth_element(fringe.begin(), fringe.begin() + fringe.size() / 2, fringe.end());
    m.fringeMedian = fringe[fringe.size() / 2];
    m.fringeMean = static_cast<float>(sum / fringe.size());
    m.fringeDarkFraction = static_cast<float>(dark) / static_cast<float>(fringe.size());
    return m;
}

bool IsEffect(const EffectMeasure& m, const EffectSpriteSettings& settings) {
    // No hole at all is a solid bitmap - a HUD panel, a road tile - and has no
    // fringe to judge.
    if (!m.hasCutout || !m.fringeTexels) return false;
    // The outline is black, so the artwork is a glow.
    if (m.fringeMedian < settings.edgeLuma) return true;
    // Or the whole sprite is black, which is the same glow three frames later.
    // Whatever it is, an opaque near-black world sprite is wrong under a path
    // tracer; drawn additively it fades out instead of sitting there.
    if (settings.faintLuma > 0.0f && m.peak < settings.faintLuma) return true;
    return false;
}

// --- The texture report ---------------------------------------------------
//
// One row per distinct frame of artwork the session built, with the numbers the
// classification was decided from and whether the world-space sprite pass ever
// drew it. Written beside the game on shutdown, next to a folder of the frames
// themselves as 32-bit TGAs, so a sprite that still has a black rim can be
// looked at rather than guessed at.
//
// Costs nothing to leave on: the frames are a few kilobytes each and there are a
// few hundred of them in a session.
struct FrameNote {
    uint64_t key = 0;
    int width = 0, height = 0;
    int palette = 0;
    EffectMeasure measure;
    bool classified = false;   // came out an effect under the settings at build time
    bool drawnAsSprite = false;
    bool dumped = false;
};

std::map<uint64_t, FrameNote> g_notes;
bool g_dumpTextures = true;
// See CloseArtworkHoles in alpha_bleed.h.
bool g_closeHoles = true;
int  g_closeHoleMax = 0;
int  g_holesClosed = 0;
char g_dumpDir[MAX_PATH] = "";
int  g_dumped = 0;
const int kMaxDumped = 2048;

// Where the game is, which is where everything else this DLL writes goes.
const char* DumpDir() {
    if (!g_dumpDir[0]) {
        GetModuleFileNameA(nullptr, g_dumpDir, sizeof(g_dumpDir));
        char* slash = strrchr(g_dumpDir, '\\');
        if (slash) strcpy(slash + 1, "gta2dx9_textures");
        else strcpy(g_dumpDir, "gta2dx9_textures");
    }
    return g_dumpDir;
}

// 32-bit uncompressed TGA, top-left origin. Chosen over anything nicer because
// the pixels are already B,G,R,A in memory order, so the body is one write and
// there is no encoder to get wrong.
bool WriteTga(const char* path, const uint32_t* image, int width, int height) {
    FILE* out = fopen(path, "wb");
    if (!out) return false;
    uint8_t header[18] = {};
    header[2] = 2;   // uncompressed true colour
    header[12] = static_cast<uint8_t>(width & 0xFF);
    header[13] = static_cast<uint8_t>((width >> 8) & 0xFF);
    header[14] = static_cast<uint8_t>(height & 0xFF);
    header[15] = static_cast<uint8_t>((height >> 8) & 0xFF);
    header[16] = 32;
    header[17] = 0x28;   // 8 alpha bits, rows top to bottom
    fwrite(header, 1, sizeof(header), out);
    fwrite(image, 4, static_cast<size_t>(width) * height, out);
    fclose(out);
    return true;
}

void DumpFrame(uint64_t key, const uint32_t* image, int width, int height) {
    if (!g_dumpTextures || g_dumped >= kMaxDumped) return;
    CreateDirectoryA(DumpDir(), nullptr);
    char path[MAX_PATH];
    _snprintf(path, sizeof(path) - 1, "%s\\%016llX.tga", DumpDir(),
              static_cast<unsigned long long>(key));
    path[sizeof(path) - 1] = '\0';
    if (WriteTga(path, image, width, height)) ++g_dumped;
}

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
struct Frame {
    IDirect3DTexture9* texture = nullptr;
    bool effect = false;
};
std::map<uint64_t, Frame> g_frames;

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

// The artwork without its colour: size and palette indices only. Two draws of
// the same strip in different palettes get the same key, which is the point.
uint64_t IndexKey(int width, int height, const uint8_t* indices, int stride) {
    uint64_t h = 0xCBF29CE484222325ULL;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 0x100000001B3ULL;
    };
    mix(static_cast<uint64_t>(width));
    mix(static_cast<uint64_t>(height));
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = indices + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) mix(row[x]);
    }
    return h;
}

std::set<uint64_t> g_decalKeys;
int g_decalMaxSide = 0;

// Per record, re-decided only when the record is pointed at other artwork.
struct DecalVerdict {
    const void* pixels = nullptr;
    uint16_t revision = 0xFFFF;
    uint16_t width = 0, height = 0;
    bool decal = false;
};
std::map<const void*, DecalVerdict> g_decalVerdicts;

// A hash that turned out not to be stable would otherwise grow this without
// limit and exhaust video memory in a long session.
const size_t kMaxFrames = 4096;
bool g_frameLimitLogged = false;

}  // namespace

EffectSpriteSettings& EffectSprites() { return g_effects; }
int EffectSpriteFrames() { return g_effectFrames; }
int ClassifiedFrames() { return g_classifiedFrames; }

IDirect3DTexture9* DeviceTextureFor(IDirect3DDevice9* device, const void* handle, bool* effect) {
    if (effect) *effect = false;
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
        if (effect) *effect = cached.effect;
        return cached.texture;
    }

    // The content decides which texture this is. An animated tile cycling back
    // to a frame it has already shown gets the same object again, and with it the
    // same Remix hash, which is the whole point.
    const uint64_t key = FrameKey(record);
    const auto existing = g_frames.find(key);
    if (existing != g_frames.end()) {
        cached.texture = existing->second.texture;
        cached.width = record->width;
        cached.height = record->height;
        cached.palette = record->palette;
        cached.revision = record->revision;
        cached.pixels = record->pixels;
        cached.provisional = false;
        cached.effect = existing->second.effect;
        cached.key = key;
        if (effect) *effect = cached.effect;
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
    //
    // Built into a local and only published to the cache once it is complete.
    // This used to clear cached.texture up front and return null from any of the
    // three failures below, which threw away the artwork that was on screen a
    // frame ago *before* its replacement existed. GTA2 rewrites a record every
    // time the menu text changes, and if the palette had not been filled in yet
    // the caller got nothing - so a glyph blinked out for that frame, which is
    // the flicker when moving through the options. Keeping the last good texture
    // turns a failed rebuild into one stale frame, which nobody can see, rather
    // than one missing frame, which everybody can.
    //
    // It also stops the leak: the old path abandoned a created texture on every
    // failing frame without releasing it.
    IDirect3DTexture9* fresh = nullptr;
    if (FAILED(device->CreateTexture(record->width, record->height, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &fresh, nullptr))) {
        return cached.texture;
    }

    bool paletteHasColour = false;
    const uint32_t* palette = PaletteColours(record->palette, &paletteHasColour);
    if (!palette) {
        ++g_trouble.paletteMissing;
        fresh->Release();
        return cached.texture;
    }
    if (record->flags & 1) ++g_trouble.builtWhileLocked;  // game is mid-rewrite

    D3DLOCKED_RECT locked;
    if (FAILED(fresh->LockRect(0, &locked, nullptr, 0))) {
        ++g_trouble.lockFailed;
        fresh->Release();
        return cached.texture;
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
    // Is this a fireball rather than a cutout? Measured on the artwork as the
    // palette produced it, before anything below has moved a texel - and the
    // measurements are kept, not just the verdict, so a sprite that still has a
    // black rim can be looked up afterwards and the thresholds moved with the
    // numbers in front of you rather than by guessing. See WriteTextureReport.
    const EffectMeasure measure = MeasureEffect(image.data(), record->width, record->height);
    const bool isEffect = g_effects.mode != EffectSpriteMode::Off && IsEffect(measure, g_effects);
    ++g_classifiedFrames;
    if (isEffect) ++g_effectFrames;

    FrameNote& note = g_notes[key];
    note.key = key;
    note.width = record->width;
    note.height = record->height;
    note.palette = record->palette;
    note.measure = measure;
    note.classified = isEffect;
    // The artwork as the game supplied it, before the effect handling below
    // rewrites texels: what is wanted from a dump is the thing being judged.
    if (measure.hasCutout && !note.dumped) {
        DumpFrame(key, image.data(), record->width, record->height);
        note.dumped = true;
    }

    if (isEffect && g_effects.mode == EffectSpriteMode::Additive) {
        // Additive wants the transparent texels to stay pure black: with
        // SRCBLEND ONE they are *added* to the frame, so a bled colour out there
        // would paint a square halo round the sprite. Black adds nothing, and
        // filtering across the boundary then fades the glow out exactly the way
        // the artwork intends.
        for (size_t i = 0, n = image.size(); i < n; ++i) {
            if ((image[i] >> 24) == 0) image[i] = 0u;
        }
    } else if (isEffect && g_effects.mode == EffectSpriteMode::Cutout) {
        // Keep the alpha test, and give it something to cut: the artwork's own
        // brightness becomes its alpha, so the black fringe fades out instead of
        // being drawn. The colour is left alone - dimming it as well would take
        // the heat out of the fireball's edge.
        for (size_t i = 0, n = image.size(); i < n; ++i) {
            if ((image[i] >> 24) == 0) continue;
            const float faded = Luminance(image[i]) * g_effects.cutoutGain;
            const uint32_t alpha = faded >= 255.0f ? 255u : static_cast<uint32_t>(faded + 0.5f);
            image[i] = (alpha << 24) | (image[i] & 0x00FFFFFFu);
        }
        gta2::BleedTransparentEdges(image.data(), record->width, record->height);
    } else {
        // Keyed texels are transparent *black*, and every filter that touches
        // this texture - ours, and the mipmaps Remix builds for its own
        // materials - averages that black into the neighbouring colour. That is
        // the hard black rim around cutouts. Give the invisible texels a colour
        // and it goes away.
        gta2::BleedTransparentEdges(image.data(), record->width, record->height);
        // Slits and speckles the artist drew in the key colour, which are holes
        // to a path tracer and were an outline to GTA2. Before the feather,
        // which reads alpha and would otherwise soften an edge about to close.
        if (CloseHoles()) {
            g_holesClosed += gta2::CloseArtworkHoles(image.data(), record->width,
                                                    record->height, CloseHoleMax());
        }
        // And then invent the gradient the artwork never had. Off by default: a
        // fence wants its hard edge, an explosion does not.
        gta2::FeatherAlpha(image.data(), record->width, record->height, SpriteFeather());
    }
    for (int y = 0; y < record->height; ++y) {
        memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch,
               image.data() + static_cast<size_t>(y) * record->width,
               static_cast<size_t>(record->width) * 4);
    }
    fresh->UnlockRect(0);

    cached.texture = fresh;
    cached.width = record->width;
    cached.height = record->height;
    cached.palette = record->palette;
    cached.revision = record->revision;
    cached.pixels = record->pixels;
    cached.provisional = !paletteHasColour;
    cached.effect = isEffect;
    cached.key = key;
    // A texture built before the game filled its palette in is the wrong picture,
    // so it is not published under this content key - the next frame builds it
    // again and that one gets remembered.
    if (!cached.provisional) g_frames[key] = Frame{cached.texture, isEffect};
    ++g_trouble.built;
    if (effect) *effect = isEffect;
    return cached.texture;
}

void ClearGroundDecalArtwork() {
    g_decalKeys.clear();
    g_decalMaxSide = 0;
    g_decalVerdicts.clear();
}

void AddGroundDecalArtwork(int width, int height, const uint8_t* indices, int stride) {
    if (!indices || width <= 0 || height <= 0) return;
    g_decalKeys.insert(IndexKey(width, height, indices, stride));
    g_decalMaxSide = (std::max)(g_decalMaxSide, (std::max)(width, height));
    g_decalVerdicts.clear();
}

int GroundDecalArtworkCount() { return static_cast<int>(g_decalKeys.size()); }

bool IsGroundDecal(const void* handle) {
    const TextureRecord* record = static_cast<const TextureRecord*>(handle);
    if (!record || !record->pixels || g_decalKeys.empty()) return false;
    // Decals are a handful of tiny strips; anything bigger is not one, and not
    // worth hashing to find out.
    if (record->width > g_decalMaxSide || record->height > g_decalMaxSide) return false;
    DecalVerdict& v = g_decalVerdicts[handle];
    if (v.pixels != record->pixels || v.revision != record->revision ||
        v.width != record->width || v.height != record->height) {
        v.pixels = record->pixels;
        v.revision = record->revision;
        v.width = record->width;
        v.height = record->height;
        v.decal = g_decalKeys.count(IndexKey(record->width, record->height,
                                             static_cast<const uint8_t*>(record->pixels),
                                             kPageStride)) != 0;
    }
    return v.decal;
}

void NoteSpriteTexture(const void* handle) {
    const auto found = g_deviceTextures.find(handle);
    if (found == g_deviceTextures.end() || !found->second.key) return;
    const auto note = g_notes.find(found->second.key);
    if (note != g_notes.end()) note->second.drawnAsSprite = true;
}

void SetTextureDumping(bool on) { g_dumpTextures = on; }
bool TextureDumping() { return g_dumpTextures; }

void SetCloseHoles(bool on) { g_closeHoles = on; }
bool CloseHoles() { return g_closeHoles; }
void SetCloseHoleMax(int texels) { g_closeHoleMax = texels < 0 ? 0 : texels; }
int  CloseHoleMax() { return g_closeHoleMax; }
int  HolesClosed() { return g_holesClosed; }
const char* TextureDumpDir() { return DumpDir(); }
int TextureFramesDumped() { return g_dumped; }

int WriteTextureReport() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, sizeof(path));
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "gta2dx9_textures.csv");
    else strcpy(path, "gta2dx9_textures.csv");

    FILE* out = fopen(path, "w");
    if (!out) {
        Log("texture report: could not write %s", path);
        return 0;
    }
    fprintf(out, "; Every distinct frame of artwork this session built.\n");
    fprintf(out, "; sprite=1 means the world-space sprite pass drew it (a car, a pedestrian, a\n");
    fprintf(out, "; fireball); sprite=0 is a tile or something the HUD drew. effect=1 means it\n");
    fprintf(out, "; was classified as additive effect artwork: edge_median<%.1f, or peak<%.1f.\n",
            g_effects.edgeLuma, g_effects.faintLuma);
    fprintf(out, "; A sprite with a black rim that reads effect=0 is a threshold to move: look\n");
    fprintf(out, "; at its edge_median and peak. The frames themselves are TGAs in %s.\n",
            DumpDir());
    fprintf(out, "key,width,height,palette,sprite,effect,cutout,edge_median,edge_mean,"
                 "edge_dark_fraction,peak,fringe_texels,opaque_texels\n");
    int rows = 0;
    for (const auto& entry : g_notes) {
        const FrameNote& n = entry.second;
        fprintf(out, "%016llX,%d,%d,%d,%d,%d,%d,%.2f,%.2f,%.3f,%.2f,%d,%d\n",
                static_cast<unsigned long long>(n.key), n.width, n.height, n.palette,
                n.drawnAsSprite ? 1 : 0, n.classified ? 1 : 0, n.measure.hasCutout ? 1 : 0,
                n.measure.fringeMedian, n.measure.fringeMean, n.measure.fringeDarkFraction,
                n.measure.peak, n.measure.fringeTexels, n.measure.opaqueTexels);
        ++rows;
    }
    fclose(out);

    int sprites = 0, effects = 0, cutouts = 0;
    for (const auto& entry : g_notes) {
        if (entry.second.drawnAsSprite) ++sprites;
        if (entry.second.classified) ++effects;
        if (entry.second.measure.hasCutout) ++cutouts;
    }
    Log("texture report: %d frame(s) to %s -- %d drawn as sprites, %d with a cutout, %d "
        "classified as effects; %d frame(s) dumped to %s",
        rows, path, sprites, cutouts, effects, g_dumped, DumpDir());
    return rows;
}

void ForgetDeviceTexture(const void* handle) {
    // Only the record's bookkeeping goes: the texture itself belongs to g_frames
    // and may well be shared with another record showing the same artwork.
    g_deviceTextures.erase(handle);
}

void ReleaseDeviceTextures() {
    g_deviceTextures.clear();
    for (auto& entry : g_frames) {
        if (entry.second.texture) entry.second.texture->Release();
    }
    g_frames.clear();
    g_frameLimitLogged = false;
    g_effectFrames = 0;
    g_classifiedFrames = 0;
}

void RebuildDeviceTextures() {
    // Same thing, said for a different reason: the classification and the pixel
    // work both happen on the way in, so the only way a threshold change reaches
    // artwork already in the cache is to throw the cache away. The device keeps
    // a reference to anything it is still bound to, so releasing here is safe
    // even mid-frame.
    ReleaseDeviceTextures();
    Log("textures: dropped for a rebuild (effect classification changed)");
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
    // A slit through the middle of a wall tile is not a cutout, it is the colour
    // the artist outlined with - and under a path tracer it shows the night sky.
    if (CloseHoles()) {
        g_holesClosed += gta2::CloseArtworkHoles(out, kTileSize, kTileSize, CloseHoleMax());
    }
    return true;
}

}  // namespace gta2dx9
