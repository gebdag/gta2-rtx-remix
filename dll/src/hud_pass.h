// Knowing when GTA2 is drawing its HUD, and which part of it, so the overlay can
// place the HUD on a wide screen without moving anything tied to the world. See
// hud_pass.cpp.
#pragma once

namespace gta2dx9 {

// Routes the game's one call to its HUD draw through us, and each of the HUD
// draw's own calls - one per part of the HUD - as well. Leaves a gta2.exe that is
// not the build these addresses came from alone.
void HudPassInstall();

// True while the game is inside its HUD draw.
bool HudPassActive();

// Which part of the HUD is drawing: the index of the call inside the HUD draw
// (FUN_004CA440) that is running, 0-18, or -1 outside one or when the parts
// could not be told apart.
int HudPassPart();

// Parts that draw at a place projected from the world - a marker over its
// target, a player's name over the player, the arrows pointing at a target -
// and so have to stay where the world is on any screen.
bool HudPartIsWorldAnchored(int part);

}  // namespace gta2dx9
