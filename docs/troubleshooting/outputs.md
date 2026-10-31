# Output troubleshooting

## An output block is rejected

`wlr-randr` lists the exact connector names and modes. Check that:

- `refresh` has a `mode` with it, and is within 1 Hz of an advertised rate;
- `scale` is from `0.5` to `4.0`;
- `transform` is `normal`, `90`, `180` or `270`;
- no two outputs overlap.

Removing the block falls back to the preferred mode at scale 1. A reload that
fails keeps the outputs as they were.

## A monitor on the second GPU stays dark

With `secondary_gpu on_demand`, Leme opens the second GPU when the kernel
reports a monitor on it. The log shows it:

```text
leme: /dev/dri/card0 stays closed until a monitor is connected
leme: opened /dev/dri/card0
leme: closing /dev/dri/card0, nothing is connected to it
```

If plugging in prints nothing, check that the kernel sends the event:

```sh
udevadm monitor --kernel --subsystem-match=drm
```

If no `change` line shows up, set `secondary_gpu always` and reload.

## An output does not turn off or back on

```sh
wayland-info | grep zwlr_output_power_manager_v1
wlopm
wlopm --off DP-1
wlopm --on DP-1
```

An output that is off keeps its tags and windows, and a reload does not wake
it. If turning it on fails, its mode or position is the problem: fix it in the
config, reload while it is off, and try again.
