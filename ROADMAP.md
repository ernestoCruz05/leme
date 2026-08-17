# Leme roadmap

Last updated: 2026-08-13

Leme is early alpha software. This page summarizes what is available and what
is planned. For setup and configuration, start with the
[documentation index](docs/README.md). For gaps in the current build, see
[current limitations](docs/reference/limitations.md).

## Available now

### Window and workspace management

Leme supports native Wayland and managed XWayland windows, per-output adaptive
tags, dwindle, master-stack, and accordion layouts, floating and fullscreen
windows, output-scoped sticky floating windows, standard and named scratchpads,
window rules, and directional keyboard navigation. Pointer operations include tiled detachment with insertion previews,
split-boundary resizing, cancellation rollback, and reinsertion across outputs.

Window open and close animations and workspace transitions are optional. See
[animation configuration](docs/configuration/animation.md).

### Appearance and presentation

Style configuration controls gaps, focus-sensitive borders, active and inactive
opacity, and per-window-rule opacity. Builds configured with `-Deffects=true`
also provide rounded corners and backdrop blur. See
[appearance configuration](docs/configuration/appearance.md).

When the backend and renderer support them, Leme advertises linux-dmabuf
feedback, explicit synchronization, presentation timing, alpha modifiers,
content type, and single-pixel buffers. Focused, visible fullscreen clients can
request asynchronous presentation through `wp-tearing-control-v1`; if the
backend rejects asynchronous state, the frame falls back to synchronized
presentation.

### Outputs and input

Multiple outputs support persistent placement, directional focus and movement,
pointer drag between outputs, temporary output-manager requests, power control,
and hotplug recovery. Keyboard layouts, binding modes, pointer configuration,
relative pointer, pointer constraints, and shortcut inhibition are available.

See the [multi-monitor guide](docs/guides/multi-monitor.md),
[input troubleshooting](docs/troubleshooting/input.md), and the
[configuration reference](docs/configuration/README.md).

### Desktop integration

Leme supports layer-shell programs, fail-closed session locking, idle
notification and inhibition, clipboard and drag and drop, whole-output capture,
direct window capture for compatible clients, XDG activation, dialogs and
transient placement, server-side decoration, workspace and window publication
with configurable activation policy, and optional lazy XWayland.

Locking covers normal content immediately, restricts input and capture to lock
content, and leaves an opaque blocker in place if the locker disappears. See
the [Wayland protocol reference](docs/reference/wayland-protocols.md).

### Session startup and external services

`leme-session` validates the runtime directory, sets the XDG desktop variables,
creates a D-Bus session when needed, and rotates the session log. Direct
sessions publish their display environment to D-Bus activation and, when
available, the systemd user manager. Configuration can set child environment
variables and start argument vectors once after compositor startup.

Bars, launchers, wallpaper programs, notification daemons, portals, PipeWire,
audio policy, and other desktop services remain external. Leme can start them
but does not supervise their complete lifetime. See the
[guides](docs/guides/README.md) and
[startup configuration](docs/configuration/startup-and-environment.md).

### Configuration and control

Leme uses reloadable scfg configuration with transactional application and
recoverable diagnostics. `leme --config-check [PATH]` validates the same
configuration without starting a compositor. Diagnostics are available in the
session log and through `timao get config`.

The `timao` client provides commands, state queries, and event subscriptions
over a private Unix socket.

See [configuration errors](docs/configuration/config-errors.md), the
[`timao` reference](docs/reference/timao.md), and the [control protocol
reference](docs/reference/control-protocol.md).

## Active priorities

### Persistent adaptive sync

Add persistent adaptive-sync policy to output configuration and allow matching
temporary output-management requests. The current build rejects adaptive-sync
changes.

### Visible configuration errors

Add a small on-screen report for configuration errors. The current build can
validate configuration offline, writes runtime diagnostics to the session log,
and exposes them through `timao get config`, but the accepted `config_errors`
settings do not draw anything yet.

### Session-owned desktop services

Add and document reliable supervision recipes for keeping portals, bars,
wallpaper programs, notification daemons, policy agents, keyrings, and network
applets tied to the graphical session. Leme can start them once and publish the
activation environment, but it does not supervise or stop them when the
compositor exits.

## Planned

### Remaining detached window workflows

Add terminal swallowing and minimize as detach-and-restore operations. Sticky
floating windows and scratchpads are already available.

### Window and tag overview

Add an overview for browsing windows and tags from one screen.

### Compatibility protocols

Add `xdg-foreign-v2` and `security-context-v1` where they fit Leme's existing
focus, lock, and process-isolation policy. Existing foreign-toplevel
publication and capture protocols do not provide `xdg-foreign-v2`.

### Extended input

Add pointer gestures, touch, tablet tools and pads, text input, input methods,
and virtual keyboards under the existing focus and lock rules.

## Deferred

The following work is outside the current daily-driver target:

- color management and HDR;
- gamma control and night-light integration;
- DRM leasing;
- restoring views to their original output after disconnect and reconnect;
- session restoration across compositor or graphical-session restarts;
- broader desktop automation beyond the existing `timao` control interface.

Deferred work is not available in the current build. Check
[current limitations](docs/reference/limitations.md) before relying on a
feature not listed above.
