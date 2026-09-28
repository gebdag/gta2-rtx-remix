// Gibs: what a grenade or a rocket leaves of a pedestrian. Experimental, and off
// unless asked for. See gibs.cpp.
#pragma once

namespace gta2dx9 {

constexpr bool kDefaultGibs = false;
void SetGibs(bool on);
bool Gibs();

// Whether a pedestrian killed by a car comes apart too, not only one killed by an
// explosion. Off by default: GTA2 runs people over a great deal.
constexpr bool kDefaultGibsCar = false;
void SetGibsCar(bool on);
bool GibsCar();

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

// Whether the sprite being drawn is a limb that has landed. It was frozen where
// its arc had got to on its last frames, which can be a little above the road,
// so it is put on the floor under it rather than left at the game's height.
bool GibsCurrentIsResting();

// Whether the sprite being drawn is a pedestrian lying on the ground: dead, or
// knocked down and not up yet.
bool GameObjectIsCorpse();

// Whether the sprite being drawn is a power-up token. About half of them have a
// near-black outline, which is what marks a fireball as effect artwork, so they
// have to be told apart by what they are rather than by how they look.
bool GameObjectIsPowerUp();

// Routes GTA2's ped death handler through us, so a death can be looked at before
// the game handles it. Leaves a gta2.exe that is not the build these addresses
// came from alone. The setting is read on every death, so this goes in whether
// or not gibs are on and the menu switch takes effect at once.
void GibsInstall();

}  // namespace gta2dx9
