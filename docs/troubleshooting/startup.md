# Startup troubleshooting

## Start with the log

`leme-session` writes the session output to:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log
```

`leme-session` writes the active session to `session.log`. After a crash, read
that file before starting another login. The next login moves it to
`session.log.1` and starts a new `session.log`.

The file contains Leme's diagnostics and output from programs started through
`exec`. A bar or notification daemon that failed to start may be recorded there
too.

```sh
state_home="${XDG_STATE_HOME:-$HOME/.local/state}"
less "$state_home/leme/session.log"
```

Set `LEME_SESSION_LOG` to another path, or to `none` to disable the session log. Keep the first error in the file; later failures often follow from it.

Record the build when reporting a problem:

```sh
git rev-parse --short HEAD
meson introspect --projectinfo build
```

## `XDG_RUNTIME_DIR` is unavailable

`leme-session` requires a writable runtime directory owned by the logged-in user:

```sh
printf '%s\n' "$XDG_RUNTIME_DIR"
stat -c '%A %U %G %a %n' "$XDG_RUNTIME_DIR"
loginctl list-sessions
```

Fix the PAM and login-session setup so logind, elogind, seatd, or the distribution's equivalent creates the directory. Do not create `/run/user/$UID` by hand, run Leme as root, or change `/dev/dri` and `/dev/input` permissions globally.

## No D-Bus session address

Check the environment and the bus helper:

```sh
printf '%s\n' "$DBUS_SESSION_BUS_ADDRESS"
command -v dbus-run-session
busctl --user status
```

Use `leme-session` as the graphical-session entry point. When the login environment did not provide a bus, the wrapper starts one with `dbus-run-session`. A direct `leme` invocation can start the compositor, but D-Bus-activated portals and desktop programs will not receive the complete display environment.

For distribution-specific PAM and OpenRC checks, see [Gentoo/OpenRC](../guides/gentoo-openrc.md).
