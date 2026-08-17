# Focus and movement

Leme chooses directional neighbors from window geometry rather than tree order. Focus and movement consider visible managed views on the focused output's current tag. A fullscreen view hides its siblings from navigation.

## Keyboard navigation

Directional focus, tiled swaps, previous-view focus, last-tag switching, tag movement, and output movement are all available as commands. A command can move a view to another tag without following it, or select the destination with the `follow` argument.

At an output edge, focus may continue to an adjacent output when `cross_output_focus` is enabled. Moving a view across that edge is a separate choice controlled by `cross_output_move`. The exact syntax is in [keybindings](../configuration/keybindings.md) and [commands](../reference/commands.md).

Output-presented sticky windows participate in global previous-focus history on
their owner output. Directional focus deliberately excludes them and continues
to search only tag-owned layout content. A tag change prefers the destination
tag's own most recent eligible view; a sticky window takes keyboard focus only
when that tag has none, which also covers closing the last window on a tag.
Waking or migrating an output never hands focus to a sticky window.

## Pointer operations

Holding SUPER provides compositor pointer operations:

- left drag moves a floating view;
- right drag resizes a floating view from its nearest edge or corner;
- left drag detaches a tiled view at its original size and previews its insertion point;
- right drag on a tiled view adjusts the split boundary at the selected edge or corner.

A cancelled tiled move restores the original tree. Tiled resize ratios remain between `0.10` and `0.90`.

A pointer drag can carry a view to another output when `cross_output_drag` is enabled. When it is disabled, a floating view stays within the source output and a tiled view has no drop target beyond that output. Dragging a sticky window across an allowed boundary changes its durable output owner rather than attaching it to a tag; its complete transient group follows and is reanchored inside the destination usable area.

## Input ownership

A focused client may inhibit keyboard shortcuts. While inhibition is active, ordinary compositor bindings and SUPER pointer grabs go to that client. `switch_vt` and `quit` remain reserved emergency commands.
