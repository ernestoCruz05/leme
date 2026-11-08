# Bars, launchers and notifications

Bars, launchers, wallpapers and notification daemons are layer-shell clients:
they pick a layer and Leme places them there. Exclusive zones push tiled windows
aside, and keyboard interactivity works. Start them with `exec`:

```scfg
exec {
    awww-daemon --no-cache
    waybar
    mako
}
```

`exec` runs once at startup and does not restart anything that exits. Use your
service manager if you want that.

## Data for a bar

Bars that support `ext-workspace-v1` and the foreign-toplevel protocols work as
they are. A custom bar can also follow Leme's state with `timao watch`, which
prints a `snapshot` line and then one line per change, with the result in
`value`:

```sh
timao watch session mode
timao watch 'EXPRESSION'
```

| Shows | Expression |
| --- | --- |
| focused output | `(get (get (session) "focused_output") "name")` |
| tags to draw | `(select (where (tags) .navigable) .number .active .view_count .urgent .output.name)` |
| focused window | `(select (where (views) .focused) .title .app_id)` |
| layout on `DP-1` | `(get (first (where (tags) (and .active (contains (list "DP-1") .output.name)))) "layout")` |
| keyboard layout | `(get (get (session) "keyboard_layout") "active")` |
| binding mode | `(get (session) "mode")` |

The focused-window query returns an empty list when nothing has focus.

## Media keys

Audio and media control are up to other programs. With `wpctl` and
`playerctl`:

```scfg
bind_group "media" {
    XF86AudioRaiseVolume spawn wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+
    XF86AudioLowerVolume spawn wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-
    XF86AudioMute spawn wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
    XF86AudioPlay spawn playerctl play-pause
    XF86AudioNext spawn playerctl next
    XF86AudioPrev spawn playerctl previous
}

binds "common" {
    inherit media
}
```
