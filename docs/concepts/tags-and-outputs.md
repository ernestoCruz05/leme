# Tags and outputs

Leme calls workspaces tags. Every output has its own tags, so tag 3 on one
monitor and tag 3 on another hold different windows. Tag commands act on the
focused output, which follows the pointer.

Tags up to `initial` always exist. Higher tags appear when a window goes there
and disappear when the last one leaves. Navigation goes through the tags that
exist plus one empty tag, so jumping around does not leave a trail of empty
tags. `focus_next_tag occupied` skips every empty tag.

Bars see the same tags, with ids that include the connector, such as `DP-1:3`.

See [tags](../configuration/tags.md) for the settings and
[outputs](../configuration/outputs.md) for placement and moving between
outputs.
