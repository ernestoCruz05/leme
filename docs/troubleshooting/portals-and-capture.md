# Portal and capture troubleshooting

Separate the compositor path from the portal and media path.

## Direct capture

Test a screenshot client first:

```sh
grim -o DP-1 capture.png
wayland-info | grep -E 'wlr_screencopy|ext_image_copy_capture|ext_output_image_capture_source|zxdg_output'
```

If direct capture fails, inspect the Leme log and the advertised globals before debugging PipeWire.

## Portal environment

Check the activated portal processes and the display environment they inherited:

```sh
pid="$(pgrep -f 'xdg-desktop-portal-(luminous|wlr)' | head -1)"
printf 'portal pid=%s\n' "${pid:-missing}"
[ -n "$pid" ] && tr '\0' '\n' <"/proc/$pid/environ" |
    grep -E '^(WAYLAND_DISPLAY|DISPLAY|XDG_CURRENT_DESKTOP|XDG_SESSION_DESKTOP|XDG_SESSION_TYPE)='
```

The portal must use the Wayland socket logged by Leme.
`XDG_CURRENT_DESKTOP` and `XDG_SESSION_DESKTOP` should both be `leme` in a
direct session.

Check the selected backends and media services:

```sh
test -r /usr/share/xdg-desktop-portal/leme-portals.conf && echo present
busctl --user list | grep -E 'org.freedesktop.portal|org.pipewire'
pgrep -af 'xdg-desktop-portal|pipewire|wireplumber'
wpctl status
```

Install `xdg-desktop-portal-luminous` or `xdg-desktop-portal-wlr` for Screenshot and ScreenCast, and `xdg-desktop-portal-gtk` for the other portals. When both capture backends are installed, luminous is used. Distribution packages place the executables and service files in different directories.

## Session target

With a systemd user manager, Leme imports its display environment and then starts `leme-session.target`, which binds `graphical-session.target`. The upstream xdg-desktop-portal 1.22 unit does not start until `graphical-session.target` is active. Services with `PartOf=graphical-session.target` stop when Leme exits. Nested and headless sessions do not start the target.

```sh
systemctl --user status leme-session.target graphical-session.target xdg-desktop-portal.service
```

If the unit is missing, check that `leme-session.target` is installed in a systemd user unit directory such as `/usr/lib/systemd/user`. If Leme crashes, the target stays active until the user manager exits, and services keep the old display environment. Stop it before starting a new session under the same user manager:

```sh
systemctl --user stop leme-session.target
```

## No stream

With luminous, the portal picker offers outputs and individual windows. With the wlr backend, the default choosers offer only outputs. If direct capture works, restart the complete graphical login before retrying so old portal processes leave with the old D-Bus session. Test a second share after closing the first.

Capture remains active during a lock, but the stream must show only the lock surface or opaque blocker. Per-window direct capture is refused while locked and for unmapped windows, and an active window capture ends when the session locks. A window on a hidden tag streams as a black frame until it is shown again.
