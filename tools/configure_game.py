"""Assert the whole known-good configuration for GTA2 under RTX Remix.

Every setting here has been arrived at the hard way, and several of them get
silently undone by things outside our control: the GTA2 manager rewrites the
renderer when you touch the resolution, the in-game options screen rewrites it
too and flips start_mode, and booting a stock copy of the game rewrites whatever
it feels like. So rather than fix them one at a time as they break, this states
the whole config in one place and deploy runs it every time.

    python configure_game.py "<game folder>" [--fps-cap|--no-fps-cap] [<width> <height>]

Passing a width and height sets GTA2's own resolution; without them whatever is
already set is left alone. That is the HUD layout only - the path traced image is
sized by render_width/render_height in gta2dx9.ini and defaults to the desktop.

Registry, per hive (both are written; the manager and the options screen write
HKLM, and the game has been seen reading either):

    rendername      d3ddll.dll   the renderer. Our DLL is installed under this
                                 name as well as its own, because the manager's
                                 renderer list is three hardcoded names and
                                 cannot be made to offer a fourth.
    lighting        1            with this clear the game never calls
                                 gbh_AddLight and there is nothing to inject
    window/full     left alone   GTA2's *own* resolution, which is only the HUD
                                 layout. NOT the size of the path traced image -
                                 that is render_width/render_height in
                                 gta2dx9.ini and defaults to the desktop. Not
                                 forced: see FALLBACK_RES below.

dxwrapper, which owns the game's DirectDraw:

    EnableWindowMode          1  Windowed, so DirectDraw never changes the
                                 display mode. Fullscreen dropped the desktop to
                                 the game's resolution, which broke Remix's
                                 Present and made "render at the desktop size"
                                 read back 640x480.
    DdrawUseNativeResolution  0  MUST be 0. Setting it to 1 also stops the mode
                                 change, but by telling the game it is at the
                                 desktop resolution - so dxwrapper then
                                 allocates 4K DirectDraw surfaces through
                                 d3d9on12 and faults in the system d3d9.dll.
                                 That was an access violation on boot, which is
                                 why the game's own resolution wants to stay
                                 modest even though nothing needs it large.
"""

import os
import re
import sys
import winreg

HIVES = [
    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\DMA Design Ltd\GTA2\Screen", "HKLM"),
    (winreg.HKEY_CURRENT_USER, r"SOFTWARE\DMA Design Ltd\GTA2\Screen", "HKCU"),
]

STRINGS = {"rendername": "d3ddll.dll"}
DWORDS = {"lighting": 1}

# GTA2's own resolution is deliberately NOT forced.
#
# It was pinned to 640x480 here, on the reasoning that nothing needs it larger -
# it is only the HUD layout, since the path traced image is sized separately by
# render_width/render_height. That reasoning still holds, but hardcoding it does
# not: a display that cannot do 640x480 then cannot run the game at all, and the
# 4K experiment before it crashed dxwrapper's DirectDraw path. Neither extreme is
# safe to assume.
#
# So whatever is already set is left alone, and --game-res sets it deliberately.
# Anything absurd or missing falls back to 800x600, which every display since
# about 1995 can do.
FALLBACK_RES = (800, 600)

# EnableWindowMode is what makes this work at all; DdrawUseNativeResolution at 1
# is what crashed it. Both are asserted rather than assumed.
DXWRAPPER = {"EnableWindowMode": 1, "DdrawUseNativeResolution": 0}


def read(hive, path):
    """Current values, or None when the key is not there at all."""
    try:
        with winreg.OpenKey(hive, path) as k:
            out = {}
            for name in ("window_width", "window_height"):
                try:
                    out[name] = winreg.QueryValueEx(k, name)[0]
                except FileNotFoundError:
                    pass
            return out
    except OSError:
        return None


def game_resolution(explicit):
    """What to write, or None to leave the existing values untouched."""
    if explicit:
        return explicit
    for hive, path, _ in HIVES:
        current = read(hive, path) or {}
        w, h = current.get("window_width", 0), current.get("window_height", 0)
        if isinstance(w, int) and isinstance(h, int) and 320 <= w <= 7680 and 240 <= h <= 4320:
            return None    # already sane, do not touch it
    return FALLBACK_RES


def registry(fps_cap, resolution):
    dwords = dict(DWORDS)
    if resolution:
        dwords["window_width"], dwords["window_height"] = resolution
        dwords["full_width"], dwords["full_height"] = resolution
    # 0 means uncapped, which the frame pacer at FUN_00462A30 implements by
    # resetting its deadline every frame. It runs fine uncapped; the cap is only
    # here because it is easy to flip and easy to blame.
    dwords["max_frame_rate"] = 1 if fps_cap else 0
    dwords["min_frame_rate"] = 1 if fps_cap else 0

    ok = True
    for hive, path, label in HIVES:
        try:
            with winreg.CreateKeyEx(hive, path, 0, winreg.KEY_SET_VALUE) as k:
                for name, value in STRINGS.items():
                    winreg.SetValueEx(k, name, 0, winreg.REG_SZ, value)
                for name, value in dwords.items():
                    winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, value)
            res = (f"{dwords['window_width']}x{dwords['window_height']}"
                   if "window_width" in dwords else "resolution left as-is")
            print(f"  {label}: renderer, lighting, {res}, fps cap "
                  f"{'on' if fps_cap else 'OFF'}")
        except OSError as e:
            print(f"  {label}: could not write ({e})")
            # HKCU missing is survivable; HKLM is the one the game actually reads.
            if label == "HKLM":
                ok = False
    return ok


def dxwrapper(folder):
    path = os.path.join(folder, "dxwrapper.ini")
    if not os.path.isfile(path):
        print("  no dxwrapper.ini here (fine for the plain install)")
        return True

    text = open(path, encoding="utf-8", errors="replace").read()
    backup = path + ".orig"
    if not os.path.exists(backup):
        open(backup, "w", encoding="utf-8").write(text)

    changed = []
    for key, want in DXWRAPPER.items():
        pattern = re.compile(rf"(?m)^({re.escape(key)}\s*=\s*)(\d+)\s*$")
        match = pattern.search(text)
        if not match:
            print(f"  WARNING: {key} not found in dxwrapper.ini, leaving it alone")
            continue
        if int(match.group(2)) != want:
            changed.append(f"{key}={want}")
        text = pattern.sub(rf"\g<1>{want}", text)

    open(path, "w", encoding="utf-8").write(text)
    print(f"  dxwrapper.ini: " + (", ".join(changed) if changed else "already correct"))
    return True


def main(argv):
    folders = [a for a in argv if not a.startswith("--") and not a.isdigit()]
    fps_cap = "--fps-cap" in argv
    if not folders:
        print(__doc__)
        return 2
    explicit = None
    numbers = [a for a in argv if a.isdigit()]
    if len(numbers) == 2:
        explicit = (int(numbers[0]), int(numbers[1]))
    resolution = game_resolution(explicit)
    ok = registry(fps_cap, resolution)
    for folder in folders:
        if os.path.isfile(os.path.join(folder, "gta2.exe")):
            ok = dxwrapper(folder) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
