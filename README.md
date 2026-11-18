# hypredge

Proportional pointer movement between neighboring monitors for Hyprland.

Hyprland moves the mouse between monitors by absolute position. With two
monitors of the same physical size but different resolutions, part of the
higher-resolution monitor's edge becomes a dead end for the mouse unless you
change your scaling. hypredge maps the whole edge of one monitor onto the
whole edge of the other by relative position instead:

```
      left 1920x1200          right 2560x1440
┌───────────────────────┐┌───────────────────────┐
│    0%  ◄──────────────┼┼──────────────►    0%  │
│                       ││                       │
│   50%  ◄──────────────┼┼──────────────►   50%  │
│                       ││                       │
│  100%  ◄──────────────┼┼──────────────►  100%  │
└───────────────────────┘└───────────────────────┘
```

No dead ends and a round trip returns to the same spot.

## Behavior

- Mouse and touchpad only (tablets and `hyprctl` warps are not affected)
- Skipped while a game locks the pointer
- Only between monitors that are each other's only neighbor on that side;
  otherwise Hyprland's normal behavior applies
- Limits: the cursor image isn't drawn across the border while the pointer is
  in a dead-end strip and rotated monitors keep Hyprland's normal cursor
  drawing. If you have a rotated setup and want to fix this, feel free to
  contribute.

## Build and load

Needs Hyprland's development headers, `g++`, `make` and `pkg-config`.

```sh
make test     # geometry tests, no Hyprland needed
make          # builds build/hypredge.so
make load     # (re)load into the running Hyprland
make unload   # unload it
```

To load at login (Omarchy), add this to `~/.config/hypr/autostart.lua`:

```lua
o.exec_on_start("hyprctl plugin load " .. os.getenv("HOME") .. "/Work/hypredge/build/hypredge.so")
```

## After a Hyprland update

The plugin only loads into the exact Hyprland build it was compiled against,
so rebuild with `make clean && make`. I try to keep it up to date, but there's
no guarantee it works after an update.

## License

MIT — see [LICENSE](LICENSE).
