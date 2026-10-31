# XWayland

XWayland is optional. Leme sets `DISPLAY` at startup but only starts XWayland
when the first X11 app connects. Wayland apps keep working if it is missing or
crashes.

```sh
printf '%s\n' "$DISPLAY"
pgrep -af Xwayland
```

X11 windows get the same tags, layouts, rules and clipboard as Wayland ones.
X11 menus and tooltips place themselves and do not tile.

If an app misbehaves under X11 and supports Wayland, try its Wayland mode to
see which side the problem is on. See
[XWayland troubleshooting](../troubleshooting/xwayland.md).
