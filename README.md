# hypredge

Proportional pointer movement between neighbouring monitors for Hyprland.

Hyprland moves the pointer between monitors 1:1 in layout pixels. With two
side-by-side monitors of different heights, that means:

- part of the taller monitor's shared edge is a **dead end**: the pointer
  cannot cross there;
- crossings land at the same **absolute** height, not the same **relative**
  height.

hypredge maps the whole shared side of one monitor onto the whole side of the
other by relative position:

```
  left 1920x1200          right 2560x1440
┌──────────────┐┌──────────────────────┐
│  0%  ◄───────┼┼────────►  0%          │
│ 50%  ◄───────┼┼────────► 50%          │
│100%  ◄───────┼┼────────►              │
└──────────────┘│                100%   │
                └──────────────────────┘
```

Leaving the right monitor 75% of the way down lands 75% of the way down the
left one and vice versa: no dead ends, and a round trip returns to the same
spot. Stacked (top/bottom) monitors are mapped the same way along x.

## Behaviour

- Applies only to relative pointer motion (mouse, touchpad). Warps such as
  `hyprctl dispatch movecursor` and tablets are untouched.
- Skipped while a client constrains the pointer (games with a locked/confined
  pointer) or an input-capture session is active.
- Only applies where a monitor has exactly one neighbour on that side, and that
  neighbour has exactly one on the opposite side. With several monitors along
  one side, Hyprland's normal behaviour is kept.
- Gaps between monitors and corner-only contact stay hard walls.
- Respects `cursor:hotspot_padding`.

### Cursor drawing across the border

While the pointer sits on a border, part of the cursor image overhangs onto
the neighbour. Hyprland draws that part at the same absolute position, which no
longer matches the proportional mapping; hypredge draws it at the proportional
spot instead, for hardware and software cursors.

If these hooks cannot be installed, hypredge shows an orange notification and
keeps only the movement mapping.

Known limits:

- Hyprland decides which monitors draw the cursor from its unmapped position,
  so while the pointer is in a dead-end strip (e.g. the bottom of the taller
  monitor) the overhang is not drawn on the neighbour.
- Rotated or flipped monitors keep Hyprland's normal cursor drawing.

## How fragile is this?

The movement mapping hooks one public function. The cursor drawing hooks four
**private** Hyprland internals (`getCursorPosForMonitor`, `onCursorMoved`,
`updateCursorBackend`, `damageIfSoftware`), writes the private per-monitor
software cursor box, and mirrors the conditions of `damageIfSoftware`. This was
verified against Hyprland 0.56.2's source and binary (including that these
functions are not inlined at any call site).

The plugin refuses to load against a Hyprland build other than the one it was
compiled for, but after rebuilding for a new Hyprland version **check the
cursor drawing by eye**: slowly move the cursor along a border and look for a
flickering or stuck half.

## Requirements

- Hyprland with development headers. The plugin is tied to the exact Hyprland
  build it was compiled against: rebuild after every Hyprland update. It
  refuses to load against a different build.
- `g++`, `make`, `pkg-config`, and the `hyprutils`, `pixman`, `libdrm` and
  `libei` headers (all pulled in by Hyprland on Arch).

## Build, test, load

```sh
make test     # geometry tests, no Hyprland needed
make          # builds build/hypredge.so
make load     # (re)load into the running Hyprland
make unload   # unload it
```

`make load` loads a uniquely named copy from `build/loaded/`: the dynamic
loader would otherwise keep reusing an old build loaded from the same path.
`make unload` deletes that copy again.

## Load at login (Omarchy, Lua config)

In `~/.config/hypr/autostart.lua`:

```lua
o.exec_on_start("hyprctl plugin load " .. os.getenv("HOME") .. "/Work/hypredge/build/hypredge.so")
```

After a Hyprland update, run `make clean && make` and log in again (or
`make load`).

## Layout of the code

- `src/geometry.hpp` — all the math, free of Hyprland, unit-tested.
- `src/main.cpp` — the Hyprland hooks (movement and cursor drawing).
- `tests/geometry_test.cpp` — synthetic layouts, including the 1920x1200 +
  2560x1440 desk this was written for.
