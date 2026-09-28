"""Emissive maps for the renderer's own car lamp quads.

The renderer draws each lit car lamp as a small quad of its own, textured with
that lamp's pixels cut out of the car's artwork (live_geometry.h, OwnCarLamps).
Those textures are built from the style file alone, so they are known before the
game ever runs: this rebuilds every one exactly as LampTexels uploads it, takes
the Remix hash, and writes an emissive map for it the way the lamp emissives in
the mod were made by hand - the texture's own colour where its brightest channel
is over the threshold.

    python lamp_emissives.py <GTA2 folder> <output folder> [--threshold 0.8]

writes <hash>_emissive.png for every lamp texture, and lamps.csv saying which car
sprite, delta and side each one is. Feed the PNGs to the mod as any other
emissive.

Needs numpy, Pillow and xxhash.
"""
import argparse
import csv
import os
import struct

import numpy as np
import xxhash
from PIL import Image

DISTRICTS = ('wil', 'ste', 'bil')
# The deltas the game may draw as its lamp quad (the masks at 0x00591E90..98);
# 22 and up are these mirrored. 11 - a door on most cars, the headlight on a
# few - is never in those masks, so it is never one of the renderer's lamp quads.
LAMP_DELTAS = (5, 6, 15, 16, 17, 18)
MIRROR_OFFSET = 17
MIRROR_FIRST_BIT = 22


class Style:
    def __init__(self, path):
        data = open(path, 'rb').read()
        assert data[:4] == b'GBST', path
        self.chunks = {}
        pos = 6
        while pos + 8 <= len(data):
            tag = data[pos:pos + 4].decode('ascii', 'replace')
            size = struct.unpack_from('<I', data, pos + 4)[0]
            self.chunks[tag] = data[pos + 8:pos + 8 + size]
            pos += 8 + size
        self.palx = np.frombuffer(self.chunks['PALX'], '<u2')
        self.ppal = self.chunks['PPAL']
        self.tile_palettes = struct.unpack_from('<H', self.chunks['PALB'])[0]
        self.sprb = struct.unpack_from('<6H', self.chunks['SPRB'])
        sprx = self.chunks['SPRX']
        self.sprites = [struct.unpack_from('<IBB', sprx, i * 8) for i in range(len(sprx) // 8)]
        self.deltas = {}
        delx, dels, p, q = self.chunks['DELX'], self.chunks['DELS'], 0, 0
        while p + 4 <= len(delx):
            sprite, count, _ = struct.unpack_from('<HBB', delx, p)
            p += 4
            sizes = struct.unpack_from('<%dH' % count, delx, p)
            p += 2 * count
            runs = []
            for s in sizes:
                runs.append(dels[q:q + s])
                q += s
            self.deltas[sprite] = runs

    def colour(self, sprite, index):
        # Gta2Style::SpriteDeltaImage: PALX slot after the tiles', 64 palettes to
        # a 64K page, entry i of palette p at (p/64)*64K + i*256 + (p%64)*4, B,G,R.
        palette = int(self.palx[self.tile_palettes + sprite])
        at = (palette // 64) * 65536 + index * 256 + (palette % 64) * 4
        b, g, r = self.ppal[at], self.ppal[at + 1], self.ppal[at + 2]
        return 0xFF000000 | (r << 16) | (g << 8) | b

    def delta_image(self, sprite, delta):
        runs = self.deltas.get(sprite)
        if not runs or delta >= len(runs) or not runs[delta]:
            return None
        data, i, pos, pixels = runs[delta], 0, 0, []
        while i + 3 <= len(data):
            skip, length = struct.unpack_from('<HB', data, i)
            i += 3
            pos += skip
            for k in range(length):
                if i + k < len(data):
                    y, x = divmod(pos + k, 256)
                    pixels.append((x, y, data[i + k]))
            i += length
            pos += length
        if not pixels:
            return None
        x0 = min(p[0] for p in pixels)
        y0 = min(p[1] for p in pixels)
        w = max(p[0] for p in pixels) + 1 - x0
        h = max(p[1] for p in pixels) + 1 - y0
        argb = np.zeros((h, w), np.uint32)
        for x, y, c in pixels:
            if c:
                argb[y - y0, x - x0] = self.colour(sprite, c)
        return argb


def lamp_texels(argb, mirrored):
    # live_geometry.cpp, LampTexels: transparent texels take the opaque ones'
    # mean colour, with integer division, and alpha 0.
    opaque = (argb >> 24) != 0
    fill = 0
    if opaque.any():
        c = argb[opaque]
        fill = (int(((c >> 16) & 0xFF).sum()) // len(c) << 16) | \
               (int(((c >> 8) & 0xFF).sum()) // len(c) << 8) | (int((c & 0xFF).sum()) // len(c))
    out = np.where(opaque, argb, np.uint32(fill)).astype(np.uint32)
    return out[:, ::-1].copy() if mirrored else out


def remix_hash(texels):
    return '%016X' % xxhash.xxh3_64_intdigest(texels.astype('<u4').tobytes())


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def emissive(texels, threshold, softness=0.05):
    # The emissive helper's mask: brightest channel over the threshold, the
    # texture's own colour, nothing where it is transparent.
    rgba = texels.view(np.uint8).reshape(texels.shape + (4,))[..., [2, 1, 0, 3]].astype(float) / 255
    bright = rgba[..., :3].max(-1)
    m = smoothstep(threshold - softness, threshold + softness + 1e-5, bright)
    m *= smoothstep(0.25, 0.5, rgba[..., 3])
    out = np.ones(texels.shape + (4,))
    out[..., :3] = np.minimum(rgba[..., :3] * m[..., None], 1)
    return (out * 255 + 0.5).astype(np.uint8), float(m.max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('out')
    ap.add_argument('--threshold', type=float, default=0.8)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    seen = {}
    dark = 0
    for district in DISTRICTS:
        path = os.path.join(args.game, 'data', district + '.sty')
        if not os.path.exists(path):
            continue
        st = Style(path)
        for sprite in range(st.sprb[0]):   # cars come first
            for delta in LAMP_DELTAS:
                argb = st.delta_image(sprite, delta)
                if argb is None:
                    continue
                for mirrored in (False, True):
                    if mirrored and delta + MIRROR_OFFSET < MIRROR_FIRST_BIT:
                        continue
                    texels = lamp_texels(argb, mirrored)
                    h = remix_hash(texels)
                    if h in seen:
                        continue
                    png, peak = emissive(texels, args.threshold)
                    if peak <= 0.0:
                        dark += 1
                    Image.fromarray(png, 'RGBA').save(os.path.join(args.out, h + '_emissive.png'))
                    seen[h] = (district, sprite, delta, 'mirrored' if mirrored else 'as drawn',
                               texels.shape[1], texels.shape[0], peak)
    with open(os.path.join(args.out, 'lamps.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['hash', 'district', 'car_sprite', 'delta', 'side', 'width', 'height', 'peak'])
        for h, row in sorted(seen.items()):
            w.writerow([h] + list(row))
    print('%d lamp textures, %d with nothing over the threshold' % (len(seen), dark))


if __name__ == '__main__':
    main()
