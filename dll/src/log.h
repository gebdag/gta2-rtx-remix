// Shared logging to gta2dx9.log beside the game executable.
#pragma once

namespace gta2dx9 {

void OpenLog();
void CloseLog();
void Log(const char* format, ...);

}  // namespace gta2dx9
