# Leme

Leme is a tiling Wayland compositor written in C17 and built on
wlroots 0.20. It supports dwindle and master-stack layouts, an experimental
desktop layout, per-output adaptive tags, multiple monitors, native Wayland
applications, and optional XWayland.

Leme is early alpha and a personal project. It is usable for daily work but is
not stable software: configuration and behavior may change, and physical
multi-monitor support has had limited hardware testing. It is built primarily
for my own use. Issues and patches are welcome, but support is best-effort.

## What works

- Native Wayland windows, and X11 windows through XWayland started on demand
- Adaptive tags with dwindle and master-stack layouts
- A desktop layout with titlebars, minimizing, window groups and a file grid
- Multiple outputs with directional focus, movement and pointer drag
- Keyboard layouts, pointer settings, binding modes and window rules
- Window animations and workspace transitions, off by default
- Layer-shell programs, clipboard, drag and drop, locking, idle inhibition and
  capture
- A control socket with the `timao` client

## Current limits

Leme does not yet provide persistent VRR policy, touch and tablet input,
text-input protocols, color management, HDR, gamma control, or DRM leasing.
See the [limitations](docs/reference/limitations.md) and the
[roadmap](ROADMAP.md).

## Build

```sh
meson setup build --buildtype=release --prefix=/usr
ninja -C build
```

The [installation guide](docs/getting-started/installation.md) lists the
dependencies.

## Documentation

The docs are on the [website](https://ernestocruz05.github.io/leme/) and in
[`docs/`](docs/README.md).

## License

Leme is free software licensed under [GPL-3.0-or-later](LICENSE).
