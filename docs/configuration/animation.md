# Animation

Animations are off until the `animation` block turns them on, one event at a
time: `open` and `close` for windows appearing and disappearing, `move` for
windows changing place or size, and `workspace` for tag switches.

```scfg
animation {
    open {
        duration 150
        curve ease_out
        effect fade scale
        scale_from 0.92
    }
    close {
        duration 120
        curve ease_in
        effect fade
    }
    move {
        duration 200
    }
    workspace {
        style glide_fade
    }
}
```

An empty block such as `open { }` turns that event on with the defaults below.
`duration 0` turns it off.

## Window events

`open`, `close` and `move` take these keys:

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `duration` | milliseconds, `0` to `1000` | `150` | animation length |
| `curve` | a [curve](#curves) | `ease_out` | shape of the motion |
| `spring` | a [spring](#springs) block | none | replaces `duration` and `curve` |
| `effect` | `fade`, `scale`, both, or `none` | `fade` | what the animation does |
| `opacity_curve` | a [curve](#curves) | same as `curve` | shape of the fade |
| `scale_from` | `0.1` to `2.0` | `0.92` | size an opening window starts at, and a closing one ends at |

`effect`, `opacity_curve` and `scale_from` only apply to `open` and `close`. On
desktop tags, `move` also animates maximize, `snap`, containers and minimizing.
Switching a tag to or from the desktop layout uses `move` too.

## Springs

A `spring` block in `open`, `close`, `move` or `workspace` replaces `duration`,
`curve` and `opacity_curve`:

```scfg
animation {
    open {
        spring {
            damping_ratio 0.8
            stiffness 900
        }
    }
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `damping_ratio` | above `0` | `1.0` | below `1.0` overshoots and springs back |
| `stiffness` | above `0` | `800` | higher settles faster |
| `epsilon` | above `0`, below `1` | `0.0001` | how close to the target counts as settled |

A spring that takes longer than 1000 ms to settle is rejected.

## Workspace transitions

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `duration` | milliseconds, `0` to `1000` | `180` | transition length |
| `style` | `glide_fade`, `full_slide` | `glide_fade` | a short glide with a cross-fade, or a slide by the full output width |
| `distance` | `0.0` to `1.0` | `0.15` | how far `glide_fade` moves, as a fraction of the output width |
| `curve` | a [curve](#curves) | `ease_out` | shape of the motion |
| `spring` | a [spring](#springs) block | none | replaces `duration` and `curve`, and sets the release of a [touchpad swipe](gestures.md) |

A switch is instant when either tag has a fullscreen window. The minimized
strip of a desktop tag doesn't slide.

## Curves

A curve is a preset name or four numbers, `x1 y1 x2 y2`, read like CSS
`cubic-bezier`. Both x values must be from `0.0` to `1.0`.

| Preset | Control points |
| --- | --- |
| `linear` | `0.0 0.0 1.0 1.0` |
| `ease_in` | `0.42 0.0 1.0 1.0` |
| `ease_out` | `0.0 0.0 0.58 1.0` |
| `ease_in_out` | `0.42 0.0 0.58 1.0` |
