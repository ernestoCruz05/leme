# XWayland

XWayland is optional and starts lazily. Leme reserves a `DISPLAY` value during startup, but does not launch XWayland until an X11 client connects.

Native Wayland clients continue to work if XWayland is missing, exits, or crashes. Check whether an X11 client is actually being used before expecting an XWayland process:

```sh
printf '%s\n' "$DISPLAY"
pgrep -af Xwayland
```

## Managed X11 windows

Managed X11 windows use the same tags, layouts, borders, focus, floating, fullscreen, input, and clipboard policy as native windows. Dialogs and fixed-size transients start floating.

X11 menus, tooltips, and similar override-redirect surfaces position themselves
and do not join a tag layout. Moving the pointer over one does not give it
keyboard focus by itself. An unmanaged X11 surface that explicitly requests
compatibility focus may still receive it.

X11 and Wayland clients share clipboard selections through the XWayland bridge. XWayland stays below layer-shell overlays and session-lock content, and its private Wayland shell global is hidden from unrelated clients.

## Troubleshooting

Confirm that the XWayland executable is installed and available on `PATH`. If it exits, native clients should remain usable. Inspect the compositor log around the first X11 connection and any restart attempt.

When an application supports both backends, force its native Wayland mode. If that works, the remaining problem is in the XWayland path. See [XWayland troubleshooting](../troubleshooting/xwayland.md).
