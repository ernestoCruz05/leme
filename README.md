# Leme

Leme is a tiling Wayland compositor written in C17 and built on
wlroots 0.20. It supports dwindle, master-stack, and accordion layouts,
per-output adaptive tags, multiple monitors, native Wayland applications, and
optional XWayland.

Leme is early alpha and a personal project. It is usable for daily work but is not stable software: configuration and behavior may change, and physicalmulti-monitor support has had limited hardware testing. It is built primarily
for my own use. Issues and patches are welcome, but support is best-effort.

## What works

- Native Wayland windows with optional lazy XWayland support
- Adaptive tags with dwindle, master-stack, and accordion layouts
- Multiple outputs with directional focus, movement, and pointer drag
- Configurable keyboard layouts, pointer devices, binding modes, and window rules
- Optional window animations and workspace transitions
- Layer-shell programs, clipboard and drag-and-drop integration, locking, idle inhibition, and capture
- A Unix-socket control interface with the `timao` client

See the [documentation index](docs/README.md) for the user path and exact references.

## Current limits

Leme does not yet provide persistent VRR policy, touch and tablet input, text-input protocols, color management, HDR, gamma control, or DRM leasing. A disconnected view does not return to its original monitor when that output reconnects.

See the [current limitations](docs/reference/limitations.md) and the [roadmap](ROADMAP.md).

## Build

```sh
meson setup build
meson compile -C build
```

The [installation guide](docs/getting-started/installation.md) covers dependencies, installation, and the user configuration path.

## Documentation

- [Documentation index](docs/README.md)
- [Getting started](docs/getting-started/README.md)
- [Concepts](docs/concepts/README.md)
- [Configuration](docs/configuration/README.md)
- [Guides](docs/guides/README.md)
- [Reference](docs/reference/README.md)
- [Troubleshooting](docs/troubleshooting/README.md)

## Project status

See the [roadmap](ROADMAP.md) for current priorities and planned features.

## License

Leme is free software licensed under [GPL-3.0-or-later](LICENSE).
