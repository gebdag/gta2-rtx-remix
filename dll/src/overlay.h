// The screen-space layer: HUD, menus, and sprites.
//
// GTA2 draws everything that is not world geometry through a handful of
// entry points that all take vertices it has already transformed itself
// (gbh_DrawQuad, gbh_DrawTriangle, gbh_DrawFlatRect, gbh_BlitImage). The
// world renderer deliberately ignores that stream, which is why the HUD,
// the menus and every car and pedestrian were missing entirely.
//
// This replays that stream as a 2D pass drawn on top of the world. It is
// pre-transformed by construction, so RTX Remix will classify it as UI and
// leave it out of the path traced image - correct for the HUD, and a known
// limitation for sprites until they are rebuilt as world-space billboards.
//
// The layout below is not guesswork: the original renderer takes
// D3DFVF_XYZRHW|DIFFUSE|SPECULAR|TEX1 (0x1C4), a 32-byte vertex, and draws
// four of them as a triangle fan (d3ddll.dll!FUN_00e02cc0).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <d3d9.h>
#include <vector>

namespace gta2dx9 {

// Flag bits the original renderer acts on, from d3ddll.dll!FUN_00e02cc0.
namespace quad_flags {
constexpr unsigned kVertexColour = 0x2000;   // diffuse is already in the vertices
constexpr unsigned kExpandFromTexture = 0x10000;  // only vertex 0 is filled in
constexpr unsigned kOpaque = 0x300;          // no colour key on this texture
}  // namespace quad_flags

class Overlay {
public:
    // Screen coordinates arrive in the game's own resolution, which is not the
    // size of our back buffer; everything is scaled on the way out.
    // The size the game lays its HUD out in, taken from the camera struct.
    // Only valid while a world is loaded - see RevertToWindowSize.
    void SetGameScreenSize(int width, int height);

    // Go back to the size gbh_SetWindow last stated. The camera struct outlives
    // the level it described, so once the game is back at the menu it is not a
    // source of truth about anything.
    void RevertToWindowSize();
    void NoteWindow(float a, float b, float c, float d);

    void BeginFrame();
    void Quad(unsigned flags, const void* texture, const float* vertices, uint8_t shade);
    void Triangle(unsigned flags, const void* texture, const float* vertices, uint8_t shade);
    void FlatRect(const float* vertices, uint32_t colour);
    void Line(int x1, int y1, int x2, int y2, uint32_t colour);

    // gbh_LoadImage is handed a 16-bit uncompressed TGA; gbh_BlitImage copies a
    // sub-rectangle of one to the screen. Menu backgrounds arrive this way.
    void InitImageTable(int count);
    void FreeImageTable();
    int LoadImage(const void* tga);
    void BlitImage(int image, int srcX1, int srcY1, int srcX2, int srcY2, int dstX, int dstY);

    void Flush(IDirect3DDevice9* device, int targetWidth, int targetHeight);
    void ReleaseResources();

    int DrawCount() const { return static_cast<int>(draws_.size()); }

private:
    struct Vertex {
        float x, y, z, rhw;
        uint32_t colour;
        float u, v;
    };

    struct Draw {
        const void* texture;   // TextureRecord*, or null for untextured
        int image;             // image-table entry, or -1
        uint32_t first;        // index into vertices_
        uint32_t count;        // 3 or 6
        bool alphaTest;
    };

    struct Image {
        IDirect3DTexture9* texture = nullptr;
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;
    };

    void PushTriangleFan(const Vertex* corners, int count, const void* texture, int image,
                         bool alphaTest);
    IDirect3DTexture9* ResolveImage(IDirect3DDevice9* device, int image);
    // gbh_ConvertColour returns 5:6:5; this expands it. See FlatRect.
    static uint32_t PanelColour(uint32_t colour);

    std::vector<Vertex> vertices_;
    std::vector<Draw> draws_;
    std::vector<Image> images_;

    int gameWidth_ = 640;
    int gameHeight_ = 480;
    // The last thing gbh_SetWindow said, kept even while the camera struct is
    // driving the size, so there is something to fall back to at the menu.
    int windowWidth_ = 0;
    int windowHeight_ = 0;
    bool sizeFromWindow_ = false;
};

}  // namespace gta2dx9
