// Everything the F4 menu can change, in one file, saved on demand.
//
// The tuning was previously scattered: per-light overrides had their own file,
// the effect categories had another, and the global light settings, the sprite
// lift and the alpha mode had nowhere at all - they reset to defaults every
// launch, which makes an evening of tuning worthless.
//
// This is the one place that knows how to write all of it. The button in the
// menu calls SettingsSaveAll, boot calls SettingsLoadAll, and the two agree by
// construction because they are the same table of fields read in both
// directions.
//
// Deliberately a separate file from gta2dx9.ini: deploy regenerates that one
// every time it runs, so anything saved into it would be lost on the next
// build. gta2dx9_settings.ini is never written by deploy, and it wins over
// gta2dx9.ini where the two overlap - it is the later and more specific word.
#pragma once

namespace gta2dx9 {

// Beside the game, like the other two.
void SettingsInit(const char* path);

// Writes this file *and* asks the light overrides and the effect settings to
// write theirs, so one button really does save everything.
void SettingsSaveAll();

// Called once at startup, after the other two have loaded.
void SettingsLoadAll();

const char* SettingsPath();

// True when something in the menu has been changed since the last save, so the
// button can say so rather than leaving you guessing.
bool SettingsDirty();
void SettingsMarkDirty();

}  // namespace gta2dx9
