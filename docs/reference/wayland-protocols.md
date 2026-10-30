# Wayland protocols

This page lists the protocol behavior that matters to clients. Leme advertises optional globals only when the backend and renderer can support them.

## Windows and shell surfaces

Leme supports XDG shell toplevels and recursive popups. Transients parented before mapping inherit their parent's output and tag, start floating unless a window rule overrides that preference, and center over the parent when freely placed. Fixed-size unparented prompts start floating; ordinary resizable windows tile.

Leme advertises XDG shell version 6. Fullscreen is available generally. Eligible
mapped desktop windows also advertise maximize and, with a usable recovery route,
minimize. Client size limits can disable maximize. Initial maximize requests are
applied after eligible mapping; fullscreen takes placement precedence. Later
maximize requests during fullscreen are refused. Native minimize requests before
mapping are ignored.

A new window receives the usable area of the focused output as its bounds. A
window that is not on an enabled output, for example on a hidden tag, in a hidden
scratchpad, or on a powered-off output, is marked suspended so its client can stop
rendering.

Layer-shell v5 supports wallpaper, panels, launchers, notifications, overlays, drag surfaces, and lock surfaces. Leme honors exclusive zones and keyboard interactivity. Viewporter and fractional-scale protocols are available to current layer-shell clients.

## Input and data exchange

Leme supports keyboard layouts, relative pointer, pointer constraints, pointer lock, shortcut inhibition, cursor shape, clipboard and primary selection, legacy and standardized data-control, pointer drag and drop, and XWayland selection bridging.

While shortcut inhibition is active, normal compositor bindings and SUPER pointer grabs go to the focused client. `switch_vt` and `quit` remain emergency actions, and `toggle_shortcuts_inhibit` deactivates the inhibitor until the window loses focus or the command runs again.

Leme supports `virtual-keyboard-unstable-v1` and `wlr-virtual-pointer-unstable-v1`. Any client can create a virtual keyboard or pointer. Its input goes to the focused client like physical input, triggers compositor bindings, and reaches the lock screen while the session is locked. A virtual keyboard keeps the keymap its client sends; configured layouts and layout switching apply only to physical keyboards. A virtual pointer created for a specific output moves within that output.

Touch-origin drag and drop, touch, tablet, gesture, text-input, and input-method protocols are not implemented.

## Locking and idle

Leme supports `ext-session-lock-v1`, `ext-idle-notify-v1`, and `idle-inhibit-unstable-v1`. Locking fails closed: normal content is covered immediately, input goes only to the lock surface, and an opaque blocker remains if the locker disappears.

Idle inhibitors count only while their surface is mapped and visible on an enabled output. Leme does not choose idle timeouts or start a locker.

## Capture

Leme exposes `wlr-screencopy-v1`, `ext-image-copy-capture-v1`, output image sources, and logical output metadata. Shared-memory capture is available, and DMA-BUF capture depends on the renderer and backend.

Capture during a lock sees only committed lock content or the opaque blocker.
Compatible clients, including `xdg-desktop-portal-luminous`, can request a
per-window source through
`ext_foreign_toplevel_image_capture_source_manager_v1`. New requests are
refused while locked and for windows that are not currently protocol-eligible.
Tagged windows, shown scratchpads, and output-presented sticky windows are
eligible; a hidden mapped scratchpad or suspended sticky window is not.

A window capture contains only that window and its popups. Borders, other
windows, and layer surfaces drawn over it do not appear. XWayland menus are
separate X11 windows and are not included. While the window is on a hidden
tag, in a hidden scratchpad, or in a suspended sticky group, the capture shows
a black frame of the window's size, and live content returns when the window
is shown again. The capture ends when the window unmaps or the session locks.
Whole-output capture includes presented sticky windows only on their owner
output.

`xdg-desktop-portal-wlr` offers a per-window source only through a custom
chooser. Portal consent, source selection, and PipeWire transport remain
external.

## Graphics and presentation

When supported, Leme advertises linux-dmabuf v5 with scene feedback, DRM sync objects with timeline synchronization, presentation time, single-pixel buffers, alpha modifiers, and content type. Software renderers omit unsupported GPU globals while shared-memory clients continue to work.

`wp-tearing-control-v1` can request asynchronous presentation. Leme honors it only for the focused, visible fullscreen view on the current tag. Native and XWayland views follow the same rule. If the backend rejects asynchronous state, the frame falls back to synchronized presentation.

## Desktop protocols

Leme supports XDG activation, `xdg-dialog-v1`, XDG decoration, legacy KDE
server decoration, and cursor shape. Clients can publish the XDG dialog hint;
window placement still follows the parent and size rules described under
Windows and shell surfaces. Both decoration protocols select server-side mode.
Desktop titlebars follow the committed XDG decoration mode, falling back to legacy
metadata when there is no XDG decoration object. Native client-side decorations
are not duplicated. Eligible windows get a draggable title and separate Minimize,
Maximize/Restore and Close controls; fullscreen hides them. See
[desktop titlebars](../concepts/layouts.md#titlebars-and-window-placement).
`xdg_toplevel.show_window_menu` remains unsupported.

XDG foreign v1 and v2 let cooperating Wayland clients establish parent
relationships across client connections. Both versions share an export
registry, so a handle exported through one version can be imported through
the other. This supports out-of-process dialogs such as portal file pickers;
both the application and the dialog service must exchange a valid handle.

Parent relationships established after mapping update stacking within the
existing tag and scene layer. They do not move or recenter windows, change
floating preferences, or transfer keyboard focus. Unrelated windows above
the whole family remain above it until that family is focused. Destroying an
import or export removes its relationship without restoring earlier geometry.
Parenting does not enforce modality or grant activation. Separate fullscreen
layers and durable ownership keep their existing policies; late parenting
does not move windows between them.

Activation requires a one-use token tied to Leme's seat and a valid input
serial. A window on the focused tag is focused directly. A window on another
tag or output follows the [publication activation
policy](../configuration/outputs.md#publication), the same one X11 and
foreign-toplevel activation use: `follow` switches to it, `urgent` marks its
tag urgent, and `ignore` drops the request. A shown scratchpad is focused
directly; a hidden scratchpad is not an activation target. A presented sticky window focuses its
owner output and then the window; a suspended sticky window is not eligible.
Requests are ignored while locked or
while an exclusive layer surface owns keyboard focus.

Cursor-shape requests require the pointer-focused client and a valid serial. Client cursor surfaces remain authoritative across output scale changes except during compositor move and resize operations.

## Publication

Leme publishes tags and managed windows through:

- `ext-workspace-v1`;
- `ext-foreign-toplevel-list-v1`;
- `wlr-foreign-toplevel-management-v1`.

Each output has one workspace group. Workspace ids include the connector, such as `DP-1:3`. The published set is the navigable set: pinned tags, occupied adaptive tags, and at most one empty candidate.

A managed mapped window is published. Ordinary tagged windows, shown
scratchpads, and output-presented sticky windows are published; a hidden mapped
scratchpad or suspended sticky window is not. A shown scratchpad and a sticky
window belong only to their displayed/owner output, and output moves send leave
and enter on the same handle. Override-redirect X11
surfaces and layer-shell clients are not. XWayland windows publish their title
and class as the app id.

Minimized windows remain published with their output association. The wlr handle
reports effective minimized state, including same-tag parent inheritance. Accepted
activation restores before focusing; ignored or urgent off-tag activation alone
does not restore. An explicit unminimize request is separate from activation.
The wlr handle also reports maximized state. Maximize and restore requests do not
activate or unminimize a window. Native, XWayland and foreign requests share the
desktop policy. XWayland supports full two-axis maximize; partial-axis requests
are rejected and the actual state is reasserted. External-shell interoperability,
including DMS, still needs live verification.

### Request policy

| Request | Policy |
| --- | --- |
| workspace `activate` | honored according to the configured activation policy |
| workspace `deactivate` | rejected; one tag is always focused per output |
| workspace `create_workspace`, `remove` | rejected; tags are bounded and adaptive |
| workspace `assign` | rejected; tag ids are local to each output |
| toplevel `close` | honored |
| toplevel `set_fullscreen`, `unset_fullscreen` | honored through the normal fullscreen path for tagged windows; rejected for pool scratchpads, which remain non-fullscreen |
| toplevel `activate` | tagged windows follow the configured activation policy; shown scratchpads focus directly; presented sticky windows focus their owner output then the view; hidden or stale durable views are rejected |
| toplevel `set_maximized`, `unset_maximized` | maximizes eligible desktop windows or restores normal geometry without activation; refused during fullscreen |
| toplevel `set_minimized` | minimizes eligible desktop windows or explicitly restores them without activation |
| toplevel `set_rectangle` | ignored |

While locked, the published set freezes, handles persist, and requests are refused. Unlocking rebuilds the set. Hidden stale toplevel requests remain rejected after the set is rebuilt.

## Output power

Leme supports `wlr-output-power-management-unstable-v1`. An external client chooses when an output turns off. The connector remains tracked with dormant tags, views, layer surfaces, and geometry. Powering it on restores the latest output configuration.

Requests remain available while locked. The wake transition prepares opaque-blocker geometry before the enabling commit so desktop content is not exposed during the transition.

The [configuration reference](../configuration/outputs.md) documents persistent output policy and publication activation. The [screen sharing guide](../guides/screen-sharing.md) covers external capture services.
