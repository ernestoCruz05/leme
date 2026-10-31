# First session

Copy the [minimal configuration](minimal-config.md) first, and try a
[nested session](nested-session.md) if you can.

From a display manager, pick the **Leme** session. From a TTY, log in and run:

```sh
exec leme-session
```

`leme-session` checks `XDG_RUNTIME_DIR`, sets the XDG desktop variables,
starts D-Bus if there is none, and logs to:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log
```

The previous log is `session.log.1`. With the minimal config, `SUPER+SHIFT+e`
quits. If Leme does not start, see
[startup troubleshooting](../troubleshooting/startup.md).

Portals, PipeWire, bars and lockers are separate programs; the
[guides](../guides/README.md) cover them.
