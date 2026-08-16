<title>GTA2 Light Systems</title>

# GTA2 Light Systems

Static analysis of `gta2.exe` (image base `0x400000`), `d3ddll.dll` (image base `0xE00000`) and the
`data/*.gmp` map files, to establish what lighting information GTA2 has and where it can be
intercepted for injection through the RTX Remix API.

**Headline result: the lights are not missing from the game — they are already being handed to the
renderer DLL and thrown away.** GTA2's swappable renderer interface has three lighting entry points
(`gbh_ResetLights`, `gbh_AddLight`, `gbh_SetAmbient`), the DX9 replacement DLL currently forwards all
three to `3dfx.dll` as no-ops. Every light the game knows about arrives at
`gbh_AddLight` in **world space, as a point light with colour, position, radius and intensity** — the
exact shape a Remix light wants.

---

## 1. The renderer interface

`FUN_004d3770` (gta2.exe) is the `GetProcAddress` binder for the renderer DLL. Among the 40 required
exports, four concern lighting:

| Export | Function pointer | Signature | Called from |
|---|---|---|---|
| `gbh_ResetLights` | `0x005952EC` | `void ()` | `FUN_00472110` (world render loop) |
| `gbh_AddLight` | `0x005952F0` | `void (LightDesc*)` | `FUN_00461400` (light submit) |
| `gbh_SetAmbient` | `0x005952F4` | `void (float)` | `FUN_00472110`, `FUN_00456A60`, `FUN_004C6DA0` |
| `SetShadeTableA` | `0x0059532C` | `void (int,int,int,int,int)` | **never called** — legacy software-renderer path |

`gbh_SetCamera` (`0x005952E8`) is called immediately after `gbh_ResetLights` with four floats: the
**visible tile rectangle** `(minX, minY, maxX, maxY)`. Useful as a cheap culling hint.

### `gbh_AddLight` descriptor — 20 bytes

Assembled on the stack in `FUN_00461400` at `0x004614C5`, decoded in `d3ddll.dll!gbh_AddLight`
(`0x00E01D70`):

```c
struct LightDesc {          // 20 bytes
    uint32_t packed;        // +0x00  byte0 = intensity 0..255
                            //        byte1 = radius, quantised
                            //        byte2 = 1  (enable flag, see below)
                            //        byte3 = 0
    float    x;             // +0x04  world tiles, east
    float    y;             // +0x08  world tiles, south
    float    z;             // +0x0C  world tiles, level
    uint32_t rgb;           // +0x10  0x00RRGGBB
};
```

The renderer expands it into an 11-float record (`DAT_00E459E0`, stride `0x2C`):

```c
intensity = (packed        & 0xFF) / 255.0f;
radius    = ((packed >> 8) & 0xFF) / 255.0f * 8.0f;   // => 0 .. 8 tiles
radius2   = radius * radius;
invRadius = 1.0f / radius;
R = ((rgb >> 16) & 0xFF) / 255.0f;
G = ((rgb >>  8) & 0xFF) / 255.0f;
B = ( rgb        & 0xFF) / 255.0f;
```

`x`, `y`, `z` are stored as-is. They are in **the same coordinate space as the world positions the
game already writes into the shadow vertex arrays** (`0x00663320` for map faces, `0x0066FE98` for
sprites) — `FUN_00E02A80` computes the light distance directly against those slots, no transform in
between. So light positions can be consumed by the world-space renderer with zero conversion.

Bit `0x10000` of `packed` (byte2) is set unconditionally by `FUN_00469010`. The renderer tests
`(packed & 0x30000) == 0x10000`; nothing in `gta2.exe` ever sets `0x20000`, so in practice byte2 is
always `1` and the test always passes.

### The reference lighting model

`d3ddll.dll!FUN_00E02A80` is the per-vertex lighting pass, called from `FUN_00E02CC0` (the shared
body behind `gbh_DrawTile`, `gbh_DrawTilePart`, `gbh_DrawQuad`, `gbh_DrawQuadClipped`,
`gbh_DrawFlatRect`) and from `gbh_DrawTriangle`. For each vertex:

```
sum = 0
for each light L:
    if (L.packed & 0x30000) != 0x10000: continue
    d2 = |vertexWorldPos - L.pos|²          // world pos read from vertex + 0x80
    if d2 > L.radius²: continue
    atten = (L.radius - sqrt(d2)) * L.invRadius     // linear, 1 at centre -> 0 at radius
    if atten <= 0: continue
    sum += L.rgb * atten * L.intensity

out.R = min(255, ambient255 + in.R * (shade/255) * sum.R)
out.G = min(255, ambient255 + in.G * (shade/255) * sum.G)
out.B = min(255, ambient255 + in.B * (shade/255) * sum.B)
```

Notes:

- **Falloff is linear in distance**, not inverse-square. `sqrt` is done through a lookup table
  (`DAT_00E13864` / `DAT_00E13868`).
- **Ambient is additive**, not multiplicative — it is the floor brightness, lights add on top.
- Lighting is gated per primitive on **draw flag `0x8000`**. `gta2.exe` ORs that bit in from
  `DAT_0067358C`, which `FUN_004CB1D0` sets to `0x8000` when the `lighting` config option is on
  (default **on**) and `0` when off.
- Lighting is skipped entirely when `ambient >= 1.0` (`FCOMP` against `255.0f` at `0x00E03187`).
  **This is the daylight fast path** — in a fully lit scene GTA2 does no lighting at all, and
  `gbh_AddLight` calls still happen but have no visible effect in the original renderer.

---

## 2. The runtime light object

`FUN_00469010` is the single light constructor. Signature:

```c
Light* CreateLight(fixed x, fixed y, fixed z, uint32 rgb, fixed radius, uint8 intensity);
```

All coordinates are **16.14 fixed point, one unit per map tile** (`FUN_00401B10` multiplies by
`2^-14`). Layout:

| Offset | Size | Field |
|---|---|---|
| `+0x00` b0 | 1 | intensity 0..255 (`FUN_0045B2D0` writes it) |
| `+0x00` b1 | 1 | radius quantised: `(radius_fixed * 32) >> 14`, i.e. `radius_tiles * 32` |
| `+0x00` b2 | 1 | `1` — enable flag |
| `+0x04` | 4 | x, 16.14 |
| `+0x08` | 4 | y, 16.14 |
| `+0x0C` | 4 | z, 16.14 |
| `+0x10` | 4 | RGB `0x00RRGGBB` |
| `+0x14` | 1 | on-time (frames) |
| `+0x15` | 1 | off-time (frames) |
| `+0x16` | 1 | random time jitter |
| `+0x17` | 1 | blink countdown |
| `+0x18` | 1 | base intensity (restored when blinking back on) |
| `+0x1C` | 4 | next in free list / next in timed-light list |
| `+0x20` | 4 | next in spatial-grid bucket |
| `+0x24` | 4 | prev in spatial-grid bucket |

Mutators: `FUN_00482D30` (move), `FUN_00482D60` (set colour+radius+intensity), `FUN_00476AE0`
(set intensity), `FUN_00476B00` (set colour), `FUN_00469070` (set up blinking).

### Spatial index and per-frame submission

`DAT_006622B4` is a `malloc(0x4000)` array = **64 × 64 buckets of 4 × 4 tiles**, each a
doubly-linked list head (`FUN_004612A0` allocates, `FUN_00461360` links, `FUN_004613B0` unlinks).

Each frame, `FUN_00472110` does, **for map level 0 only**, when `lighting` is enabled:

```
gbh_SetAmbient(currentAmbient)            // once, before the level loop
gbh_ResetLights()
gbh_SetCamera(minX, minY, maxX, maxY)     // visible tile rect
FUN_00461400(minX, minY, maxX, maxY)      // walk buckets, gbh_AddLight() each light
```

`FUN_00461400` expands the tile rect by 2 buckets (8 tiles) on each side, clamps to `0..63`, and
submits **every light in range with no cap and no distance sort**. So the light set the DX9 renderer
sees is already view-culled for you.

Note the light list is submitted **once**, not per level — the same set lights all 8 levels.

---

## 3. Where lights come from

Everything in the game that emits light goes through `FUN_00469010`. There are exactly four call
sites, and two of them funnel the whole object system.

### 3.1 Map lights — the `LGHT` chunk

`FUN_004692B0`, called once from level init `FUN_0045B5F0` ("game.cpp"). It walks
`count = *(mapObj + 0x348)` records of 16 bytes at `*(mapObj + 0x33C)` — the `LGHT` chunk of the
`.gmp`, loaded verbatim.

```c
struct LGHT {              // 16 bytes
    uint32_t argb;         // +0  0x00RRGGBB (top byte always 0)
    uint16_t x, y, z;      // +4  1/128 tile units  (value << 7 => 16.14 fixed)
    uint16_t radius;       // +10 1/128 tile units
    uint8_t  intensity;    // +12 0..255
    uint8_t  timeRandom;   // +13 random jitter added to the blink timer
    uint8_t  onTime;       // +14 frames lit
    uint8_t  offTime;      // +15 frames dark
};
```

> **Correction to the commonly published GMP format docs:** byte 13 is *not* a "shape" field. It is
> fed to `FUN_00469070` as the light's `+0x16` and used by the blink tick `FUN_0045B330` as a
> **random range added to the timer**, which is why it is `0` on 12081 of 12083 lights across the
> shipped maps.

If `onTime != 0` the light is registered as a blinking light and **starts switched off**
(`FUN_00469070` sets intensity to 0 before linking it into the timed list).

**Shipped map content:**

| Map | Lights | Blinking | Jitter | Intensity < 255 | Radius min/max (tiles) |
|---|---:|---:|---:|---:|---|
| wil.gmp | 1982 | 0 | 0 | 81 | 0.25 / 8.00 |
| wil-multi.gmp | 1982 | 0 | 0 | 81 | 0.25 / 8.00 |
| ste.gmp | 1743 | 14 | 0 | 57 | 1.00 / 8.00 |
| bil.gmp | 1646 | 40 | 1 | 63 | 1.00 / 6.00 |
| bil-multi.gmp | 1646 | 40 | 1 | 63 | 1.00 / 6.00 |
| mike1e.gmp | 706 | 5 | 0 | 40 | 1.00 / 7.00 |
| mike1h.gmp | 697 | 5 | 0 | 46 | 1.00 / 6.00 |
| MP5-comp.gmp | 581 | 6 | 0 | 2 | 0.00 / 6.00 |
| MP1-comp.gmp | 305 | 2 | 0 | 2 | 0.00 / 7.21 |
| MP2-comp.gmp | 301 | 0 | 0 | 40 | 0.00 / 8.00 |
| mike2e.gmp | 121 | 0 | 0 | 1 | 1.00 / 5.00 |
| mike2m.gmp | 103 | 0 | 0 | 0 | 1.00 / 8.00 |
| mike2h.gmp | 101 | 0 | 0 | 0 | 1.00 / 8.00 |
| mike1m.gmp | 91 | 0 | 0 | 7 | 1.00 / 5.00 |
| lorne2e.gmp | 46 | 0 | 0 | 0 | 2.00 / 4.00 |
| lorne2h.gmp | 18 | 0 | 0 | 0 | 8.00 / 8.00 |
| lorne2m.gmp | 14 | 0 | 0 | 0 | 8.00 / 8.00 |

**12 083 lights total, 765 distinct colours.** Most common:

| Colour | Count | Share | Likely |
|---|---:|---:|---|
| `#62CC8C` | 1369 | 11.3% | green neon |
| `#FF8000` | 1138 | 9.4% | sodium street lamp |
| `#FF8040` | 939 | 7.8% | sodium street lamp |
| `#FFFFFF` | 924 | 7.6% | white |
| `#FFFF00` | 675 | 5.6% | yellow |
| `#FFDB5E` | 500 | 4.1% | warm white |
| `#FF9224` | 370 | 3.1% | amber |
| `#8080FF` | 312 | 2.6% | cold blue |
| `#80FFFF` | 260 | 2.2% | cyan |
| `#00FFFF` | 255 | 2.1% | cyan neon |

Radii cluster hard on whole tiles: in `wil.gmp`, 538 lights at r=3, 534 at r=4, 518 at r=2, 199 at
r=5, 38 at r=6. Z values are quantised to 0.25 tile steps over the range 0 .. 6.5.

### 3.2 Traffic lights

`FUN_004C3C70` builds a junction's traffic lights. Per approach direction it creates a light with
`FUN_00469010(x, y, z, colour, DAT_006722A0, 200)`:

- **north/south pair** — initial colour `0xFF0000` (red)
- **east/west pair** — initial colour `0x00FF00` (green)

`FUN_004C3A10` is the phase state machine, driving the light colour through `FUN_00476B00`:

| Phase | Colour |
|---|---|
| 1, 4 | `0x00FF00` green |
| 2, 5 | `0xFF8000` amber |
| 3, 6 | `0xFF0000` red |

Related strings: `"traffic lights at (%d,%d) are too close to the edge of the world"`,
`"Too many traffic lights"`, and the debug flags `do_show_traffic_lights_info` / `skip_traffic_lights`.

### 3.3 Vehicle lights

`FUN_00424700`, called at the end of vehicle construction `FUN_00426AC0` — **every vehicle gets
light objects attached at spawn**. It creates up to four, at fixed local offsets from the car body,
as light-class game objects of types `0xA5`, `0xAB`, `0xAC`, `0xAD` (front-left, front-right,
rear-left, rear-right):

| Vehicle shape | Lights created (type, colour, offsetX, offsetY) |
|---|---|
| 4-light car | `A5 #FF2010 (+10,+16)`, `AB #FF2010 (−10,+16)`, `AC #FF2010 (+10,−32)`, `AD #FF2010 (−10,−32)` |
| 2-light | `A5 #FF2010 (+10,−16)`, `AB #0000FF (−10,−16)` |
| mixed | `A5 #FF2010 (+10,+32)`, `AB #0000FF (−10,+32)`, `AC #FF2010 (+10,−32)`, `AD #0000FF (−10,−32)` |
| large | `A5 #FF2010 (+16,+48)`, `AB #FF2010 (−16,+48)`, `AC #FF2010 (+16,−16)`, `AD #FF2010 (−16,−38)` |
| single | `A5 #FF2010 (0,+16)` |

`#FF2010` is the warm headlight colour; `#0000FF` the blue rear/roof light. Radius comes from
`DAT_005E5040`, intensity is hard-coded to **200**. `FUN_004BE870` → `FUN_00486130` keeps the light
in sync with the parent object's position each frame.

### 3.4 Light game-objects (the generic mechanism)

`FUN_00484E00` is the object constructor. When the object type's class field (`typeDesc + 0x34`)
is **`0x0B`**, it allocates a light via `FUN_00469010` and stores it at `obj + 0x0C`. Wrappers:

- `FUN_00485370(x, y, z, rgb, radius, intensity)` — spawn object type `0xA5` with a light
- `FUN_004853C0(type, x, y, z, rgb, radius, intensity)` — same, arbitrary type
- `FUN_00420950(type, rgb, offX, offY)` — spawn attached to the current vehicle, intensity `200`

Known emitters:

| Caller | Light | Meaning |
|---|---|---|
| `FUN_004AFE20` ("pubtrans.cpp") | `#FF8000`, r=3, i=255 | train / bus headlight, created with the vehicle |
| `FUN_00491240` | `#FF8000`, r=3, i=255 | spawned alongside object sprites `0x91`/`0x71` |
| `FUN_004CE970` | `#FF8000`, r=3, i=255 | weapon/vehicle event light |
| `FUN_0047F710` | from a template at `obj+0x0C..0x23` | light restore for a serialised object |

`FUN_0047F4F0` destroys a light and returns it to the pool.

### 3.5 Mission script control

`FUN_004805B0` is the `.scr` mission-script opcode dispatcher (jump table over opcodes
`0x29 .. 0x1BE`). It calls, among others:

- `FUN_0046C140(ambientState, target, fadeFrames)` — **set ambient level**, with an optional fade
- `FUN_0047F710` — create a light object
- `FUN_00476AE0` — set a light's intensity
- `FUN_00476B00` — set a light's colour

So per-mission and per-cutscene ambient changes and scripted lights both exist and both flow through
the same three renderer entry points.

---

## 4. Ambient light

The ambient state is a three-word record on the world-render object:

| Word | Meaning |
|---|---|
| `[0]` | current ambient, 16.14 fixed |
| `[1]` | target ambient |
| `[2]` | per-frame step |

`FUN_0046C140` sets the target (with fade time, or snaps immediately when `fadeFrames == 0`);
`FUN_0046C1A0` steps `[0]` toward `[1]` once per frame, called directly after `gbh_SetAmbient`.
`FUN_00472110` passes `current * 2^-14` to `gbh_SetAmbient` as a float.

Two other callers force ambient to `1.0f`:

- `FUN_00456A60` — frontend/menu init ("frontend2.cpp")
- `FUN_004C6DA0` — when `DAT_00595011` (lighting enabled) is set

The error string `"Invalid ambient light value : %f"` exists in the assert table (`FUN_0044DC50`),
alongside `"Invalid map light data"`.

There is **no day/night cycle** — GTA2 ambient is purely map/script-driven, constant unless a script
changes it.

---

## 5. Blinking / animated lights

`FUN_00469070(light, onTime, offTime, jitter)` registers a light in the timed list
(`FUN_00464C60` links it at `+0x1C`) and sets intensity to 0 so it starts dark.

`FUN_0045BF50` walks that list once per frame and calls `FUN_0045B330` on each:

```c
if (--light->counter == 0) {
    if (isOn(light)) { setIntensity(light, 0);              light->counter = light->offTime; }
    else             { setIntensity(light, light->baseInt); light->counter = light->onTime;  }
    if (light->jitter) light->counter += rand(light->jitter);
}
```

Blink periods in the shipped maps are symmetric and coarse — `(40,40)` on 64 lights, `(20,20)` on 16,
`(10,10)` and `(6,6)` on 6 each, `(48,48)` on 4. Only two lights in the whole game (both in
`bil.gmp`) use the jitter field, with value 51.

---

## 6. Things that are *not* lights

- **`SetShadeTableA`** is bound but never called — dead software-renderer path.
- **Per-face static shading.** The `shade` byte carried by every draw call (splatted to grey unless
  flag `0x2000` supplies vertex colours) is a per-primitive brightness, applied *multiplicatively to
  the light sum*, not an independent light source. It is the game's own darkening of upper levels
  and sloped faces.
- **`light_space`** is a named memory pool (one of 18 in the table at `0x00591F00`), not a light
  concept.
- **Muzzle flashes, explosions, fires** are drawn as **sprites** through the screen-space
  `gbh_DrawQuad` stream, not as lights — except where they also spawn a class-`0x0B` object as listed
  in §3.4. There is no separate "explosion light" system.
- **`ANIM` chunk** in the `.gmp` animates *tile graphics* (including glowing signage), not lights.

---

## 7. Implications for the Remix port

1. **No new hooking is needed.** `renderer/dll/src/dllmain.cpp` already exports and receives
   `gbh_ResetLights`, `gbh_AddLight` and `gbh_SetAmbient` — they are currently
   `PASSTHROUGH_0` / `PASSTHROUGH_1` stubs forwarding to `3dfx.dll`. Replacing the three bodies with
   a light collector is the whole job on the interception side.

2. **The data is already world-space and already view-culled.** `x, y, z` are floats in tile units in
   the same frame as the world geometry, so they map straight onto Remix light positions with the
   same scale factor the world mesh uses. No unprojection, no violation of the world-space-only rule.

3. **Lifetime is per-frame.** `gbh_ResetLights` → N × `gbh_AddLight` happens once per frame before
   any geometry. Buffer them and emit to Remix after the last `AddLight` — the count is known only
   when the first draw call arrives.

4. **Watch the ambient gate.** If a map/script leaves ambient at `1.0`, the original renderer
   short-circuits lighting; the calls still arrive, so a Remix implementation would light a scene the
   original left flat. Reading `gbh_SetAmbient` and scaling light contribution the same way keeps the
   look consistent, or ignore it deliberately if the path-traced result looks better.

5. **Falloff mismatch is expected and fine.** GTA2 uses linear `(r − d)/r` falloff with a hard cutoff
   at `r`; Remix uses physical inverse-square. A reasonable mapping is radiance ∝ `intensity` with
   the Remix light radius set from `radius`, then tune. Do not try to reproduce the linear ramp.

6. **`gbh_SetCamera`'s tile rect** is free view-bounds information if the world mesh needs it.

7. **A light can blink**, and blinking is expressed purely as intensity going to 0 and back. A Remix
   light with intensity 0 should be dropped rather than emitted, or traffic lights will glow on all
   three colours at once.

---

## Appendix — key addresses

**gta2.exe** (base `0x400000`)

| Address | Symbol |
|---|---|
| `0x004612A0` | light grid alloc (64×64 buckets of 4×4 tiles) |
| `0x004612D0` | light grid free |
| `0x00461360` | link light into grid |
| `0x004613B0` | unlink light from grid |
| `0x00461400` | submit visible lights → `gbh_AddLight` |
| `0x00469010` | create light `(x,y,z,rgb,radius,intensity)` |
| `0x00469070` | set up blinking `(light,onTime,offTime,jitter)` |
| `0x004692B0` | load `LGHT` chunk → lights |
| `0x00472110` | world render loop (`SetAmbient`/`ResetLights`/`SetCamera`) |
| `0x00476AE0` | set light intensity |
| `0x00476B00` | set light colour |
| `0x00482D30` | move light |
| `0x00482D60` | set colour + radius + intensity |
| `0x00484E00` | object ctor — class `0x0B` allocates a light |
| `0x00485370` | spawn light object type `0xA5` |
| `0x004853C0` | spawn light object, arbitrary type |
| `0x00420950` | spawn vehicle-attached light |
| `0x00424700` | create a vehicle's headlights/tail lights |
| `0x0045B330` | blink tick (one light) |
| `0x0045BF50` | blink tick (whole list) |
| `0x0046C140` | set ambient target + fade |
| `0x0046C1A0` | step ambient toward target |
| `0x004C3C70` | build junction traffic lights |
| `0x004C3A10` | traffic light phase → colour |
| `0x004CB1D0` | config parse: `lighting` → `DAT_00595011`, `DAT_0067358C` |
| `0x005952EC/F0/F4` | `gbh_ResetLights` / `gbh_AddLight` / `gbh_SetAmbient` pointers |
| `0x006622B4` | light bucket grid |
| `0x00595011` | lighting enabled flag |
| `0x0067358C` | `0x8000` draw-flag bit when lighting on |

**d3ddll.dll** (base `0xE00000`)

| Address | Symbol |
|---|---|
| `0x00E01D40` | `gbh_ResetLights` |
| `0x00E01D50` | `gbh_SetAmbient` |
| `0x00E01D70` | `gbh_AddLight` |
| `0x00E01E90` | `gbh_SetCamera` |
| `0x00E02A80` | per-vertex lighting pass |
| `0x00E02CC0` | shared draw body (lighting gated on flag `0x8000`) |
| `0x00E10838` | ambient × 255 |
| `0x00E43E38` | live light count |
| `0x00E459E0` | light array, stride `0x2C` |

---

## 8. Synthetic lights — reverse engineering for effects the game does not light

GTA2 emits no light for gunfire, bullets, sparks, cigarettes or headlight beams;
the lights in §3 are the complete set it knows about. Adding them means reading
the game's own effect state and inventing lights from it. This section records
what that needs. **Not yet implemented** — this is the groundwork.

### 8.1 The particle system

`particle.cpp`. The manager is a single `0x947C`-byte object at `0x00669E70`
(`FUN_00491B90` allocates it), and it is also the pool:

| Address | Meaning |
|---|---|
| `*(void**)(mgr + 0x00)` | free list head |
| `*(void**)(mgr + 0x04)` | **live list head** |

Walk the live list through `+0x3C`. `FUN_0048A900` is the allocator that does
the linking, `FUN_0048A8F0` the "any left?" test.

Per particle:

| Offset | Field |
|---|---|
| `+0x00` | heading, `uint16` (`FUN_00420690`) |
| `+0x14` | x, 16.14 fixed, tiles east |
| `+0x18` | y, 16.14 fixed, tiles south |
| `+0x1C` | z, 16.14 fixed, map level |
| `+0x2C` | life in frames |
| `+0x38` | **type id** |
| `+0x3C` | next in the live list |

Position and heading are set by `FUN_00420600` / `FUN_00420690`, the same pair
the object system uses, so particles, objects and vehicles all share this
placement layout.

Type ids seen so far, by spawner:

| Spawner | Type | Called from |
|---|---|---|
| `FUN_0048C9C0` | `0x01` | 6-particle burst, several call sites |
| `FUN_0048D4E0` | `0x1F` | weapon module (`0x4CDD25`, `0x4CFD39`); also makes object `0xC2` |
| `FUN_0048D8B0` | `0x22` | weapon module (`0x4CFE84`); also makes object `0xC6` |
| `FUN_0048D1F0` | `0x23` | 6-particle burst, four call sites |
| `FUN_0048DDC0` | `0x25` | weapon module (`0x4CCEA1`, `0x4CCFC5`) |
| `FUN_0048DFC0` | `0x26` | `FUN_004825C0` |
| `FUN_0048CC50` | `0x27` | `0x004413FA` |
| `FUN_0048CD10` | `0x28`, `0x29` | weapon module, six call sites — fires **two** particles per shot |

The weapon module is the `0x4CCE00`–`0x4D0800` range, identified by the
`weapon.cpp` assert string at `0x0057593C` (referenced from `FUN_004D0740`).

**Which id is the muzzle flash, the bullet, the spark and the cigarette is still
open.** Reading it out of the remaining spawners is slow and easy to get wrong;
the reliable way is a live histogram of particle types in the F4 menu, then
trigger each effect and watch which id appears.

### 8.2 Vehicles

The vehicle pool pointer is `0x005E4CA0`, same two-list shape:

| Address | Meaning |
|---|---|
| `*(void**)(*pool + 0x00)` | free list head |
| `*(void**)(*pool + 0x04)` | **live list head** |

Walk through `+0x4C` (`FUN_004254A0` appends, `FUN_00425480` prepends). Per
vehicle: the shared placement layout above, plus `+0x84` = car model id and
`+0x88` = 1. Created by `FUN_00426AC0`, which also calls `FUN_00424700` to hang
the four light objects of §3.3 on it.

**"Is it being driven" has not been found**, and does not need to be: a vehicle
that has moved since the previous frame is being driven, by the player or by
anyone else, and a parked one has not. That is a runtime test needing no further
reverse engineering, and it is what the headlight cones should key off.

### 8.3 What still has to be decided at runtime, not statically

Two unknowns remain, and both are better answered by the running game than by
more decompilation:

- **which particle type is which effect** — bind categories to type ids from the
  F4 menu, seeded with the weapon-module ids above, and persist the binding
- **which cars are driven** — compare position between frames

Neither blocks the implementation; both argue for the type binding being a
setting rather than a constant.
