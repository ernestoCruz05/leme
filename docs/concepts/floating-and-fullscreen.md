# Floating and fullscreen

Resizable windows tile. Dialogs, windows with a parent, and fixed-size windows
float. A dialog opens on its parent's tag and output.
[Window rules](../configuration/window-rules.md) can change any of this.

A tile keeps the size the layout gives it. An app that draws larger, for
example because its minimum size is bigger than the tile, is cut off at the
tile's edge.

## Floating

`toggle_floating` floats or tiles the focused window. SUPER-drag moves a
floating window and SUPER-right-drag resizes it.

## Sticky

`toggle_sticky` keeps a floating window on screen on its output while you
switch tags. Its dialogs come with it. Running it again puts it back on the
current tag. A sticky window stays above fullscreen windows, like a shown
scratchpad. A fullscreen window cannot be made sticky.

## Fullscreen

`toggle_fullscreen` hides the rest of the tag, and only one window per tag can
be fullscreen. A fullscreen window is always opaque, so it can be shown without
compositing. [`fullscreen_covers`](../configuration/appearance.md#fullscreen)
decides which bars and panels it covers, per output.
