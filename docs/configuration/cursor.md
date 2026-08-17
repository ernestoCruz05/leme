# Cursor

The `cursor` block sets the pointer theme and size:

```scfg
cursor {
    theme Adwaita
    size 24
}
```

| Key | Values | Default |
| --- | --- | --- |
| `theme` | an installed XCursor theme name | system default |
| `size` | integer from `1` through `512` in logical pixels | `24` |

An unset `theme` keeps whatever theme the system resolves. A size outside the accepted range is reported rather than clamped, so a value that would not work says so instead of being silently changed.

Leme exports `XCURSOR_SIZE`, and `XCURSOR_THEME` when a theme is configured, before starting XWayland. Both names also reach the D-Bus activation environment alongside `WAYLAND_DISPLAY` and `DISPLAY`.

That export is what keeps X11 clients consistent with the rest of the session. An X11 client chooses its own pointer through libXcursor, which falls back to a size derived from the screen resolution when the environment says nothing. The fallback is unrelated to the size Leme draws, so without the export the pointer changes size as it crosses into an X11 window.

The [`env`](startup-and-environment.md) block cannot replace this. It applies to programs Leme launches, not to XWayland, which inherits Leme's own environment. An `XCURSOR_SIZE` set through `env` still overrides the exported value for those programs.

Leme also gives XWayland the matching image for the X root window, which no client sets.

## Reload

A reload that changes `theme` or `size` reloads the theme at every active output scale, applies the pointer again, and updates the root cursor. When the new theme cannot be loaded, the previous one stays in place and the failure is logged. Programs already running keep the environment they started with.
