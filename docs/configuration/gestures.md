# Gestures

The `gestures` block sets up horizontal touchpad swipes that switch tags.

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
