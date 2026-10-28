# Nested session

A nested session is the safest first test. Run it inside an existing Wayland desktop so a failed compositor does not take over your login session.

## Run Leme inside Wayland

From the repository root:

```sh
mkdir -p runtime
./build/leme 2>runtime/leme.log &
LEME_PID=$!
```

Wait for Leme to print its socket name, then start a native Wayland terminal on that socket:

```sh
LEME_DISPLAY="$(awk -F= '/leme: WAYLAND_DISPLAY=/{print $2}' runtime/leme.log | tail -1)"
WAYLAND_DISPLAY="$LEME_DISPLAY" foot
```

With the minimal configuration, press `SUPER+SHIFT+e` to exit. If that
binding is not available, stop the nested process from the parent shell:

```sh
kill -INT "$LEME_PID"
wait "$LEME_PID"
```

Nested Leme leaves the parent desktop's D-Bus activation environment unchanged. It is not a substitute for testing a direct login session.

## Send keys to the nested session

The parent compositor sees every key first. When both use the same modifier, `SUPER+Return` runs the parent's binding, so the terminal opens in the parent session instead of the nested one.

If the parent is Leme, give it a passthrough mode. Only the active mode's bindings are checked, so every other key goes to the focused window, which is the nested Leme. Add the first binding to your existing `common` block:

```scfg
binds "common" {
    SUPER+SHIFT+p mode passthrough
}

binds "passthrough" {
    escape_exits false
    SUPER+SHIFT+p mode common
}
```

Click the nested window and press `SUPER+SHIFT+p`. Its bindings now work, and programs it starts open inside it with `WAYLAND_DISPLAY` and `LEME_SOCKET` pointing at the nested session, so `timao` in a nested terminal talks to the nested Leme. Press `SUPER+SHIFT+p` again to get the parent's bindings back.

## Check headless startup

A headless backend checks basic startup without opening a window:

```sh
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ./build/leme
```

Stop it with `Ctrl+C`. This checks compositor startup and configuration
loading, but not DRM, libinput, VT switching, physical outputs, or hardware
rendering.

## Where to go next

If the nested session starts, read [minimal configuration](minimal-config.md) and [first session](first-session.md). If it does not, start with [troubleshooting](../troubleshooting/README.md).
