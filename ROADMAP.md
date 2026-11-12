# Leme roadmap

Last updated: 2026-11-12

Leme is early alpha. This page lists what works and what is planned. Setup and
configuration are in the [documentation](docs/README.md), and missing features
in [limitations](docs/reference/limitations.md).

## Available now

- Windows: native Wayland and XWayland windows, per-output adaptive tags,
  dwindle and master-stack layouts, an experimental
  [desktop layout](docs/configuration/desktop.md), floating and fullscreen
  windows, sticky windows, scratchpads and window rules.
- Pointer: tiled drags with drop previews, split resizing, and drags across
  outputs.
- Look: gaps, borders, opacity and optional
  [animations](docs/configuration/animation.md). Builds with `-Deffects=true`
  add rounded corners and blur on the GLES2 and pixman renderers.
- Outputs: persistent placement, hotplug, windows that return to their monitor,
  power control, outputs turned off from the config or a binding, adaptive
  sync, night light and temporary changes from output managers. See
  [multi-monitor](docs/guides/multi-monitor.md).
- Input: keyboard layouts, binding modes, touchpad and mouse options, touchpad
  swipes between tags, [gesture bindings](docs/configuration/keybindings.md#gestures),
  touchpad edges, lid and tablet switch bindings, pinch and hold gestures in
  apps, relative pointer, pointer constraints, shortcut inhibition, and virtual
  keyboards and pointers.
- Desktop integration: layer shell, session lock, idle notification and
  inhibition, clipboard, drag and drop, screen and window capture,
  [screen sharing](docs/guides/screen-sharing.md) through a portal, XDG
  activation, `xdg-foreign`, server-side decorations, foreign toplevel
  lists, [shell decorations](docs/guides/shell-decorations.md), and
  `security-context-v1` for sandboxed apps.
- Session: `leme-session` sets up the environment, D-Bus and the session log,
  and `leme-session.target` starts and stops systemd user services with Leme.
  See [startup](docs/configuration/startup-and-environment.md).
- Configuration and control: reloadable scfg with diagnostics and
  `leme --config-check` ([config errors](docs/configuration/config-errors.md)),
  and [`timao`](docs/reference/timao.md) for commands, queries and watches.

## Next

- Input methods and text input.
- Drop shadows, and animations for bars, launchers and notifications.
- Monocle, grid and centered master layouts.
- Moving and swapping tags between monitors.
- Terminal swallowing.
- An overview of all windows and tags.

## Planned

- Touchscreens, drawing tablets and pads.
- Custom shaders: shaders compiled to SPIR-V, applied to windows or outputs on
  both the GLES2 and Vulkan renderers.
- Supervision recipes for services started without systemd or from `exec`.

## Not planned for now

- Color management and HDR.
- DRM leasing.
- Session restore across restarts.
