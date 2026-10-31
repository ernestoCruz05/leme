# Gentoo and OpenRC

This is one known working setup: Gentoo, OpenRC, elogind and Ly. Leme does not
depend on any of them; it needs a login that gives you a seat and a writable
`XDG_RUNTIME_DIR`, which systemd-logind or seatd also provide.

## Login and seat

elogind's PAM module creates `XDG_RUNTIME_DIR` and tracks the seat. Check it:

```sh
printf '%s\n' "$XDG_RUNTIME_DIR"
stat -c '%A %U %G %a %n' "$XDG_RUNTIME_DIR"
loginctl list-sessions
loginctl show-session "$XDG_SESSION_ID" \
  -p Type -p Class -p Active -p State -p Seat -p TTY
```

Don't create `/run/user/$UID` by hand, loosen device permissions or run Leme as
root. Fix the PAM and seat setup instead.

In Ly, pick **Leme** from the Wayland sessions. Any display manager that reads
`/usr/share/wayland-sessions` works.

## D-Bus and PipeWire

`leme-session` only starts a D-Bus session when `DBUS_SESSION_BUS_ADDRESS` is
unset. If nothing else starts PipeWire:

```scfg
exec {
    gentoo-pipewire-launcher
}
```

Programs started from `exec`, like bars, can outlive Leme when it quits, unless
a service manager owns them.
