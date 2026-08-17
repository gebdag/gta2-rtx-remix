"""Set GTA2's own screen resolution, past what its manager will offer.

Two things are going on and they are easy to confuse.

The **path traced image** is sized by render_width / render_height in
gta2dx9.ini and has nothing to do with any of this - the renderer presents into
a window of its own. That is the setting to change if the picture is the wrong
size.

GTA2's **own** resolution is what its HUD and menus are laid out in, and it is
also the mode its video device puts the display into during startup. That is the
one this script sets, and it matters for two reasons: a 640x480 HUD stretched
over a 4K image looks like a 640x480 HUD, and a display dropped to 640x480 means
"render at the desktop resolution" reads back 640x480.

The manager will not offer much here. Its list comes from DirectDraw mode
enumeration, which on current Windows reports a short and arbitrary set - no 4K
among it. The registry takes any value the game can handle, though, and GTA2 has
been observed running happily at 3840x2160, so this writes it directly.

Both hives are written. The manager and the in-game options screen write HKLM,
and the game itself has been seen reading either, so leaving them disagreeing is
asking for confusion.

Usage:
    python set_resolution.py                # match the desktop
    python set_resolution.py 2560 1440      # explicit
    python set_resolution.py --show         # just report
"""

import ctypes
import sys
import winreg

HKLM_PATH = r"SOFTWARE\WOW6432Node\DMA Design Ltd\GTA2\Screen"
HKCU_PATH = r"SOFTWARE\DMA Design Ltd\GTA2\Screen"

ENUM_REGISTRY_SETTINGS = -2


class DEVMODE(ctypes.Structure):
    _fields_ = [
        ("dmDeviceName", ctypes.c_char * 32),
        ("dmSpecVersion", ctypes.c_ushort),
        ("dmDriverVersion", ctypes.c_ushort),
        ("dmSize", ctypes.c_ushort),
        ("dmDriverExtra", ctypes.c_ushort),
        ("dmFields", ctypes.c_uint),
        ("dmPositionX", ctypes.c_int),
        ("dmPositionY", ctypes.c_int),
        ("dmDisplayOrientation", ctypes.c_uint),
        ("dmDisplayFixedOutput", ctypes.c_uint),
        ("dmColor", ctypes.c_short),
        ("dmDuplex", ctypes.c_short),
        ("dmYResolution", ctypes.c_short),
        ("dmTTOption", ctypes.c_short),
        ("dmCollate", ctypes.c_short),
        ("dmFormName", ctypes.c_char * 32),
        ("dmLogPixels", ctypes.c_ushort),
        ("dmBitsPerPel", ctypes.c_uint),
        ("dmPelsWidth", ctypes.c_uint),
        ("dmPelsHeight", ctypes.c_uint),
        ("dmDisplayFlags", ctypes.c_uint),
        ("dmDisplayFrequency", ctypes.c_uint),
        ("dmICMMethod", ctypes.c_uint),
        ("dmICMIntent", ctypes.c_uint),
        ("dmMediaType", ctypes.c_uint),
        ("dmDitherType", ctypes.c_uint),
        ("dmReserved1", ctypes.c_uint),
        ("dmReserved2", ctypes.c_uint),
        ("dmPanningWidth", ctypes.c_uint),
        ("dmPanningHeight", ctypes.c_uint),
    ]


def desktop_size():
    """The desktop's persistent mode, in real pixels.

    Not GetSystemMetrics, which reports the DPI-scaled desktop, and not
    ENUM_CURRENT_SETTINGS, which reports whatever mode a running game has
    temporarily imposed.
    """
    dm = DEVMODE()
    dm.dmSize = ctypes.sizeof(DEVMODE)
    if ctypes.windll.user32.EnumDisplaySettingsA(None, ENUM_REGISTRY_SETTINGS, ctypes.byref(dm)):
        if dm.dmPelsWidth:
            return int(dm.dmPelsWidth), int(dm.dmPelsHeight)
    return None


def read(hive, path):
    try:
        with winreg.OpenKey(hive, path) as k:
            out = {}
            for name in ("start_mode", "window_width", "window_height", "full_width",
                         "full_height", "rendername"):
                try:
                    out[name] = winreg.QueryValueEx(k, name)[0]
                except FileNotFoundError:
                    pass
            return out
    except OSError:
        return None


def write(hive, path, width, height):
    with winreg.OpenKey(hive, path, 0, winreg.KEY_SET_VALUE) as k:
        for name in ("window_width", "full_width"):
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, width)
        for name in ("window_height", "full_height"):
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, height)


def main(argv):
    hives = [(winreg.HKEY_LOCAL_MACHINE, HKLM_PATH, "HKLM"),
             (winreg.HKEY_CURRENT_USER, HKCU_PATH, "HKCU")]

    if "--show" in argv:
        print(f"desktop (persistent mode): {desktop_size()}")
        for hive, path, label in hives:
            print(f"{label}: {read(hive, path)}")
        return 0

    args = [a for a in argv if not a.startswith("--")]
    if len(args) == 2:
        width, height = int(args[0]), int(args[1])
    else:
        size = desktop_size()
        if not size:
            print("Could not read the desktop mode; pass a width and height explicitly.")
            return 1
        width, height = size

    # The game copes with odd sizes but not with nonsense, and a mistake here is
    # a game that will not start.
    if not (320 <= width <= 7680 and 240 <= height <= 4320):
        print(f"{width}x{height} is out of range.")
        return 1

    failed = False
    for hive, path, label in hives:
        try:
            write(hive, path, width, height)
            print(f"  {label}: GTA2 resolution set to {width}x{height}")
        except OSError as e:
            print(f"  {label}: could not write ({e})")
            failed = label == "HKLM"   # HKCU missing is fine, HKLM is the one that counts
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
