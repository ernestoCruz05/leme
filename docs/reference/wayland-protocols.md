# Wayland protocols

Leme only advertises a global when the backend and renderer support it.

| Area | Protocols |
| --- | --- |
| windows | `xdg-shell` v6, `xdg-decoration`, KDE `server-decoration`, `xdg-dialog-v1`, `xdg-foreign` v1 and v2, `xdg-activation-v1` |
| shell | `wlr-layer-shell` v5, `ext-session-lock-v1`, `ext-idle-notify-v1`, `idle-inhibit-unstable-v1`, `leme-decoration-v1` |
| input | `relative-pointer`, `pointer-constraints`, `pointer-gestures`, `keyboard-shortcuts-inhibit`, `cursor-shape`, `virtual-keyboard-unstable-v1`, `wlr-virtual-pointer-unstable-v1` |
| clipboard | primary selection, `wlr-data-control`, `ext-data-control` |
| outputs | `xdg-output`, `wlr-output-management`, `wlr-output-power-management`, `viewporter`, `fractional-scale` |
| graphics | `linux-dmabuf` v5 with feedback, `linux-drm-syncobj`, `presentation-time`, `single-pixel-buffer`, `alpha-modifier`, `content-type`, `tearing-control` |
| capture | `wlr-screencopy`, `ext-image-copy-capture-v1` with output and window sources |
| publication | `ext-workspace-v1`, `ext-foreign-toplevel-list-v1`, `wlr-foreign-toplevel-management-v1` |

Not implemented: touch, tablet, text input and input methods, and
`xdg_toplevel.show_window_menu`.

## Windows

Both decoration protocols get server-side decorations. On
[desktop tags](../configuration/desktop.md#windows) that means a titlebar; on
other layouts, a border. Dialogs open floating over their parent, on its tag.
Windows on a hidden tag, in a hidden scratchpad or on an output that is off are
told they are suspended, so they can stop drawing.

Maximize and minimize are only offered on desktop tags. Tearing is only honored
for the focused fullscreen window.

`leme-decoration-v1` is Leme's own protocol for shells that draw window
decorations, see [shell decorations](../guides/shell-decorations.md).

## Activation

An activation token must come from Leme's seat with a recent input serial. A
window on the focused tag is focused. Anywhere else, the
[`activation`](../configuration/outputs.md#activation) setting decides, the same
as for X11 and foreign-toplevel requests. Nothing is activated while the session
is locked.

## Capture

A window capture holds the window and its popups only: no border, nothing drawn
over it, and no XWayland menus. A window on a hidden tag or in a hidden
scratchpad captures as a black frame of its size until it shows again. A
minimized window on the shown tag keeps its last content. Capture ends when the
window closes or the session locks, and a locked session only captures the lock
screen.

`xdg-desktop-portal-luminous` offers window sources. `xdg-desktop-portal-wlr`
only does with a custom chooser. See [screen sharing](../guides/screen-sharing.md).

## Publication

Each output is one workspace group, and its workspaces are the tags you can
navigate to, with ids like `DP-1:3`. Windows are published while mapped, except
hidden scratchpads, X11 override-redirect windows and layer surfaces. XWayland
windows publish their class as the app id.

A tagged window is on its output only while its tag is shown there, so a bar
that lists one output's windows lists its shown tag. Minimized windows stay
published.

Leme creates and destroys a window's `ext-foreign-toplevel-list-v1` and
`wlr-foreign-toplevel-management-v1` handles together, and both lists announce
windows in creation order. A client that binds both in one registry pass can
pair them by order.

| Request | Result |
| --- | --- |
| workspace `activate` | follows the `activation` setting |
| workspace `deactivate`, `create_workspace`, `remove`, `assign` | refused |
| toplevel `close` | closes the window |
| toplevel `set_fullscreen`, `unset_fullscreen` | honored, except for scratchpads |
| toplevel `activate` | follows the `activation` setting; restores a minimized window first |
| toplevel `set_maximized`, `unset_maximized` | honored on desktop tags, without focusing |
| toplevel `set_minimized`, `unset_minimized` | honored on desktop tags, without focusing |
| toplevel `set_rectangle` | ignored |

While locked, the published set freezes and requests are refused.

## Locking and power

Locking covers everything at once and sends input only to the lock surface. If
the locker dies, an opaque blocker stays until a new one locks. Leme does not
start a locker or pick idle timeouts; idle inhibitors only count while their
surface is visible.

`wlr-output-power-management` turns outputs off and on. An output that is off
keeps its tags and windows, and turning it on restores its config.
