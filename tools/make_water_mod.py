"""Builds the water mod, package/rtx-remix/mods/gta2water, from a baked wave sheet.

    make_water_mod.py <water_normal sheet.png>

The normal map it writes is over the 1 MB the repository takes, so it is kept
out of git and only ships in the release zip; package.bat refuses to package
without it.

The sheet comes from a water flipbook generator: 120 frames of tileable,
loopable waves, 12 columns by 10 rows of 256 px, tangent-space normals (x = +U,
y = +V, z up) in RGB. The shipped one was baked with the fractal cascade model
over a 10 m tile: wave height 0.5 m, stillness 0, choppiness 1.6, wave size 0.3,
direction spread 1.0, wind 25 degrees, detail 96, seed 2002, at 30 fps - a four
second loop.

What the mod does. GTA2 animates its water by swapping the tile's artwork (tiles
608-619, the map's first ANIM entry), so Remix sees twelve texture hashes on one
surface. Every one of them gets the same translucent material, and Remix plays
the wave sheet itself (sprite_sheet_*), so the game's own animation only ever
picks between identical materials. The renderer gives water lids texture
coordinates from the world, one repeat every 2.5 blocks (10 m), and puts a sand
riverbed under them to be seen through the water - see SetWaterUvBlocks and
SetRiverbedDepth in src/world_mesh.h.

The normal map. Remix's runtime reads a normal map as unsigned octahedral in R/G
and nothing else (unsignedOctahedralToHemisphereDirection:
t = ((px + py) / 2, (px - py) / 2), n = (t, 1 - |t.x| - |t.y|)); the Toolkit
converts on ingestion, so a map made anywhere else has to arrive converted. It
is written as BC5 with a full mip chain, the normals averaged as vectors for
each mip before encoding.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, 'package', 'rtx-remix', 'mods', 'gta2water')

COLS, ROWS, FPS = 12, 10, 30

# Tiles 608-619, hashed as the renderer uploads them; identical in all three
# districts.
HASHES = [
    'A8B83A83052A85C0', '242CC049392EB1C7', '8471637CC12C9CCE', '8D2E06C15AA0A168',
    'A41C669D0F88E0FE', '83DD0D2963D8F8E6', '27D814B223C008A4', 'EBCCC2CF700A35CE',
    'A8EA3821BB08B0BD', 'B2AB657C3297E6E6', '1CEDF04B826D09CE', '9C52D4ABDFF4F6AE',
]

MATERIAL = '''        def Material "mat_{h}"
        {{
            token outputs:mdl:displacement.connect = </RootNode/Looks/mat_{h}/Shader.outputs:out>
            token outputs:mdl:surface.connect = </RootNode/Looks/mat_{h}/Shader.outputs:out>
            token outputs:mdl:volume.connect = </RootNode/Looks/mat_{h}/Shader.outputs:out>

            def Shader "Shader" (
                kind = "Material"
            )
            {{
                uniform token info:implementationSource = "sourceAsset"
                uniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Translucent"
                custom float inputs:ior_constant = 1.33
                custom asset inputs:normalmap_texture = @./SubUSDs/water_normal.n.dds@ (
                    colorSpace = "raw"
                )
                custom int inputs:sprite_sheet_cols = {cols}
                custom int inputs:sprite_sheet_fps = {fps}
                custom int inputs:sprite_sheet_rows = {rows}
                custom bool inputs:thin_walled = 0
                custom color3f inputs:transmittance_color = (0.45, 0.75, 0.55)
                custom float inputs:transmittance_measurement_distance = 1.0
                custom bool inputs:use_legacy_alpha_state = 0
                custom int inputs:wrap_mode_u = 0
                custom int inputs:wrap_mode_v = 0
                token outputs:out
            }}
        }}
'''


def write_usda(path):
    body = '\n'.join(MATERIAL.format(h=h, cols=COLS, rows=ROWS, fps=FPS) for h in HASHES)
    text = ('#usda 1.0\n(\n    upAxis = "Y"\n)\n\nover "RootNode"\n{\n    over "Looks"\n    {\n'
            + body + '    }\n}\n')
    open(path, 'w', encoding='utf-8', newline='\n').write(text)


def halve(a):
    h, w = a.shape[:2]
    h2, w2 = max(h // 2, 1), max(w // 2, 1)
    a = a[:max(h2 * 2, 1) if h > 1 else 1, :max(w2 * 2, 1) if w > 1 else 1]
    ry, rx = (2 if h > 1 else 1), (2 if w > 1 else 1)
    return a.reshape(h2, ry, w2, rx, a.shape[2]).mean(axis=(1, 3))


def octahedral(n):
    n = n / np.maximum(np.abs(n).sum(-1, keepdims=True), 1e-8)
    return np.stack([n[..., 0] + n[..., 1], n[..., 0] - n[..., 1]], -1) * 0.5 + 0.5


def bc4_blocks(channel):
    """One BC4 block per 4x4 texels of a 0..255 channel, 8-value mode."""
    h, w = channel.shape
    bh, bw = (h + 3) // 4, (w + 3) // 4
    padded = np.pad(channel, ((0, bh * 4 - h), (0, bw * 4 - w)), mode='edge')
    blocks = padded.reshape(bh, 4, bw, 4).transpose(0, 2, 1, 3).reshape(bh, bw, 16)
    hi = blocks.max(-1).astype(np.int32)
    lo = blocks.min(-1).astype(np.int32)
    span = np.maximum(hi - lo, 1)[..., None]
    # Palette from hi down to lo: index 0 = hi, 2..7 the six steps, 1 = lo.
    step = np.clip(np.rint((hi[..., None] - blocks) * 7.0 / span), 0, 7).astype(np.uint64)
    step[(hi == lo)] = 0
    index = np.array([0, 2, 3, 4, 5, 6, 7, 1], np.uint64)[step]
    bits = np.zeros((bh, bw), np.uint64)
    for i in range(16):
        bits |= index[..., i] << np.uint64(3 * i)
    out = np.zeros((bh, bw, 8), np.uint8)
    out[..., 0] = hi
    out[..., 1] = lo
    for b in range(6):
        out[..., 2 + b] = ((bits >> np.uint64(8 * b)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def bc5(rg):
    r = bc4_blocks(rg[..., 0])
    g = bc4_blocks(rg[..., 1])
    return np.concatenate([r, g], -1).tobytes()


def write_normal_dds(png, path):
    n = np.asarray(Image.open(png).convert('RGB')).astype(np.float32) / 127.5 - 1.0
    n[..., 2] = np.maximum(n[..., 2], 1e-3)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    height, width = n.shape[:2]
    levels = [n]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        levels.append(halve(levels[-1]))
    data = []
    for level in levels:
        level = level / np.maximum(np.linalg.norm(level, axis=-1, keepdims=True), 1e-8)
        data.append(bc5(np.clip(np.rint(octahedral(level) * 255), 0, 255).astype(np.uint8)))
    # caps, height, width, pixel format, mip count, linear size - as the
    # Toolkit writes its own BC5 normal maps.
    header = struct.pack('<7I44x', 124, 0xA1007, height, width, len(data[0]), 0, len(levels))
    pixfmt = struct.pack('<2I4s5I', 32, 0x4, b'BC5U', 0, 0, 0, 0, 0)
    caps = struct.pack('<5I', 0x401008, 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'DDS ' + header + pixfmt + caps + b''.join(data))
    return len(levels)


if __name__ == '__main__':
    os.makedirs(os.path.join(MOD, 'SubUSDs'), exist_ok=True)
    mips = write_normal_dds(sys.argv[1], os.path.join(MOD, 'SubUSDs', 'water_normal.n.dds'))
    write_usda(os.path.join(MOD, 'mod.usda'))
    print('wrote', MOD, '- normal map with', mips, 'mips')
