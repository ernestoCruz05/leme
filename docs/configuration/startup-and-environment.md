# Startup and environment

`exec` starts programs once, after Leme starts. A reload does not run them
again. `env` sets variables for the programs Leme starts. It does not change
Leme's own environment.

```scfg
env {
    MOZ_ENABLE_WAYLAND 1
}

exec {
    foot --server
    waybar
}
```

Each `exec` line is a program and its arguments, run without a shell (see
[no shell](syntax-and-variables.md#no-shell)). Use an absolute path for
programs that are not on `PATH`.

Leme sets `WAYLAND_DISPLAY`, `DISPLAY` and, when the control socket is up,
`LEME_SOCKET` for its children. `env` cannot override the first two. `env` does
not reach XWayland, so set the cursor in the [cursor](cursor.md) block.

`env` is unrelated to `leme.env`, which only feeds
[`$(env.NAME)`](syntax-and-variables.md#environment) while the config loads.
