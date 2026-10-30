# Floating and fullscreen

Ordinary resizable windows enter the focused tag's layout. Dialogs and child
windows with a parent start floating. Fixed-size prompts without a parent also
start floating, so they do not become another tile in the main layout.

Tiles and container slots keep the size the layout gives them. If an app draws
larger than its tile, for example because the tile is narrower than the app's
minimum size, the part outside the tile is cut off. Its popups and menus are not
cut.

A managed child with a valid parent keeps its parent's output and tag. Window
rules can still set the child's floating state, fullscreen state, and opacity.
See [window rules](../configuration/window-rules.md) for matching and placement.

## Floating views

Floating views keep their own position and size. SUPER-left drag moves one, and SUPER-right drag resizes it from the nearest edge or corner. Client-requested move and resize operations require floating geometry and valid focus and grab state. Desktop layout uses floating geometry without changing the explicit floating preference. Maximized windows must be restored before moving or resizing in place.

## Container members

On desktop tags, [containers](layouts.md#containers) own their members' geometry.
A member keeps its explicit floating preference, but client geometry requests
cannot move or resize its assigned tile. Detach it before changing that preference
or using standalone floating resize. A member's maximize request changes the
container's placement; individual tiles are not reported as maximized.

Fullscreen keeps the member in its container and retains its layout slot. Other
members and the frame stay hidden until fullscreen ends. A fullscreen request
from a minimized member restores the group first. Container dialogs remain
separate clients and never occupy layout slots.

## Sticky floating views

`toggle_sticky` keeps a managed non-fullscreen window above every tag on its
current output. Sticky windows remain floating, support normal move and resize,
and may coexist. They share the durable overlay with the shown scratchpad,
above tagged floating and fullscreen content but below top/overlay layer-shell
and lock content. When a fullscreen window is covering layer surfaces, the
durable overlay rises with it and stays above, so a summoned scratchpad remains
reachable over fullscreen content. Focus orders complete durable units without reversing a
root's managed child-window order.

Managed child windows and nested dialogs join their sticky root as one group.
Toggling the root again attaches the complete group as floating content to the
owner output's current tag. Moving it to a tag does the same on the requested
tag. If its output disappears, the group migrates to a usable output; with no
usable output it is suspended, excluded from focus/publication/capture, and
restored without stealing focus when the first output returns.

## Fullscreen views

Fullscreen hides the other views on the current tag from navigation and
rendering. A fullscreen view is always fully opaque, even when a style or
window rule requests a lower opacity. This keeps it eligible for direct
scanout, where the display presents the application's buffer without
compositing it with other content.

By default a fullscreen view covers the background, bottom, and top layer-shell
surfaces and takes the whole output, so bars and most notifications stay behind
it. `fullscreen_covers` in [appearance](../configuration/appearance.md) changes
which layers it may cover. Stacking is decided per window, so a fullscreen
window on one output never hides a bar on another.

Entering fullscreen from a sticky root first
attaches its complete group to the owner output's current tag and preserves the
floating frame as the normal fullscreen restore box. A fullscreen window cannot
become sticky directly.

Eligible server-decorated windows in [desktop layout](layouts.md#titlebars-and-window-placement) have titlebars and window controls. Other layouts use borders only. Opacity, border colors, and border width are configured in [appearance](../configuration/appearance.md).
