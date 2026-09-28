// A benchmark for how many lights the RTX Remix bridge can carry, so the
// per-category light caps can be set from a measurement instead of a guess.
//
// Started from the F4 menu. It adds its own lights around the camera in steps -
// none, then 25, 50, 100 ... 6400 - and holds each step long enough to measure
// it. It runs twice: once with lights that never change, which costs one
// DrawLightInstance per light per frame across the bridge, and once with lights
// that move every frame, which adds a CreateLight each - the case every car's
// headlights are in. A pass stops at the first step where Remix refuses lights
// or the frame collapses.
//
// Results go to gta2dx9.log and to gta2dx9_lightbench.csv beside the game.
#pragma once

namespace gta2dx9 {

void LightBenchStart();
void LightBenchStop();
bool LightBenchRunning();

// Once a frame, before LightsReconcile: this step's lights.
void LightBenchSubmit();

// Once a frame, after the flip: how long the light reconcile and the light draw
// took this frame, in milliseconds.
void LightBenchRecord(double reconcileMs, double drawMs);

// One line for the menu: what is running, or how the last run ended.
const char* LightBenchStatus();

}  // namespace gta2dx9
