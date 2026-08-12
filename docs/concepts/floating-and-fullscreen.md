# Floating and fullscreen

Ordinary resizable windows enter the focused tag's layout. Dialogs and child
windows with a parent start floating. Fixed-size prompts without a parent also
start floating, so they do not become another tile in the main layout.

A managed child with a valid parent keeps its parent's output and tag. Window
rules can still set the child's floating state, fullscreen state, and opacity.
See [window rules](../configuration/window-rules.md) for matching and placement.

## Floating views

Floating views keep their own position and size. SUPER-left drag moves one, and SUPER-right drag resizes it from the nearest edge or corner. Client-requested move and resize operations are accepted only when the view is floating and the request has valid focus and grab state.

## Fullscreen views

Fullscreen hides the other views on the current tag from navigation and
rendering. A fullscreen view is always fully opaque, even when a style or
window rule requests a lower opacity. This keeps it eligible for direct
scanout, where the display presents the application's buffer without
compositing it with other content.

Leme draws borders but no titlebars or buttons. Opacity, border colors, and border width are configured in [appearance](../configuration/appearance.md).
