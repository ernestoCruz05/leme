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
