// What plugins written for the original renderer draw, routed into the overlay.
//
// Some plugins for GTA2 draw their own 2D elements - a minimap, its icons -
// straight through the original renderer's DirectX 6 device. That device does
// not exist here, so plugin_d3d6.cpp stands one in, and what it is asked to draw
// arrives through these functions. Kept free of any DirectX header: the legacy
// DirectX 6 headers and d3d9.h cannot share a translation unit.
//
// Positions are in the game's own screen pixels, the space the game's own HUD
// draws in; colours are 0xAARRGGBB; texture coordinates are 0..1.
#pragma once

#include <cstdint>

namespace gta2dx9 {

struct PluginVertex {
    float x, y;
    uint32_t colour;
    float u, v;
};

// A plugin texture, as 0xAARRGGBB rows, top mip only. Returns an id for
// PluginDraw, or -1. Update replaces the pixels after the plugin rewrites them.
int  PluginImageCreate(int width, int height, const uint32_t* argb);
void PluginImageUpdate(int id, int width, int height, const uint32_t* argb);
void PluginImageRelease(int id);

enum class PluginPrimitive { LineList, LineStrip, TriangleList, TriangleStrip, TriangleFan };

// image -1 draws untextured. additive is a one-one blend; otherwise the usual
// source-alpha blend. linear asks for bilinear sampling instead of point.
void PluginDraw(PluginPrimitive type, const PluginVertex* vertices, int count, int image,
                bool additive, bool linear);

// Installs the stand-in: patches the import of GetModuleHandleA in every
// plugin module loaded so far, so their lookup of the original renderer's state
// finds ours. Called once the renderer is up. See plugin_d3d6.cpp.
void PluginD3D6Install();

}  // namespace gta2dx9
