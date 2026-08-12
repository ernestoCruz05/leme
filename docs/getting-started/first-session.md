# First session

## Before you start

Leme is early alpha software. Use a nested session first if you can. For a direct session, log in normally through your display manager or a TTY and make sure the login created a writable `XDG_RUNTIME_DIR` owned by your user.

Do not run Leme as root. Device access should come from the login session and its seat provider.

Copy the [minimal configuration](minimal-config.md) before starting. If your terminal is not `foot`, change the `term` variable first.

## Display manager

A system installation adds a Wayland session named **Leme**. Select it from a display manager that reads `/usr/share/wayland-sessions`.

Leme does not require a particular display manager. The login stack must create a normal user session with a seat, a VT, and `XDG_RUNTIME_DIR`.

## TTY

Log in through the normal TTY prompt and run:

```sh
exec leme-session
```

`leme-session` checks the runtime directory, sets the XDG desktop variables, creates a D-Bus session only when one is missing, and writes the compositor output to the session log.

## Exit and logs

With the minimal configuration, press `SUPER+SHIFT+e` to exit. If startup
fails, read the [startup troubleshooting guide](../troubleshooting/startup.md)
before changing permissions or running another process as root.

The default session log is:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log
```

The previous log is kept as `session.log.1`.

## Where to go next

Use the [configuration reference](../configuration/README.md) to change keys, outputs, pointers, and startup commands. Use the [guides](../guides/README.md) for desktop services such as portals, PipeWire, and lock programs.
