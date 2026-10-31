# Multi-monitor

Every connected output is enabled. `wlr-randr` lists connector names and modes,
and the log has the names too. Give each monitor an `output` block:

```scfg
output "DP-1" {
    mode 1920x1080
    refresh 165.0
    position 0 0
}

output "DP-2" {
    mode 1920x1080
    right_of "DP-1"
}

output {
    cross_output_focus true
    cross_output_move false
    cross_output_drag true
    warp_cursor true
}
```

The unnamed block decides whether `focus`, `move` and dragging cross to the
next monitor. All keys are in [outputs](../configuration/outputs.md).

Each monitor has its own tags, and focus follows the pointer. A window sent to
another monitor lands on the tag shown there.

## Unplugging

When a monitor goes away, its windows move to the same tag numbers on another
one. When a monitor with the same connector name comes back, they return, along
with its shown tag and tag layouts. Windows you moved in the meantime stay
where you put them. Switching VT counts as an unplug.

Tools like `wlr-randr` and `kanshi` can change outputs until the next
`reload_config`. See [output troubleshooting](../troubleshooting/outputs.md) if
a mode or layout is rejected.
