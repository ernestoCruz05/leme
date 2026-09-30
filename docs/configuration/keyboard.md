# Keyboard

A `keyboard` block holds up to four ordered XKB layouts, XKB options, and key repeat settings:

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

Each `layout` entry takes an XKB layout name and an optional variant. Leme compiles the entries into one keymap and starts with the first group. The `cycle_keyboard_layout` command selects the next group. A block without `layout` entries uses `us`.

Without a `keyboard` block, Leme uses the `us` layout, no options, and the default repeat settings.

## Options

`options` takes one or more XKB options, as separate words or as one comma-separated word. Common ones:

| Option | Effect |
| --- | --- |
| `caps:escape` | Caps Lock is another Esc |
| `caps:swapescape` | Caps Lock and Esc swap |
| `ctrl:nocaps` | Caps Lock is another Ctrl |
| `ctrl:swapcaps` | Caps Lock and left Ctrl swap |
| `compose:ralt` | Right Alt is a Compose key |
| `lv3:ralt_switch` | Right Alt selects the third level |
| `altwin:swap_alt_win` | Alt and Super swap |
| `grp:caps_toggle` | Caps Lock selects the next layout; Shift+Caps Lock is Caps Lock |
| `grp:alt_shift_toggle` | Alt+Shift selects the next layout |

The full list is in `/usr/share/X11/xkb/rules/evdev.lst`. Configured options replace `XKB_DEFAULT_OPTIONS` from the environment.

`grp:*` options switch layouts inside XKB. Leme follows the switch: the layout it reports and your other physical keyboards change with it, and `cycle_keyboard_layout` continues from there. `terminate:ctrl_alt_bksp` has no effect under Wayland; bind `quit` for that.

### Switching layouts with Caps Lock

Either use `options grp:caps_toggle`, or bind the key:

```scfg
binds "common" {
    Caps_Lock cycle_keyboard_layout
}
```

A binding on Caps Lock, Shift Lock, or Num Lock runs its command without toggling the lock. Shift+Caps Lock does not match that binding, so it still turns Caps Lock on and off. Avoid `caps:none`: it removes the key's symbol, so no binding can match it.

## Key repeat

`repeat_rate` is the number of repeats per second, from 0 to 1000; 0 turns repeat off. `repeat_delay` is the time in milliseconds a key must be held before it repeats, from 1 to 10000. The defaults are 25 and 600. Repeat applies to every keyboard, including virtual keyboards from remote desktop clients.

## Bindings and layouts

Bindings match the symbol a key produces in the active layout. When nothing matches, Leme tries the same physical key in the other layouts of that keyboard's keymap, in order. With `us` and `ru`, for example, `SUPER+q` still works while `ru` is active. A binding that matches the active layout always takes priority.

## Reloading

A configuration reload applies new repeat settings. When layouts or options change, the reload also compiles and applies the new keymap and returns to the first layout; a reload that leaves them unchanged keeps the active layout. If the new keymap does not compile, Leme keeps the current configuration.

The keybinding command and `timao` query for the active group are documented in the [command reference](../reference/commands.md) and [timao reference](../reference/timao.md).
