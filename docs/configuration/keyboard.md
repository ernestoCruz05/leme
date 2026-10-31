# Keyboard

The `keyboard` block sets XKB layouts, options and key repeat for every
keyboard.

```scfg
keyboard {
    layout pt
    layout us
    layout us intl
    options caps:escape compose:ralt
    repeat_rate 40
    repeat_delay 250
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `layout` | an XKB layout and an optional variant | `us` | adds a layout, up to four |
| `options` | XKB options, as words or comma-separated | none | replaces `XKB_DEFAULT_OPTIONS` |
| `repeat_rate` | `0` to `1000` | `25` | repeats per second, `0` turns repeat off |
| `repeat_delay` | `1` to `10000` ms | `600` | how long a key is held before it repeats |

The session starts on the first layout, and `cycle_keyboard_layout` moves to
the next. Bindings still work on the other layouts: when nothing matches in the
active one, Leme tries the same physical key in the others.

A reload that changes the layouts or options goes back to the first layout.

## Options

The full list is in `/usr/share/X11/xkb/rules/evdev.lst`. Some common ones:

| Option | Effect |
| --- | --- |
| `caps:escape` | Caps Lock is another Esc |
| `caps:swapescape` | Caps Lock and Esc swap |
| `ctrl:nocaps` | Caps Lock is another Ctrl |
| `ctrl:swapcaps` | Caps Lock and left Ctrl swap |
| `compose:ralt` | Right Alt is a Compose key |
| `lv3:ralt_switch` | Right Alt selects the third level |
| `altwin:swap_alt_win` | Alt and Super swap |
| `grp:caps_toggle` | Caps Lock selects the next layout, Shift+Caps Lock is Caps Lock |
| `grp:alt_shift_toggle` | Alt+Shift selects the next layout |

`terminate:ctrl_alt_bksp` does nothing under Wayland; bind `quit` instead.

To switch layouts with Caps Lock without an XKB option, bind it. The lock does
not toggle, and Shift+Caps Lock still does:

```scfg
binds "common" {
    Caps_Lock cycle_keyboard_layout
}
```

Don't combine that with `caps:none`, which removes the key so no binding can
match it.
