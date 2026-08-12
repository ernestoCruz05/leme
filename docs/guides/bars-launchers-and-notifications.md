# Bars, launchers, and notifications

Bars, launchers, wallpapers and notification daemons place themselves through
layer-shell, the Wayland interface for desktop surfaces. Leme positions them
around managed windows according to the layer they ask for.

## Layer-shell surfaces

Leme supports background, bottom, top, overlay, tiled, floating, drag, and lock surfaces. It honors exclusive zones and keyboard interactivity. Background surfaces stay below managed windows. Top and overlay surfaces stay above them.

Viewporter and fractional-scale support are available to current layer-shell clients. Leme does not load wallpaper images or animate those clients itself.

## Start external programs

You can start a program after Leme finishes compositor startup with an `exec`
block:

```scfg
exec {
    awww-daemon --no-cache
    waybar
    mako
}
```

`exec` receives argument vectors directly and runs once at startup. A configuration reload does not start the programs again. The programs are not build dependencies, and Leme does not manage their complete lifetime on exit.

If you need a bar to remain tied to the graphical login, use the service manager or session wrapper provided by your distribution.

## Media keys

Audio and media control remain external. If `wpctl` and `playerctl` are
installed, add a reusable group such as:

```scfg
bind_group "media" {
    XF86AudioRaiseVolume spawn wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+
    XF86AudioLowerVolume spawn wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-
    XF86AudioMute spawn wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
    XF86AudioPlay spawn playerctl play-pause
    XF86AudioNext spawn playerctl next
    XF86AudioPrev spawn playerctl previous
}
```

Then place `inherit media` inside the existing `binds "common"` block.

## Input and focus

A layer-shell client can request keyboard interactivity. Exclusive zones reserve space for tiled content; a surface without an exclusive zone overlays content instead. Session-lock surfaces follow a separate fail-closed policy described in the [locking and idle guide](locking-and-idle.md).
