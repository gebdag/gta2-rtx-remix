// The F4 menu: an ImGui panel for the Remix light injection.
//
// Getting the game's lights into a path tracer is mostly a tuning problem -
// whether a lamp reached Remix at all, and what radiance it ended up with, are
// things that have to be read off the running game and changed without a
// rebuild. This is that readout and those controls.
//
// It draws through the renderer's own device, inside the existing
// BeginScene/EndScene pair, so it needs no separate presentation path. Note that
// the panel is drawn into the *present* window while input arrives at the game's
// - the renderer presents into a topmost window of its own, because GTA2's video
// device owns the game's. See world_view.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct IDirect3DDevice9;

namespace gta2dx9 {

// inputWindow is the game's, which owns the focus; presentWindow is ours, which
// the panel is drawn into and which mouse coordinates are measured against. Safe
// to call repeatedly; only the first call does work.
void DebugMenuInit(HWND inputWindow, HWND presentWindow, IDirect3DDevice9* device);

void DebugMenuShutdown();

// Handles the F4 edge and feeds ImGui the mouse. Call once per frame before
// DebugMenuRender, outside the scene.
void DebugMenuPoll();

// Builds and draws the panel. Must run inside a BeginScene/EndScene pair, after
// the world and before the flip.
void DebugMenuRender();

bool DebugMenuVisible();

}  // namespace gta2dx9
