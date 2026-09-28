// Gibs: what a grenade or a rocket leaves of a pedestrian. Experimental, and off
// unless asked for. See gibs.cpp.
#pragma once

namespace gta2dx9 {

constexpr bool kDefaultGibs = false;
// Whether limbs stay where they land. On by default, so gibs read as a body
// that came apart rather than a moment of debris.
constexpr bool kDefaultGibsStay = true;

void SetGibs(bool on);
bool Gibs();
void SetGibsStay(bool on);
bool GibsStay();

// Once a frame, after the game has updated its particles: settles landed limbs
// and lets go of anything the game has since reused.
void GibsUpdate();
// Forgets everything; the particle pool does not survive a level.
void GibsReset();
// Whether the object the game is drawing right now is a gibbed body.
bool GibsHideCurrentSprite();

// The sprite object gta2.exe's object draw (FUN_004BE060) was entered with, for
// as long as that draw is running - so every world sprite can be told which game
// object it belongs to. Null before the first draw or if the hook could not go
// in. The hook lives here because gibs needed it first.
const void* GameObjectBeingDrawn();

// How much larger than its artwork the sprite being drawn should be: more than 1
// for a limb, 1 for everything else.
float GibsCurrentScale();

// Routes GTA2's ped death handler through us, so a death can be looked at before
// the game handles it. Leaves a gta2.exe that is not the build these addresses
// came from alone. The setting is read on every death, so this goes in whether
// or not gibs are on and the menu switch takes effect at once.
void GibsInstall();

}  // namespace gta2dx9
