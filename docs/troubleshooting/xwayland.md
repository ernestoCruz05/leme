# XWayland troubleshooting

XWayland only starts when an X11 app connects, so no Xwayland process at
startup is normal. After starting an X11 app:

```sh
printf '%s\n' "$DISPLAY"
pgrep -af Xwayland
```

If it does not start, check that `Xwayland` is on `PATH` and read the log
around the app's first connection. If the app also runs on Wayland, try that to
see whether the problem is X11 only. See the [XWayland guide](../guides/xwayland.md).
