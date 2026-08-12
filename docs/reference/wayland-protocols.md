# Wayland protocols

This page lists the protocol behavior that matters to clients. Leme advertises optional globals only when the backend and renderer can support them.

## Windows and shell surfaces

Leme supports XDG shell toplevels and recursive popups. Parented transients keep their parent's output and tag, float above tiled views, and center over the parent. Fixed-size unparented prompts start floating; ordinary resizable windows tile.

Layer-shell v5 supports wallpaper, panels, launchers, notifications, overlays, drag surfaces, and lock surfaces. Leme honors exclusive zones and keyboard interactivity. Viewporter and fractional-scale protocols are available to current layer-shell clients.

## Input and data exchange

Leme supports keyboard layouts, relative pointer, pointer constraints, pointer lock, shortcut inhibition, cursor shape, clipboard and primary selection, legacy and standardized data-control, pointer drag and drop, and XWayland selection bridging.

While shortcut inhibition is active, normal compositor bindings and SUPER pointer grabs go to the focused client. `switch_vt` and `quit` remain emergency actions.

Touch-origin drag and drop, touch, tablet, gesture, text-input, input-method, and virtual-keyboard protocols are not implemented.

## Locking and idle

Leme supports `ext-session-lock-v1`, `ext-idle-notify-v1`, and `idle-inhibit-unstable-v1`. Locking fails closed: normal content is covered immediately, input goes only to the lock surface, and an opaque blocker remains if the locker disappears.

Idle inhibitors count only while their surface is mapped and visible on an enabled output. Leme does not choose idle timeouts or start a locker.

## Capture

Leme exposes `wlr-screencopy-v1`, `ext-image-copy-capture-v1`, output image sources, and logical output metadata. Shared-memory capture is available, and DMA-BUF capture depends on the renderer and backend.

Capture during a lock sees only committed lock content or the opaque blocker.
Portal integration offers whole-output sources. Compatible direct clients can
request a per-window source through
`ext_foreign_toplevel_image_capture_source_manager_v1`; the source is refused
while locked or when the window is not currently protocol-eligible. Tagged
windows and shown scratchpads are eligible. A hidden mapped scratchpad is not:
new direct capture requests and stale requests are rejected, and an already
accepted source is invalidated when it hides. A window capture is a region of
the scene, so surfaces drawn over that region appear in the result.

The current `xdg-desktop-portal-wlr` picker does not expose that direct
per-window source. Portal consent, source selection, and PipeWire transport
remain external.

## Graphics and presentation

When supported, Leme advertises linux-dmabuf v5 with scene feedback, DRM sync objects with timeline synchronization, presentation time, single-pixel buffers, alpha modifiers, and content type. Software renderers omit unsupported GPU globals while shared-memory clients continue to work.

`wp-tearing-control-v1` can request asynchronous presentation. Leme honors it only for the focused, visible fullscreen view on the current tag. Native and XWayland views follow the same rule. If the backend rejects asynchronous state, the frame falls back to synchronized presentation.

## Desktop protocols

Leme supports XDG activation, `xdg-dialog-v1`, XDG decoration, legacy KDE
server decoration, and cursor shape. Clients can publish the XDG dialog hint;
window placement still follows the parent and size rules described under
Windows and shell surfaces. Both decoration protocols select server-side mode.

Activation requires a one-use token tied to Leme's seat and a valid input
serial. It can focus only a mapped view on the current tag and never changes
tags. A shown scratchpad on the focused output is focused directly; a hidden
scratchpad is not an activation target. Requests are ignored while locked or
while an exclusive layer surface owns keyboard focus.

Cursor-shape requests require the pointer-focused client and a valid serial. Client cursor surfaces remain authoritative across output scale changes except during compositor move and resize operations.

## Publication

Leme publishes tags and managed windows through:

- `ext-workspace-v1`;
- `ext-foreign-toplevel-list-v1`;
- `wlr-foreign-toplevel-management-v1`.

Each output has one workspace group. Workspace ids include the connector, such as `DP-1:3`. The published set is the navigable set: pinned tags, occupied adaptive tags, and at most one empty candidate.

A managed mapped window is published. Ordinary tagged windows and shown
scratchpads are published; a hidden mapped scratchpad is not. A shown
scratchpad belongs only to its displayed output. Override-redirect X11
surfaces and layer-shell clients are not. XWayland windows publish their title
and class as the app id.

### Request policy

| Request | Policy |
| --- | --- |
| workspace `activate` | honored according to the configured activation policy |
| workspace `deactivate` | rejected; one tag is always focused per output |
| workspace `create_workspace`, `remove` | rejected; tags are bounded and adaptive |
| workspace `assign` | rejected; tag ids are local to each output |
| toplevel `close` | honored |
| toplevel `set_fullscreen`, `unset_fullscreen` | honored through the normal fullscreen path for tagged windows; rejected for pool scratchpads, which remain non-fullscreen |
| toplevel `activate` | tagged windows follow the configured activation policy; shown scratchpads focus directly when otherwise eligible and unlocked; hidden or stale scratchpads are rejected |
| toplevel `set_maximized`, `set_minimized` | dropped; Leme has no maximized or minimized state |
| toplevel `set_rectangle` | ignored |

While locked, the published set freezes, handles persist, and requests are refused. Unlocking rebuilds the set. Hidden stale toplevel requests remain rejected after the set is rebuilt.

## Output power

Leme supports `wlr-output-power-management-unstable-v1`. An external client chooses when an output turns off. The connector remains tracked with dormant tags, views, layer surfaces, and geometry. Powering it on restores the latest output configuration.

Requests remain available while locked. The wake transition prepares opaque-blocker geometry before the enabling commit so desktop content is not exposed during the transition.

The [configuration reference](../configuration/outputs.md) documents persistent output policy and publication activation. The [screen sharing guide](../guides/screen-sharing.md) covers external capture services.
