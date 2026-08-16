// Access to the RTX Remix API from inside the renderer.
//
// Remix ships as a d3d9.dll dropped beside the game. That module exports
// remixapi_InitializeLibrary, which hands back a table of function pointers for
// describing scene elements the D3D9 stream cannot carry - here, lights with
// real radiance and emitter size rather than the eight fixed-function slots
// D3D9 would flatten them into.
//
// The API is 64-bit in principle; a 32-bit game reaches it through the Remix
// bridge, whose client half is that same d3d9.dll and which forwards every call
// to the 64-bit server process. bridge_remix_api.h exists precisely to let x86
// callers use the headers, so this is a supported path and not a trick.
//
// Everything here is optional at runtime: with no Remix present the init fails
// and every entry point becomes a no-op, so a plain D3D9 run is unaffected.
#pragma once

// remix_c.h refuses to compile for x86 - the ray tracing runs 64-bit, so the
// API is meant to be called from a 64-bit host. Only its type declarations are
// needed on this side of the bridge, and defining REMIX_ALLOW_X86 around the
// include is how bridge_remix_api.h itself sanctions that.
#if _WIN64 != 1
#define REMIX_ALLOW_X86
#endif
#include <remix/remix_c.h>
#if _WIN64 != 1
#undef REMIX_ALLOW_X86
#endif

namespace gta2dx9 {

// Finds the already-loaded Remix bridge client and initialises the API. Safe to
// call repeatedly; only the first success does work, and a failure is retried
// on later calls because the bridge is not necessarily up on the first frame.
//
// Deliberately does not use remixapi::bridge_initRemixApi: that helper looks the
// module up as "d3d9.dll", and deploy.bat renames the bridge client to
// d3d9_remix.dll so GTA2's own DirectDraw startup cannot find it by that name
// and bring Remix up on a device we never draw through.
bool RemixApiInit();

void RemixApiShutdown();

bool RemixApiAvailable();

// Null until RemixApiInit has succeeded.
const remixapi_Interface* RemixApi();

// Human-readable result of the last init attempt, for the menu and the log.
const char* RemixApiStatusText();

}  // namespace gta2dx9
