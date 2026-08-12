# Multi-monitor

Leme enables every connected output. List connector names, current settings,
and advertised modes from inside the session:

```sh
wlr-randr
```

The same connector names appear in the compositor log.

## Configure placement

Use one named `output` block per connector:

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
```

Connector names must match the DRM names exactly. You can use absolute `position`, or place a connector with `left_of`, `right_of`, `top_of`, or `bottom_of`. The [output configuration reference](../configuration/outputs.md) covers mode selection, scale, transform, and placement fallback.

Leme rejects overlapping output geometry on reload. If a relation names a disconnected connector or forms a cycle, Leme appends the unresolved output to the right of the current layout.

## Understand tags across outputs

Each output owns its own tag table. The tag called 3 on `DP-1` is not the tag called 3 on `DP-2`. A window moved between outputs lands on the destination output's current tag unless a command says otherwise.

The focused output follows the pointer. Directional focus can continue across an output edge, and a view can cross that edge through a movement command or pointer drag when the corresponding policies allow it.

## Choose movement policy

The unnamed `output` block controls cross-output behavior:

```scfg
output {
    cross_output_focus true
    cross_output_move false
    cross_output_drag true
    warp_cursor true
}
```

Use `cross_output_focus` when directional focus should leave the current monitor. Use `cross_output_move` when a keyboard movement at the edge should carry the view to the adjacent monitor. Use `cross_output_drag` for pointer drags. `warp_cursor` controls pointer warping after an output-focus command.

## Temporary output changes

Clients using `zwlr_output_manager_v1` can request temporary positions, modes, scales, and transforms. Those requests do not rewrite scfg. A later configuration reload restores the persistent output policy.

See [tags and outputs](../concepts/tags-and-outputs.md) for the tag model and [troubleshooting outputs](../troubleshooting/outputs.md) when a mode or layout is rejected.
