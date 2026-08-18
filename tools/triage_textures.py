"""Look at what the renderer classified, and at what it did not.

The effect-sprite classification (see dll/src/texture_store.cpp) is a threshold on
a measurement, and a threshold is only as good as the cases it was set from. The
renderer writes gta2dx9_textures.csv and a folder of TGAs beside the game so those
cases can be looked at rather than argued about.

    python triage_textures.py "<game folder>" [--all] [--out sheet.png]

By default it shows only the frames the *world-space sprite pass* drew - a road
tile with a hole in it is not the thing being judged - sorted by how close they
came to qualifying, so a fireball that was missed is at the top of the sheet
rather than buried. Each cell is captioned with the two numbers the decision is
made on: the median luminance of the opaque texels touching the hole, and the
brightest texel anywhere in the sprite.

    edge < 16 and peak > 96      the shipped thresholds

The contact sheet needs Pillow; without it the table still prints.
"""

import argparse
import csv
import os
import struct
import sys


def read_report(folder):
    path = os.path.join(folder, "gta2dx9_textures.csv")
    if not os.path.isfile(path):
        sys.exit(f"no report at {path} -- run the game, then quit it cleanly")
    rows = []
    with open(path, newline="", encoding="utf-8", errors="replace") as handle:
        # The file opens with a few ';' comment lines before the header.
        lines = [l for l in handle if not l.startswith(";")]
    for row in csv.DictReader(lines):
        try:
            rows.append({
                "key": row["key"],
                "w": int(row["width"]),
                "h": int(row["height"]),
                "sprite": row["sprite"] == "1",
                "effect": row["effect"] == "1",
                "cutout": row["cutout"] == "1",
                "edge": float(row["edge_median"]),
                "peak": float(row["peak"]),
                "dark": float(row["edge_dark_fraction"]),
            })
        except (KeyError, ValueError):
            continue
    return rows


def load_tga(path):
    """The renderer writes 32-bit uncompressed BGRA, rows top to bottom."""
    from PIL import Image

    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 18 or data[2] != 2 or data[16] != 32:
        return None
    width, height = struct.unpack_from("<HH", data, 12)
    body = data[18 + data[0]:]
    if len(body) < width * height * 4:
        return None
    image = Image.frombytes("RGBA", (width, height), body[:width * height * 4], "raw", "BGRA")
    if not data[17] & 0x20:
        image = image.transpose(Image.FLIP_TOP_BOTTOM)
    return image


def distance(row, edge_limit=16.0, peak_limit=96.0):
    """How far this frame is from qualifying, 0 when it already does.

    Both conditions have to hold, so the distance is whichever one is further
    out, each scaled by its own threshold so they are comparable.
    """
    if row["effect"]:
        return 0.0
    miss_edge = max(0.0, row["edge"] - edge_limit) / edge_limit
    miss_peak = max(0.0, peak_limit - row["peak"]) / peak_limit
    return max(miss_edge, miss_peak)


def sheet(folder, rows, out, cell=96, columns=12):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("(Pillow not installed, skipping the contact sheet)")
        return
    directory = os.path.join(folder, "gta2dx9_textures")
    cells = []
    for row in rows:
        image = None
        path = os.path.join(directory, row["key"] + ".tga")
        if os.path.isfile(path):
            image = load_tga(path)
        if image is not None:
            cells.append((row, image))
    if not cells:
        print(f"(no TGAs in {directory} -- was dump_textures on?)")
        return

    height = ((len(cells) + columns - 1) // columns) * cell
    canvas = Image.new("RGB", (columns * cell, height), (32, 32, 32))
    draw = ImageDraw.Draw(canvas)
    for index, (row, image) in enumerate(cells):
        scale = max(1, min(cell // max(image.width, image.height), 4))
        thumb = image.convert("RGB").resize(
            (image.width * scale, image.height * scale), Image.NEAREST)
        x, y = (index % columns) * cell, (index // columns) * cell
        canvas.paste(thumb, (x + 2, y + 2))
        colour = (120, 255, 120) if row["effect"] else (255, 220, 80)
        draw.text((x + 2, y + cell - 22), f"e{row['edge']:.0f} p{row['peak']:.0f}", fill=colour)
        draw.text((x + 2, y + cell - 11), row["key"][:8], fill=(150, 150, 150))
    canvas.save(out)
    print(f"contact sheet: {out}  ({len(cells)} frames, green = classified as an effect)")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder")
    parser.add_argument("--all", action="store_true",
                        help="include frames the sprite pass never drew (tiles, HUD)")
    parser.add_argument("--out", default="texture_triage.png")
    parser.add_argument("--edge", type=float, default=16.0)
    parser.add_argument("--peak", type=float, default=96.0)
    args = parser.parse_args(argv)

    rows = read_report(args.folder)
    sprites = [r for r in rows if r["sprite"]]
    print(f"{len(rows)} frame(s); {len(sprites)} drawn as world sprites; "
          f"{sum(1 for r in rows if r['cutout'])} with a cutout; "
          f"{sum(1 for r in rows if r['effect'])} classified as effects")

    shown = rows if args.all else sprites
    shown = [r for r in shown if r["cutout"]]
    shown.sort(key=lambda r: (r["effect"], distance(r, args.edge, args.peak)))

    print()
    print(f"{'key':>16} {'size':>8} {'edge':>7} {'peak':>7} {'dark':>6}  verdict")
    for row in shown[:60]:
        verdict = "EFFECT" if row["effect"] else f"miss by {distance(row, args.edge, args.peak):.2f}"
        print(f"{row['key']:>16} {row['w']:>3}x{row['h']:<4} {row['edge']:>7.1f} "
              f"{row['peak']:>7.1f} {row['dark']:>6.2f}  {verdict}")
    if len(shown) > 60:
        print(f"... and {len(shown) - 60} more")

    print()
    sheet(args.folder, shown, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
