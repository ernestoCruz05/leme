# Appearance

The `style` block controls gaps, borders, and opacity:

```scfg
style {
    gap 8
    border_width 2
    border_active "#296bb8"
    border_inactive "#3a3a3a"
    opacity_active 1.0
    opacity_inactive 1.0
}
```

| Key | Values | Default |
| --- | --- | --- |
| `gap` | nonnegative integer in logical pixels | `0` |
| `border_width` | nonnegative integer in logical pixels | `0` |
| `corner_radius` | nonnegative integer in logical pixels | `0` |
| `blur` | integer from `0` through `64` | `0` |
| `border_active` | `#RRGGBB` or `#RRGGBBAA` | `#296bb8` |
| `border_inactive` | `#RRGGBB` or `#RRGGBBAA` | `#3a3a3a` |
| `opacity_active` | decimal from `0.0` through `1.0` | `1.0` |
| `opacity_inactive` | decimal from `0.0` through `1.0` | `1.0` |

The active and inactive border colors follow keyboard focus. Leme draws the border as its complete server-side decoration. It does not draw titlebars or buttons.

`corner_radius` rounds the window, frame and content together. The value is
clamped to half the shorter side, so a small window keeps the largest radius
that fits. Fullscreen windows are never rounded, because a window covering the
output has nothing to show through its corners.

Rounding requires a build configured with `-Deffects=true`, which compiles
against a patched wlroots. Other builds accept the key and ignore it.

`blur` blurs whatever is behind a window. It is only visible where the window
is translucent, so it does nothing unless `opacity_active` or
`opacity_inactive` is below 1, or the client draws its own transparency.
Behind an opaque window the work is skipped entirely. Fullscreen windows are
never blurred.

A blur radius above 64 is reported rather than clamped, so a value that would
do nothing useful says so instead of being silently reduced.

`blur` needs the same `-Deffects=true` build as `corner_radius`.

A matching window-rule `opacity` replaces the active or inactive style opacity;
the values are not multiplied.

A fullscreen view is always fully opaque, even when a style or window rule
requests lower opacity. A view with opacity below `1.0` cannot use direct
scanout, which presents the application's fullscreen buffer without compositing
it with other content.

Leme does not support drop shadows. For window and workspace animations, see
[animation](animation.md).
