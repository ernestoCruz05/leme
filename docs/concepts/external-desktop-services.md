# External desktop services

Leme is a compositor, not a complete desktop environment. It manages Wayland surfaces, windows, input, outputs, layout, focus, locking protocols, capture protocols, and its control socket.

Other programs provide the rest of the desktop. Common examples are bars, launchers, wallpaper programs, notification daemons, portal backends, PipeWire, audio policy, idle timers, lock screens, clipboard managers, and settings tools.

## Why the boundary matters

Leme publishes the Wayland interfaces those programs need, but it does not choose one bar, launcher, portal, or audio stack for you. A configuration can start such programs with `exec`, but Leme does not supervise their complete lifetime or rerun them on every reload.

The [bars and notifications guide](../guides/bars-launchers-and-notifications.md), [locking and idle guide](../guides/locking-and-idle.md), and [screen sharing guide](../guides/screen-sharing.md) cover common integrations. Distribution-specific login and service setup belongs in the [Gentoo/OpenRC guide](../guides/gentoo-openrc.md).
