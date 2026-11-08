"""Builds a GTA2 map of Montreal's Plateau-Mont-Royal.

    python make_plateau_map.py <output folder> [--palette palette.json]
                               [--style ste.sty] [--nav-zones] [--no-lights]

Writes, into the output folder:

    plateau.gmp          the map (DMAP, LGHT and, with --nav-zones, ZONE)
    plateau.mmp          a level list entry so the game and the renderer know
                         which style the map is played with
    plateau.mis          a starting mission script for the GTA2 script compiler
    plateau_preview.png  a top-down picture of the map, with street names
    plateau_report.txt   what was built, and where the player starts

The streets, parks and landmarks come from plateau_layout.py. Which of the
style's tiles each surface uses comes from --palette, a JSON file that
pick_tiles.py writes from your own copy of GTA2. Without it the map uses
placeholder tile numbers: it is still a valid map, but the textures are wrong.

Needs Python 3.8+ and Pillow for the preview (the map itself needs only the
standard library).
"""
import argparse
import json
import math
import os
import sys
import zlib

import gmp
import plateau_layout as layout

# Cell materials.
LOT, ROAD, SIDEWALK, PARK, WATER, PLAZA, ALLEY, YARD, BUILDING = range(9)
MATERIAL_NAMES = ['lot', 'road', 'sidewalk', 'park', 'water', 'plaza', 'alley', 'yard', 'building']

GROUND_LEVEL = 1          # the street surface is the lid of the block at this level
MAX_LEVEL = gmp.LEVELS - 1

# Ring road centre lines land on these cells; two cells of grass lie outside.
EDGE_LO, EDGE_HI = 4.0, 252.0

PALETTE_KEYS = ['road', 'sidewalk', 'alley', 'plaza', 'grass', 'water', 'roof', 'roof_alt',
                'wall_brick', 'wall_brick_alt', 'wall_stone', 'wall_commercial']

# Used when no palette is given. Valid tile numbers, wrong pictures.
PLACEHOLDER_PALETTE = {key: i + 1 for i, key in enumerate(PALETTE_KEYS)}

# Montreal's LED street lamps are 3000 K.
LAMP_RGB = (255, 180, 107)

# The renderer finds which .gmp the game has loaded by comparing the first 4000
# block records of each map in the data folder with the game's own block array
# (kMatchBlocks in dll/src/world_view.cpp). Renderer builds before that check
# learned the live map's length read 4000 records whatever the map held, past
# the end of a small map's array. Padding the block list to 4000 keeps those
# builds inside the array.
MIN_BLOCKS = 4000


def stable_hash(*parts):
    return zlib.crc32(repr(parts).encode('utf-8'))


class Grid:
    """Metres on Montreal's street grid to map cells."""

    def __init__(self):
        self.sx = (layout.EAST - layout.WEST) / (EDGE_HI - EDGE_LO)
        self.sy = (layout.NORTH - layout.SOUTH) / (EDGE_HI - EDGE_LO)

    def x(self, metres):
        return EDGE_LO + (metres - layout.WEST) / self.sx

    def y(self, metres):
        # Montreal north is up the screen, which is y - 1 in GTA2.
        return EDGE_LO + (layout.NORTH - metres) / self.sy

    def rect(self, west, south, east, north):
        """Cells whose centres fall inside the rectangle: x0, y0, x1, y1 inclusive."""
        x0 = math.ceil(self.x(west) - 0.5)
        x1 = math.floor(self.x(east) - 0.5)
        y0 = math.ceil(self.y(north) - 0.5)
        y1 = math.floor(self.y(south) - 0.5)
        return (max(0, x0), max(0, y0), min(255, x1), min(255, y1))


def lane_cells(centre, lanes, median):
    """First lane cell and the list of cells across a street, west/north first.
    With a median the middle cell is returned as None."""
    width = lanes + (1 if median else 0)
    first = int(round(centre - width / 2.0))
    cells = list(range(first, first + width))
    if median:
        cells[lanes // 2] = None
    return cells


def lane_arrows(street, i):
    """Arrows for lane i (0 = westmost or northmost) of a street."""
    d = street['dir']
    n = street['lanes']
    if street['axis'] == 'ns':
        if d == 'n':
            return gmp.UP
        if d == 's':
            return gmp.DOWN
        # Two-way, driving on the right: west half southbound, east half northbound.
        return gmp.DOWN if i < n // 2 else gmp.UP
    if d == 'e':
        return gmp.RIGHT
    if d == 'w':
        return gmp.LEFT
    # North half westbound, south half eastbound.
    return gmp.LEFT if i < n // 2 else gmp.RIGHT


class Plateau:
    def __init__(self):
        self.grid = Grid()
        n = gmp.MAP_W * gmp.MAP_H
        self.mat = [LOT] * n
        self.arrows = [0] * n
        self.street_of = [-1] * n     # street index of a road or sidewalk cell
        self.height = [0] * n         # storeys above the street, for buildings
        self.wall = [None] * n        # palette key of a building's walls
        self.roof = [None] * n
        self.labels = {}              # preview label per building cell group
        self.streets = layout.streets()
        self.by_name = {s['name']: i for i, s in enumerate(self.streets)}
        self.lights = []
        self.medians = []

    # --- helpers ----------------------------------------------------------

    def at(self, x, y):
        return y * gmp.MAP_W + x

    def inside(self, x, y):
        return 0 <= x < gmp.MAP_W and 0 <= y < gmp.MAP_H

    def cross_cells(self, street):
        """The cells across a street: lanes (None for a median) and sidewalks."""
        centre = self.grid.x(street['pos']) if street['axis'] == 'ns' else self.grid.y(street['pos'])
        lanes = lane_cells(centre, street['lanes'], street['median'])
        real = [c for c in lanes if c is not None]
        return lanes, min(real) - 1, max(real) + 1

    def extent(self, street_name):
        """The span of cells a street's road covers across its own axis."""
        if street_name not in self.by_name:
            raise KeyError('unknown street in spans: %s' % street_name)
        lanes, _, _ = self.cross_cells(self.streets[self.by_name[street_name]])
        real = [c for c in lanes if c is not None]
        return min(real), max(real)

    def spans(self, street):
        """(lo, hi) cell ranges along a street's own axis, ends inclusive."""
        if street['spans'] is None:
            if street['axis'] == 'ns':
                a, b = 'Ring road (south)', 'Ring road (north)'
            else:
                a, b = 'Ring road (west)', 'Ring road (east)'
            pairs = [(a, b)]
        else:
            pairs = street['spans']
        out = []
        for a, b in pairs:
            ea, eb = self.extent(a), self.extent(b)
            out.append((min(ea[0], eb[0]), max(ea[1], eb[1])))
        return out

    def cell(self, street, across, along):
        return (across, along) if street['axis'] == 'ns' else (along, across)

    # --- painting ---------------------------------------------------------

    def paint_border(self):
        for y in range(gmp.MAP_H):
            for x in range(gmp.MAP_W):
                if x < 2 or y < 2 or x > 253 or y > 253:
                    self.mat[self.at(x, y)] = PARK

    def paint_sidewalks(self):
        for si, street in enumerate(self.streets):
            _, west, east = self.cross_cells(street)
            for lo, hi in self.spans(street):
                for along in range(lo - 1, hi + 2):
                    for across in (west, east):
                        x, y = self.cell(street, across, along)
                        if self.inside(x, y) and self.mat[self.at(x, y)] in (LOT, PARK):
                            self.mat[self.at(x, y)] = SIDEWALK
                            self.street_of[self.at(x, y)] = si

    def paint_rects(self, items, material, over):
        for item in items:
            x0, y0, x1, y1 = self.grid.rect(*item['rect'])
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    if self.mat[self.at(x, y)] in over:
                        self.mat[self.at(x, y)] = material

    def paint_parks(self):
        self.paint_rects(layout.PARKS, PARK, (LOT,))
        for park in layout.PARKS:
            x0, y0, x1, y1 = self.grid.rect(*park['rect'])
            for axis, pos in park.get('paths', []):
                if axis == 'ew':
                    y = int(round(self.grid.y(pos) - 0.5))
                    cells = [(x, y) for x in range(x0, x1 + 1)]
                else:
                    x = int(round(self.grid.x(pos) - 0.5))
                    cells = [(x, y) for y in range(y0, y1 + 1)]
                for x, y in cells:
                    if self.mat[self.at(x, y)] == PARK:
                        self.mat[self.at(x, y)] = PLAZA

    def paint_roads(self):
        for si, street in enumerate(self.streets):
            lanes, _, _ = self.cross_cells(street)
            median = lanes[lanes.index(None) - 1] + 1 if street['median'] else None
            for lo, hi in self.spans(street):
                for along in range(lo, hi + 1):
                    for i, across in enumerate(c for c in lanes if c is not None):
                        x, y = self.cell(street, across, along)
                        k = self.at(x, y)
                        if self.mat[k] != ROAD:
                            self.street_of[k] = si
                        self.mat[k] = ROAD
                        self.arrows[k] |= lane_arrows(street, i)
                    if median is not None:
                        self.medians.append(self.cell(street, median, along))
        for x, y in self.medians:
            if self.mat[self.at(x, y)] != ROAD:
                self.mat[self.at(x, y)] = PARK

    def prune_arrows(self):
        """Drops every arrow that leads off the road, so no car is sent into a
        kerb. A T-junction gets its turns this way: the side street's lanes are
        drawn across the whole main road, and the arrows that would carry on
        into the far pavement are removed here."""
        changed = True
        while changed:
            changed = False
            for y in range(gmp.MAP_H):
                for x in range(gmp.MAP_W):
                    k = self.at(x, y)
                    if self.mat[k] != ROAD:
                        continue
                    for bit, (dx, dy) in gmp.ARROW_STEP.items():
                        if not self.arrows[k] & bit:
                            continue
                        nx, ny = x + dx, y + dy
                        if not self.inside(nx, ny) or self.mat[self.at(nx, ny)] != ROAD:
                            self.arrows[k] &= ~bit
                            changed = True

    def paint_water_and_plazas(self):
        self.paint_rects(layout.PLAZAS, PLAZA, (LOT, PARK, SIDEWALK))
        self.paint_rects(layout.WATER, WATER, (LOT, PARK, PLAZA))

    def paint_landmarks(self):
        for mark in layout.LANDMARKS:
            x0, y0, x1, y1 = self.grid.rect(*mark['rect'])
            tower = self.grid.rect(*mark['tower']) if 'tower' in mark else None
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    k = self.at(x, y)
                    # A landmark stands behind the sidewalk, never on it.
                    if self.mat[k] in (ROAD, WATER, SIDEWALK):
                        continue
                    levels = mark['levels']
                    if tower and tower[0] <= x <= tower[2] and tower[1] <= y <= tower[3]:
                        levels = mark['tower_levels']
                    self.mat[k] = BUILDING
                    self.height[k] = levels
                    self.wall[k] = mark['wall']
                    self.roof[k] = 'roof'
            self.labels[mark['name']] = ((x0 + x1) / 2.0, (y0 + y1) / 2.0)

    def runs(self, x, y):
        """LOT cells beyond (x, y) to the west, east, north and south."""
        out = []
        for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            n, cx, cy = 0, x + dx, y + dy
            while self.inside(cx, cy) and self.mat[self.at(cx, cy)] == LOT:
                n += 1
                cx += dx
                cy += dy
            out.append(n)
        return out

    def frontage(self, x, y, runs):
        """The street a building cell faces: the one past its nearest lot edge."""
        west, east, north, south = runs
        options = [(west, (-west - 1, 0)), (east, (east + 1, 0)),
                   (north, (0, -north - 1)), (south, (0, south + 1))]
        options.sort(key=lambda o: o[0])
        for _, (dx, dy) in options:
            nx, ny = x + dx, y + dy
            if self.inside(nx, ny) and self.street_of[self.at(nx, ny)] >= 0:
                return self.streets[self.street_of[self.at(nx, ny)]], (dx, dy)
        return None, options[0][1]

    def fill_lots(self):
        """Rows of plexes on every block, with a lane (ruelle) down the middle
        of the long ones and yards behind the houses where a block is deep."""
        plan = {}
        for y in range(gmp.MAP_H):
            for x in range(gmp.MAP_W):
                if self.mat[self.at(x, y)] != LOT:
                    continue
                west, east, north, south = runs = self.runs(x, y)
                w, h = west + east + 1, north + south + 1
                kind = BUILDING
                if min(w, h) >= 5:
                    if h >= w:   # blocks are long north-south: the lane runs that way
                        alley = west == (w - 1) // 2
                        near_end = min(north, south) < 2
                        side = min(west, east)
                    else:
                        alley = north == (h - 1) // 2
                        near_end = min(west, east) < 2
                        side = min(north, south)
                    # Two cells of house on each side, three on a deep block.
                    depth = 3 if min(w, h) >= 9 else 2
                    if alley:
                        kind = ALLEY
                    elif not near_end and side >= depth:
                        kind = YARD
                plan[(x, y)] = (kind, runs)

        for (x, y), (kind, runs) in plan.items():
            k = self.at(x, y)
            self.mat[k] = kind
            if kind != BUILDING:
                continue
            street, (dx, dy) = self.frontage(x, y, runs)
            # Houses in a row share a height in lots three cells wide.
            along = y if dx else x
            lot = stable_hash(street['name'] if street else 'none', dx, dy, along // 3)
            kind_name = street['kind'] if street else 'residential'
            if kind_name in ('commercial', 'boulevard'):
                self.height[k] = 3 + lot % 2
                self.wall[k] = 'wall_commercial' if lot % 4 else 'wall_brick'
            else:
                self.height[k] = 2 + (lot >> 3) % 2
                self.wall[k] = ('wall_brick', 'wall_brick_alt', 'wall_brick', 'wall_stone',
                                'wall_brick_alt')[(lot >> 5) % 5]
            self.roof[k] = 'roof_alt' if (lot >> 9) % 3 == 0 else 'roof'

    def place_lights(self):
        """Lamps along the sidewalks of every street but the ring road, and
        along the park paths."""
        def lamp(x, y, intensity=170, radius=3.5):
            self.lights.append(dict(rgb=LAMP_RGB, x=x + 0.5, y=y + 0.5,
                                    z=GROUND_LEVEL + 1 + 1.3, radius=radius,
                                    intensity=intensity))

        for street in self.streets:
            if street['kind'] == 'ring':
                continue
            _, west, east = self.cross_cells(street)
            step = 4 if street['kind'] in ('commercial', 'boulevard') else 6
            for lo, hi in self.spans(street):
                for along in range(lo, hi + 1, step):
                    for j, across in enumerate((west, east)):
                        if street['kind'] == 'residential' and (along // step + j) % 2:
                            continue   # residential lamps alternate sides
                        x, y = self.cell(street, across, along)
                        if self.inside(x, y) and self.mat[self.at(x, y)] == SIDEWALK:
                            lamp(x, y)
        for park in layout.PARKS:
            x0, y0, x1, y1 = self.grid.rect(*park['rect'])
            for y in range(y0, y1 + 1, 5):
                for x in range(x0, x1 + 1, 5):
                    if self.mat[self.at(x, y)] == PLAZA:
                        lamp(x, y, intensity=140, radius=3.0)

    def build(self):
        self.paint_border()
        self.paint_sidewalks()
        self.paint_parks()
        self.paint_water_and_plazas()
        self.paint_roads()
        self.prune_arrows()
        self.paint_landmarks()
        self.fill_lots()
        self.place_lights()

    # --- output -----------------------------------------------------------

    def top_level(self, x, y):
        """Level of the topmost solid block in a cell, or -1 off the map."""
        if not self.inside(x, y):
            return -1
        k = self.at(x, y)
        if self.mat[k] == WATER:
            return 0
        if self.mat[k] == BUILDING:
            return min(MAX_LEVEL, GROUND_LEVEL + self.height[k])
        return GROUND_LEVEL

    def to_map(self, palette):
        writer = gmp.MapWriter()
        lid_of = {ROAD: ('road', gmp.ROAD), SIDEWALK: ('sidewalk', gmp.PAVEMENT),
                  PARK: ('grass', gmp.FIELD), YARD: ('grass', gmp.FIELD),
                  PLAZA: ('plaza', gmp.PAVEMENT), ALLEY: ('alley', gmp.PAVEMENT)}
        sides = ((-1, 0), (1, 0), (0, -1), (0, 1))   # left, right, top, bottom
        for y in range(gmp.MAP_H):
            for x in range(gmp.MAP_W):
                k = self.at(x, y)
                m = self.mat[k]
                if m == WATER:
                    lid = gmp.face(palette['water'])
                    writer.set_column(x, y, 0, [gmp.block(lid=lid, ground=gmp.AIR)])
                    continue

                # The street-level block. Its sides show only where the cell
                # beside it is water, as the edge of a pond.
                faces = []
                for dx, dy in sides:
                    low = self.top_level(x + dx, y + dy) == 0
                    faces.append(gmp.face(palette['wall_stone']) if low else 0)
                if m == BUILDING:
                    ground = gmp.block(*faces, ground=gmp.PAVEMENT)
                else:
                    key, ground_type = lid_of[m]
                    ground = gmp.block(*faces, lid=gmp.face(palette[key]),
                                       arrows=self.arrows[k], ground=ground_type)
                records = [ground]

                if m == BUILDING:
                    top = self.top_level(x, y)
                    wall_tile = palette[self.wall[k]]
                    for level in range(GROUND_LEVEL + 1, top + 1):
                        walls = []
                        for dx, dy in sides:
                            exposed = self.top_level(x + dx, y + dy) < level
                            walls.append(gmp.face(wall_tile, wall=True) if exposed else 0)
                        lid = gmp.face(palette[self.roof[k]]) if level == top else 0
                        records.append(gmp.block(*walls, lid=lid, ground=gmp.PAVEMENT))
                writer.set_column(x, y, GROUND_LEVEL, records)
        return writer

    def zones(self):
        """The first zone in the layout is the main one, covering the map; the
        rest are local areas inside it."""
        out = []
        for i, (name, west, south, east, north) in enumerate(layout.ZONES):
            x0, y0, x1, y1 = self.grid.rect(west, south, east, north)
            if x0 <= x1 and y0 <= y1:
                kind = gmp.ZONE_NAVIGATION if i == 0 else gmp.ZONE_LOCAL_NAVIGATION
                out.append((kind, x0, y0, x1 - x0 + 1, y1 - y0 + 1, name))
        return out

    def spawn(self):
        """The north-east corner of Saint-Denis and Mont-Royal."""
        denis = self.streets[self.by_name['Rue Saint-Denis']]
        royal = self.streets[self.by_name['Avenue du Mont-Royal']]
        _, _, x = self.cross_cells(denis)
        _, y, _ = self.cross_cells(royal)
        return x, y


def write_mmp(path, style):
    with open(path, 'w', newline='\r\n') as f:
        f.write('[MapFiles]\n')
        f.write('Description = Plateau-Mont-Royal, Montreal\n')
        f.write('GMPFile = plateau.gmp\n')
        f.write('STYFile = %s\n' % style)
        f.write('SCRFile = plateau.scr\n')


def write_mission(path, spawn):
    x, y = spawn
    with open(path, 'w', newline='\r\n') as f:
        f.write('// Plateau-Mont-Royal: free roam.\n')
        f.write('// A starting point for GTA2\'s mission compiler. Untested: check it\n')
        f.write('// against the compiler\'s own sample scripts before you build it.\n')
        f.write('// The player starts on the corner of Saint-Denis and Mont-Royal.\n\n')
        f.write('PLAYER_PED p1 = (%d.5, %d.5, %d.0) 25 0\n\n' % (x, y, GROUND_LEVEL + 1))
        f.write('LEVELSTART\n\nLEVELEND\n')


def write_preview(path, plateau, scale=4):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print('Pillow is not installed: skipping the preview')
        return False
    colours = {
        LOT: (90, 90, 90), ROAD: (52, 54, 58), SIDEWALK: (165, 160, 152), PARK: (88, 140, 70),
        WATER: (52, 104, 160), PLAZA: (196, 182, 150), ALLEY: (130, 118, 104),
        YARD: (110, 150, 86),
    }
    walls = {'wall_brick': (150, 74, 56), 'wall_brick_alt': (126, 64, 52),
             'wall_stone': (150, 146, 136), 'wall_commercial': (176, 142, 104)}
    img = Image.new('RGB', (gmp.MAP_W * scale, gmp.MAP_H * scale))
    draw = ImageDraw.Draw(img)
    for y in range(gmp.MAP_H):
        for x in range(gmp.MAP_W):
            k = plateau.at(x, y)
            m = plateau.mat[k]
            if m == BUILDING:
                base = walls.get(plateau.wall[k], (120, 120, 120))
                shade = 0.65 + 0.07 * plateau.height[k]
                c = tuple(min(255, int(v * shade)) for v in base)
            else:
                c = colours[m]
            draw.rectangle([x * scale, y * scale, x * scale + scale - 1, y * scale + scale - 1], c)
            if m == ROAD and scale >= 4:
                cx, cy = x * scale + scale // 2, y * scale + scale // 2
                for bit, (dx, dy) in gmp.ARROW_STEP.items():
                    if plateau.arrows[k] & bit:
                        draw.line([cx, cy, cx + dx * (scale // 2), cy + dy * (scale // 2)],
                                  (230, 220, 90))
    for light in plateau.lights:
        cx, cy = light['x'] * scale, light['y'] * scale
        draw.ellipse([cx - 1, cy - 1, cx + 1, cy + 1], (255, 200, 120))
    for street in plateau.streets:
        if street['kind'] == 'ring':
            continue
        lanes, _, _ = plateau.cross_cells(street)
        real = [c for c in lanes if c is not None]
        lo, hi = plateau.spans(street)[0]
        # Stagger labels so neighbouring streets do not overprint.
        mid = lo + (hi - lo) * (1 + stable_hash(street['name']) % 6) // 8
        if street['axis'] == 'ns':
            text = Image.new('RGBA', (len(street['name']) * 6 + 4, 11), (0, 0, 0, 160))
            ImageDraw.Draw(text).text((2, 0), street['name'], (255, 255, 255))
            text = text.rotate(90, expand=True)
            img.paste(text, (int(real[0] * scale), int(mid * scale)), text)
        else:
            draw.rectangle([mid * scale, real[0] * scale - 1,
                            mid * scale + 4 + len(street['name']) * 6, real[0] * scale + 10],
                           (0, 0, 0))
            draw.text((mid * scale + 2, real[0] * scale - 1), street['name'], (255, 255, 255))
    for name, (cx, cy) in plateau.labels.items():
        draw.text((cx * scale + 6, cy * scale - 4), name, (255, 240, 200))
    for park in layout.PARKS:
        x0, y0, x1, y1 = plateau.grid.rect(*park['rect'])
        draw.text((x0 * scale + 6, y0 * scale + 6), park['name'], (230, 255, 220))
    sx, sy = plateau.spawn()
    draw.rectangle([sx * scale - 3, sy * scale - 3, sx * scale + scale + 2, sy * scale + scale + 2],
                   outline=(255, 0, 255), width=2)
    img.save(path)
    return True


def load_palette(path):
    if not path:
        print('warning: no --palette given, so every surface uses a placeholder tile.')
        print('         Run pick_tiles.py on your GTA2 folder to make one.')
        return dict(PLACEHOLDER_PALETTE), None
    with open(path) as f:
        data = json.load(f)
    if not isinstance(data, dict):
        sys.exit('palette %s: expected a JSON object' % path)
    tiles = data.get('tiles', data)
    if not isinstance(tiles, dict):
        sys.exit('palette %s: "tiles" must be a JSON object' % path)
    missing = [k for k in PALETTE_KEYS if k not in tiles]
    if missing:
        sys.exit('palette %s has no tile for: %s' % (path, ', '.join(missing)))
    palette = {}
    for k in PALETTE_KEYS:
        try:
            palette[k] = int(tiles[k])
        except (TypeError, ValueError):
            sys.exit('palette %s: tile for %s is not a number: %r' % (path, k, tiles[k]))
    bad = [k for k, v in palette.items() if not 0 < v < 1024]
    if bad:
        sys.exit('palette %s: tile numbers must be 1-1023: %s' % (path, ', '.join(bad)))
    return palette, data.get('style')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out', help='output folder')
    ap.add_argument('--palette', help='palette.json from pick_tiles.py')
    ap.add_argument('--style', help='style file the map is played with (default: the '
                    "palette's, else ste.sty)")
    ap.add_argument('--nav-zones', action='store_true',
                    help='write navigation zones (named areas); see the README first')
    ap.add_argument('--no-lights', action='store_true', help='leave out the street lamps')
    ap.add_argument('--no-preview', action='store_true')
    args = ap.parse_args(argv)

    palette, palette_style = load_palette(args.palette)
    style = args.style or palette_style or 'ste.sty'
    os.makedirs(args.out, exist_ok=True)

    plateau = Plateau()
    plateau.build()
    writer = plateau.to_map(palette)

    chunks = [('DMAP', writer.dmap(MIN_BLOCKS))]
    if args.nav_zones:
        chunks.append(('ZONE', gmp.zone_chunk(plateau.zones())))
    if not args.no_lights:
        chunks.append(('LGHT', gmp.light_chunk(plateau.lights)))
    gmp.write_gmp(os.path.join(args.out, 'plateau.gmp'), chunks)
    write_mmp(os.path.join(args.out, 'plateau.mmp'), style)
    spawn = plateau.spawn()
    write_mission(os.path.join(args.out, 'plateau.mis'), spawn)
    if not args.no_preview:
        write_preview(os.path.join(args.out, 'plateau_preview.png'), plateau)

    counts = {name: plateau.mat.count(i) for i, name in enumerate(MATERIAL_NAMES)}
    with open(os.path.join(args.out, 'plateau_report.txt'), 'w') as f:
        f.write('Plateau-Mont-Royal for GTA2\n\n')
        f.write('style: %s\n' % style)
        f.write('scale: %.1f m per block east-west, %.1f m north-south\n'
                % (plateau.grid.sx, plateau.grid.sy))
        f.write('blocks: %d distinct\n' % len(writer.blocks))
        f.write('lights: %d\n' % (0 if args.no_lights else len(plateau.lights)))
        f.write('player start: cell %d, %d (Saint-Denis at Mont-Royal)\n' % spawn)
        f.write('\ncells by surface:\n')
        for name, n in counts.items():
            f.write('  %-9s %6d\n' % (name, n))
        f.write('\npalette:\n')
        for k in PALETTE_KEYS:
            f.write('  %-16s tile %d\n' % (k, palette[k]))
    print('wrote %s: %d blocks, %d lights, start at %d,%d'
          % (os.path.join(args.out, 'plateau.gmp'), len(writer.blocks),
             0 if args.no_lights else len(plateau.lights), spawn[0], spawn[1]))
    return plateau, writer


if __name__ == '__main__':
    main()
