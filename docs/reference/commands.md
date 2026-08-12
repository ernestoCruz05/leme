# Commands

These commands can be bound in scfg. Most can also be sent through the control socket. The socket refuses `spawn`, `quit`, and `switch_vt`; see the [control protocol](control-protocol.md).

Directions are `left`, `right`, `up`, and `down`. Tag ids, counts, and pixel amounts are positive decimal integers.

| Command | Behavior |
| --- | --- |
| `focus_next_tag` | Move forward through the navigable tag ring. |
| `focus_previous_tag` | Move backward through the navigable tag ring. |
| `focus_tag ID` | Select a pinned, occupied, or empty candidate tag up to `maximum`. |
| `focus_last_tag` | Swap the current and previous tag states, including a candidate. |
| `focus_previous_view` | Focus the most recently focused alternative on the current tag. |
| `focus DIR` | Focus the nearest visible view in a direction. |
| `move DIR [PIXELS]` | Swap a tiled view with its directional neighbor, or move a floating view. The default is 40 pixels. |
| `move_view_to_tag next\|previous\|ID [follow]` | Move the focused view to an adjacent or numbered tag. `follow` selects the destination. |
| `focus_output DIR\|NAME` | Focus an adjacent output or named connector. |
| `move_view_to_output DIR\|NAME [follow]` | Move the focused view to the target output's current tag. `follow` also moves focus and the pointer. |
| `set_layout dwindle\|master_stack\|accordion` | Set the focused tag's layout and retile its views. |
| `switch_layout` | Cycle dwindle, master-stack, and accordion. |
| `remove_empty_tag ID` | Remove an empty non-initial materialized tag. |
| `toggle_floating` | Toggle the focused managed view between tiled and floating. |
| `toggle_fullscreen` | Toggle fullscreen for the focused managed view. |
| `scratchpad_send` | Put the focused managed view in the global scratchpad pool. A normal tagged view becomes unnamed. |
| `scratchpad_toggle [NAME]` | Without a name, show or hide the newest unnamed pool member. With a configured name, show, hide, move, adopt, or start that named scratchpad. |
| `scratchpad_retrieve` | Return the shown scratchpad to the focused tag, or return the newest unnamed member when none is shown. |
| `resize DIR PIXELS` | Resize the focused view along one edge. The amount must be greater than zero. |
| `close_view` | Ask the focused client to close. |
| `spawn PROGRAM [ARG ...]` | Start one detached process with the listed argument vector. |
| `reload_config` | Parse, validate, and apply the configuration file. |
| `mode NAME` | Select an existing binding mode. |
| `cycle_keyboard_layout` | Select the next configured XKB group. |
| `switch_vt N` | Change to VT 1 through 12 through the active libseat session. |
| `quit` | Terminate the compositor. |

Directional focus and tiled movement consider visible managed views on the focused output's current tag. A fullscreen view hides its siblings. Floating movement is not clamped to output edges.

Scratchpads use one global pool and Leme shows at most one member at a time. A
shown member can move to the focused output without joining that output's tag.
Scratchpad commands refuse while the session is locked. See
[scratchpads](../configuration/scratchpads.md) for configuration, matching, and
output behavior.

`move_view_to_tag next` and `previous` use the same tag ring as focus navigation. Moving to an unmaterialized tag creates it up to `maximum`.

At an output edge, `cross_output_focus`, `cross_output_move`, and `cross_output_drag` determine whether focus, keyboard movement, or pointer drag can cross to the adjacent output. `switch_vt` is unavailable in nested and headless sessions.

See [keybindings](../configuration/keybindings.md) for binding syntax and [concepts](../concepts/README.md) for navigation behavior.
