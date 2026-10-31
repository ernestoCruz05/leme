# Portal and capture troubleshooting

## Direct capture

```sh
grim -o DP-1 capture.png
wayland-info | grep -E 'wlr_screencopy|ext_image_copy_capture|ext_output_image_capture_source'
```

If this fails, the problem is in Leme: check its log. If it works, look at the
portal.

## Portal environment

The portal has to see Leme's display:

```sh
pid="$(pgrep -f 'xdg-desktop-portal-(luminous|wlr)' | head -1)"
printf 'portal pid=%s\n' "${pid:-missing}"
[ -n "$pid" ] && tr '\0' '\n' <"/proc/$pid/environ" |
    grep -E '^(WAYLAND_DISPLAY|DISPLAY|XDG_CURRENT_DESKTOP|XDG_SESSION_DESKTOP|XDG_SESSION_TYPE)='
```

`WAYLAND_DISPLAY` must match the one in Leme's log, and both desktop variables
should be `leme`. Then check the backends and PipeWire:

```sh
test -r /usr/share/xdg-desktop-portal/leme-portals.conf && echo present
busctl --user list | grep -E 'org.freedesktop.portal|org.pipewire'
pgrep -af 'xdg-desktop-portal|pipewire|wireplumber'
wpctl status
```

Screenshots and screen casts need `xdg-desktop-portal-luminous` or
`xdg-desktop-portal-wlr`, and the other portals need `xdg-desktop-portal-gtk`.

## Session target

With systemd, Leme starts `leme-session.target`, which binds
`graphical-session.target`. The portal waits for that target. Nested and
headless sessions don't start it.

```sh
systemctl --user status leme-session.target graphical-session.target xdg-desktop-portal.service
```

If the target is missing, check that `leme-session.target` is installed in a
user unit directory such as `/usr/lib/systemd/user`.

On exit Leme stops the target and removes its display variables from the
systemd environment. A Leme that crashed without `leme-session` leaves the
target running with the old display, so stop it before the next session:

```sh
systemctl --user stop leme-session.target
```

## No stream

The `wlr` portal only offers whole outputs. If `grim` works and the share does
not, log out and back in so stale portals go away. While locked, a stream only
shows the lock screen, and a window on a hidden tag streams black.
