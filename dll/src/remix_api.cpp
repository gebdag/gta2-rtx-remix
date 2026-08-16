#include "remix_api.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace gta2dx9 {
namespace {

// The same list, in the same order, that renderer.cpp uses to find Direct3D 9.
// The renamed copy comes first because that is the one deploy.bat leaves for us;
// a plain "d3d9.dll" here means either an unrenamed Remix install or the system
// library, and the export check below tells those apart.
const char* const kBridgeModules[] = {"d3d9_remix.dll", "d3d9.dll"};

bool               g_available = false;
remixapi_Interface g_interface = {};
char               g_status[160] = "not initialised";
int                g_attempts = 0;

// The bridge server is a separate process that finishes starting only after the
// game has pumped messages for a moment, so an early failure is not final. It
// is not worth retrying forever either - a stock d3d9.dll will never grow the
// export.
const int kMaxAttempts = 600;

void SetStatus(const char* format, ...) {
    va_list args;
    va_start(args, format);
    _vsnprintf(g_status, sizeof(g_status) - 1, format, args);
    va_end(args);
    g_status[sizeof(g_status) - 1] = '\0';
}

}  // namespace

bool RemixApiInit() {
    if (g_available) return true;
    if (g_attempts >= kMaxAttempts) return false;
    ++g_attempts;

    // GetModuleHandle rather than LoadLibrary: the renderer's device has already
    // been created through one of these, and loading a second copy would
    // initialise an API that is not driving that device.
    HMODULE module = nullptr;
    const char* moduleName = nullptr;
    for (const char* name : kBridgeModules) {
        module = GetModuleHandleA(name);
        if (module) {
            moduleName = name;
            break;
        }
    }
    if (!module) {
        SetStatus("no Direct3D 9 module loaded yet");
        return false;
    }

    using InitFn = remixapi_ErrorCode(REMIXAPI_PTR*)(const remixapi_InitializeLibraryInfo*,
                                                     remixapi_Interface*);
    const auto initialize =
        reinterpret_cast<InitFn>(GetProcAddress(module, "remixapi_InitializeLibrary"));
    if (!initialize) {
        // The ordinary case for a stock system d3d9.dll. Stop asking.
        g_attempts = kMaxAttempts;
        SetStatus("%s exports no remixapi_InitializeLibrary (not running under Remix)", moduleName);
        Log("remix: %s", g_status);
        return false;
    }

    const remixapi_InitializeLibraryInfo info = {
        REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO, nullptr,
        REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR,
                              REMIXAPI_VERSION_PATCH)};

    remixapi_Interface candidate = {};
    const remixapi_ErrorCode rc = initialize(&info, &candidate);
    if (rc != REMIXAPI_ERROR_CODE_SUCCESS) {
        SetStatus("%s: remixapi_InitializeLibrary failed (code %d)", moduleName,
                  static_cast<int>(rc));
        if (g_attempts == 1) Log("remix: %s", g_status);
        return false;
    }

    // A bridge that answered but left the light entry points empty would fault on
    // first use, so a partial table counts as unavailable rather than trusting
    // the success code alone.
    if (!candidate.CreateLight || !candidate.DestroyLight || !candidate.DrawLightInstance) {
        g_attempts = kMaxAttempts;
        SetStatus("%s: Remix API is missing the light entry points", moduleName);
        Log("remix: %s", g_status);
        return false;
    }

    g_interface = candidate;
    g_available = true;
    SetStatus("ready via %s (API %d.%d.%d)", moduleName, REMIXAPI_VERSION_MAJOR,
              REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    Log("remix: %s", g_status);
    return true;
}

void RemixApiShutdown() {
    if (g_available && g_interface.Shutdown) g_interface.Shutdown();
    g_available = false;
    g_interface = remixapi_Interface{};
    SetStatus("shut down");
}

bool RemixApiAvailable() { return g_available; }

const remixapi_Interface* RemixApi() { return g_available ? &g_interface : nullptr; }

const char* RemixApiStatusText() { return g_status; }

}  // namespace gta2dx9
