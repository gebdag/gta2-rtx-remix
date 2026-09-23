#include "remix_api.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <initializer_list>

namespace gta2dx9 {
namespace {

// The same list, in the same order, that renderer.cpp uses to find Direct3D 9.
// The renamed copy comes first because that is the one deploy.bat leaves for us;
// a plain "d3d9.dll" here means either an unrenamed Remix install or the system
// library, and the export check below tells those apart.
const char* const kBridgeModules[] = {"d3d9_remix.dll", "d3d9.dll"};

bool               g_available = false;
bool               g_atmosphere = false;
remixapi_Interface g_interface = {};
char               g_status[160] = "not initialised";
int                g_attempts = 0;

// The bridge server is a separate process that finishes starting only after the
// game has pumped messages for a moment, so an early failure is not final. It
// is not worth retrying forever either - a stock d3d9.dll will never grow the
// export.
const int kMaxAttempts = 600;

// Which Remix API function table the bridge handed back.
//
// The bridge client never checks the version it is asked for: it answers
// success and fills its own remixapi_Interface by field name, laid out however
// the header it was compiled against lays it out. Three layouts are out there,
// and read through the wrong one every call lands in a different function -
// on stock NVIDIA Remix, SetConfigVariable runs dxvk_RegisterD3D9Device and
// CreateLight runs DestroyLight with one argument too many, which unbalances
// the __stdcall stack and takes the game down.
//
// The light structs themselves are compatible across all three: the fork only
// appends isDynamic and ignoreViewModel to remixapi_LightInfo, which a stock
// bridge does not serialise, and the sphere and distant extensions and their
// sType values are identical. So what differs is only where the four entry
// points this renderer calls sit, and that is recognisable: the bridge fills a
// fixed set of slots and leaves the rest null, and the pattern of filled slots
// 1..13 differs between every layout. Slot 0, Shutdown, is null in all of them.
struct TableLayout {
    const char* name;
    unsigned filled;  // bit n set: slot n is filled by this layout's bridge
    int createLight, destroyLight, drawLightInstance, setConfigVariable;
    // Whether the runtime behind it has the rtx.atmosphere.* sky the day/night
    // clock drives. Remix Plus does; NVIDIA's own renders no sky of its own.
    bool atmosphere;
};

constexpr unsigned Slots(std::initializer_list<int> slots) {
    unsigned mask = 0;
    for (int slot : slots) mask |= 1u << slot;
    return mask;
}

const TableLayout kLayouts[] = {
    // This header: Remix Plus 1.5.0 and newer. Slots 4, 6, 9 are
    // CreateMeshBatched, SetupCamera and CreateLightBatched, never filled.
    {"Remix Plus 1.5+, API 0.1000", Slots({1, 2, 3, 5, 7, 8, 10, 11, 12}), 8, 10, 11, 12, true},
    // NVIDIA's own, API 0.6.x: no batched entry points, SetupCamera at 5.
    {"NVIDIA RTX Remix, API 0.6", Slots({1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12}), 7, 8, 9, 10,
     false},
    // Remix Plus 1.4.x, API 0.6.3: SetCameraMediumMaterial still at 7.
    {"Remix Plus 1.4, API 0.6", Slots({1, 2, 3, 5, 8, 9, 11, 12, 13}), 9, 11, 12, 13, true},
};
constexpr int kProbedSlots = 14;

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

    // Room past the end of our own table: a bridge built against a longer one
    // writes all of it, and that must land here rather than on the stack.
    struct {
        remixapi_Interface table;
        void* spare[64];
    } candidate = {};
    const remixapi_ErrorCode rc = initialize(&info, &candidate.table);
    // The bridge client answers NOT_INITIALIZED for one reason only: the API is
    // switched off in .trex\bridge.conf, which it is by default. That is a
    // setting, not a bridge still starting up, so asking again cannot help.
    if (rc == REMIXAPI_ERROR_CODE_NOT_INITIALIZED) {
        g_attempts = kMaxAttempts;
        SetStatus("%s: Remix API is switched off - set exposeRemixApi = True in "
                  ".trex\\bridge.conf", moduleName);
        Log("remix: %s", g_status);
        return false;
    }
    if (rc != REMIXAPI_ERROR_CODE_SUCCESS) {
        SetStatus("%s: remixapi_InitializeLibrary failed (code %d)", moduleName,
                  static_cast<int>(rc));
        if (g_attempts == 1) Log("remix: %s", g_status);
        return false;
    }

    // Work out whose table this is from the slots the bridge filled, and take
    // the four entry points from where that layout keeps them. See kLayouts.
    void* const* slots = reinterpret_cast<void* const*>(&candidate.table);
    unsigned filled = 0;
    for (int i = 1; i < kProbedSlots; ++i) {
        if (slots[i]) filled |= 1u << i;
    }
    const TableLayout* layout = nullptr;
    for (const TableLayout& known : kLayouts) {
        if (known.filled == filled) layout = &known;
    }
    if (!layout) {
        // A table we do not recognise is one whose every call might land in the
        // wrong function. Run without lights rather than find out.
        g_attempts = kMaxAttempts;
        SetStatus("%s: unrecognised Remix API table (slots %04X); lights off", moduleName,
                  filled);
        Log("remix: %s", g_status);
        return false;
    }

    remixapi_Interface mapped = {};
    mapped.Shutdown = reinterpret_cast<PFN_remixapi_Shutdown>(slots[0]);
    mapped.CreateLight = reinterpret_cast<PFN_remixapi_CreateLight>(slots[layout->createLight]);
    mapped.DestroyLight = reinterpret_cast<PFN_remixapi_DestroyLight>(slots[layout->destroyLight]);
    mapped.DrawLightInstance =
        reinterpret_cast<PFN_remixapi_DrawLightInstance>(slots[layout->drawLightInstance]);
    mapped.SetConfigVariable =
        reinterpret_cast<PFN_remixapi_SetConfigVariable>(slots[layout->setConfigVariable]);

    g_interface = mapped;
    g_available = true;
    g_atmosphere = layout->atmosphere;
    SetStatus("ready via %s (%s)", moduleName, layout->name);
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

bool RemixApiHasAtmosphere() { return g_available && g_atmosphere; }

const remixapi_Interface* RemixApi() { return g_available ? &g_interface : nullptr; }

const char* RemixApiStatusText() { return g_status; }

bool RemixSetConfig(const char* key, const char* value) {
    if (!g_available || !g_interface.SetConfigVariable || !key || !value) return false;
    return g_interface.SetConfigVariable(key, value) == REMIXAPI_ERROR_CODE_SUCCESS;
}

}  // namespace gta2dx9
