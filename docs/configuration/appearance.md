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

The active and inactive border colors follow keyboard focus. Eligible windows in
[desktop layout](../concepts/layouts.md#titlebars-and-window-placement) also have a
titlebar with window controls, styled with [named `style` blocks](#desktop-styling).
Other layouts use borders only.

`corner_radius` rounds the window, frame and content together. Like
`border_width`, it is in logical pixels, so both grow with the output scale. The
value is clamped to half the shorter side, so a small window keeps the largest
radius that fits. Fullscreen windows are never rounded, because a window covering the
output has nothing to show through its corners. A window with a titlebar is
rounded as a whole: the titlebar's top corners and the content's bottom corners.
Maximized desktop windows and desktop containers stay square.

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

## Desktop styling

Leme draws the desktop titlebars and their buttons, the window and desktop
menus, the rename and error prompts, the minimized strip, desktop icons and the
selection box. A `style` block with a name styles one of them:

```scfg
style "titlebar" {
    background "#2a2e32"
    color      "#fcfcfc"
}

style "close:hover" {
    background "#da4453"
}
```

The name is an element, optionally followed by `:` and a state. The unnamed
`style { }` block keeps the settings above, and named blocks never change them.
Each element and state takes one block; a second one is reported and ignored.
Unknown elements, states and properties are reported with the closest valid
name.

These blocks are not live settings. `reload_config` applies them to open
windows and redraws the desktop.

### How values combine

Leme starts from an element's defaults and its block without a state, then
applies the blocks for each state it is in, in this order: `inactive`,
`maximized`, `hover`, `pressed`, `focus`, `selected`, `drop`, `disabled`. A
later state wins, so a hovered button on an unfocused window uses `:inactive`
first and `:hover` on top of it. A pressed button is also hovered.

`minimize`, `maximize` and `close` start from `button`: `style "button:hover"`
styles all three, and `style "close:hover"` overrides it for close only.
`color` and `font` come from the containing element when they are not set.
Buttons take the titlebar's, menu items and separators take the menu's, and
prompt fields and buttons take the prompt's.

Sizes, `align` and `layout` can only be set without a state, because they
decide the layout and Leme computes it once.

Anything that is not set keeps its default, so a config without `style`
blocks looks the same as one that sets every default below.

### Elements

| Element | What it draws | States | Properties |
| --- | --- | --- | --- |
| `titlebar` | titlebars of windows and containers | `inactive` | `background` `color` `font` `height` `align` `layout` |
| `button` | all titlebar buttons | `inactive` `hover` `pressed` `disabled` | `background` `color` `radius` `width` `size` `opacity` `icon` |
| `minimize`, `close` | one titlebar button | as `button` | as `button`, without `width` |
| `maximize` | the maximize button | as `button`, and `maximized` | as `button`, without `width` |
| `menu` | window and desktop menus | | `background` `color` `font` `radius` `border_color` |
| `menu_item` | menu rows | `hover` `disabled` | `background` `color` `border_color` |
| `menu_separator` | lines between groups of rows | | `color` |
| `prompt` | rename, new folder and error prompts | | `background` `color` `font` `radius` `border_color` |
| `prompt_field` | the name field | `focus` | `background` `color` `border_color` |
| `prompt_button` | prompt buttons | `focus` | `background` `color` `border_color` |
| `strip` | the minimized-window strip and its arrows | `disabled` | `background` `color` |
| `icon` | desktop icons and their labels | `selected` `drop` | `background` `border_color` `color` `font` `shadow` `size` `spacing` `padding` `width` `gap` `theme` |
| `selection` | the selection box and selected prompt text | | `background` `border_color` |

`maximize:maximized` is the button while its window is maximized, where the
built-in glyph turns into the restore diamond. `icon:drop` is a folder or cell
under a drag. `strip:disabled` is a page arrow with nothing more to show.

### Properties

| Property | Values |
| --- | --- |
| `background`, `color`, `border_color` | `#RRGGBB` or `#RRGGBBAA` |
| `font` | Pango font description, such as `"Inter Semi-Bold 10"`, up to 127 bytes |
| `radius` | `0` to `64` logical pixels |
| `width` | `16` to `512` logical pixels |
| `height` | `16` to `96` logical pixels |
| `size`, `padding` | `8` to `256` and `0` to `256` logical pixels |
| `spacing` | `32` to `512` logical pixels |
| `gap` | `0` to `64` logical pixels |
| `opacity` | decimal from `0.0` to `1.0` |
| `align` | `left` or `center` |
| `layout` | `title` and up to three of `minimize`, `maximize`, `close`, each once |
| `shadow` | `true` or `false` |
| `icon` | path to a `.svg` or `.png` file |
| `theme` | icon theme name, such as `Papirus` |

On titlebars, `height` is the titlebar height and `layout` lists the buttons
around the title: buttons before `title` sit on the left, buttons after it on
the right. `align center` centers the title on the whole titlebar and moves it
only to keep it clear of the buttons. A window narrower than three buttons has
no titlebar, even when fewer buttons are shown.

On buttons, `width` is the button width and `size` the square the glyph or
image is drawn in, centered in the button. `background` fills the whole button
and `radius` rounds it. `opacity` applies to the glyph or image. `icon` replaces
the built-in glyph with an image, which keeps its own colors; `color` only
tints the built-in glyphs. A path starting with `~/` is in your home directory,
and a relative path is relative to the config file. A missing or unreadable
file is reported when the config loads, and the built-in glyph is drawn.

On desktop icons, `size` is the icon size, `spacing` the size of each grid
cell, `padding` the margin around the grid, `width` the label width and `gap`
the space between icon and label. If `spacing` is not set, cells grow to fit
the icon and the label; a `spacing` smaller than either is reported. `theme`
picks the icon theme for desktop icons and the minimized strip. A theme that is
not installed is reported and Adwaita is used instead. Desktop icons the theme
lacks also come from Adwaita.

### Defaults

The defaults follow KDE Plasma's Breeze Dark:

```scfg
style "titlebar" {
    background "#2a2e32"
    color      "#fcfcfc"
    font       "Sans 10"
    height     32
    align      center
    layout     title minimize maximize close
}

style "titlebar:inactive" {
    background "#202326"
    color      "#a1a9b1"
}

style "button" {
    width 32
    size  32
}

style "button:disabled" {
    opacity 0.35
}

style "icon" {
    color   "#ffffff"
    font    "Sans 10"
    shadow  true
    size    48
    spacing 96
    padding 16
    width   88
    gap     8
    theme   Adwaita
}

style "strip" {
    background "#000000"
    color      "#cccccc"
}

style "strip:disabled" {
    color "#4d4d4d"
}
```

Menus and prompts use `#2a2e32` and `#fcfcfc` with `Sans 10`, and their
`radius` is `corner_radius` up to 6. Disabled menu rows use `#a1a9b1`. Some
defaults follow other settings, so changing those settings changes them too:

- the hovered menu row and the selection box are `border_active` at 45%,
  outlined with `border_active` at 80%;
- focused prompt fields and buttons, and selected desktop icons, are outlined
  with `border_active`; a drop target is filled like the selection box;
- menu and prompt borders, prompt field and button borders, and menu
  separators are the inactive titlebar `color` at 30%.

Menu and prompt sizes, and the minimized strip's size, cannot be changed.

### Example: buttons on the left

This titlebar puts round buttons on the left, using an image per button and
state:

```scfg
style "titlebar" {
    layout close minimize maximize title
}

style "button" {
    width 24
    size  14
}

style "close"       { icon "~/.config/leme/theme/close.svg" }
style "close:hover" { icon "~/.config/leme/theme/close-hover.svg" }
style "minimize"    { icon "~/.config/leme/theme/minimize.svg" }
style "maximize"    { icon "~/.config/leme/theme/maximize.svg" }
style "button:inactive" { opacity 0.5 }
```

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
