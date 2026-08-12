# XWayland troubleshooting

XWayland starts only after an X11 client connects. No XWayland process at Leme startup is normal.

Check the reserved display and the process list after launching an X11 client:

```sh
printf '%s\n' "$DISPLAY"
pgrep -af Xwayland
```

Confirm that the XWayland executable is installed and available through `PATH`. If it exits, native Wayland clients should continue running. Inspect the compositor log around the first X11 connection and any restart attempt.

When an application supports both backends, force its native Wayland mode. If the native client works, the remaining failure is in the XWayland path.

For managed X11 behavior, dialogs, override-redirect surfaces, and clipboard sharing, see the [XWayland guide](../guides/xwayland.md).
