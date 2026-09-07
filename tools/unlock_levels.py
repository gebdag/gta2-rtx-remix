"""Unlock every city and bonus level in a GTA2 player slot.

The renderer has to be judged in all three districts - each one has its own
light palette, and the district heuristics in remix_lights.cpp were drawn from
all of them - so being gated behind finishing City 1 is a testing problem before
it is anything else. This opens the level select without touching anything else
in the save.

The file format, read off gta2.exe rather than guessed:

  player\\plyslot%d.dat is exactly 126 bytes, written by the function at
  0x004a89e0 (from plydat.cpp). It writes an 18-byte header copied from the
  slot record at +0x2730, then loops 3 x 4 over the level array at +0x26a0,
  emitting 9 bytes per level - one flag byte followed by two dwords, packed,
  from an in-memory struct of {byte flag; dword; dword} on a 12-byte stride.

      offset 0x00 .. 0x11    18-byte header
      offset 0x12 + 9*i      level i: +0 flag byte, +1 dword, +5 dword

  18 + 12*9 = 126, which is the whole file.

  The index is city*4 + level, from the setter at 0x004a8a90: it computes
  12 * (0x338 + city*4 + level) as a byte offset into the slot record, and
  12 * 0x338 is 0x26a0, the array base. level 0 is the city's MAIN map and
  levels 1..3 are its three BONUS maps, which is also the order the twelve
  entries appear in data\\test1.seq.

  A freshly created slot has exactly one non-zero byte: the flag for level 0,
  City 1's main map. That is what makes the flag "you may play this".

The two dwords per level are left exactly as they are - one of them holds a
score, and none of this is about the scores.
"""

import argparse
import os
import shutil
import sys

SLOT_SIZE = 126
HEADER = 0x12
RECORD = 9
CITIES = 3
LEVELS_PER_CITY = 4

# The names come from data\test1.seq, which is the list the front end reads.
CITY_NAMES = ["City 1 (Industrial, wil)", "City 2 (Residential, ste)", "City 3 (Downtown, bil)"]
LEVEL_NAMES = ["main map", "bonus 1", "bonus 2", "bonus 3"]


def flag_offset(city, level):
    return HEADER + RECORD * (city * LEVELS_PER_CITY + level)


def read_slot(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) != SLOT_SIZE:
        raise ValueError(f"{path}: expected {SLOT_SIZE} bytes, found {len(data)}")
    return data


def show(path):
    data = read_slot(path)
    print(f"{path}")
    for city in range(CITIES):
        for level in range(LEVELS_PER_CITY):
            off = flag_offset(city, level)
            score = int.from_bytes(data[off + 1:off + 5], "little")
            state = "unlocked" if data[off] else "locked  "
            extra = f"   score {score}" if score else ""
            print(f"  {CITY_NAMES[city]:28s} {LEVEL_NAMES[level]:8s}  {state}{extra}")


def unlock(path, dry_run=False):
    data = read_slot(path)
    changed = []
    for city in range(CITIES):
        for level in range(LEVELS_PER_CITY):
            off = flag_offset(city, level)
            if data[off] != 1:
                changed.append(f"{CITY_NAMES[city]} {LEVEL_NAMES[level]}")
                data[off] = 1
    if not changed:
        print(f"{path}: already fully unlocked")
        return 0
    if dry_run:
        print(f"{path}: would unlock {len(changed)}")
        for c in changed:
            print(f"    {c}")
        return len(changed)

    backup = path + ".locked.bak"
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
        print(f"  kept the original as {os.path.basename(backup)}")
    with open(path, "wb") as f:
        f.write(data)
    print(f"{path}: unlocked {len(changed)}")
    for c in changed:
        print(f"    {c}")
    return len(changed)


def restore(path):
    backup = path + ".locked.bak"
    if not os.path.exists(backup):
        print(f"{path}: no backup to restore from")
        return False
    shutil.copy2(backup, path)
    print(f"{path}: restored from {os.path.basename(backup)}")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", nargs="+", help="game folder(s), the ones holding gta2.exe")
    ap.add_argument("--slot", default="0",
                    help="slot number, or 'all' for every slot present (default 0)")
    ap.add_argument("--show", action="store_true", help="print the current state and stop")
    ap.add_argument("--restore", action="store_true", help="put the saved originals back")
    ap.add_argument("--dry-run", action="store_true", help="say what would change, change nothing")
    args = ap.parse_args()

    slots = range(8) if args.slot == "all" else [int(args.slot)]
    touched = 0
    for game in args.game:
        for slot in slots:
            path = os.path.join(game, "player", f"plyslot{slot}.dat")
            if not os.path.exists(path):
                if args.slot != "all":
                    print(f"{path}: not there")
                continue
            touched += 1
            try:
                if args.show:
                    show(path)
                elif args.restore:
                    restore(path)
                else:
                    unlock(path, args.dry_run)
            except ValueError as e:
                print(f"  {e}")
    if not touched:
        print("nothing to do - no player slots found")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
