# Startup and environment

The `env` block sets variables for programs launched by Leme. It does not change Leme's own environment or the parent login session:

```scfg
env {
    XCURSOR_THEME Adwaita
}
```

The `exec` block starts each argument vector once after startup:

```scfg
exec {
    foot --server
    waybar
}
```

A configuration reload does not rerun `exec` entries.

`spawn` and `exec` call programs directly without a shell. Leme expands its
own `$name` configuration variables, but it does not expand `~`, shell
environment variables, pipes, redirections, command substitution, or globs.
Use `$$VARIABLE` to pass a literal environment reference to an explicitly
invoked shell. Use an absolute path when the executable is not on `PATH`. The
[syntax and variables](syntax-and-variables.md) page has a complete shell
example.

After applying the `env` block, Leme sets `WAYLAND_DISPLAY` and `DISPLAY` to
the compositor's active values. Those two names cannot be overridden through
`env`. Leme also exports `LEME_SOCKET` when the control socket is available.
The [timao reference](../reference/timao.md) explains how the client uses it.
