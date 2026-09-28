// Gibs: what a grenade or a rocket leaves of a pedestrian. Experimental, and off
// unless asked for. See gibs.cpp.
#pragma once

namespace gta2dx9 {

constexpr bool kDefaultGibs = false;

void SetGibs(bool on);
bool Gibs();

// Routes GTA2's ped death handler through us, so a death can be looked at before
// the game handles it. Leaves a gta2.exe that is not the build these addresses
// came from alone. The setting is read on every death, so this goes in whether
// or not gibs are on and the menu switch takes effect at once.
void GibsInstall();

}  // namespace gta2dx9
