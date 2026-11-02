# Shell decorations

A shell can draw window titlebars and borders with its own surfaces through
`leme-decoration-v1`. One client decorates at a time. Leme places its surfaces
with each window, reserves the space they ask for and hides its own titlebar
where a shell decoration shows. Without a client, Leme draws its own titlebars.

Every request and event is described in `protocols/leme-decoration-v1.xml` in
the Leme repository.

## Windows

The manager sends `window` for each window, then its `id`, `title`, `app_id`,
`state` and `size`, closed by `done`. Answer each `done` with `ack_configure`
once its decorations are committed. A new window's open animation waits up to
200 ms for that answer. `id` is the window's id on the
[control socket](../reference/control-protocol.md).

A window group on a desktop tag is offered as a window of its own, with the
`group` state. Its decorations frame the whole group and its requests act on
the group. Its members report `grouped` and their decorations stay hidden.

## Decorations

`get_decoration` makes a surface a decoration above or below a window.
`set_offset` places it relative to the content's top left corner, so a 28 px
titlebar sits at `0, -28`. `set_margins` reserves space around the content:
tiled windows give it from their content, floating windows grow to fit it.
`set_border(replaced)` removes Leme's border, so the decoration is the whole
frame.

`set_states` picks where a decoration shows:

| Bit | Value | Window |
| --- | --- | --- |
| `floating` | 1 | floating on a tiling tag |
| `tiled` | 2 | placed by a tiling layout |
| `maximized` | 4 | maximized |
| `fullscreen` | 8 | fullscreen |
| `desktop` | 16 | on a desktop tag |

The default is `23`, everything but fullscreen. `20` decorates only desktop
tags and their maximized windows.

For titlebar buttons, the window object takes `move`, `resize`,
`show_window_menu`, `close`, `set_maximized`, `unset_maximized` and
`set_minimized`. `move` and `resize` need the serial of a press on one of the
window's decorations.

## Limits

Leme does not wait for decorations when it resizes a window, so they can trail
it during a resize. Move animations keep decorations at their size. Leme does
not round or blur decoration buffers. A group's decorations sit under its
members, so they only show in its margins.
