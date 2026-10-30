# Commands

These commands can be bound in scfg. Most can also be sent through the control socket. The socket refuses `spawn`, `quit`, and `switch_vt`; see the [control protocol](control-protocol.md).

Directions are `left`, `right`, `up`, and `down`. Tag ids, counts, and pixel amounts are positive decimal integers.

| Command | Behavior |
| --- | --- |
| `focus_next_tag [occupied]` | Move forward through the navigable tag ring. With `occupied`, skip to the next tag that has a window, wrapping past the last one; nothing happens when no other tag has a window. |
| `focus_previous_tag [occupied]` | Move backward through the navigable tag ring. `occupied` skips empty tags the same way. |
| `focus_tag ID` | Select a pinned, occupied, or empty candidate tag up to `maximum`. |
| `focus_last_tag` | Swap the current and previous tag states, including a candidate. |
| `focus_previous_view` | Focus the most recently focused alternative on the current tag. |
| `focus DIR` | Focus the nearest visible view in a direction. |
| `move DIR [PIXELS]` | Swap a tiled view or container member with its directional neighbor, or move a floating view. The default is 40 pixels. With no neighbor in that direction, nothing moves and IPC reports `noop`. |
| `move_view_to_tag next\|previous\|ID [follow]` | Move the focused view to an adjacent or numbered tag. `follow` selects the destination. |
| `focus_output DIR\|NAME` | Focus an adjacent output or named connector. |
| `move_view_to_output DIR\|NAME [follow]` | Move the focused view to the target output's current tag. `follow` also moves focus and the pointer. |
| `set_layout dwindle\|master_stack\|accordion\|desktop` | Set the focused tag's layout. |
| `switch_layout` | Cycle dwindle, master-stack, accordion, and desktop. |
| `remove_empty_tag ID` | Remove an empty non-initial materialized tag. |
| `toggle_floating` | Toggle the focused managed view's floating preference. On desktop tags, the window stays freely placed until the layout changes. Sticky views refuse this command. |
| `toggle_sticky` | Keep the focused managed window visible across tags on its current output, or attach its complete sticky group to that output's current tag. Takes no arguments. |
| `toggle_fullscreen` | Toggle fullscreen for the focused managed view. A sticky group attaches to its owner output's current tag first. A tag has at most one fullscreen view: making another view fullscreen returns the previous one to its place. |
| `scratchpad_send` | Put the focused managed view in the global scratchpad pool. A normal tagged view becomes unnamed. |
| `scratchpad_toggle [NAME]` | Without a name, show or hide the newest unnamed pool member. With a configured name, show, hide, move, adopt, or start that named scratchpad. |
| `scratchpad_retrieve` | Return the shown scratchpad to the focused tag, or return the newest unnamed member when none is shown. |
| `resize DIR PIXELS` | Resize the focused view along one edge, or adjust its container's inner layout. The amount must be greater than zero. |
| `container_layout dwindle\|master_stack\|accordion` | Set the focused member's container layout. Does not change the tag layout. |
| `container_detach` | Detach the focused member at its current content location. |
| `container_dissolve` | Release every member of the focused member's container as a normal desktop window. |
| `container_join left\|right\|up\|down` | Group the focused standalone desktop window with the nearest window or container member in that direction. |
| `close_view` | Ask the focused client to close. |
| `minimize_view` | Hide the focused eligible desktop window or its whole container. Dialogs inherit hiding through the same-tag parent chain. |
| `restore_minimized` | Restore minimized windows and containers on the focused output's current desktop tag, once per group. No-op when none exist. |
| `snap left\|right\|up\|down` | Move the focused desktop window, or its container, to the next placement in that direction. |
| `place normal\|maximized\|left\|right\|top_left\|top_right\|bottom_left\|bottom_right` | Give the focused desktop window, or its container, that placement. `normal` restores its saved size and position. |
| `window_menu` | Open the focused desktop window's titlebar menu, or its container's menu, at the window's top-left corner. |
| `show_desktop` | Minimize every window on the focused desktop tag. Run it again to restore the windows it minimized. |
| `focus_cycle next\|previous` | Focus the next or previous window on the focused tag, wrapping at the ends. Minimized windows are skipped. |
| `focus_desktop` | Give keyboard focus to the file grid on the focused output's desktop tag. |
| `desktop_menu` | Focus the file grid and open its menu: the entry menu for the selection, or the blank-space menu when nothing is selected. |
| `grid_open` | Open every selected grid entry. |
| `grid_trash` | Move the selected grid entries to Trash. |
| `grid_rename` | Rename the selected grid entry. Exactly one entry must be selected. |
| `grid_new_folder` | Open the New folder prompt. |
| `grid_select_all` | Select every entry in the grid. |
| `grid_clear` | Clear the grid selection. |
| `grid_select left\|right\|up\|down [extend]` | Select the nearest grid entry in that direction. With `extend`, select every entry in reading order from the first selected entry to that one. |
| `spawn PROGRAM [ARG ...]` | Start one detached process with the listed argument vector. |
| `reload_config` | Parse, validate, and apply the configuration file. A successful reload also refreshes desktop artwork. |
| `mode NAME` | Select an existing binding mode. |
| `cycle_keyboard_layout` | Select the next configured XKB group. |
| `toggle_shortcuts_inhibit` | Release the focused window's shortcut inhibition so compositor bindings work again, or restore it. Works while shortcuts are inhibited. |
| `switch_vt N` | Change to VT 1 through 12 through the active libseat session. |
| `quit` | Terminate the compositor. |

Minimization is desktop-only. Leaving desktop layout restores its minimized
windows. Restore-all does not replace a valid current focus; clicking a recovery
icon or explicitly focusing a minimized window restores and focuses it. The
built-in defaults and example configuration use SUPER+m and SUPER+SHIFT+m.
See [desktop minimization](../concepts/layouts.md#minimized-windows) for eligibility,
family behavior and the optional recovery strip.

`snap`, `place` and `window_menu` need a focused window on a desktop tag and act
on its whole container when it is a member. Fullscreen windows refuse them, and
placements that do not fit a window's size limits are refused. `snap` steps
through placements as follows, where `-` leaves the placement unchanged:

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

`show_desktop` minimizes when any window on the focused desktop tag is visible
and restores otherwise. It restores only the windows it minimized itself, so
windows that were already minimized stay hidden.

The `grid_*` commands act only while the file grid has keyboard focus. A binding
to one of them does not take its key away from applications: while a window has
focus, the key goes to that window. This makes plain keys usable:

```scfg
binds "common" {
    Delete grid_trash
    F2 grid_rename
    CTRL+a grid_select_all
    SHIFT+Right grid_select right extend
}
```

Without such bindings, Return, Escape and the arrow keys still open, clear and
move the selection while the grid has focus. None of these commands have default
bindings.

`container_layout`, `container_detach` and `container_dissolve` require a focused
member on the current desktop tag. `container_join` requires a focused standalone
desktop window and fails when nothing eligible lies in that direction. None of
them have default bindings. `container_layout` rejects `desktop`. Locked,
fullscreen and inhibited contexts refuse these actions. A member's ordinary
`close_view` closes only that client; the frame Close button requests all current
members. Moving a member to another tag or output detaches it, while moving the
frame carries the whole group. Detach before `toggle_floating` or standalone
floating move/resize. See [containers](../concepts/layouts.md#containers).

Directional focus and tiled movement consider visible managed views on the focused output's current tag. A fullscreen view hides its siblings. Floating movement is not clamped to output edges. Desktop windows use floating move and resize behavior even when their explicit floating preference is off.

`toggle_sticky` makes the focused managed window floating and keeps it visible
on its current output across tag changes. Running it again attaches the complete
sticky group to that output's current tag. It refuses fullscreen windows,
scratchpad members, and locked sessions. Managed child windows join their
sticky root's group; owner-changing commands applied to a dependent resolve to
the root. Sticky move, resize, and move-to-output operations retain sticky
ownership, while move-to-tag and fullscreen end it transactionally.

Scratchpads use one global pool and Leme shows at most one member at a time. A
shown member can move to the focused output without joining that output's tag.
Scratchpad commands refuse while the session is locked. See
[scratchpads](../configuration/scratchpads.md) for configuration, matching, and
output behavior.

`move_view_to_tag next` and `previous` use the same tag ring as focus navigation. Moving to an unmaterialized tag creates it up to `maximum`.

At an output edge, `cross_output_focus`, `cross_output_move`, and `cross_output_drag` determine whether focus, keyboard movement, or pointer drag can cross to the adjacent output. `switch_vt` is unavailable in nested and headless sessions.

See [keybindings](../configuration/keybindings.md) for binding syntax and [concepts](../concepts/README.md) for navigation behavior.
