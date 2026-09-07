// The F4 menu: the in-game panel for everything this renderer can be told to do.
//
// Two audiences share one window. By default it is the short list a player
// wants - brightness, the day/night cycle, the frame cap, how sprites sit on the
// road - and the Advanced switch in its title bar unfolds the rest: the light
// tables, the classification thresholds, the per-light overrides and the
// particle-type binding the port was actually tuned with. Nothing was removed to
// make the simple view simple.
//
// Long explanations live in comments in debug_overlay.cpp rather than on screen;
// what survives in the UI is a one-line tooltip per control.
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

// How much bigger than ImGui's 1080p defaults to draw the panel; 0 follows the
// display, which is what puts it at 2x on a 4K screen. Saved with the rest of
// the settings, hence the reference - see settings.cpp.
float& DebugMenuUiScale();

}  // namespace gta2dx9
