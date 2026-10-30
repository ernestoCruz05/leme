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

## Desktop containers

Clicking a container member, its dialog or its frame raises the whole group.
Hover focus does not raise it. Clicking an inner gap focuses the remembered
member; the frame itself never takes client keyboard focus.

Hold Shift (the `container_modifier` setting) while dragging a window to group
it with the window or group under the cursor. `container_join DIR` groups the
focused window from the keyboard.

Drag the frame titlebar to move the group. SUPER-left on a member begins a detach
only after moving more than four logical pixels and leaving the frame. The drag
then continues as a standalone window move. Cancelling after detachment does not
rebuild the old group. Release inside the frame leaves membership unchanged.
`move DIR` swaps the focused member with its neighbor.

Outer edges resize the frame; the `resize` command changes the inner layout. In a
dwindle group, SUPER-right drag on a member moves the split.
`move_view_to_tag` and `move_view_to_output` move only the selected member and
detach it first. Dragging the whole group across outputs requires
`cross_output_drag` and a desktop destination. Whole containers cannot merge.
See [containers](layouts.md#containers) for placement, insertion and lifecycle
behavior.

## Input ownership

A focused client may inhibit keyboard shortcuts. While inhibition is active, ordinary compositor bindings and SUPER pointer grabs go to that client. `switch_vt`, `quit` and `toggle_shortcuts_inhibit` remain reserved commands.

`toggle_shortcuts_inhibit` releases the focused window's inhibition, so every binding works again, and running it again restores it. The release lasts while that window keeps focus: when focus leaves and returns, the window inhibits shortcuts again.
