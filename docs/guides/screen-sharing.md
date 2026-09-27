# Screen sharing

Screen sharing crosses three components: Leme provides capture protocols, a portal handles consent and source selection, and PipeWire carries the media stream. Test them separately when a share fails.

## Test compositor capture

Use a direct capture client such as `grim` before debugging portals:

```sh
grim -o DP-1 capture.png
```

You can inspect the globals Leme advertises with:

```sh
wayland-info | grep -E 'wlr_screencopy|ext_image_copy_capture|ext_output_image_capture_source|zxdg_output'
```

Leme exposes legacy and modern output capture, logical output metadata, damage, transforms, timestamps, and cursor composition. Shared-memory capture copies pixels into CPU-accessible memory. DMA-BUF capture can pass GPU buffers directly when the graphics driver and output backend support it.

## Test the portal and media stack

The installed portal selector uses `xdg-desktop-portal-luminous` for Screenshot
and ScreenCast when it is installed, and `xdg-desktop-portal-wlr` otherwise. GTK
handles the other desktop portals. The luminous picker offers outputs and
individual windows through Leme's per-window capture source. The default
`xdg-desktop-portal-wlr` choosers offer only outputs; it shares a window only
through a custom `chooser_cmd` that prints `Window: ` followed by an `lswt`
identifier.

The portal service needs `graphical-session.target`. See
[the session target](../troubleshooting/portals-and-capture.md#session-target).

Check the session environment and activated services:

```sh
printf 'desktop=%s session=%s type=%s wayland=%s display=%s\n' \
  "$XDG_CURRENT_DESKTOP" "$XDG_SESSION_DESKTOP" \
  "$XDG_SESSION_TYPE" "$WAYLAND_DISPLAY" "${DISPLAY:-}"

busctl --user list | grep -E 'org.freedesktop.portal|org.pipewire'
pgrep -af 'xdg-desktop-portal|pipewire|wireplumber'
```

The Wayland socket used by the portal must be the one logged by Leme. A nested or headless Leme intentionally does not update the parent desktop's D-Bus activation environment, so test portal behavior from a direct graphical login.

## Capture while locked

Capture continues during a lock, but it sees only committed lock content or the opaque blocker. Per-window capture requests are refused while locked and for unmapped windows, and an active window capture ends when the session locks. A source refused by Leme becomes inert and reports the failure when the client opens its capture session.

If direct capture works but a portal stream does not, restart the full graphical login before retrying. That clears portals that inherited an old display environment. See [portal troubleshooting](../troubleshooting/portals-and-capture.md).

## Remote desktop

A remote desktop server needs capture and input injection. Leme provides both:

- `wayvnc` uses Leme's capture and virtual input protocols directly, without a portal.
- Portal clients such as RustDesk use the RemoteDesktop portal. The installed portal configuration sends RemoteDesktop and Clipboard requests to `xdg-desktop-portal-luminous`. `xdg-desktop-portal-wlr` has no remote desktop support.

Remote input reaches the lock screen, so a remote session can unlock a locked Leme session. See [virtual input](../reference/wayland-protocols.md#input-and-data-exchange).
