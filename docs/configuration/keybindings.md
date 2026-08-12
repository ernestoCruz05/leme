# Keybindings

A binding starts with a key combination, followed by a command and its arguments:

```scfg
binds "common" {
    SUPER+Return spawn foot
    SUPER+space mode resize
}

binds "resize" {
    h resize left 32
    j resize down 32
    k resize up 32
    l resize right 32
    Escape mode common
}
```

The supported modifiers are `SUPER`, `ALT`, `CTRL` or `CONTROL`, and `SHIFT`. Their order does not matter, but each may appear only once. Matching uses the exact modifier combination, so an extra held modifier prevents the binding from firing.

The complete command list is in the [command reference](../reference/commands.md).

## Modes

Every `binds "name"` block creates a persistent keyboard mode. A mode named `common` is required. Modes replace the keymap rather than extending it: only the active mode's bindings are checked, and every other key goes to the focused client.

By default, Escape returns to `common` before ordinary mode lookup. A mode can keep Escape for the client:

```scfg
binds "game" {
    escape_exits false
    SUPER+SHIFT+g mode common
}
```

A mode with `escape_exits false` must bind a `mode` command. This prevents a configuration edit from creating a mode with no keyboard-only way out. The setting belongs to a `binds` block, not a `bind_group`.

`switch_vt` is unavailable in nested and headless sessions. `switch_vt` and `quit` remain emergency compositor actions while a client inhibits shortcuts or the session is locked.

## Binding groups

A `bind_group` stores bindings for reuse. It is not a mode and cannot be selected with `mode`:

```scfg
bind_group "movement" {
    ALT+Left focus left
    ALT+Right focus right
}

binds "common" {
    inherit movement
    SUPER+Return spawn foot
}
```

Groups may inherit other groups. Resolution happens when the configuration loads, so the active mode receives a flat list of bindings.

Rules for inheritance are:

- a direct binding overrides one inherited from a group;
- two different groups binding the same key is an error;
- reaching the same group through several paths is allowed;
- repeating a binding in one block is an error;
- `inherit` accepts group names, not mode names;
- inheritance cycles are errors;
- a name cannot be used for both a group and a mode;
- unused and empty groups are allowed.

The full group and mode example is in [`config/leme-full.scfg`](../../config/leme-full.scfg). The [timao scripting guide](../guides/writing-timao-scripts.md) shows how an external script can select a mode.
