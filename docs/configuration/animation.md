# Animation

The `animation` block controls optional window and workspace animations. Each
event remains instant unless its own sub-block is present.

## Window animations

Use `open` and `close` to animate windows as they appear and disappear:

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
}
```

You can configure either event by itself. For example, an `open` block without
a `close` block leaves closing instant.

### Window options

| Key | Accepted values | Default | Effect |
| --- | --- | --- | --- |
| `duration` | nonnegative integer in milliseconds, clamped to `1000` | `150` | sets the animation length |
| `curve` | preset name or four decimal control points | `ease_out` | shapes scale and other movement |
| `opacity_curve` | same values as `curve` | value of `curve` | shapes the fade separately |
| `effect` | `fade`, `scale`, both, or `none` alone | `fade` | selects the visible effects |
| `scale_from` | decimal from `0.1` through `2.0` | `0.92` | sets the starting and ending scale |

Defaults apply when a sub-block is present, so `open { }` enables a 150 ms
`ease_out` fade. `duration 0` and `effect none` disable that event without a
diagnostic.

A duration above 1000 ms is clamped and reported. An invalid curve, effect, or
scale records a diagnostic and leaves that key at its sub-block default. See
[configuration errors](config-errors.md) for reporting and reload behavior.

### What a window animation shows

During an animation, Leme draws a temporary image of the complete window,
including its border and application content. The real window already has its
final position and size, so pointer input, bars, window lists, and `timao`
report the final state.

Opening fades from transparent to visible and scales from `scale_from` to full
size. Closing performs the reverse. The border keeps its configured thickness
throughout the scale, and scaling stays centered on the window. A value above
`1.0` starts an opening window larger than its final size and shrinks it into
place.

An application may continue drawing while the temporary image is visible
without changing the image partway through. A closing window keeps its last
content visible until the animation ends instead of becoming blank.

An opening window waits until the application has drawn at its configured
size. If that takes longer than 200 ms, Leme shows the window immediately
instead of delaying it further.

### When window animations stay instant

A window opens or closes without animation when:

- the session is locked;
- the window is fullscreen;
- the window is an unmanaged XWayland surface, such as a menu or tooltip;
- the window belongs to a tag that is not visible.

A running window animation ends immediately when the displayed tag changes or
the session locks. An animation never delays a window from opening or closing.

## Workspace transitions

Tag switching remains instant unless `animation` contains a `workspace` block.
This complete example enables the default glide and fade:

```scfg
animation {
    workspace {
        duration 180
        style glide_fade
        distance 0.15
        curve ease_out
        opacity_curve ease_out
    }
}
```

A full-width slide needs only:

```scfg
animation {
    workspace {
        style full_slide
    }
}
```

### Workspace options

| Key | Accepted values | Default | Effect |
| --- | --- | --- | --- |
| `duration` | nonnegative integer in milliseconds, clamped to `1000` | `180` | sets the transition length |
| `style` | `glide_fade` or `full_slide` | `glide_fade` | selects the transition style |
| `distance` | decimal from `0.0` through `1.0` | `0.15` | sets glide travel as a fraction of output width |
| `curve` | preset name or four decimal control points | `ease_out` | shapes movement |
| `opacity_curve` | same values as `curve` | value of `curve` | shapes the cross-fade |

`glide_fade` moves the source and destination windows by `distance` times the
output width while cross-fading them. `full_slide` moves them by one complete
output width and does not fade. `distance` and `opacity_curve` are accepted in
a `full_slide` block but do not affect that style.

An empty `workspace { }` block uses all defaults in the table. `duration 0`
disables workspace transitions without a diagnostic. An invalid style,
distance, or curve records a diagnostic and leaves that key at its default.

### Direction

- `focus_next_tag` and a followed `next` move travel forward, including the
  wrap from the highest navigable tag to tag 1.
- `focus_previous_tag` and a followed `previous` move travel backward,
  including the reverse wrap.
- A direct numbered selection or followed numbered move uses the tag numbers:
  a higher destination travels forward and a lower destination travels
  backward.
- Workspace and taskbar activation use the same numbered rule.
- `focus_last_tag` reverses the direction of the tag change it is undoing.

Forward movement sends the current tag left and brings the destination from
the right. Backward movement reverses both directions. A numbered jump shows
only its source and destination; intermediate tags do not appear.

### What moves

Managed tiled and floating windows on the source and destination tags take part
in the transition. Wallpaper, panels, notifications, overlays, the cursor, and
lock content stay fixed. Each transition remains clipped to its own output, so
it cannot draw across the edge of an adjacent monitor.

Switching between an occupied and an empty tag moves the occupied windows into
or out of the fixed background. Switching between two empty tags is instant.

### Interrupting a transition

Focus and input switch to the destination as soon as the transition begins.
The moving image is visual only and does not receive input. Bars, task switchers,
workspace publication, and `timao` also report the destination immediately.

If another switch happens before the animation finishes, the new transition
continues from what is currently visible instead of snapping back or waiting
in a queue. Reloading the configuration does not alter a transition already in
progress.

### When workspace transitions stay instant

A tag switch remains instant when:

- either tag has a fullscreen window;
- the session is locked or inactive;
- the output is powered off, disabled, or has no valid size;
- Leme cannot prepare the visual transition.

A running transition ends before an output changes mode, scale, transform,
position, or connection state. Locking, deactivating the session,
powering off the output, or entering fullscreen also ends it immediately.

Every tag change finishes any window open or close animation already running on
that output. Gesture-controlled workspace transitions are not supported.

## Curves

A curve is a cubic bezier. Use a preset name or four control-point coordinates:

```scfg
animation {
    open {
        duration 200
        curve 0.34 1.56 0.64 1.0
        opacity_curve ease_out
        effect fade scale
        scale_from 0.85
    }
}
```

The four numbers are `x1 y1 x2 y2`, using the same convention as CSS
`cubic-bezier`.

| Preset | Control points |
| --- | --- |
| `linear` | `0.0 0.0 1.0 1.0` |
| `ease_in` | `0.42 0.0 1.0 1.0` |
| `ease_out` | `0.0 0.0 0.58 1.0` |
| `ease_in_out` | `0.42 0.0 0.58 1.0` |

The two x coordinates must lie from `0.0` through `1.0`. Values outside that
range are rejected with a diagnostic. The y coordinates are unconstrained. A
y value above `1.0` can overshoot the target before settling, while a negative
y value can move away from the target first.

`opacity_curve` shapes a fade separately from movement. When omitted, it
follows `curve`. This allows a window to overshoot slightly while its opacity
rises smoothly.

Preset names resolve to the control points in the table. Specify all four
control points when a configuration needs an exact curve independent of preset
definitions.

## Known limits

While a window scales, a subsurface such as a video overlay or client-side
header can be displaced by up to one pixel. The offset disappears when the
animation finishes and is most visible with a small `scale_from`.

Animation duration follows elapsed time rather than frame count. Monitors with
different refresh rates remain synchronized, and a dropped frame does not
extend the configured duration.

See [appearance](appearance.md) for gaps, borders, and opacity.
