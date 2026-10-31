# Keybindings

A `binds "MODE"` block maps keys to [commands](../reference/commands.md). Each
line is a key combination, the command and its arguments. A mode named
`common` is required and is active at startup.

```scfg
binds "common" {
    SUPER+Return spawn foot
    SUPER+q close_view
    SUPER+r mode resize
}

binds "resize" {
    h repeat resize right -32
    l repeat resize right 32
    k repeat resize down -32
    j repeat resize down 32
    Escape mode common
}
```

The modifiers are `SUPER`, `ALT`, `CTRL` (or `CONTROL`) and `SHIFT`, in any
order. A binding only fires with exactly those modifiers held. Keys are named by
their unshifted symbol, so it is `SUPER+SHIFT+1`, not `SUPER+exclam`.

`repeat` before the command fires it again while the key is held, at the
[keyboard](keyboard.md) repeat rate.

`grid_*` commands only take the key while the desktop file grid has focus, so
plain keys like `Delete` or `F2` can be bound to them.

## Modes

Each `binds` block is a mode, and `mode NAME` switches to it. Only the active
mode's bindings work, and other keys go to the focused window. Escape goes back
to `common` unless the mode turns that off, in which case it must bind its own
way out:

```scfg
binds "common" {
    SUPER+SHIFT+g mode game
}

binds "game" {
    escape_exits false
    SUPER+SHIFT+g mode common
}
```

## Binding groups

A `bind_group` holds bindings that modes pull in with `inherit`:

```scfg
bind_group "vt" {
    for i in 1..12 {
        CTRL+ALT+F$i switch_vt $i
    }
}

binds "common" {
    inherit vt
    SUPER+Return spawn foot
}
```

Groups can inherit other groups. A binding written in the mode wins over an
inherited one, but two groups binding the same key is an error. Groups and
modes cannot share a name.

## When an app grabs the keyboard

Remote desktops and virtual machines can inhibit compositor shortcuts. Only
`quit`, `switch_vt` and `toggle_shortcuts_inhibit` still work then, so bind the
last one to get your keys back:

```scfg
binds "common" {
    SUPER+SHIFT+Escape toggle_shortcuts_inhibit
}
```
