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
pid="$(pgrep -f 'xdg-desktop-portal-wlr' | head -1)"
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
pgrep -af 'xdg-desktop-portal|xdg-desktop-portal-wlr|xdg-desktop-portal-gtk|pipewire|wireplumber'
wpctl status
```

Install the wlr backend for Screenshot and ScreenCast and a general desktop backend such as GTK. Distribution packages place the executables and service files in different directories.

## No stream

Leme offers whole-output portal sources. A client asking the portal for an individual window will not receive one. If direct capture works, restart the complete graphical login before retrying so old portal processes leave with the old D-Bus session. Test a second share after closing the first.

Capture remains active during a lock, but the stream must show only the lock surface or opaque blocker. Per-window direct capture is refused while locked and for unmapped windows.
