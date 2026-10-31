# Startup troubleshooting

## Start with the log

```sh
less "${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log"
```

The log has Leme's messages and the output of programs started with `exec`.
The previous session's log is `session.log.1`, so read it before logging in
again after a crash. Start with the first error; later ones often follow from
it.

`LEME_SESSION_LOG` sets another path, or `none` to turn the log off.
`leme-session --debug` adds wlroots debug messages, which grow fast, so only use
it while reproducing a problem.

When reporting a bug, include the build:

```sh
git rev-parse --short HEAD
meson introspect --projectinfo build
```

## `XDG_RUNTIME_DIR` is missing

```sh
printf '%s\n' "$XDG_RUNTIME_DIR"
stat -c '%A %U %G %a %n' "$XDG_RUNTIME_DIR"
loginctl list-sessions
```

Your login (logind, elogind, seatd or similar) has to create it, owned by you.
Don't create `/run/user/$UID` by hand, run Leme as root or loosen `/dev/dri`
and `/dev/input` permissions.

## No D-Bus session

```sh
printf '%s\n' "$DBUS_SESSION_BUS_ADDRESS"
command -v dbus-run-session
busctl --user status
```

Start Leme with `leme-session`, which starts a bus when there is none. Plain
`leme` runs, but portals and other D-Bus programs won't see its display. See
[Gentoo and OpenRC](../guides/gentoo-openrc.md) for one working login setup.
