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
    g_log = fopen(path, "w");
}

void CloseLog() {
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

void Log(const char* format, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

}  // namespace gta2dx9
