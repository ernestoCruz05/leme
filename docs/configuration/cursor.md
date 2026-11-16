# Cursor

The `cursor` block sets the cursor theme and size, and when the cursor hides.

```scfg
cursor {
    theme Adwaita
    size 24
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `theme` | an installed XCursor theme | the system default | cursor theme |
| `size` | `1` to `512` | `24` | cursor size in logical pixels |
| `hide_after` | milliseconds, `0` to `600000` | `0` | hides the cursor after this long without pointer input, `0` turns it off |
| `hide_when_typing` | `true`, `false` | `false` | hides the cursor while you type |

Leme exports `XCURSOR_THEME` and `XCURSOR_SIZE` to XWayland and D-Bus activated
programs, so X11 windows use the same cursor. Programs that are already running
keep the cursor they started with after a reload.
