# Outputs

Leme enables every connected output. Use one named `output` block per connector when you need a specific mode or layout:

```scfg
output "DP-1" {
    mode 1920x1080
    refresh 165.0
    scale 1.0
    transform normal
    position 0 0
}

output "HDMI-A-1" {
    mode 2560x1440
    right_of "DP-1"
}
```

## Display settings

- `mode` uses `WIDTHxHEIGHT`.
- `refresh` is optional, but requires `mode`. Leme chooses the nearest advertised fixed refresh for that resolution when it is within 1 Hz.
- `scale` accepts `0.5` through `4.0`.
- `transform` accepts `normal`, `90`, `180`, or `270`.
- A connector may appear in only one named block.

Without output blocks, Leme uses each output's preferred mode, scale 1, normal transform, and a left-to-right layout.

## Placement

Use one of these placement forms:

- `position X Y` for absolute logical coordinates;
- `left_of NAME`;
- `right_of NAME`;
- `top_of NAME`;
- `bottom_of NAME`.

A block accepts at most one relation. Placement resolves explicit positions first, then relative relations until no more can be placed. Outputs that have no placement, name a disconnected connector, or belong to a cycle are appended to the right of the current layout in connect order.

A block may contain both `position` and a relation. `position` wins and the ignored relation is logged. Overlapping outputs are rejected on reload and fall back to automatic placement at startup.

The scfg output block is persistent policy. `zwlr_output_manager_v1` clients can request temporary modes, scales, transforms, positions, and multi-output arrangements. The next configuration reload restores scfg values. Adaptive sync requests remain rejected.

## Cross-output policy

An unnamed `output` block controls movement between outputs:

```scfg
output {
    cross_output_focus true
    cross_output_move false
    cross_output_drag true
    warp_cursor true
}
```

| Key | Default | Effect |
| --- | --- | --- |
| `cross_output_focus` | `true` | lets directional focus continue to an adjacent output |
| `cross_output_move` | `false` | lets a view cross to an adjacent output when no local movement target exists |
| `cross_output_drag` | `true` | lets pointer drags carry a view to another output |
| `warp_cursor` | `true` | moves the pointer when a command changes the focused output |

The unnamed policy block and named connector blocks are independent.

## Publication

Leme publishes tags and managed windows for bars, taskbars, and window pickers. The `publication` block controls what happens when an activation request would change the focused output or tag:

```scfg
publication {
    activation follow
}
```

| Key | Values | Default |
| --- | --- | --- |
| `activation` | `follow`, `urgent`, `ignore` | `follow` |

If the target is already on the focused tag of the focused output, Leme focuses it under every policy. Otherwise:

- `follow` switches to the target output and tag, then focuses it;
- `urgent` leaves focus alone and marks the target workspace urgent;
- `ignore` drops the request.

`follow` moves the pointer to the target output when activation crosses
outputs. This happens even when `warp_cursor` is `false`; that output-policy
key controls keyboard commands, not external activation requests.

The same setting applies to foreign-toplevel activation and cross-output workspace activation. The tag model determines the rest of publication behavior. See the [publication reference](../reference/wayland-protocols.md).
