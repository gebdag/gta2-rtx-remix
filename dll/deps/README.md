# Vendored dependencies

Third-party code, unmodified, checked in so the renderer builds without a
package step. Both are also what the Shogo RTX Remix work builds against, which
is where this copy came from.

## `bridge_api/`

The RTX Remix API headers — `bridge_remix_api.h` and `remix/{remix.h,remix_c.h}`,
API version **0.1000.0**. Copyright NVIDIA CORPORATION, MIT licence (the notice
is at the top of each file).

`remix_c.h` refuses to compile for x86, because the ray tracing itself runs
64-bit. A 32-bit game reaches the API through the Remix bridge, whose client half
is the `d3d9.dll` beside the game and which forwards every call to the 64-bit
server process; `bridge_remix_api.h` exists precisely to sanction that, via the
`REMIX_ALLOW_X86` guard `remix_api.h` uses.

**The version matters.** `remixapi_LightInfo` and its extensions have gained
fields over time and the structs are serialised across the bridge, so a header
set that does not match the deployed Remix runtime corrupts silently rather than
failing to compile. Replace these only together with the runtime.

Note that `remixapi::bridge_initRemixApi` is *not* used: it looks the module up
as `d3d9.dll`, and `deploy.bat` renames the bridge client to `d3d9_remix.dll` so
GTA2's own DirectDraw startup cannot find it by that name and bring Remix up on a
device the renderer never draws through. `remix_api.cpp` does the same work
against the renamed module.

## `imgui/`

Dear ImGui 1.91.7 WIP, plus the `imgui_impl_dx9` and `imgui_impl_win32` backends.
Copyright Omar Cornut, MIT licence. Drives the F4 menu in `debug_overlay.cpp`.

Only the core, tables and widgets translation units are compiled — see
`../build.bat`. The Win32 backend is used for its display size and keyboard
plumbing only; the mouse is polled instead, because the panel is drawn into a
topmost `WS_EX_NOACTIVATE` window that never becomes the focused one.
