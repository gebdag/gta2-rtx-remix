#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gta2dx9 {
namespace {
FILE* g_log = nullptr;
}

void OpenLog() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "gta2dx9.log");
    else strcpy(path, "gta2dx9.log");

    // The game is free to load and unload the renderer more than once in a run,
    // and every load lands here. Truncating each time would throw away the
    // earlier pass, which is the interesting one, so only the first open in a
    // process starts a fresh file. The marker lives in the environment because
    // it has to outlive our own globals across an unload.
    static const char kOpened[] = "GTA2DX9_LOG_OPENED";
    const bool first = GetEnvironmentVariableA(kOpened, nullptr, 0) == 0;
    g_log = fopen(path, first ? "w" : "a");
    SetEnvironmentVariableA(kOpened, "1");
}

void CloseLog() {
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

// Wall-clock stamped, because the only way to tell our own D3D9 calls apart from
// anyone else's in RTX Remix's bridge logs is to line the timestamps up.
void Log(const char* format, ...) {
    if (!g_log) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

}  // namespace gta2dx9
