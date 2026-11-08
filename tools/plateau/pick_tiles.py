"""Chooses the style tiles the Plateau map is drawn with, from your own GTA2.

    python pick_tiles.py <GTA2 folder> [--district ste] [--out palette.json]
                         [--sheet tiles.png]

GTA2's tiles are numbered differently in every style, and none of them ship with
this repository. This reads one district's map and style (ste, the residential
district, by default), counts which tiles that map really uses for its roads,
pavements, grass, water, roofs and walls, and writes the most fitting of each to
palette.json for make_plateau_map.py --palette.

--sheet also draws every tile of the style with its number, the chosen ones
outlined, so a choice can be checked and changed by hand: edit the number in
palette.json and build the map again.

Needs numpy and Pillow.
"""
import argparse
import collections
import json
import os
import struct
import sys

import numpy as np

import gmp

TILE_SIZE = 64
TILES_PER_PAGE = 16
PAGE = 256


class Style:
    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        if data[:4] != b'GBST':
            raise ValueError('%s is not a GTA2 style' % path)
        chunks = {}
        pos = 6
        while pos + 8 <= len(data):
            tag = data[pos:pos + 4].decode('ascii', 'replace')
            size = struct.unpack_from('<I', data, pos + 4)[0]
            chunks[tag] = data[pos + 8:pos + 8 + size]
            pos += 8 + size
        for tag in ('TILE', 'PPAL', 'PALX'):
            if tag not in chunks:
                raise ValueError('%s has no %s chunk' % (path, tag))
        self.tile_data = np.frombuffer(chunks['TILE'], np.uint8)
        self.ppal = np.frombuffer(chunks['PPAL'], np.uint8)
        self.palx = np.frombuffer(chunks['PALX'][:len(chunks['PALX']) // 2 * 2], '<u2')
        self.count = len(chunks['TILE']) // (PAGE * PAGE) * TILES_PER_PAGE
        self.water = self._water_tiles(chunks.get('SPEC', b''))
        self._cache = {}

    @staticmethod
    def _water_tiles(spec):
        # SPEC holds zero-terminated lists of tile numbers; the third is water
        # (src/gta2_style.cpp, IsWaterTile).
        lists, current = [], []
        for (tile,) in struct.iter_unpack('<H', spec[:len(spec) // 2 * 2]):
            if tile == 0:
                lists.append(current)
                current = []
            else:
                current.append(tile)
        return set(lists[2]) if len(lists) > 2 else set()

    def tile(self, index):
        """64x64x4 uint8 RGBA."""
        if index in self._cache:
            return self._cache[index]
        page, slot = divmod(index, TILES_PER_PAGE)
        base = page * PAGE * PAGE + (slot // 4) * TILE_SIZE * PAGE + (slot % 4) * TILE_SIZE
        rows = np.lib.stride_tricks.as_strided(
            self.tile_data[base:], shape=(TILE_SIZE, TILE_SIZE), strides=(PAGE, 1))
        palette = int(self.palx[index]) if index < len(self.palx) else 0
        at = (palette // 64) * 65536 + (palette % 64) * 4 + rows.astype(np.int64) * 256
        rgba = np.zeros((TILE_SIZE, TILE_SIZE, 4), np.uint8)
        for channel, offset in ((0, 2), (1, 1), (2, 0)):
            rgba[..., channel] = self.ppal[np.minimum(at + offset, len(self.ppal) - 1)]
        rgba[..., 3] = np.where(rows == 0, 0, 255)
        self._cache[index] = rgba
        return rgba

    def stats(self, index):
        rgba = self.tile(index)
        rgb = rgba[..., :3].reshape(-1, 3).astype(float)
        mean = rgb.mean(0)
        return dict(mean=mean, spread=float(rgb.std(0).mean()),
                    transparent=bool((rgba[..., 3] == 0).any()))


def surface_counts(columns, blocks):
    """Tile use in a shipped map, sorted into the surfaces the Plateau needs."""
    top = {}
    for y in range(gmp.MAP_H):
        for x in range(gmp.MAP_W):
            offset, indices = columns[y][x]
            for i in range(len(indices) - 1, -1, -1):
                b = blocks[indices[i]]
                tile = b.lid & 0x3FF
                if tile and tile != 0x3FF:
                    top[(x, y)] = (offset + i, b.ground, tile)
                    break

    levels = collections.Counter(z for z, g, _ in top.values() if g == gmp.ROAD)
    ground = levels.most_common(1)[0][0] if levels else 1

    def is_road(x, y):
        t = top.get((x, y))
        return t is not None and t[1] == gmp.ROAD

    use = collections.defaultdict(collections.Counter)
    for (x, y), (z, g, tile) in top.items():
        if g == gmp.ROAD:
            use['road'][tile] += 1
        elif g == gmp.FIELD:
            use['grass'][tile] += 1
        elif g == gmp.PAVEMENT and z == ground:
            by_road = any(is_road(x + dx, y + dy) for dx, dy in gmp.ARROW_STEP.values())
            use['sidewalk' if by_road else 'alley'][tile] += 1
        elif g == gmp.PAVEMENT and z >= ground + 2:
            use['roof'][tile] += 1
        if z < ground:
            use['low'][tile] += 1

    for y in range(gmp.MAP_H):
        for x in range(gmp.MAP_W):
            offset, indices = columns[y][x]
            for i, index in enumerate(indices):
                if offset + i <= ground:
                    continue
                b = blocks[index]
                for side in (b.left, b.right, b.top, b.bottom):
                    if side & gmp.WALL and side & 0x3FF:
                        use['wall'][side & 0x3FF] += 1
    return use, ground


def pick(use, style):
    """One tile per palette key, with the reason it was chosen."""
    chosen, why = {}, {}
    taken = set()

    def usable(tile):
        return 0 < tile < style.count and not style.stats(tile)['transparent']

    def plainest(counter, key, label):
        """The least patterned of the commonly used ones: plain asphalt rather
        than a junction's markings."""
        common = [(t, n) for t, n in counter.most_common(12) if usable(t) and t not in taken]
        if not common:
            return False
        top_n = common[0][1]
        candidates = [(t, n) for t, n in common if n >= top_n * 0.25]
        tile = min(candidates, key=lambda c: style.stats(c[0])['spread'])[0]
        chosen[key], why[key] = tile, '%s, used %d times' % (label, counter[tile])
        taken.add(tile)
        return True

    def most(counter, key, label, test=lambda s: True):
        for tile, n in counter.most_common():
            if usable(tile) and tile not in taken and test(style.stats(tile)):
                chosen[key], why[key] = tile, '%s, used %d times' % (label, n)
                taken.add(tile)
                return True
        return False

    def greenness(s):
        r, g, b = s['mean']
        return g - (r + b) / 2

    plainest(use['road'], 'road', 'plainest common road lid')
    plainest(use['sidewalk'], 'sidewalk', 'plainest common pavement beside a road')
    plainest(use['alley'], 'alley', 'plainest common pavement away from roads') or \
        most(use['sidewalk'], 'alley', 'next pavement beside a road')
    most(use['sidewalk'], 'plaza', 'next pavement beside a road') or \
        most(use['alley'], 'plaza', 'next pavement away from roads')

    grass = [(t, n) for t, n in use['grass'].most_common(10) if usable(t)]
    if grass:
        tile = max(grass, key=lambda c: greenness(style.stats(c[0])))[0]
        chosen['grass'], why['grass'] = tile, 'greenest common field lid'
        taken.add(tile)

    water = collections.Counter({t: n for t, n in use['low'].items() if t in style.water})
    if not most(water, 'water', "most used of the style's water tiles"):
        if style.water:
            tile = min(style.water)
            chosen['water'], why['water'] = tile, "first of the style's water tiles"

    most(use['roof'], 'roof', 'most used roof lid')
    most(use['roof'], 'roof_alt', 'next roof lid')

    def brick(s):
        r, g, b = s['mean']
        return r > g * 1.35 and r > b * 1.4

    def stone(s):
        r, g, b = s['mean']
        return max(r, g, b) - min(r, g, b) < 28 and (r + g + b) / 3 > 105

    most(use['wall'], 'wall_brick', 'most used reddish wall', brick)
    most(use['wall'], 'wall_brick_alt', 'next reddish wall', brick)
    most(use['wall'], 'wall_stone', 'most used grey wall', stone)
    most(use['wall'], 'wall_commercial', 'most used other wall',
         lambda s: not brick(s) and not stone(s))

    # Anything a class could not fill takes the most used wall left.
    for key in ('wall_brick', 'wall_brick_alt', 'wall_stone', 'wall_commercial'):
        if key not in chosen:
            most(use['wall'], key, 'most used wall left (no better match)')
    return chosen, why


def contact_sheet(style, chosen, path, columns=32, cell=40):
    from PIL import Image, ImageDraw
    rows = (style.count + columns - 1) // columns
    sheet = Image.new('RGB', (columns * cell, rows * cell), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    labels = collections.defaultdict(list)
    for key, tile in chosen.items():
        labels[tile].append(key)
    for i in range(style.count):
        x, y = (i % columns) * cell, (i // columns) * cell
        img = Image.fromarray(style.tile(i), 'RGBA').resize((cell - 8, cell - 8))
        sheet.paste(img, (x + 4, y + 2), img)
        draw.text((x + 2, y + cell - 11), str(i), (255, 255, 255))
        if i in labels:
            draw.rectangle([x, y, x + cell - 1, y + cell - 1], outline=(255, 0, 255), width=2)
    sheet.save(path)


def find_data(folder, district):
    for candidate in (os.path.join(folder, 'data'), folder):
        gmp_path = os.path.join(candidate, district + '.gmp')
        sty_path = os.path.join(candidate, district + '.sty')
        if os.path.exists(gmp_path) and os.path.exists(sty_path):
            return gmp_path, sty_path
    sys.exit('no %s.gmp and %s.sty in %s or its data folder' % (district, district, folder))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('game', help='GTA2 folder, or its data folder')
    ap.add_argument('--district', default='ste', help='wil, ste or bil (default ste)')
    ap.add_argument('--out', default='palette.json')
    ap.add_argument('--sheet', help='write a numbered picture of every tile here')
    args = ap.parse_args(argv)

    gmp_path, sty_path = find_data(args.game, args.district)
    style = Style(sty_path)
    columns, blocks = gmp.read_dmap(gmp_path)
    use, ground = surface_counts(columns, blocks)
    chosen, why = pick(use, style)

    missing = [k for k in ('road', 'sidewalk', 'alley', 'plaza', 'grass', 'water', 'roof',
                           'roof_alt', 'wall_brick', 'wall_brick_alt', 'wall_stone',
                           'wall_commercial') if k not in chosen]
    if missing:
        print('could not choose: %s. Fill them in by hand from the sheet.' % ', '.join(missing))
    with open(args.out, 'w') as f:
        json.dump(dict(style=args.district + '.sty', source=os.path.basename(gmp_path),
                       street_level=ground, tiles=chosen, why=why), f, indent=2)
    for key, tile in chosen.items():
        print('  %-16s %4d  %s' % (key, tile, why[key]))
    print('wrote %s' % args.out)
    if args.sheet:
        contact_sheet(style, chosen, args.sheet)
        print('wrote %s' % args.sheet)
    return 0 if not missing else 1


if __name__ == '__main__':
    sys.exit(main())
