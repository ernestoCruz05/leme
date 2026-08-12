# Keyboard

A `keyboard` block contains one to four ordered XKB layouts:

```scfg
keyboard {
    layout pt
    layout us
    layout us intl
}
```

Each entry takes an XKB layout name and an optional variant. Leme compiles the entries into one keymap and starts with the first group. The `cycle_keyboard_layout` command selects the next group.

Without a `keyboard` block, Leme configures `pt` first and `us` second. The
minimal example selects only `us`, so copy that example if the built-in order
does not match your keyboard.

A successful configuration reload compiles the replacement keymap and returns to its first group. If compilation fails, Leme keeps the current configuration.

The keybinding command and `timao` query for the active group are documented in the [command reference](../reference/commands.md) and [timao reference](../reference/timao.md).
