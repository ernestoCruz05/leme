# Layouts

Every tag has a layout. You can change the focused tag at runtime or set persistent defaults in the [`tags` configuration](../configuration/tags.md).

## Dwindle

Dwindle splits the tile containing the focused window. New splits alternate
direction as windows are added. A tiled SUPER-drag previews a new position and
reinserts the window when released.

## Master-stack

Master-stack places the first `nmaster` windows in the master area. `mfact`
sets that area's share of the available width. Remaining windows occupy the
stack beside it. Use `move` and `resize` for adjustments; tiled SUPER-drag is
for dwindle tags.

## Accordion

Accordion keeps non-focused views as narrow strips in tag order and gives the focused view the remaining space. It focuses a strip on click instead of on pointer hover. Otherwise, moving the pointer over a strip could change the geometry underneath it and move focus again.

## Persistent and runtime changes

A `set_layout` command changes the live tag. Runtime layout changes do not rewrite the scfg file. A configuration reload resets a tag only when its effective configured settings changed; unrelated reloads preserve runtime layout and resize adjustments.

Tag changes can remain instant or use an optional visual transition. See
[animation configuration](../configuration/animation.md).

The exact command forms are in the [command reference](../reference/commands.md).
