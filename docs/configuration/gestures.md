# Gestures

The `gestures` block sets up the touchpad swipe that switches tags and the
[touchpad edges](#touchpad-edges). Other gestures are
[bindings](keybindings.md#gestures).

```scfg
gestures {
    workspace_switch {
        mode single
        fingers 3
        distance 300
        threshold 0.5
        deceleration 0.997
        velocity_window 150
        occupied false
    }
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `mode` | `single`, `scrub`, `free` | `single` | how far one swipe can go, see below |
| `fingers` | `0`, or `3` and up | `3` | fingers for the swipe, `0` turns it off |
| `distance` | above `0` | `300` | swipe distance for one tag, in touchpad units |
| `threshold` | above `0`, below `1` | `0.5` | how far into the next tag a release must be to land there |
| `deceleration` | above `0`, below `1` | `0.997` | how much a flick coasts after release |
| `velocity_window` | ms, above `0` | `150` | how much of the end of the swipe sets the flick speed |
| `occupied` | `true`, `false` | `false` | skip tags with no windows, like `focus_next_tag occupied` |

| `mode` | Effect |
| --- | --- |
| `single` | one swipe reaches the next or previous tag only |
| `scrub` | drag across several tags, release lands on one of the two in view |
| `free` | drag across several tags and let a flick coast further |

Two-finger swipes are rejected because libinput does not deliver them. The
swipe direction follows the device's `natural_scroll`. The release settles with
a spring that [`animation.workspace.spring`](animation.md#springs) can tune. A
swipe switches instantly when any tag it can reach uses the desktop layout.

## Touchpad edges

`edges` sizes the [edge bindings](keybindings.md#touchpad-edges).

```scfg
gestures {
    edges {
        size 8
        step 5
    }
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `size` | `2` to `30` | `8` | how far from an edge a slide can start, in mm |
| `step` | `1` to `50` | `5` | how far a finger slides to run the binding once, in mm |

Edge bindings on the right and bottom edges win over `scroll_method edge`.
