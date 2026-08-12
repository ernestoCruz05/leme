# Window rules

A `window` block matches a native Wayland `app_id` or an X11 class. Matching is exact and case-sensitive. `*` matches any application.

```scfg
window "firefox" {
    opacity 0.95
}

window "steam" {
    tag 5
    floating true
}

window "*" {
    title "*Picture-in-Picture*"
    floating true
    opacity 1.0
}
```

## Criteria and keys

| Item | Values | Applied |
| --- | --- | --- |
| block argument | `app_id`, X11 `class`, or `*` | selects the application |
| `title` | `fnmatch(3)` glob with `*`, `?`, and bracket expressions | selects the title |
| `tag` | integer from `1` through the output maximum | once, when the window opens |
| `floating` | `true` or `false` | once, when the window opens |
| `fullscreen` | `true` or `false` | once, when the window opens |
| `output` | connector name such as `DP-1` | once, when the window opens |
| `opacity` | decimal from `0.0` through `1.0` | continuously |

Both criteria must match. A block without `title` matches the application alone. A block with `*` as its argument matches the title alone. A missing protocol title cannot satisfy a title criterion.

Every matching rule is applied in file order. For each key, the last matching rule that sets it wins. A wildcard block can provide a baseline for later rules to override.

## Placement behavior

`tag`, `floating`, `fullscreen`, and `output` apply once when the window opens. Moving a window later does not cause a rule to move it back. `opacity` is reevaluated while the window lives, so a title match can follow a title change. A matching rule replaces the active or inactive style opacity; the values are not multiplied.

An output rule without a tag uses the named output's focused tag. If the output is disconnected or the tag is above its maximum, Leme drops that placement key and opens the window normally. A rule cannot prevent a window from opening.

A child with a usable managed parent keeps the parent's output and tag. Matching `floating`, `fullscreen`, and `opacity` keys still apply, but placement keys do not override the parent relationship.

A title is useful only if the application sets it before the window appears. Electron applications often map with a placeholder title. Use `app_id` for one-shot placement in that case; opacity continues to reevaluate.

Fullscreen views are always fully opaque. See [appearance](appearance.md) for the reason.
