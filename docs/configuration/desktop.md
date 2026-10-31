# Desktop layout

The desktop layout is experimental. Windows are placed freely and get
titlebars, the desktop folder shows as icons behind them, and windows can be
minimized and grouped. Set it per tag with `layout desktop` in
[tags](tags.md), or switch the focused tag with `set_layout desktop` or
`switch_layout`.

```scfg
tags {
    maximum 9
    tag 3 {
        layout desktop
    }
}

desktop {
    minimized_strip true
    container_modifier shift
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `minimized_strip` | `true`, `false` | `true` | shows minimized windows in a strip at the bottom of the output |
| `container_modifier` | `shift`, `ctrl`, `alt`, `super` | `shift` | key held while dragging a window onto another to group them |

Turn the strip off when a bar or dock already lists minimized windows.

## Windows

Drag a titlebar, or SUPER-drag anywhere, to move a window. SUPER-right-drag
resizes it. Focus does not raise a window; a click does, along with its
dialogs.

Titlebars have the title and minimize, maximize and close buttons.
Double-clicking the title maximizes. Right-clicking it opens the window menu,
also bound with `window_menu`: minimize, maximize, fullscreen, keep on all tags,
move to tag or output, and close. Apps that draw their own titlebar keep it.

Drag a window against an edge of the output to place it: the top maximizes, the
sides take a half, the corners a quarter. Dragging it away restores its size.
`snap DIR` and `place POSITION` do the same from the keyboard, see the
[command reference](../reference/commands.md#desktop).

Leaving the desktop layout tiles the windows again, except the ones set
floating.

## Minimizing

`minimize_view` hides the focused window, `restore_minimized` brings back every
minimized window on the tag, and `show_desktop` minimizes all of them and
restores them on the second press. Click a window in the strip to restore it.

Minimized windows stay in bars and window lists. Leaving the desktop layout
restores them. Fullscreen, sticky and scratchpad windows cannot be minimized.

## Containers

Hold `container_modifier` while dragging a window onto another to group them,
or use `container_join DIR`. A container has one titlebar and tiles its members
inside it.

| Command | Effect |
| --- | --- |
| `container_layout dwindle\|master_stack` | layout inside the container |
| `move DIR` | swaps the focused member with its neighbor |
| `resize DIR N` | moves the split next to the focused member |
| `container_detach` | takes the focused member out |
| `container_dissolve` | breaks the container up |

SUPER-dragging a member out of the container also detaches it. The titlebar
buttons and `minimize_view` act on the whole container. Containers cannot
nest, dialogs, sticky windows and scratchpads cannot join one, and leaving the
desktop layout dissolves them.

## File grid

The grid shows the XDG desktop folder, or `~/Desktop`, without hidden files.
There is no grid when the desktop folder is your home directory.
Icons stay where you drop them, saved in `$XDG_STATE_HOME/leme/desktop-positions`.
Changes to the folder show up on their own.

- Click selects, Ctrl+click adds or removes, and dragging on empty space draws
  a selection box. The arrow keys move the selection.
- Double-click or Enter opens the selection with `xdg-open`. Executables and
  `.desktop` launchers are never run.
- Drag icons to move them on the grid, onto a folder to move the files into it,
  or onto a window to drop them there as a copy.
- Right-click empty space for New folder and Open desktop folder, or an icon for
  Open, Rename and Move to Trash.

Clicking the grid gives it keyboard focus, as does `focus_desktop`. While it has
focus, `grid_*` bindings work and `desktop_menu` opens its menu. The
[command reference](../reference/commands.md#file-grid) lists them.

## Desktop menus

A `desktop_menu` block replaces the right-click menu: `"blank"` for empty space,
`"files"` for icons.

```scfg
desktop_menu "blank" {
    entry "New folder" grid_new_folder
    entry "Open terminal here" spawn foot --working-directory=%dir
    separator
    entry "Open desktop folder" grid_open
}

desktop_menu "files" {
    entry "Open" grid_open
    entry "Edit" spawn kate %files
    separator
    entry "Rename" grid_rename
    entry "Move to Trash" grid_trash
}
```

| Row | Effect |
| --- | --- |
| `entry "LABEL" COMMAND` | a row running any Leme command |
| `separator` | a line between rows |

In `spawn` arguments, `%dir` is the desktop folder and `%files` is one argument
per selected file, as full paths. `%files` must be an argument on its own and
only works in `"files"`. The `grid_*` commands act on the icons that were
right-clicked. A menu holds up to 64 rows with labels of 1 to 63 bytes, and an
empty block turns that menu off.

## Styling

A `style` block with a name styles one part of the desktop: titlebars,
buttons, menus, prompts, the strip, icons or the selection box. The name is an
element, optionally followed by `:` and a state. `reload_config` applies
changes.

```scfg
style "titlebar" {
    background "#2a2e32"
    color      "#fcfcfc"
}

style "close:hover" {
    background "#da4453"
}
```

| Element | Draws | States | Properties |
| --- | --- | --- | --- |
| `titlebar` | titlebars of windows and containers | `inactive` | `background` `color` `font` `height` `align` `layout` |
| `button` | all titlebar buttons | `inactive` `hover` `pressed` `disabled` | `background` `color` `radius` `width` `size` `opacity` `icon` |
| `minimize`, `close` | one titlebar button | as `button` | as `button`, without `width` |
| `maximize` | the maximize button | as `button`, and `maximized` | as `button`, without `width` |
| `menu` | window and desktop menus | | `background` `color` `font` `radius` `border_color` |
| `menu_item` | menu rows | `hover` `disabled` | `background` `color` `border_color` |
| `menu_separator` | lines between rows | | `color` |
| `prompt` | rename, new folder and error prompts | | `background` `color` `font` `radius` `border_color` |
| `prompt_field` | the name field | `focus` | `background` `color` `border_color` |
| `prompt_button` | prompt buttons | `focus` | `background` `color` `border_color` |
| `strip` | the minimized strip and its arrows | `disabled` | `background` `color` |
| `icon` | desktop icons and labels | `selected` `drop` | `background` `border_color` `color` `font` `shadow` `size` `spacing` `padding` `width` `gap` `theme` |
| `selection` | the selection box and selected prompt text | | `background` `border_color` |

| Property | Values |
| --- | --- |
| `background`, `color`, `border_color` | `#RRGGBB` or `#RRGGBBAA` |
| `font` | a Pango font, such as `"Inter Semi-Bold 10"` |
| `radius` | `0` to `64` |
| `width` | `16` to `512` |
| `height` | `16` to `96` |
| `size` | `8` to `256` |
| `padding` | `0` to `256` |
| `spacing` | `32` to `512` |
| `gap` | `0` to `64` |
| `opacity` | `0.0` to `1.0` |
| `align` | `left`, `center` |
| `layout` | `title` and up to three of `minimize`, `maximize`, `close` |
| `shadow` | `true`, `false` |
| `icon` | path to an `.svg` or `.png` |
| `theme` | an icon theme, such as `Papirus` |

Sizes are logical pixels. States apply in this order, the later one winning:
`inactive`, `maximized`, `hover`, `pressed`, `focus`, `selected`, `drop`,
`disabled`. `minimize`, `maximize` and `close` start from `button`. Unset
`color` and `font` come from the parent: buttons from the titlebar, rows from
the menu, prompt parts from the prompt. Sizes, `align` and `layout` cannot be
set on a state.

- `titlebar`: buttons listed before `title` in `layout` go on the left, the
  rest on the right.
- buttons: `width` is the button and `size` the glyph inside it. `icon`
  replaces the glyph with an image, relative to the config file or `~/`;
  `color` only tints the built-in glyphs, and `opacity` fades either.
- `icon`: `size` is the icon, `spacing` the grid cell, `padding` the margin
  around the grid, `width` the label width and `gap` the space between icon
  and label. `theme` also applies to the strip and falls back to Adwaita.

The defaults are Breeze Dark:

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

Menus and prompts use the titlebar colors and font, with a `radius` of
`corner_radius` up to 6. Hovered rows, the selection box and focused prompt
parts follow `border_active`. Menu, prompt and strip sizes cannot be changed.

Buttons on the left, each with its own image:

```text
style "titlebar" {
    layout close minimize maximize title
}

style "button" {
    width 24
    size  14
}

style "close"           { icon "~/.config/leme/theme/close.svg" }
style "close:hover"     { icon "~/.config/leme/theme/close-hover.svg" }
style "minimize"        { icon "~/.config/leme/theme/minimize.svg" }
style "maximize"        { icon "~/.config/leme/theme/maximize.svg" }
style "button:inactive" { opacity 0.5 }
```
