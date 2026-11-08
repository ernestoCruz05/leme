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

## Expressions

`act` runs an [action expression](../reference/timao.md#expressions) when the
key is pressed, so one key can do different things depending on the state.
Write the expression in single quotes, which keep the double quotes inside it
and can span lines. A variable can hold a part you use more than once:

```scfg
vars {
    layout '(get (get (get (session) "focused_output") "active_tag") "layout")'
}

binds "common" {
    SUPER+o act '(if (= $layout "desktop")
                  (command "show_desktop" (list))
                  (command "toggle_floating" (list)))'
}
```

Leme checks the expression when it loads the config, and logs
`act binding failed` when one fails on a press.

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

## Gestures

Touchpad gestures go in `binds` blocks like keys, so modes and groups work for
them too:

```scfg
binds "common" {
    swipe:3:up          spawn wofi --show drun
    SUPER+swipe:4:down  close_view
    pinch:3:in          toggle_fullscreen
    hold:4              mode resize
    swipe:4             gesture_move
    SUPER+swipe:4       gesture_resize
}
```

| Gesture | Fingers | Direction |
| --- | --- | --- |
| `swipe:F:DIR` | 3 to 5 | `up`, `down`, `left`, `right` |
| `swipe:F` | 3 to 5 | none, only with `gesture_move` or `gesture_resize` |
| `pinch:F:DIR` | 2 to 5 | `in`, `out` |
| `hold:F` | 1 to 5 | none |
| `edge:SIDE:DIR` | 1 | `up`, `down` on the `left` and `right` edges; `left`, `right` on the `top` and `bottom` edges |

Swipes, pinches and holds run their command when you lift your fingers.
Directions follow your fingers, whatever `natural_scroll` says, and `repeat`
does not work on gestures.
[`gesture_move` and `gesture_resize`](../reference/commands.md#gestures) move
or resize the window under the pointer with your fingers, like a SUPER-drag.

Gestures without a binding go to the app under the pointer, so pinch to zoom
still works in browsers.

The [tag swipe](gestures.md) owns the left and right swipes on its finger
count. With the default of 3 fingers, `swipe:3:left`, `swipe:3:right` and
`swipe:3` are errors.

### Touchpad edges

An edge binding runs its command each time one finger slides `step` mm along
that edge of the touchpad, so a pair of them works like a slider:

```scfg
binds "common" {
    edge:left:up       spawn wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+
    edge:left:down     spawn wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-
    edge:bottom:right  spawn playerctl position 5+
    edge:bottom:left   spawn playerctl position 5-
}
```

The slide has to start within `size` mm of the edge and move along it right
away. Anything else is a normal touch. `size` and `step` are set in the
[gestures block](gestures.md#touchpad-edges).
