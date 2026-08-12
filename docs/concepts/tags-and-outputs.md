# Tags and outputs

Leme calls workspaces **tags**. Each output has its own tag table. Tag 3 on one monitor and tag 3 on another monitor are different tags with different windows.

## Adaptive tags

The initial tags stay available for the whole session. Higher numbers are
candidates: available tag numbers that do not yet have a persistent workspace.
When a window opens or moves to one, Leme creates that tag. After its last
window leaves, Leme removes the empty tag while keeping its number available.

Navigation includes the pinned tags, occupied adaptive tags, and at most the
current empty candidate. Selecting unused tag numbers does not fill the session
with empty tags.

The `initial` and `maximum` limits, tag ranges, and `drop_mode` setting are documented in [tag configuration](../configuration/tags.md).

## One tag table per output

Tag commands act on the focused output. A window moved to tag 3 on one output does not appear on tag 3 of another output. This model is close to dwl and river, rather than a shared workspace list spread across monitors.

The focused output follows the pointer, including when the pointer moves over an empty desktop area. Keyboard commands can also select an output by direction or connector name. The [output configuration](../configuration/outputs.md) page covers placement and cross-output policy.

## Publication

Bars and task switchers see the navigable tag set: pinned tags, occupied adaptive tags, and the current empty candidate. The published workspace id includes the connector, such as `DP-1:3`, so clients can distinguish equal tag numbers on different outputs.

The [Wayland protocol reference](../reference/wayland-protocols.md) describes
publication requests. The [publication setting](../configuration/outputs.md#publication)
documents the activation policy.
