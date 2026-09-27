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

The full list is in `/usr/share/X11/xkb/rules/evdev.lst`. Configured options replace `XKB_DEFAULT_OPTIONS` from the environment.

Avoid `grp:*` layout-switching options. They change the group inside XKB without Leme knowing, so the active layout that Leme reports becomes wrong. Bind `cycle_keyboard_layout` instead. `terminate:ctrl_alt_bksp` has no effect under Wayland; bind `quit` for that.

## Key repeat

`repeat_rate` is the number of repeats per second, from 0 to 1000; 0 turns repeat off. `repeat_delay` is the time in milliseconds a key must be held before it repeats, from 1 to 10000. The defaults are 25 and 600. Repeat applies to every keyboard, including virtual keyboards from remote desktop clients.

## Bindings and layouts

Bindings match the symbol a key produces in the active layout. When nothing matches, Leme tries the same physical key in the other layouts of that keyboard's keymap, in order. With `us` and `ru`, for example, `SUPER+q` still works while `ru` is active. A binding that matches the active layout always takes priority.

## Reloading

A configuration reload applies new repeat settings. Layout and option changes need a restart: a reload that changes them fails with "unsupported live keyboard keymap change", and Leme keeps the current configuration.

The keybinding command and `timao` query for the active group are documented in the [command reference](../reference/commands.md) and [timao reference](../reference/timao.md).
