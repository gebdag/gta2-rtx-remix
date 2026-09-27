// Knowing when GTA2 is drawing its HUD, so the overlay can place the HUD on a
// wide screen without moving anything tied to the world. See hud_pass.cpp.
#pragma once

namespace gta2dx9 {

// Routes the game's one call to its HUD draw through us. Leaves a gta2.exe that
// is not the build these addresses came from alone.
void HudPassInstall();

// True while the game is inside its HUD draw.
bool HudPassActive();

}  // namespace gta2dx9
