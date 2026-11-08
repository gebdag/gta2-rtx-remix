"""Read and write GTA2 .gmp map files (GBMP version 500).

Only the chunks this tool needs: DMAP (the map itself), ZONE and LGHT. The DMAP
layout is the one src/gta2_map.cpp reads:

    u32 base[256 * 256]        word index of each cell's column in the pool
    u32 column_words
    u32 columns[column_words]  per column: u8 height, u8 offset, u16 pad, then
                               (height - offset) u32 block indices
    u32 block_count
    block_info blocks[block_count]   12 bytes each

block_info is five u16 faces (left, right, top, bottom, lid), u8 arrows and u8
slope_type. A face word holds the tile in bits 0-9, then for a side face bit 10
"wall" (stops peds and cars) and bit 11 "bullet wall"; bit 12 flat, bit 13 flip
and bits 14-15 the rotation in quarter turns. slope_type keeps the ground type
in bits 0-1 (0 air, 1 road, 2 pavement, 3 field) and the slope in bits 2-7.
arrows: bits 0-3 are the green (normal traffic) arrows left, right, up, down;
bits 4-7 the red ones in the same order. Up is towards y - 1.
"""
import struct

MAP_W = MAP_H = 256
LEVELS = 8

AIR, ROAD, PAVEMENT, FIELD = 0, 1, 2, 3

WALL = 0x0400
BULLET_WALL = 0x0800
FLAT = 0x1000
FLIP = 0x2000

LEFT, RIGHT, UP, DOWN = 1, 2, 4, 8
# The grid step each green arrow points along.
ARROW_STEP = {LEFT: (-1, 0), RIGHT: (1, 0), UP: (0, -1), DOWN: (0, 1)}

ZONE_NAVIGATION = 1


def face(tile, wall=False, rotation=0, flip=False, flat=False):
    if not 0 <= tile < 1024:
        raise ValueError('tile %d out of range' % tile)
    word = tile | ((rotation & 3) << 14)
    if wall:
        word |= WALL | BULLET_WALL
    if flip:
        word |= FLIP
    if flat:
        word |= FLAT
    return word


def block(left=0, right=0, top=0, bottom=0, lid=0, arrows=0, ground=AIR, slope=0):
    return struct.pack('<5HBB', left, right, top, bottom, lid, arrows, ground | (slope << 2))


AIR_BLOCK = block()


class MapWriter:
    """Collects columns of blocks and writes them as a DMAP, sharing every
    identical block and every identical column."""

    def __init__(self):
        self.blocks = [AIR_BLOCK]
        self.block_index = {AIR_BLOCK: 0}
        self.cells = [None] * (MAP_W * MAP_H)  # (offset, (block, ...)) or None

    def add_block(self, record):
        index = self.block_index.get(record)
        if index is None:
            index = len(self.blocks)
            self.blocks.append(record)
            self.block_index[record] = index
        return index

    def set_column(self, x, y, offset, records):
        """records[i] is the block record at level offset + i."""
        if offset < 0 or offset + len(records) > LEVELS:
            raise ValueError('column at %d,%d leaves levels 0-7' % (x, y))
        indices = tuple(self.add_block(r) for r in records)
        self.cells[y * MAP_W + x] = (offset, indices) if indices else None

    def dmap(self):
        pool = []
        where = {}
        base = []
        for cell in self.cells:
            key = cell or (0, ())
            word = where.get(key)
            if word is None:
                word = len(pool)
                where[key] = word
                offset, indices = key
                height = offset + len(indices)
                pool.append(height | (offset << 8))
                pool.extend(indices)
            base.append(word)
        out = bytearray()
        out += struct.pack('<%dI' % len(base), *base)
        out += struct.pack('<I', len(pool))
        out += struct.pack('<%dI' % len(pool), *pool)
        out += struct.pack('<I', len(self.blocks))
        for record in self.blocks:
            out += record
        return bytes(out)


def zone_chunk(zones):
    """zones: (type, x, y, w, h, name) with x, y, w, h in cells."""
    out = bytearray()
    for kind, x, y, w, h, name in zones:
        raw = name.encode('ascii')
        out += struct.pack('<6B', kind, x, y, w, h, len(raw)) + raw
    return bytes(out)


def light_chunk(lights):
    """lights: dicts with rgb (0-255 each), x, y, z, radius in blocks,
    intensity 0-255."""
    out = bytearray()
    for light in lights:
        r, g, b = light['rgb']
        argb = (r << 16) | (g << 8) | b
        fix = [int(round(light[k] * 128)) for k in ('x', 'y', 'z', 'radius')]
        out += struct.pack('<I4H4B', argb, *fix, light['intensity'], 0, 0, 0)
    return bytes(out)


def write_gmp(path, chunks):
    """chunks: list of (tag, bytes)."""
    with open(path, 'wb') as f:
        f.write(b'GBMP' + struct.pack('<H', 500))
        for tag, body in chunks:
            f.write(tag.encode('ascii') + struct.pack('<I', len(body)))
            f.write(body)


def read_chunks(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'GBMP':
        raise ValueError('%s is not a GBMP map' % path)
    chunks = {}
    pos = 6
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4].decode('ascii', 'replace')
        size = struct.unpack_from('<I', data, pos + 4)[0]
        chunks[tag] = data[pos + 8:pos + 8 + size]
        pos += 8 + size
    return chunks


class Block:
    __slots__ = ('left', 'right', 'top', 'bottom', 'lid', 'arrows', 'slope_type')

    def __init__(self, raw):
        (self.left, self.right, self.top, self.bottom, self.lid,
         self.arrows, self.slope_type) = struct.unpack('<5HBB', raw)

    @property
    def ground(self):
        return self.slope_type & 3

    @property
    def slope(self):
        return self.slope_type >> 2


def read_dmap(path):
    """Returns (columns, blocks): columns[y][x] is (offset, [block index, ...])."""
    dmap = read_chunks(path)['DMAP']
    base = struct.unpack_from('<%dI' % (MAP_W * MAP_H), dmap, 0)
    at = MAP_W * MAP_H * 4
    words = struct.unpack_from('<I', dmap, at)[0]
    pool = struct.unpack_from('<%dI' % words, dmap, at + 4)
    at += 4 + words * 4
    count = struct.unpack_from('<I', dmap, at)[0]
    at += 4
    blocks = [Block(dmap[at + i * 12:at + i * 12 + 12]) for i in range(count)]
    columns = []
    for y in range(MAP_H):
        row = []
        for x in range(MAP_W):
            word = base[y * MAP_W + x]
            height, offset = pool[word] & 0xFF, (pool[word] >> 8) & 0xFF
            n = max(0, height - offset)
            row.append((offset, list(pool[word + 1:word + 1 + n])))
        columns.append(row)
    return columns, blocks
