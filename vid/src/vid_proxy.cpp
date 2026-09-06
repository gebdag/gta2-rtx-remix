// A proxy for GTA2's video device.
//
// GTA2 loads two DLLs from one registry key: the renderer named by `rendername`,
// which this project replaced long ago, and the *video device* named by
// `videoname`, which owns the screen and until now was still the original. This
// is the first step in taking that over too, and it exists to answer one
// question with evidence rather than argument.
//
// --- What is actually wrong -------------------------------------------------
//
// Deleting dxwrapper's ddraw.dll makes GTA2 say
//
//     Videomode 16x16x16 is not available
//
// which is a lie in two different ways. The message is error 0xBBB, and its arm
// in gta2.exe at 0x0044F841 reads one argument and prints it three times:
//
//     0044F841  mov  eax, [ebp+0x1c]
//     0044F844  push eax
//     0044F845  push eax
//     0044F846  push eax
//     0044F847  push 0x56F640          ; "Videomode %dx%dx%d is not available"
//
// The caller at 0x004CB5DC pushes width and height right next to it and the
// message arm never touches either. So "16x16x16" is a single number - the bit
// depth - and the resolution has nothing to do with it. A monitor that does
// 640x480 was never going to help.
//
// What the game actually does, at 0x004CB583:
//
//     004CB583  push 0x10                ; 16 bpp, hardcoded
//     004CB58A  call [0x595024]          ; Vid_CheckMode(dev, w, h, 16)
//     004CB594  jne  ok
//     004CB5A5  push 0x10                ; retry, still 16 bpp
//     004CB5AC  mov  [0x6732E4], 0x280   ; at 640x480
//     004CB5C4  call [0x595024]
//     004CB5EA  push 0xBBB               ; give up
//
// GTA2 asks for 16-bit colour and nothing else. The obvious suspicion was that
// this is the fault - Windows has been 32-bit everywhere for years - and the
// first run of this proxy disproved it. DirectDraw on this machine enumerates 93
// modes, 31 of them 16 bpp, and the plain install picks 640x480x16 on the first
// try and boots. So the hardcoded depth is real, and it is not what breaks when
// dxwrapper is removed.
//
// Which is exactly why this exists. The substitution below stays as a cheap
// safety net for a machine that really has no 16-bit mode; the *log* is the part
// that earns its keep, because it prints the mode list, the device, and every
// argument of every call, and one of those is the real answer.
//
// --- Why the fix is a substitution and not a lie ----------------------------
//
// Vid_CheckMode does not return a boolean. Disassembled from the original
// (Dmavideo.dll+0x18D0), it walks a linked list of modes built at init and
// returns the matching mode's *id*:
//
//     ctx+0x24  head of the mode list      mode+0x00  id      <- the return value
//     ctx+0x34  current device             mode+0x04  device
//     ctx+0x08  width  }  written back     mode+0x08  width
//     ctx+0x0C  height }  from the mode     mode+0x0C  height
//     ctx+0x10  bpp    }  that matched      mode+0x14  bpp
//                                          mode+0x38  next
//
// and the game hands that id straight to Vid_SetMode at 0x004CB617. So a
// made-up non-zero would hand the original a mode id that is not in its own
// list - which is why, when the game asks for 16 and the answer is genuinely no,
// this asks the original again for 32 and returns *that* real id instead of
// inventing one. The mode is then one the display actually has, the write-back
// fields get real numbers, and Vid_SetMode receives something it recognises.
//
// The depth mismatch costs nothing here. GTA2 only ever hands the video surface
// to the renderer, through MakeScreenTable (slot 0x595324) - it does not read it
// - and this project's renderer ignores it entirely and draws its own world.
//
// --- Owning the screen ------------------------------------------------------
//
// Fixing the mode check was not the end of it. With dxwrapper gone the original
// video device does what it always did and calls DirectDraw's SetDisplayMode, so
// the monitor drops to the mode GTA2 asked for - 640x480 - and the renderer's
// 4K window is then far larger than the screen, showing only its top left
// corner. dxwrapper had been suppressing that with EnableWindowMode=1.
//
// The renderer already owns the picture, so nothing needs a DirectDraw screen at
// all. With own_screen=1 (the default, gta2dx9_vid.ini beside the game) the
// exports that create or touch one stop forwarding:
//
//     Vid_SetMode        returns success without setting a display mode
//     Vid_GetSurface     hands back nothing
//     Vid_FreeSurface    nothing to free
//     Vid_ClearScreen    nothing to clear
//     Vid_FlipBuffers    nothing to flip
//     Vid_CloseScreen    nothing to close
//
// Note the return convention flips between the two calls that matter. For
// Vid_CheckMode a non-zero result is the mode id and zero is failure; for
// Vid_SetMode, gta2.exe at 0x004CB61D does `test eax,eax / je` past the error
// report, so **zero is success** there.
//
// One field cannot simply be dropped. After Vid_GetSurface the game reads
// ctx+0x48 and ctx+0x4C, and those are not surface geometry - they are the
// screen size, and they go straight into gbh_SetWindow (0x004CAF38), which is
// what lays the menus and the HUD out. Leave them stale and the front end is
// laid out to garbage. So Vid_SetMode writes them from the mode the game just
// chose, which Vid_CheckMode already stored at ctx+0x08 and ctx+0x0C, and the
// game sees exactly the screen size it always did.
//
// --- What this is for -------------------------------------------------------
//
// Everything forwards, so the game behaves exactly as before except for the one
// substitution, and every call is logged with its arguments. That log is the
// specification for the standalone stub that comes next: of the 22 exports GTA2
// only calls 13, and the mode enumeration - Vid_FindMode, Vid_FindFirstMode,
// Vid_FindNextMode - it never calls at all.
//
// Log goes to gta2dx9_vid.log beside gta2.exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <intrin.h>
#include <cstdio>

namespace {

// The original, renamed aside by deploy.bat exactly the way the renderer's
// d3ddll_orig.dll is.
// Handed back by Vid_CheckMode when the display has no such mode and we are
// not setting one anyway. Only ever tested against zero.
const int kSyntheticMode = 0x7FFF;

const char kBackslash = 92;  // written by code point: escaping it has bitten twice
const char* const kOriginalName = "Dmavideo_orig.dll";

HMODULE g_original = nullptr;
bool g_bindTried = false;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

char g_logPath[MAX_PATH] = "";
int g_logLines = 0;
// The frame calls - FlipBuffers and ClearScreen - arrive sixty times a second
// and would bury everything that only happens once. They are counted instead.
int g_flips = 0;
int g_clears = 0;
int g_surfaces = 0;

// Where this DLL is, which is where the original and the log live too. The
// game's working directory is not reliably its own folder.
void BuildPaths(HMODULE self) {
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA(self, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';
    _snprintf(g_logPath, sizeof(g_logPath) - 1, "%sgta2dx9_vid.log", path);
    g_logPath[sizeof(g_logPath) - 1] = '\0';
}

void Log(const char* format, ...) {
    if (!g_logPath[0] || g_logLines > 4000) return;
    if (g_lockReady) EnterCriticalSection(&g_lock);
    FILE* out = fopen(g_logPath, "a");
    if (out) {
        ++g_logLines;
        va_list args;
        va_start(args, format);
        vfprintf(out, format, args);
        va_end(args);
        fputc('\n', out);
        fclose(out);
    }
    if (g_lockReady) LeaveCriticalSection(&g_lock);
}

// Deferred to the first call rather than done in DllMain: loading a library
// under the loader lock is how you deadlock a process at startup.
HMODULE Original() {
    if (g_bindTried) return g_original;
    g_bindTried = true;

    char path[MAX_PATH] = "";
    if (g_logPath[0]) {
        _snprintf(path, sizeof(path) - 1, "%s", g_logPath);
        char* slash = strrchr(path, '\\');
        if (slash) {
            *(slash + 1) = '\0';
            strncat(path, kOriginalName, sizeof(path) - strlen(path) - 1);
        }
    }
    g_original = path[0] ? LoadLibraryA(path) : nullptr;
    if (!g_original) g_original = LoadLibraryA(kOriginalName);
    Log("bind %s -> %p", path[0] ? path : kOriginalName, g_original);
    return g_original;
}

// Whether we, rather than DirectDraw, own the screen. Read lazily for the same
// reason the original is bound lazily.
bool OwnScreen() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    char ini[MAX_PATH] = "";
    if (g_logPath[0]) {
        _snprintf(ini, sizeof(ini) - 1, "%s", g_logPath);
        char* slash = strrchr(ini, kBackslash);
        if (slash) {
            *(slash + 1) = '\0';
            strncat(ini, "gta2dx9_vid.ini", sizeof(ini) - strlen(ini) - 1);
        }
    }
    cached = GetPrivateProfileIntA("video", "own_screen", 1, ini) ? 1 : 0;
    Log("own_screen=%d  (%s)", cached,
        cached ? "no display mode is ever set; the renderer owns the output"
               : "forwarding everything, DirectDraw will set the display mode");
    return cached != 0;
}

// The per-frame surface calls, split from own_screen deliberately.
//
// own_screen is what stops the monitor changing resolution and is not in doubt.
// This one covers Vid_GetSurface, Vid_FreeSurface, Vid_ClearScreen and
// Vid_FlipBuffers, which are a separate bet: nothing in takeover mode reads that
// surface, so they should be free to drop - but they are also the only thing
// that changed when the menu started flickering. Two switches means that can be
// settled by trying it rather than argued about.
bool OwnSurface() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    char ini[MAX_PATH] = "";
    if (g_logPath[0]) {
        _snprintf(ini, sizeof(ini) - 1, "%s", g_logPath);
        char* slash = strrchr(ini, kBackslash);
        if (slash) {
            *(slash + 1) = 0;
            strncat(ini, "gta2dx9_vid.ini", sizeof(ini) - strlen(ini) - 1);
        }
    }
    cached = GetPrivateProfileIntA("video", "own_surface", 1, ini) ? 1 : 0;
    // The two are not independent, and pretending they were cost a boot.
    //
    // own_screen=1 means Vid_SetMode never ran, so the original video device has
    // no screen and no surfaces. Forwarding the per-frame calls into it then
    // hands it a screen it does not have, and the game goes down on startup.
    // Testing that path means turning both off together.
    if (!cached && OwnScreen()) {
        Log("own_surface=0 ignored: own_screen=1 means no screen was ever created, so there is "
            "nothing for the per-frame surface calls to forward into. Set own_screen=0 too if "
            "that is really what you want to test.");
        cached = 1;
    }
    Log("own_surface=%d  (%s)", cached,
        cached ? "the per-frame surface calls do nothing"
               : "the per-frame surface calls forward to the original");
    return cached != 0;
}

// Offsets into the video context, all recovered from the original and from the
// game's use of it. +0x08/+0x0C are written by Vid_CheckMode from the mode that
// matched; +0x48/+0x4C are what the game reads back as the screen size.
int* CtxField(void* context, int offset) {
    return reinterpret_cast<int*>(static_cast<char*>(context) + offset);
}

// The renderer is loaded under GTA2's own Direct3D name, with its own as a
// fallback for an install that has not claimed that slot.
void NoteScreenClear() {
    using Notify = void(__stdcall*)();
    static Notify notify = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        static const char* const kNames[2] = {"d3ddll.dll", "gta2dx9.dll"};
        for (int i = 0; i < 2; ++i) {
            const HMODULE module = GetModuleHandleA(kNames[i]);
            if (!module) continue;
            notify = reinterpret_cast<Notify>(GetProcAddress(module, "gta2dx9_NoteScreenClear"));
            if (notify) {
                Log("screen clears will be reported to %s", kNames[i]);
                break;
            }
        }
        if (!notify) Log("no renderer to report screen clears to; the 2D layer will not be cleared");
    }
    if (notify) notify();
}

void* Entry(const char* name) {
    HMODULE module = Original();
    return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}

// --- The mode list, read out of the original's own context ------------------
//
// Offsets recovered from Dmavideo.dll!Vid_CheckMode (+0x18D0). Dumping this on
// the first check is the whole diagnostic: it says in one place exactly which
// modes DirectDraw offered this machine, and therefore whether any 16-bit one
// exists at all.
struct VidMode {
    int id;        // +0x00, what Vid_CheckMode returns
    int device;    // +0x04
    int width;     // +0x08
    int height;    // +0x0C
    int unknown10; // +0x10
    int bpp;       // +0x14
    int pad[8];    // +0x18 .. +0x34
    VidMode* next; // +0x38
};

void DumpModes(void* context) {
    if (!context) {
        Log("  mode list: no context");
        return;
    }
    const char* bytes = static_cast<const char*>(context);
    const VidMode* mode = *reinterpret_cast<VidMode* const*>(bytes + 0x24);
    const int device = *reinterpret_cast<const int*>(bytes + 0x34);
    Log("  mode list (ctx=%p, current device=%d):", context, device);
    int count = 0, sixteen = 0;
    for (; mode && count < 128; mode = mode->next, ++count) {
        Log("    id=%-4d device=%-3d %4dx%-4d %2d bpp", mode->id, mode->device, mode->width,
            mode->height, mode->bpp);
        if (mode->bpp == 16) ++sixteen;
    }
    Log("  %d modes, %d of them 16 bpp %s", count, sixteen,
        sixteen ? "" : "<- this is why the game gives up");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
        BuildPaths(module);
        // Truncate: one file per run, the way the renderer's log works.
        if (g_logPath[0]) {
            FILE* out = fopen(g_logPath, "w");
            if (out) fclose(out);
        }
        Log("gta2dx9 video proxy attached (pid %lu)", GetCurrentProcessId());
    }
    return TRUE;
}

// Every export forwards. Argument counts are the originals', read off their
// epilogues: a __stdcall callee cleans its own stack, so getting one wrong
// corrupts the caller's rather than merely passing rubbish.
#define FORWARD(name, ret, params, args, fmt, ...)                          \
    extern "C" __declspec(dllexport) ret __stdcall name params {            \
        using Fn = ret(__stdcall*) params;                                  \
        static Fn fn = reinterpret_cast<Fn>(Entry(#name));                  \
        if (!fn) {                                                          \
            Log(#name ": NOT FOUND in the original");                       \
            return 0;                                                       \
        }                                                                   \
        Log(fmt, __VA_ARGS__);                                              \
        return fn args;                                                     \
    }

FORWARD(Vid_GetVersion, int, (), (), "Vid_GetVersion()%s", "")
FORWARD(Vid_InitDLL, int, (void* a, void* b), (a, b), "Vid_InitDLL(%p, %p)", a, b)
FORWARD(Vid_Init_SYS, int, (void* a, int b), (a, b), "Vid_Init_SYS(%p, %d)", a, b)
FORWARD(Vid_ShutDown_SYS, int, (void* a), (a), "Vid_ShutDown_SYS(%p)", a)
FORWARD(Vid_FindDevice, int, (void* a, int b), (a, b), "Vid_FindDevice(%p, %d)", a, b)
FORWARD(Vid_SetDevice, int, (void* a, int b), (a, b), "Vid_SetDevice(%p, %d)", a, b)
FORWARD(Vid_FindMode, int, (void* a, int b), (a, b), "Vid_FindMode(%p, %d)", a, b)
FORWARD(Vid_FindFirstMode, int, (void* a, int b), (a, b), "Vid_FindFirstMode(%p, %d)", a, b)
FORWARD(Vid_FindNextMode, int, (void* a), (a), "Vid_FindNextMode(%p)", a)
FORWARD(Vid_GrabSurface, int, (void* a), (a), "Vid_GrabSurface(%p)", a)
FORWARD(Vid_ReleaseSurface, int, (void* a), (a), "Vid_ReleaseSurface(%p)", a)
FORWARD(Vid_EnableWrites, int, (void* a), (a), "Vid_EnableWrites(%p)", a)
FORWARD(Vid_DisableWrites, int, (void* a), (a), "Vid_DisableWrites(%p)", a)
FORWARD(Vid_SetGamma, int, (void* a, int b, int c, int d), (a, b, c, d),
        "Vid_SetGamma(%p, %d, %d, %d)", a, b, c, d)
FORWARD(Vid_WindowProc, int, (void* a, void* b, unsigned c, unsigned d, long e), (a, b, c, d, e),
        "Vid_WindowProc(%p, %p, msg 0x%X)", a, b, c)

#undef FORWARD

// Called every frame. Counted rather than logged, so the once-only calls stay
// findable in the file.
extern "C" __declspec(dllexport) int __stdcall Vid_FlipBuffers(void* context) {
    using Fn = int(__stdcall*)(void*);
    static Fn fn = reinterpret_cast<Fn>(Entry("Vid_FlipBuffers"));
    if (OwnSurface()) {
        if (++g_flips <= 2) Log("Vid_FlipBuffers(%p) -> nothing to flip  [then counted]", context);
        return 0;
    }
    if (!fn) return 0;
    if (++g_flips <= 3) Log("Vid_FlipBuffers(%p)  [logged 3 times, then counted]", context);
    return fn(context);
}

extern "C" __declspec(dllexport) int __stdcall Vid_ClearScreen(void* a, int b, int c, int d, int e,
                                                               int f, int g, int h) {
    using Fn = int(__stdcall*)(void*, int, int, int, int, int, int, int);
    static Fn fn = reinterpret_cast<Fn>(Entry("Vid_ClearScreen"));
    // Not every clear means "throw the screen away".
    //
    // GTA2 has five call sites and they do not mean the same thing. Two of its
    // loops clear unconditionally as the last step of their own draw-flip-clear
    // cycle - 0x0045863B in the pass that blits the artwork, and 0x00481E88 -
    // and those passes redraw their own content next time round. Only
    // 0x004619BC is a decision: gta2.exe!0x00461977 tests five repaint flags
    // after flipping and clears just when one is set, otherwise returning with
    // the screen left alone.
    //
    // That distinction is the whole thing. The menu is composed by more than one
    // pass over one persistent surface - the artwork pass draws only images, the
    // menu pass only text and flat rects - so treating either pass's own
    // end-of-cycle clear as "discard everything" wipes out what the other one
    // put there. Only the considered clear, and the two at startup, reset the
    // layer.
    {
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool repaint = from == 0x004619C2u ||   // the menu's conditional clear
                             from == 0x004CC7A0u ||   // screen set-up
                             from == 0x004CC7CBu;
        if (repaint) NoteScreenClear();
        static int said = 0;
        if (said < 6) {
            ++said;
            Log("Vid_ClearScreen from %08X -> %s", static_cast<unsigned>(from),
                repaint ? "repaint, layer reset" : "end of a draw pass, layer kept");
        }
    }
    if (OwnSurface()) {
        if (++g_clears <= 2) Log("Vid_ClearScreen(%p, ...) -> forwarded as a repaint signal only",
                                 a);
        return 0;
    }
    if (!fn) return 0;
    if (++g_clears <= 3) Log("Vid_ClearScreen(%p, %d, %d, %d, %d, %d, %d, %d)", a, b, c, d, e, f, g, h);
    return fn(a, b, c, d, e, f, g, h);
}

// --- The screen ------------------------------------------------------------
//
// Everything below refuses to give DirectDraw a screen when own_screen is set,
// which is what stops the monitor dropping to 640x480. See the note at the top
// for why zero is success here and for the two context fields that still have to
// be written.

extern "C" __declspec(dllexport) int __stdcall Vid_SetMode(void* context, int window, int mode) {
    using Fn = int(__stdcall*)(void*, int, int);
    static Fn fn = reinterpret_cast<Fn>(Entry("Vid_SetMode"));
    if (!OwnScreen()) {
        Log("Vid_SetMode(%p, hwnd %08X, mode %d) -> forwarding", context, window, mode);
        return fn ? fn(context, window, mode) : 0;
    }
    if (context) {
        // The screen size the game reads back and hands to gbh_SetWindow. Taken
        // from the mode it just chose rather than invented, so the front end is
        // laid out exactly as it was when DirectDraw really did set the mode.
        const int width = *CtxField(context, 0x08);
        const int height = *CtxField(context, 0x0C);
        *CtxField(context, 0x48) = width;
        *CtxField(context, 0x4C) = height;
        // The surface itself. Only MakeScreenTable is ever handed these, and in
        // takeover mode that export does nothing at all.
        *CtxField(context, 0x50) = 0;
        *CtxField(context, 0x54) = 0;
        Log("Vid_SetMode(%p, hwnd %08X, mode %d) -> no display mode set; screen kept at %dx%d",
            context, window, mode, width, height);
    }
    return 0;  // zero is success for this one
}

extern "C" __declspec(dllexport) int __stdcall Vid_CloseScreen(void* context) {
    using Fn = int(__stdcall*)(void*);
    static Fn fn = reinterpret_cast<Fn>(Entry("Vid_CloseScreen"));
    if (!OwnScreen()) return fn ? fn(context) : 0;
    Log("Vid_CloseScreen(%p) -> nothing to close", context);
    return 0;
}

// Also per-frame: the game takes and releases the surface once every frame so
// the renderer can be handed it. Counted, for the same reason as the two above.
extern "C" __declspec(dllexport) int __stdcall Vid_GetSurface(void* context) {
    using Fn = int(__stdcall*)(void*);
    static Fn fn = reinterpret_cast<Fn>(Entry("Vid_GetSurface"));
    if (OwnSurface()) {
        if (++g_surfaces <= 2) Log("Vid_GetSurface(%p) -> no surface  [then counted]", context);
        return 0;
    }
    if (!fn) return 0;
    if (++g_surfaces <= 2) Log("Vid_GetSurface(%p)  [logged twice, then counted]", context);
    return fn(context);
}

extern "C" __declspec(dllexport) int __stdcall Vid_FreeSurface(void* context) {
    using Fn = int(__stdcall*)(void*);
    static Fn fn = reinterpret_cast<Fn>(Entry("Vid_FreeSurface"));
    if (OwnSurface()) return 0;
    if (!fn) return 0;
    if (g_surfaces <= 2) Log("Vid_FreeSurface(%p)", context);
    return fn(context);
}

// The one export that does not simply forward.
//
// GTA2 asks for 16-bit colour and only 16-bit colour. Whether the original can
// answer depends on a Windows compatibility shim, DWM8And16BitMitigation, which
// is registered per executable path under AppCompatFlags\Layers: with it the
// mode list here came back 93 entries deep with 31 at 16 bpp, and without it 31
// entries with none. When the answer is no, ask again for 32 and return that.
// The id handed back is a real entry in the original's own list, so Vid_SetMode
// recognises it and the context's width/height/bpp are a mode the display has.
extern "C" __declspec(dllexport) int __stdcall Vid_CheckMode(void* context, int width, int height,
                                                             int bpp) {
    using Fn = int(__stdcall*)(void*, int, int, int);
    static Fn fn = reinterpret_cast<Fn>(Entry("Vid_CheckMode"));
    if (!fn) {
        Log("Vid_CheckMode: NOT FOUND in the original");
        return 0;
    }

    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        Log("--- first Vid_CheckMode: what DirectDraw offered this machine ---");
        DumpModes(context);
        Log("---");
    }

    const int id = fn(context, width, height, bpp);
    if (id) {
        Log("Vid_CheckMode(%dx%d, %d bpp) -> mode %d", width, height, bpp, id);
        return id;
    }

    if (bpp == 16) {
        const int substitute = fn(context, width, height, 32);
        if (substitute) {
            Log("Vid_CheckMode(%dx%d, 16 bpp) -> none; substituting 32 bpp -> mode %d", width,
                height, substitute);
            return substitute;
        }
    }

    // Nothing in the display's list matches, at any depth. That used to be the
    // end of it - error 0xBBB - and it is why a monitor with no 640x480 mode
    // could not boot the game even though 640x480 is only ever the size of a
    // back buffer nobody looks at.
    //
    // With own_screen there is no display mode to set, so there is nothing left
    // for the list to be right about. Answer the question ourselves: write the
    // three fields the original would have written and hand back an id that only
    // has to be non-zero, because the only consumer is Vid_SetMode and that no
    // longer does anything with it. Vid_FindMode, which is the one call that
    // could look an id up again, GTA2 never makes.
    if (OwnScreen() && context && width > 0 && height > 0) {
        *CtxField(context, 0x08) = width;
        *CtxField(context, 0x0C) = height;
        *CtxField(context, 0x10) = bpp;
        Log("Vid_CheckMode(%dx%d, %d bpp) -> not in the display's list; accepted anyway "
            "(no display mode is being set)", width, height, bpp);
        return kSyntheticMode;
    }

    Log("Vid_CheckMode(%dx%d, %d bpp) -> none", width, height, bpp);
    return 0;
}
