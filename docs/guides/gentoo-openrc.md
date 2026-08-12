# Gentoo and OpenRC

This page documents a known working Gentoo/OpenRC setup. It is not a
requirement for Leme. Other distributions can provide the same pieces through
systemd-logind, another logind implementation, seatd, or their own display
manager.

## Login and seat

A normal PAM login should create a user session and a writable
`XDG_RUNTIME_DIR`. In this setup, elogind's PAM module creates the directory and
tracks the active seat and VT.

Leme uses wlroots and libseat for DRM, input, session activation, and VT access. It does not link directly to OpenRC or elogind.

Check the session from a TTY or graphical terminal:

```sh
printf '%s\n' "$XDG_RUNTIME_DIR"
stat -c '%A %U %G %a %n' "$XDG_RUNTIME_DIR"
loginctl list-sessions
loginctl show-session "$XDG_SESSION_ID" \
  -p Type -p Class -p Active -p State -p Seat -p TTY
```

Do not create `/run/user/$UID` by hand, change device permissions globally, or run Leme as root. Fix the PAM and seat setup instead.

## Ly

If Ly is installed, select **Leme** from its Wayland session list. Another display manager works when its PAM stack creates an active user session and reads `/usr/share/wayland-sessions/leme.desktop`.

A typical process tree is:

```text
Ly and PAM
└─ elogind user session on seat0
   └─ one D-Bus session
      └─ leme-session
         └─ leme
```

The exact tree depends on the display manager. Ly and elogind are not part of Leme itself.

## D-Bus and PipeWire

`leme-session` uses `dbus-run-session` only when `DBUS_SESSION_BUS_ADDRESS` is missing. It does not create a second bus when the login manager already supplied one.

On a Gentoo/OpenRC setup where no other session service starts PipeWire, `gentoo-pipewire-launcher` can be placed in an `exec` block:

```scfg
exec {
    gentoo-pipewire-launcher
}
```

Do not add that command if another session service already starts PipeWire, PipeWire Pulse, and WirePlumber.

For portal setup and screen sharing, use the [screen sharing guide](screen-sharing.md). For missing runtime directories or D-Bus errors, use [startup troubleshooting](../troubleshooting/startup.md).

## Logout

Exit Leme with its configured quit command. When `leme-session` created the D-Bus bus, `dbus-run-session` exits with Leme. Programs started as detached children, including bars and wallpaper daemons, may outlive the compositor unless a service manager or session wrapper owns them.
