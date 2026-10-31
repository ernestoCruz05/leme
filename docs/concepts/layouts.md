# Layouts

Each tag has a layout, set in [tags](../configuration/tags.md) or changed live
with `set_layout` and `switch_layout`. Live changes are not written to the
config, and a reload keeps them unless that tag's settings in the file changed.

## Dwindle

A new window splits the focused tile along its longer side: side by side when
it is wide, top and bottom when it is tall. `split_ratio` sets how much the old
tile keeps. Existing splits never flip when you resize.

## Master-stack

The first `nmaster` windows share the master area, which takes `mfact` of the
width. The rest stack beside it. SUPER-drag reordering only works on dwindle
tags; use `move` here.

## Desktop (experimental)

Windows are placed freely, with titlebars, minimizing, window groups and the
desktop folder as icons. See [desktop layout](../configuration/desktop.md).
