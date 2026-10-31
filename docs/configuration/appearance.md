# Appearance

The `style` block sets gaps, borders, opacity, effects and what a fullscreen
window covers.

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

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `gap` | pixels | `0` | space between tiled windows, also settable [per tag](tags.md#per-tag-settings) |
| `gap_outer` | one to four pixel values | `0` | margin around tiled windows, see [gaps](#gaps) |
| `smart_gaps` | `true`, `false` | `false` | no outer margin on a tag with one tiled window |
| `border_width` | pixels | `0` | border thickness |
| `border_active` | `#RRGGBB` or `#RRGGBBAA` | `#296bb8` | border of the focused window |
| `border_inactive` | `#RRGGBB` or `#RRGGBBAA` | `#3a3a3a` | border of other windows |
| `opacity_active` | `0.0` to `1.0` | `1.0` | opacity of the focused window |
| `opacity_inactive` | `0.0` to `1.0` | `1.0` | opacity of other windows |
| `corner_radius` | pixels | `0` | rounded corners, see [effects](#effects) |
| `blur` | `0` to `64` | `0` | blur behind translucent windows, see [effects](#effects) |
| `fullscreen_covers` | `none`, `top`, `overlay` | `top` | layers a fullscreen window covers, see [fullscreen](#fullscreen) |

Sizes are in logical pixels, so they grow with the output scale. A window
rule's `opacity` replaces the style opacity instead of multiplying it.

## Gaps

`gap_outer` takes values like a CSS margin:

| Values | Top | Right | Bottom | Left |
| --- | --- | --- | --- | --- |
| `10` | 10 | 10 | 10 | 10 |
| `10 20` | 10 | 20 | 10 | 20 |
| `10 20 30` | 10 | 20 | 30 | 20 |
| `10 20 30 40` | 10 | 20 | 30 | 40 |

Outer gaps sit inside the space bars reserve and only apply to tiled windows.

## Effects

`corner_radius` and `blur` need a build configured with `-Deffects=true`, which
uses a patched wlroots. Other builds accept both keys and ignore them.

The radius is capped at half the window's shorter side. Fullscreen windows,
maximized desktop windows and desktop containers stay square.

`blur` only shows where a window is translucent, so it needs `opacity_*` below
`1.0` or a client that draws its own transparency. The value is roughly the
radius in pixels. [Measuring performance](../troubleshooting/performance.md)
shows what it costs. There are no drop shadows.

## Fullscreen

| `fullscreen_covers` | A fullscreen window covers |
| --- | --- |
| `none` | nothing: it is sized to fit between bars and panels |
| `top` | the background, bottom and top layers, so overlay surfaces such as on-screen displays stay above |
| `overlay` | every layer |

The lock screen and a shown scratchpad or sticky group are always above a
fullscreen window. Fullscreen windows are always opaque.

Desktop titlebars, menus and icons are styled with named `style` blocks, see
[desktop styling](desktop.md#styling).
