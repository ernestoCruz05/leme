# Nested session

Run Leme inside your current Wayland desktop first. If it fails, your session
is fine.

```sh
mkdir -p runtime
./build/leme 2>runtime/leme.log &
LEME_PID=$!
```

Then open a terminal in it, using the socket name from its log:

```sh
LEME_DISPLAY="$(awk -F= '/leme: WAYLAND_DISPLAY=/{print $2}' runtime/leme.log | tail -1)"
WAYLAND_DISPLAY="$LEME_DISPLAY" foot
```

Quit with `SUPER+SHIFT+e`, or from the parent shell:

```sh
kill -INT "$LEME_PID"
```

## Keys in a nested session

The parent compositor gets keys first, so shared bindings like `SUPER+Return`
run in the parent. If the parent is Leme, a mode with no bindings passes every
key through:

```scfg
binds "common" {
    SUPER+SHIFT+p mode passthrough
}

binds "passthrough" {
    escape_exits false
    SUPER+SHIFT+p mode common
}
```

Click the nested window and press `SUPER+SHIFT+p`. Programs started inside it,
including `timao`, talk to the nested Leme.

## Headless

```sh
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ./build/leme
```

This checks that Leme starts and loads its config, without a window, real
outputs or input.
