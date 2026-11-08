# Commands

Commands go in [bindings](../configuration/keybindings.md), desktop menus and
`timao`. `timao` cannot run `spawn`, `quit`, `switch_vt` or `act`.

`DIR` is `left`, `right`, `up` or `down`. Ids, counts and pixel amounts are
positive integers unless a row says otherwise.

## Tags

| Command | Effect |
| --- | --- |
| `focus_next_tag [occupied]` | next tag, wrapping; `occupied` skips empty tags |
| `focus_previous_tag [occupied]` | previous tag, wrapping; `occupied` skips empty tags |
| `focus_tag ID` | tag `ID` |
| `focus_last_tag` | the tag you were on before |
| `move_view_to_tag next\|previous\|ID [follow]` | moves the focused window to a tag; `follow` goes with it |
| `remove_empty_tag ID` | removes an empty tag above `initial` |
| `set_layout dwindle\|master_stack\|desktop` | layout of the focused tag |
| `switch_layout` | cycles dwindle, master-stack and desktop |

## Focus and movement

| Command | Effect |
| --- | --- |
| `focus DIR` | the nearest window in that direction |
| `focus_cycle next\|previous` | the next or previous window on the tag, wrapping |
| `focus_previous_view` | the window focused before this one |
| `focus_urgent` | the window that most recently asked for attention |
| `focus_output DIR\|NAME` | an output, by direction or connector name |
| `move DIR [PIXELS]` | swaps a tiled window with its neighbor, or moves a floating one, 40 pixels by default |
| `resize DIR PIXELS` | moves the focused window's edge in that direction; a negative amount shrinks it |
| `move_view_to_output DIR\|NAME [follow]` | moves the focused window to another output; `follow` goes with it |

Whether `focus`, `move` and dragging cross onto the next output is set in
[outputs](../configuration/outputs.md#behavior-across-outputs).

## Windows

| Command | Effect |
| --- | --- |
| `close_view` | asks the focused window to close |
| `toggle_floating` | floats or tiles the focused window |
| `toggle_fullscreen` | fullscreens the focused window, one per tag |
| `toggle_sticky` | keeps the focused window on screen across tags on its output |

## Gestures

These only work on a [swipe binding](../configuration/keybindings.md#gestures)
without a direction, such as `swipe:4 gesture_move`.

| Command | Effect |
| --- | --- |
| `gesture_move` | moves the window under the pointer with your fingers, like a SUPER-drag |
| `gesture_resize` | resizes the window under the pointer with your fingers, like a SUPER-right-drag |

## Scratchpads

| Command | Effect |
| --- | --- |
| `scratchpad_send` | moves the focused window into the pool |
| `scratchpad_toggle [NAME]` | shows or hides the newest pool window, or a [named scratchpad](../configuration/scratchpads.md#named-scratchpads) |
| `scratchpad_retrieve` | puts the shown scratchpad, or the newest one, back on the tag |

## Desktop

These need the focused tag to use the [desktop layout](../configuration/desktop.md).

| Command | Effect |
| --- | --- |
| `minimize_view` | minimizes the focused window or container |
| `restore_minimized` | restores every minimized window on the tag |
| `show_desktop` | minimizes every window on the tag, and restores them on the next run |
| `snap DIR` | steps the focused window through placements, see below |
| `place normal\|maximized\|left\|right\|top_left\|top_right\|bottom_left\|bottom_right` | puts the focused window in that placement |
| `window_menu` | opens the focused window's titlebar menu |
| `container_join DIR` | groups the focused window with the nearest one in that direction |
| `container_layout dwindle\|master_stack` | layout inside the focused container |
| `container_detach` | takes the focused member out of its container |
| `container_dissolve` | breaks up the focused container |

`snap` moves between placements like this, where `-` does nothing:

| From | `left` | `right` | `up` | `down` |
| --- | --- | --- | --- | --- |
| normal | left | right | maximized | - |
| maximized | left | right | - | normal |
| left | - | normal | top_left | bottom_left |
| right | normal | - | top_right | bottom_right |
| top_left | - | top_right | maximized | left |
| top_right | top_left | - | maximized | right |
| bottom_left | - | bottom_right | left | - |
| bottom_right | bottom_left | - | right | - |

## File grid

The `grid_*` commands only take their key while the grid has focus, so they
can be bound to plain keys.

| Command | Effect |
| --- | --- |
| `focus_desktop` | gives the grid keyboard focus |
| `desktop_menu` | focuses the grid and opens its menu |
| `grid_open` | opens the selected entries |
| `grid_rename` | renames the selected entry |
| `grid_trash` | moves the selected entries to Trash |
| `grid_new_folder` | creates a folder |
| `grid_select DIR [extend]` | selects the nearest entry in that direction; `extend` selects the range up to it |
| `grid_select_all` | selects every entry |
| `grid_clear` | clears the selection |

## Session

| Command | Effect |
| --- | --- |
| `spawn PROGRAM [ARG ...]` | starts a program, without a shell |
| `act EXPRESSION` | runs an [action expression](../configuration/keybindings.md#expressions) |
| `reload_config` | reloads the config file |
| `mode NAME` | switches to a [binding mode](../configuration/keybindings.md#modes) |
| `cycle_keyboard_layout` | next keyboard layout |
| `toggle_shortcuts_inhibit` | takes the keyboard back from an app that grabbed it, or gives it back |
| `switch_vt N` | switches to VT 1 to 12 |
| `quit` | exits Leme |
