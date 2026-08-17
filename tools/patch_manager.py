"""Teach "gta2 manager.exe" to offer this renderer.

The manager's renderer list is not data - it is three hardcoded comparisons in
code. At 0x8F6F it matches the selected device's polygon DLL and writes the
matching renderer name into the registry:

    "polygon.dll"  -> rendername = "softdll.dll"
    "d3dpoly.dll"  -> rendername = "d3ddll.dll"
    "3dfxpoly.dll" -> rendername = "3dfx.dll"

and at 0x87E8 it adds the "Direct3D" entry to the list, but only when a Direct3D
device was actually enumerated. There is no table to extend and no fourth branch
to fill in, so adding an entry would mean writing new code into the binary.

Repointing the Direct3D entry instead is a two-string edit, and both strings
happen to have room for it:

    0x0439CB8  "Direct3D"    9 bytes used of 12  ->  "RTX Remix"    10 of 12
    0x0439D14  "d3ddll.dll" 11 bytes used of 12  ->  "gta2dx9.dll"  12 of 12

The second one fits exactly, which is why the renderer DLL is named what it is.
Picking "RTX Remix" in the manager now writes rendername=gta2dx9.dll, so the
resolution can be changed without the renderer being swapped out from under it.

The 3dfx and software entries are untouched, so the original renderers are still
reachable. The original manager is kept beside the patched one and
`deploy.bat restore` puts it back.

Usage:  python patch_manager.py "<game folder>" [--restore]
"""

import os
import shutil
import sys

# (file offset, expected bytes, replacement). The replacement must fit in the
# slack up to the next string, so each is checked against its own budget rather
# than just against the string it replaces.
PATCHES = [
    (0x0039CB8, b"Direct3D\0", b"RTX Remix\0", 12, "list entry label"),
    (0x0039D14, b"d3ddll.dll\0", b"gta2dx9.dll\0", 12, "renderer written to the registry"),
]

MANAGER = "gta2 manager.exe"
BACKUP = "gta2 manager.orig.exe"


def patch(folder):
    target = os.path.join(folder, MANAGER)
    backup = os.path.join(folder, BACKUP)
    if not os.path.isfile(target):
        print(f"  no {MANAGER} in {folder}, skipping")
        return True

    data = bytearray(open(target, "rb").read())

    # Already done? Recognise it by the replacement rather than by the backup
    # existing, so a half-finished run is still detected.
    if all(bytes(data[off:off + len(new)]) == new for off, _, new, _, _ in PATCHES):
        print(f"  {MANAGER} already offers RTX Remix")
        return True

    for off, old, new, room, what in PATCHES:
        found = bytes(data[off:off + len(old)])
        if found != old:
            print(f"  REFUSING: expected {old!r} at 0x{off:06X} ({what}) but found {found!r}.")
            print("  This is not the manager this patch was written against; nothing changed.")
            return False
        if len(new) > room:
            print(f"  REFUSING: {new!r} needs {len(new)} bytes, only {room} available at "
                  f"0x{off:06X}")
            return False

    if not os.path.exists(backup):
        shutil.copy2(target, backup)
        print(f"  original kept as {BACKUP}")

    for off, old, new, room, what in PATCHES:
        # Blank the whole slot first: the new string is shorter than the slack in
        # one case, and leaving the tail of the old one behind is untidy at best.
        data[off:off + room] = new + b"\0" * (room - len(new))
        print(f"  0x{off:06X}  {old.rstrip(chr(0).encode()).decode():<12} -> "
              f"{new.rstrip(chr(0).encode()).decode():<12} ({what})")

    open(target, "wb").write(bytes(data))
    print(f"  patched {MANAGER}")
    return True


def restore(folder):
    target = os.path.join(folder, MANAGER)
    backup = os.path.join(folder, BACKUP)
    if os.path.isfile(backup):
        shutil.move(backup, target)
        print(f"  original {MANAGER} restored in {folder}")
    return True


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--restore"]
    if not args:
        print(__doc__)
        sys.exit(2)
    action = restore if "--restore" in sys.argv else patch
    sys.exit(0 if all(action(f) for f in args) else 1)
