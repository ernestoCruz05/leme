# Floating and fullscreen

Ordinary resizable windows enter the focused tag's layout. Dialogs and child
windows with a parent start floating. Fixed-size prompts without a parent also
start floating, so they do not become another tile in the main layout.

A managed child with a valid parent keeps its parent's output and tag. Window
rules can still set the child's floating state, fullscreen state, and opacity.
See [window rules](../configuration/window-rules.md) for matching and placement.

## Floating views

Floating views keep their own position and size. SUPER-left drag moves one, and SUPER-right drag resizes it from the nearest edge or corner. Client-requested move and resize operations are accepted only when the view is floating and the request has valid focus and grab state.

## Sticky floating views

`toggle_sticky` keeps a managed non-fullscreen window above every tag on its
current output. Sticky windows remain floating, support normal move and resize,
and may coexist. They share the durable overlay with the shown scratchpad,
above tagged floating and fullscreen content but below top/overlay layer-shell
and lock content. Focus orders complete durable units without reversing a
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
compositing it with other content. Entering fullscreen from a sticky root first
attaches its complete group to the owner output's current tag and preserves the
floating frame as the normal fullscreen restore box. A fullscreen window cannot
become sticky directly.

Leme draws borders but no titlebars or buttons. Opacity, border colors, and border width are configured in [appearance](../configuration/appearance.md).
