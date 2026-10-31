# Screen sharing

Sharing goes through three pieces: Leme captures, a portal asks which source to
share, and PipeWire carries the stream. When it fails, test them in that order.

## Leme

```sh
grim -o DP-1 capture.png
wayland-info | grep -E 'wlr_screencopy|ext_image_copy_capture|ext_output_image_capture_source'
```

If `grim` works, capture is fine.

## Portal and PipeWire

The installed portal config uses `xdg-desktop-portal-luminous` for screenshots
and screen casts when it is installed, and `xdg-desktop-portal-wlr` otherwise.
Luminous offers outputs and single windows. `xdg-desktop-portal-wlr` only
offers outputs, unless its `chooser_cmd` prints `Window: ` and an `lswt`
identifier.

The portal needs `graphical-session.target`, see
[the session target](../troubleshooting/portals-and-capture.md#session-target).
Check the environment and services:

```sh
printf 'desktop=%s session=%s type=%s wayland=%s display=%s\n' \
  "$XDG_CURRENT_DESKTOP" "$XDG_SESSION_DESKTOP" \
  "$XDG_SESSION_TYPE" "$WAYLAND_DISPLAY" "${DISPLAY:-}"

busctl --user list | grep -E 'org.freedesktop.portal|org.pipewire'
pgrep -af 'xdg-desktop-portal|pipewire|wireplumber'
```

Test portals from a real login, not a nested Leme, which leaves the parent's
D-Bus environment alone. If `grim` works and the portal does not, log out and
back in so the portals pick up the right display. More in
[portal troubleshooting](../troubleshooting/portals-and-capture.md).

While locked, capture only sees the lock screen, and window captures end.

## Remote desktop

- `wayvnc` uses Leme's capture and virtual input directly, with no portal.
- RustDesk and other portal clients use the RemoteDesktop portal, which the
  installed config sends to luminous. `xdg-desktop-portal-wlr` has no remote
  desktop support.

Remote input reaches the lock screen, so a remote session can unlock Leme.
