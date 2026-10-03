# Appearance

The `style` block controls gaps, borders, opacity, and how fullscreen windows
stack:

```scfg
style {
    gap 8
    gap_outer 10 20 30 40
    smart_gaps true
    border_width 2
    border_active "#296bb8"
    border_inactive "#3a3a3a"
    opacity_active 1.0
    opacity_inactive 1.0
    fullscreen_covers top
}
```

| Key | Values | Default |
| --- | --- | --- |
| `gap` | nonnegative integer in logical pixels | `0` |
| `gap_outer` | one to four integers from `0` through `2147483647`, in logical pixels | `0` |
| `smart_gaps` | `true` or `false` | `false` |
| `border_width` | nonnegative integer in logical pixels | `0` |
| `corner_radius` | nonnegative integer in logical pixels | `0` |
| `blur` | integer from `0` through `64` | `0` |
| `border_active` | `#RRGGBB` or `#RRGGBBAA` | `#296bb8` |
| `border_inactive` | `#RRGGBB` or `#RRGGBBAA` | `#3a3a3a` |
| `opacity_active` | decimal from `0.0` through `1.0` | `1.0` |
| `opacity_inactive` | decimal from `0.0` through `1.0` | `1.0` |
| `fullscreen_covers` | `none`, `top`, or `overlay` | `top` |

## Gaps

`gap` sets the total spacing between tiled frames, with optional
[per-tag overrides](tags.md#per-tag-settings). `gap_outer` adds margins around
the tiled workspace using CSS shorthand:

| Arguments | Top | Right | Bottom | Left |
| --- | --- | --- | --- | --- |
| `10` | 10 | 10 | 10 | 10 |
| `10 20` | 10 | 20 | 10 | 20 |
| `10 20 30` | 10 | 20 | 30 | 20 |
| `10 20 30 40` | 10 | 20 | 30 | 40 |

Outer margins start after bars reserve space and shrink to fit the usable area.
They only affect tiled windows; floating, fullscreen, scratchpad and sticky
windows keep their geometry.

With `smart_gaps true`, outer margins disappear on tags with at most one mapped,
managed tile. Floating, fullscreen and drag-detached windows do not count.
Borders and inner gaps stay unchanged. Both settings are global.

### Live settings

The `loaded` and `effective` config records report configured values, even when
margins shrink or are suppressed. `gap_outer` has integer `top`, `right`,
`bottom` and `left` members.

```sh
timao get config effective style gap_outer
timao get config effective style smart_gaps
timao eval '(act (set-config (list "style" "gap_outer" "top") 20))'
timao eval '(act (set-config (list "style" "smart_gaps") true))'
```

IPC writes each side separately; whole-object writes and CSS shorthand are
unsupported. A successful reload clears live overrides.

## Borders and effects

The active and inactive border colors follow keyboard focus. Leme draws the border as its complete server-side decoration. It does not draw titlebars or buttons.

`corner_radius` rounds the window, frame and content together. Like
`border_width`, it is in logical pixels, so both grow with the output scale. The
value is clamped to half the shorter side, so a small window keeps the largest
radius that fits. Fullscreen windows are never rounded, because a window covering the
output has nothing to show through its corners.

Rounding requires a build configured with `-Deffects=true`, which compiles
against a patched wlroots. Other builds accept the key and ignore it.

`blur` blurs whatever is behind a window. It is only visible where the window
is translucent, so it does nothing unless `opacity_active` or
`opacity_inactive` is below 1, or the client draws its own transparency.
Behind an opaque window the work is skipped entirely, rounded or not.
Fullscreen windows are never blurred.

The value is roughly the blur radius in logical pixels. Leme uses a dual Kawase
blur: it shrinks the backdrop in a few steps and scales it back up, so large
values stay smooth and cost little more than small ones. The blurred area
follows `corner_radius`, and it fades with the window during open and close
animations and workspace fades. The software renderer (`WLR_RENDERER=pixman`)
keeps a plain box blur without rounding or fading. To see what blur costs on
your hardware, see [measuring performance](../troubleshooting/performance.md).

A blur radius above 64 is reported rather than clamped, so a value that would
do nothing useful says so instead of being silently reduced.

`blur` needs the same `-Deffects=true` build as `corner_radius`.

A matching window-rule `opacity` replaces the active or inactive style opacity;
the values are not multiplied.

## Fullscreen stacking

`fullscreen_covers` decides which layer-shell surfaces a fullscreen window is
allowed to cover. Panels, bars, and notification daemons are layer-shell
clients, and each one picks the layer it sits on.

| Value | Effect |
| --- | --- |
| `none` | Layer surfaces stay above fullscreen windows, which are sized to the usable area. |
| `top` | Fullscreen windows cover the background, bottom, and top layers. |
| `overlay` | Fullscreen windows cover every layer, including overlay. |

`top` is the default because it is what most setups want: Waybar and Mako both
default to the top layer, so a fullscreen video hides them, while an on-screen
display or keyboard on the overlay layer still comes through.

Choose `overlay` when nothing at all should interrupt a fullscreen window, and
`none` to keep a bar permanently visible.

Stacking is decided per window, not per output, so a fullscreen window on one
monitor never hides the bar on another.

Two things are always above a fullscreen window regardless of this key: the
session lock screen, and a shown scratchpad or sticky group. A scratchpad is a
window you summon over whatever is playing, so it stays reachable.

With `none`, a fullscreen window is given the usable area rather than the whole
output, so a bar with an exclusive zone does not clip it. The other two values
give it the entire output.

A fullscreen view is always fully opaque, even when a style or window rule
requests lower opacity. A view with opacity below `1.0` cannot use direct
scanout, which presents the application's fullscreen buffer without compositing
it with other content.

Leme does not support drop shadows. For window and workspace animations, see
[animation](animation.md).
