# Window rules

A `window` block applies settings to windows whose Wayland `app_id` or X11
class matches one of its patterns.

```scfg
window "firefox" {
    opacity 0.95
}

window "steam" {
    tag 5
    floating true
}

window "steam_app_*" {
    tag 7
}

window "app1" "app2" {
    floating true
}

window "*" {
    title "*Picture-in-Picture*"
    floating true
    opacity 1.0
}
```

| Key | Values | Effect |
| --- | --- | --- |
| `title` | a pattern | the title must match too |
| `tag` | `1` to `tags.maximum` | open on this tag |
| `floating` | `true`, `false` | open floating or tiled |
| `fullscreen` | `true`, `false` | open fullscreen |
| `output` | a connector name such as `DP-1` | open on this output |
| `opacity` | `0.0` to `1.0` | replaces the style opacity |

Patterns are case-sensitive `fnmatch(3)` globs: `*`, `?` and `[0-9]`. A block
matches when any of its patterns does. `"*"` also matches windows with no
`app_id` or class; any other pattern needs one.

Every matching rule applies, in file order, and the last one to set a key wins.

`tag`, `floating`, `fullscreen` and `output` apply once, when the window opens
or when a later title or class change first makes them match. Moving the window
afterwards is never undone. `opacity` follows the window for as long as it
lives. Dialogs stay with their parent's tag and output, and an `output` that is
not connected is ignored.
