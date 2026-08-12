# Leme roadmap

Last updated: 2026-08-09

Leme is early alpha software. This page summarizes what is available and what
is planned. For setup and configuration, start with the
[documentation index](docs/README.md). For gaps in the current build, see
[current limitations](docs/reference/limitations.md).

## Available now

### Window and workspace management

Leme supports native Wayland and managed XWayland windows, per-output adaptive
tags, dwindle, master-stack, and accordion layouts, floating and fullscreen
windows, standard and named scratchpads, window rules, and directional keyboard
and pointer operations.

Window open and close animations and workspace transitions are optional. See
[animation configuration](docs/configuration/animation.md).

### Outputs and input

Multiple outputs support persistent placement, directional focus and movement,
pointer drag between outputs, temporary output-manager requests, power control,
and hotplug recovery. Keyboard layouts, binding modes, pointer configuration,
relative pointer, pointer constraints, and shortcut inhibition are available.

See the [multi-monitor guide](docs/guides/multi-monitor.md),
[input troubleshooting](docs/troubleshooting/input.md), and the
[configuration reference](docs/configuration/README.md).

### Desktop integration

Leme supports layer-shell programs, session locking, idle notification and
inhibition, clipboard and drag and drop, whole-output capture, direct window
capture for compatible clients, XDG activation and dialogs, server-side
decoration, workspace and window publication, and optional lazy XWayland.

Bars, launchers, wallpaper programs, notification daemons, portals, PipeWire,
audio policy, and other desktop services remain external. See the
[guides](docs/guides/README.md) and [Wayland protocol
reference](docs/reference/wayland-protocols.md).

### Configuration and control

Leme uses reloadable scfg configuration with recoverable diagnostics. The
`timao` client provides commands, state queries, and event subscriptions over a
private Unix socket.

See [configuration errors](docs/configuration/config-errors.md), the
[`timao` reference](docs/reference/timao.md), and the [control protocol
reference](docs/reference/control-protocol.md).

## Active priorities

### Persistent adaptive sync

Add persistent adaptive-sync policy to output configuration and allow matching
temporary output-management requests. The current build rejects adaptive-sync
changes.

### Visible configuration errors

Add a small on-screen report for configuration errors. The current build writes
diagnostics to the session log and exposes them through `timao get config`.

### Session-owned desktop services

Document reliable ways to keep portals, bars, wallpaper programs, notification
daemons, policy agents, keyrings, and network applets tied to the graphical
session instead of relying only on detached startup commands.

## Planned

### Detached window workflows

Add sticky floating windows, terminal swallowing, and minimize as a detach and
restore operation.

### Window and tag overview

Add an overview for browsing windows and tags from one screen.

### Compatibility protocols

Add `xdg-foreign-v2` and `security-context-v1` where they fit Leme's existing
focus, lock, and process-isolation policy.

### Extended input

Add pointer gestures, touch, tablet tools and pads, text input, input methods,
and virtual keyboards under the existing focus and lock rules.

## Deferred

The following work is outside the current daily-driver target:

- color management and HDR;
- gamma control and night-light integration;
- DRM leasing;
- session restoration;
- broader desktop automation.

Deferred work is not available in the current build. Check
[current limitations](docs/reference/limitations.md) before relying on a
feature not listed above.
