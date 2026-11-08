"""Checks for the Plateau map generator. No GTA2 files needed.

    python test_plateau.py

Builds the map with a made-up style, then checks it: the file reads back, every
road cell sends traffic somewhere, the road network is one connected loop
system, buildings stay inside GTA2's eight levels, and pick_tiles.py finds the
right tiles again in a map it knows the answer for. When a C++ compiler is on
the PATH it also loads the map with the renderer's own parser and mesh builder
(tools/dump_block.cpp, audit mode) and checks no block's triangle leaves its cell.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import gmp  # noqa: E402
import make_plateau_map  # noqa: E402
import pick_tiles  # noqa: E402

# Made-up colours for the placeholder tiles 1-12, in PALETTE_KEYS order.
TILE_COLOURS = [
    (60, 60, 64), (170, 166, 160), (128, 118, 106), (190, 178, 150), (70, 140, 60),
    (40, 80, 150), (96, 96, 96), (120, 116, 110), (150, 70, 52), (130, 60, 50),
    (160, 158, 150), (176, 150, 110),
]


def write_style(path):
    """A style with two pages of tiles: tile i is palette i's colour 1, with a
    speckle of colour 2 (a darker shade) so tiles differ in pattern."""
    pages = 2
    tiles = bytearray(pages * 256 * 256)
    for i in range(pages * 16):
        page, slot = divmod(i, 16)
        base = page * 65536 + (slot // 4) * 64 * 256 + (slot % 4) * 64
        for y in range(64):
            for x in range(64):
                speckle = (x * 7 + y * 13 + i) % (5 if i == 1 else 23) == 0
                tiles[base + y * 256 + x] = 2 if speckle else 1
    ppal = bytearray(65536)
    for p in range(64):
        r, g, b = TILE_COLOURS[p - 1] if 1 <= p <= len(TILE_COLOURS) else (200, 0, 200)
        for c, scale in ((1, 1.0), (2, 0.8)):
            at = c * 256 + p * 4
            ppal[at:at + 3] = bytes((int(b * scale), int(g * scale), int(r * scale)))
    palx = struct.pack('<%dH' % (pages * 16), *range(pages * 16))
    spec = struct.pack('<4H', 0, 0, 6, 0)
    with open(path, 'wb') as f:
        f.write(b'GBST' + struct.pack('<H', 700))
        for tag, body in (('TILE', tiles), ('PPAL', ppal), ('PALX', palx),
                          ('PALB', struct.pack('<H', pages * 16)), ('SPEC', spec)):
            f.write(tag.encode() + struct.pack('<I', len(body)) + body)


class PlateauTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='plateau_')
        cls.plateau, cls.writer = make_plateau_map.main(
            [cls.dir, '--nav-zones', '--no-preview'])
        cls.gmp_path = os.path.join(cls.dir, 'plateau.gmp')
        cls.columns, cls.blocks = gmp.read_dmap(cls.gmp_path)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir)

    def top_block(self, x, y):
        offset, indices = self.columns[y][x]
        return offset + len(indices) - 1, self.blocks[indices[-1]] if indices else None

    def test_chunks(self):
        chunks = gmp.read_chunks(self.gmp_path)
        self.assertEqual(set(chunks), {'DMAP', 'ZONE', 'LGHT'})
        self.assertEqual(len(chunks['LGHT']) % 16, 0)
        self.assertGreater(len(chunks['LGHT']) // 16, 500)

    def test_every_cell_has_ground(self):
        for y in range(gmp.MAP_H):
            for x in range(gmp.MAP_W):
                offset, indices = self.columns[y][x]
                self.assertTrue(indices, 'empty cell %d,%d' % (x, y))
                self.assertLessEqual(offset + len(indices), gmp.LEVELS)
                top = self.blocks[indices[-1]]
                self.assertNotEqual(top.lid & 0x3FF, 0, 'no lid on top at %d,%d' % (x, y))

    def road_graph(self):
        roads = {}
        for y in range(gmp.MAP_H):
            for x in range(gmp.MAP_W):
                offset, indices = self.columns[y][x]
                b = self.blocks[indices[0]]
                if offset == make_plateau_map.GROUND_LEVEL and b.ground == gmp.ROAD:
                    roads[(x, y)] = b.arrows
        return roads

    def test_arrows_stay_on_the_road(self):
        roads = self.road_graph()
        self.assertGreater(len(roads), 5000)
        for (x, y), arrows in roads.items():
            self.assertTrue(arrows & 0x0F, 'road cell %d,%d has no arrow' % (x, y))
            for bit, (dx, dy) in gmp.ARROW_STEP.items():
                if arrows & bit:
                    self.assertIn((x + dx, y + dy), roads,
                                  'arrow at %d,%d leaves the road' % (x, y))

    def test_road_network_is_connected(self):
        """Every road cell can reach every other one by following arrows."""
        roads = self.road_graph()
        succ = {c: [(c[0] + dx, c[1] + dy) for bit, (dx, dy) in gmp.ARROW_STEP.items()
                    if a & bit] for c, a in roads.items()}
        pred = {c: [] for c in roads}
        for c, nexts in succ.items():
            for n in nexts:
                pred[n].append(c)

        def reach(start, edges):
            seen, stack = {start}, [start]
            while stack:
                for n in edges[stack.pop()]:
                    if n not in seen:
                        seen.add(n)
                        stack.append(n)
            return seen

        start = next(iter(roads))
        forward, backward = reach(start, succ), reach(start, pred)
        stuck = set(roads) - (forward & backward)
        self.assertFalse(stuck, '%d road cells are not in the main network, e.g. %s'
                         % (len(stuck), sorted(stuck)[:5]))

    def test_buildings_have_walls(self):
        walls = 0
        for y in range(1, gmp.MAP_H - 1):
            for x in range(1, gmp.MAP_W - 1):
                level, top = self.top_block(x, y)
                if level <= make_plateau_map.GROUND_LEVEL:
                    continue
                self.assertLessEqual(level, 7)
                left_level, _ = self.top_block(x - 1, y)
                if left_level < level:
                    self.assertTrue(top.left & gmp.WALL, 'open wall at %d,%d' % (x, y))
                    walls += 1
        self.assertGreater(walls, 1000)

    def test_pick_tiles_finds_the_placeholders(self):
        data = os.path.join(self.dir, 'data')
        os.makedirs(data, exist_ok=True)
        shutil.copy(self.gmp_path, os.path.join(data, 'ste.gmp'))
        write_style(os.path.join(data, 'ste.sty'))
        out = os.path.join(self.dir, 'palette.json')
        pick_tiles.main([self.dir, '--out', out])
        with open(out) as f:
            tiles = json.load(f)['tiles']
        expected = make_plateau_map.PLACEHOLDER_PALETTE
        for key in ('road', 'sidewalk', 'grass', 'water', 'roof', 'wall_brick', 'wall_stone',
                    'wall_commercial'):
            self.assertEqual(tiles.get(key), expected[key], key)
        # And the map builds with that palette.
        make_plateau_map.main([os.path.join(self.dir, 'second'), '--palette', out,
                               '--no-preview'])

    def test_renderer_parser_and_mesh(self):
        """Loads the map with src/gta2_map.cpp and builds its mesh with
        src/world_mesh.cpp, through tools/dump_block.cpp's audit."""
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('no C++ compiler')
        exe = os.path.join(self.dir, 'dump_block')
        sources = [os.path.join(ROOT, 'tools', 'dump_block.cpp')] + [
            os.path.join(ROOT, 'src', f) for f in ('gta2_map.cpp', 'gta2_style.cpp', 'world_mesh.cpp')]
        subprocess.run([compiler, '-std=c++17', '-O1', '-include', 'algorithm', '-o', exe] + sources,
                       check=True)
        style = os.path.join(self.dir, 'test.sty')
        write_style(style)
        result = subprocess.run([exe, 'audit', self.gmp_path, style], capture_output=True,
                                text=True)
        print(result.stdout.strip())
        # The only wide triangles allowed are the two of the seal, the quad
        # world_mesh.cpp lays below level 0 past every edge of any map.
        self.assertIn(' 0 degenerate', result.stdout)
        oversized = [l for l in result.stdout.splitlines() if 'oversized:' in l]
        self.assertLessEqual(len(oversized), 2, result.stdout)
        for line in oversized:
            ys = line.split('(')[1:]
            self.assertTrue(all(p.split()[1] == '-2.00' for p in ys), line)


if __name__ == '__main__':
    unittest.main(verbosity=2)
