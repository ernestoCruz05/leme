# Outputs

Every connected output is enabled. An `output "NAME"` block sets the mode,
scale and placement of one connector. Without one, an output uses its
preferred mode at scale 1, placed left to right.

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

| Key | Values | Effect |
| --- | --- | --- |
| `mode` | `WIDTHxHEIGHT` | resolution |
| `refresh` | Hz, needs `mode` | the advertised rate within 1 Hz of it |
| `scale` | `0.5` to `4.0` | output scale |
| `transform` | `normal`, `90`, `180`, `270` | rotation |
| `position` | `X Y` | position in the layout |
| `left_of`, `right_of`, `top_of`, `bottom_of` | a connector name | position next to another output |

`wlr-randr` lists connector names and modes. A block takes one relation, and
`position` wins over it. Outputs that cannot be placed go to the right.
Overlapping outputs are rejected.

The layout is shifted so no output sits at a negative coordinate, because X11
windows cannot be reached there. Output-management tools such as `wlr-randr`
or `kanshi` can change outputs until the next `reload_config`, and they are not
shifted.

## Behavior across outputs

An `output` block without a name sets how focus and windows move between
outputs:

```scfg
output {
    cross_output_focus true
    cross_output_move false
    cross_output_drag true
    warp_cursor true
    secondary_gpu on_demand
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `cross_output_focus` | `true`, `false` | `true` | `focus DIR` continues onto the next output |
| `cross_output_move` | `true`, `false` | `false` | `move DIR` continues onto the next output |
| `cross_output_drag` | `true`, `false` | `true` | dragging carries a window to another output |
| `warp_cursor` | `true`, `false` | `true` | the pointer follows commands that change the focused output |
| `secondary_gpu` | `on_demand`, `always` | `on_demand` | when GPUs other than the boot GPU are opened |

With `on_demand`, a second GPU, such as the discrete GPU of a laptop, is only
opened while a monitor is connected to it and closed 5 seconds after the last
one is unplugged, so it can power down. `always` opens every GPU at startup.
`WLR_DRM_DEVICES` overrides both.

## Activation

The `publication` block decides what happens when an app asks for a window on
another tag or output to be focused, such as a browser opening a link:

```scfg
publication {
    activation follow
}
```

| `activation` | Effect |
| --- | --- |
| `follow` (default) | switches to that tag and output and focuses the window |
| `urgent` | marks the window urgent and leaves focus alone |
| `ignore` | drops the request |

A window on the focused tag is always focused. `follow` moves the pointer even
with `warp_cursor false`.
