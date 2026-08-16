# gta2dx9.dll — in-process renderer

Replaces GTA2's `gbh_*` renderer DLL so rendering happens inside `gta2.exe`. This
removes the second-window problem: GTA2 stops simulating when it loses focus, so a
separate viewer window can never show live motion while you look at it.

## Modes

`gta2dx9.ini`, beside `gta2.exe`:

```ini
[renderer]
backend=3dfx.dll
mode=takeover        ; or "proxy"
own_window=2         ; 0 = game's window, 1 = child of it, 2 = our own topmost

[camera]
; tenths of a degree, because the ini API only reads integers
pitch_tenths=-900    ; -900 = straight down, matching GTA2's own view
fov_tenths=400
```

`proxy` is the default so a bad build can never leave the game unbootable.

## Takeover

Owns a D3D9 device on the game's own window and draws world-space geometry built
from the game's live map. The game's `gbh_DrawTile` / `gbh_DrawQuad` /
`gbh_DrawTriangle` stream is deliberately ignored — it is pre-transformed screen
space, which Remix skips.

The camera comes from two sources: `gbh_SetCamera` hands us the **visible tile
rectangle** every frame, which is exactly the game's zoom and centre, and the
camera struct gives sub-tile position. Nothing is reverse-projected.

Two camera problems worth remembering:

- **Vertical jitter** came from following the *top* of the column under the
  camera. The topmost block is a building roof, so the camera leapt whenever it
  crossed one. It now follows the bottom of the column (street level) and eases
  toward it, since ground height is a per-tile step function.
- **Looking straight down** makes a `+Y` up vector degenerate. `Camera::ViewMatrix`
  switches to a north up vector past 89.9°, so `pitch_tenths=-900` is valid.

## Earlier stage: transparent proxy

Every export forwards to the original renderer, so the game behaves exactly as before
while the call stream is logged to `gta2dx9.log` next to `gta2.exe`. This proves the hook
and shows which entry points carry the data the world-space renderer needs.

Verified working: the game boots and runs with the proxy in the chain, producing the real
per-frame sequence — `gbh_BeginScene` → `gbh_DrawTile` / `gbh_DrawTriangle` →
`gbh_EndScene`, alongside `gbh_SetCamera`, `gbh_AddLight`, and texture lock/unlock.

## Deployment

```bash
build.bat
```

Copy `build\gta2dx9.dll` and a `gta2dx9.ini` next to `gta2.exe`:

```ini
[renderer]
backend=3dfx.dll
```

Then point the game at it. The value that matters is under **HKLM** — the HKCU copy is
read for some settings but the renderer name is taken from HKLM, and setting only HKCU
silently does nothing:

```
HKLM\SOFTWARE\WOW6432Node\DMA Design Ltd\GTA2\Screen\rendername = gta2dx9.dll
```

`do_play_movie` must also be `0` under **HKLM**; with the intro enabled the game crashes
in `binkw32.dll` with an access violation before reaching gameplay.

## Running under RTX Remix

Remix froze the game at the menu before any of this was in place. Three separate
things were wrong, and all three had to be fixed before a frame ever appeared.

**GTA2 gets Remix's `d3d9.dll` by accident.** `rendername` is only half the
story: GTA2 also loads a separate **video device**, named by `videoname` in the
same registry key, and that is what owns the screen. Here it is `dmaglide.dll`,
which drives Glide through `C:\Windows\SysWOW64\glide2x.dll` — an nGlide wrapper
carrying spoofed 3Dfx version strings — and nGlide renders Glide with Direct3D 9.
So about 0.7 s into the process, long before the renderer DLL is loaded, nGlide
asks the loader for `d3d9.dll` by name and creates a fullscreen 3840x2160
A8R8G8B8 software-vertex-processing device on the game's window. In a Remix
install the thing sitting under that name is the Remix bridge client, so the
Remix runtime came up on nGlide's device and the renderer's own arrived second.
Remix drives one device per process: the second `CreateDevice` re-hooked the
window procedure out from under the bridge's message channel, the runtime sat
retrying `UWM_REMIX_BRIDGE_REGISTER_THREADPROC_MSG` forever, and the game never
returned from `gbh_Init`.

The fix is to rename the bridge to `d3d9_remix.dll` and ask for it by that name
(`renderer.cpp`, `CreateD3D9`). DirectDraw then gets the system `d3d9.dll`, and
the renderer's device is the only one Remix ever sees. `deploy.bat` does the
rename; **if you reinstall or update Remix it will drop a fresh `d3d9.dll` in and
you have to re-run `deploy.bat`**, or the freeze comes straight back.

**Device creation has to be allowed to fail.** Remix hands back an `IDirect3D9`
before the 64-bit runtime behind it can serve a device, and turns everything down
with `D3DERR_INVALIDCALL` until it can — it only gets there once the game has run
its own loop for a moment. `WorldView::EnsureDevice` therefore retries from the
frame loop instead of giving up in `gbh_Init`. A transient adapter loss (a
monitor sleeping, for one) recovers through the same path.

**We need our own window, and it has to be topmost.** `rendername` is only half
the story: GTA2 also loads a *video device*, named by `videoname` in the same
registry key, and that is what owns the screen — `dmaglide.dll` driving nGlide,
or `Dmavideo.dll` driving DirectDraw. It takes the game's window into a
fullscreen device, and Microsoft's D3D9 tolerates a second swap chain there but
Remix does not: the first `Present` never returns and the runtime spins at 100%.
So `own_window` presents into a window of ours instead.

Which *kind* of window is the whole ballgame, and the two wrong answers both fail
as a black screen with the game visible only when you alt-tab away — which reads
as a rendering fault rather than a windowing one:

- **the game's window** (`own_window=0`) — the `Present` deadlock above.
- **a child of it** (`own_window=1`) — a child is always painted above its
  parent's client area, which sounds airtight, and is not. The video device
  presents past the window manager entirely, so being correctly ordered *within*
  the game's window buys nothing.
- **our own topmost window** (`own_window=2`) — this is the one that works. A
  `WS_EX_TOPMOST` window sits in the topmost band, above the activated game
  window, and that is enough to be in front of the video device's presentation.

Note the trap in the middle option: `SetWindowPos(..., HWND_TOP, ...)` is *not*
topmost. It orders the window within its own band, so a popup created without
`WS_EX_TOPMOST` still loses to the game the instant it is activated. Getting this
wrong looks exactly like the topmost case having been tried and failed.

The window is `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`, so it never
takes focus and the input still goes to the game; `RenderFrame` re-states
`HWND_TOPMOST` every 64 frames and drains its messages by hand, because the game
pumps only its own.

### If the video device ever has to go too

It did not come to this, but the interface is recovered and worth keeping: 22
`__stdcall` exports, identical in `DMAGlide.dll` and `Dmavideo.dll`, with
argument counts read off the originals' epilogues.

| export | args | export | args |
| --- | --- | --- | --- |
| `Vid_GetVersion` | 0 | `Vid_FindMode` | 2 |
| `Vid_InitDLL` | 2 | `Vid_FindNextMode` | 1 |
| `Vid_Init_SYS` | 2 | `Vid_CheckMode` | 4 |
| `Vid_ShutDown_SYS` | 1 | `Vid_SetMode` | 3 |
| `Vid_FindDevice` | 2 | `Vid_CloseScreen` | 1 |
| `Vid_SetDevice` | 2 | `Vid_ClearScreen` | 8 |
| `Vid_FindFirstMode` | 2 | `Vid_FlipBuffers` | 1 |
| `Vid_GetSurface` | 1 | `Vid_GrabSurface` | 1 |
| `Vid_FreeSurface` | 1 | `Vid_ReleaseSurface` | 1 |
| `Vid_EnableWrites` | 1 | `Vid_DisableWrites` | 1 |
| `Vid_SetGamma` | 4 | `Vid_WindowProc` | 5 |

`Vid_GetVersion` returns `0x01008020`, and `Vid_InitDLL` returns 0 for success
after stashing both arguments. A forwarding proxy would not be enough: the
fullscreen device covers the screen whether or not `Vid_FlipBuffers` runs, so
`Vid_SetMode` would have to never create one — and the game reads a device list
off `system+0x2C` and a mode list from it, so a stub has to fabricate both.

## Seeing what is actually on screen

Do not trust a screenshot. GDI's `CopyFromScreen` returns solid black for both a
fullscreen-exclusive D3D window and a Vulkan swap chain, so it cannot tell "our
window is covered" from "our window is showing" — which is exactly what made this
look like a rendering fault for two debugging cycles. **DXGI output duplication**
sees the real display; `..\tools\dupcap.cpp` captures one frame and reports what
percentage of it is not black. Desktop reads ~98%, the black-screen failure reads
0.0%. Build it x64 and run it with the game up:

```bash
cl /nologo /EHsc /MT dupcap.cpp /Fe:dupcap.exe
```

Symptoms worth recognising in `rtx-remix\logs\`, since none of them name a cause:

| log line | what it actually means |
| --- | --- |
| `Message channel ... handshake timeout. Retrying...` | a second device stole the window procedure |
| `Still waiting on the Present semaphore to be released...` | `Present` is wedged; look at what owns the window |
| `No winproc detected, initiating bridge message channel` | normal, not a fault |

`bridge64.log` dumps the last ten commands the client sent and the server
received when the client dies, which is the only way to see which call the
runtime is actually stuck on — kill the frozen game and read it.

## Things that cost a debugging cycle

- **The interface is `__stdcall`**, not `__cdecl` — the originals end in `ret 4`. Calling
  one as cdecl cleans the argument twice, trips the `/GS` cookie check on return and
  raises `__fastfail`, which exits with `0xC0000409` and **cannot** be caught by an
  exception filter, so the log just stops with no crash record. Naked `jmp` thunks are
  immune, since a jump forwards the frame either way; only typed wrappers need annotating.
- **All 40 resolved exports must exist** or startup aborts with "Can't Find Function
  Called ...". The list lives in `src/gbh_exports.h`.
- An exception filter installed in `DllMain` is replaced by the game's own during startup.
  Installing it from `gbh_InitDLL` runs later and wins.

## The screen-space pass

The world is not the whole picture. GTA2 transforms the HUD, the menus and **every
sprite** itself and pushes them through `gbh_DrawQuad`, `gbh_DrawTriangle`,
`gbh_DrawFlatRect` and `gbh_BlitImage`. Dropping that stream — which is what this DLL
originally did — is precisely why the menus, the HUD, and every car and pedestrian were
absent. `overlay.cpp` replays it as a 2D pass drawn over the world.

The vertex layout is not guesswork; it comes from `d3ddll.dll!FUN_00e02cc0`, the shared
body behind all of those entry points. Eight floats per vertex, `u`/`v` in texels, four
vertices as a triangle fan. Three details cost real debugging if missed:

- `flags & 0x10000` means **only vertex 0 is filled in** and the renderer lays the quad
  out from there at the texture's own size.
- `gbh_LoadImage` returns an **index**, not a handle — the game hands it straight back to
  `gbh_BlitImage`. Its argument is an uncompressed 16-bit A1R5G5B5 TGA, rows bottom-up.
- Screen coordinates are in the **game's** resolution (640x480, at camera `+0x68`/`+0x6C`),
  not the back buffer's, so everything is scaled on the way out.

Remix classifies this pass as UI and will not path trace it. That is right for the HUD and
a known limitation for sprites — see below.

## The world coordinates the game keeps anyway

The screen-space vertices are only half the story. GTA2 also writes each vertex's
**absolute** world position four slots further along the same array — `FUN_0046bbf0` for
map faces, `FUN_004b9990` for sprites:

| screen vertices | world shadow |
| --- | --- |
| map faces `0x006632A0` | `0x00663320` |
| sprites `0x0066FE18` | `0x0066FE98` |

`FUN_0046bbf0` adds the camera position back in before storing, so these are absolute map
coordinates. They are the *input* to the game's transform, not its output, so reading them
is not an unprojection — the world-space-only rule holds.

`live_geometry.cpp` uses this for two things:

- **Sprites** — cars, pedestrians, powerups — become real 3D quads, depth tested against
  the map instead of painted over it, and path traced by Remix like everything else. They
  are told apart from the UI by `_ReturnAddress()`: the object draw `FUN_004be060` is the
  only producer of world sprites on `gbh_DrawQuad`.
- **Partial and corner blocks** (slope types 49-61) are *not* built by the static mesh at
  all. Their shapes are awkward — a triangular half-wall plus a rotated triangular lid,
  spread across `FUN_0046ee40` and `FUN_00471c30` — so instead each face is captured from
  the game's own draw stream the first time it is seen, with texture coordinates produced
  by replaying `gbh_DrawTile`'s own algorithm. Exact, and no model of the shape is needed.

The one cost of capture is that a block has to be drawn once before it is kept, so those
few hundred blocks appear as the camera first reaches them and then stay.

## Not yet implemented

Mip chains, and the view-rotation states other than the default `0xFF`.
