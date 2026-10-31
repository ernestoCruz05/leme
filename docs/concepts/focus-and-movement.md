# Focus and movement

`focus DIR` and `move DIR` pick the nearest window on screen in that direction,
by geometry. They only look at the focused output's tag, and a fullscreen window
hides the others. At an output edge, `cross_output_focus` and
`cross_output_move` decide whether they continue onto the next output.

## Mouse

| Action | Floating window | Tiled window |
| --- | --- | --- |
| SUPER-drag | moves it | lifts it out and shows where it will drop |
| SUPER-right-drag | resizes from the nearest edge or corner | moves the split at that edge |

Escape during a tiled drag puts the window back. Dragging onto another output
needs `cross_output_drag`. On desktop tags, windows also move by their
titlebar; see [desktop layout](../configuration/desktop.md).

## Sticky windows

A sticky window stays on its output across tags. Directional focus skips it,
and it only takes focus on a tag switch when the new tag has no windows.

## Apps that grab the keyboard

Remote desktops and virtual machines can inhibit Leme's shortcuts. While they
do, bindings and SUPER-drags go to the app, except `quit`, `switch_vt` and
`toggle_shortcuts_inhibit`. That last one takes the keyboard back until the app
loses focus.
