# Window rules

A `window` block matches a native Wayland `app_id` or an X11 class using `fnmatch(3)` pattern matching. Pattern matching is case-sensitive. The wildcard `*` matches any window.

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

## Criteria and keys

| Item | Values | Applied |
| --- | --- | --- |
| block argument | one or more `fnmatch(3)` patterns for `app_id`, X11 `class`, or `*` | selects the application |
| `title` | `fnmatch(3)` glob with `*`, `?`, and bracket expressions | selects the title |
| `tag` | integer from `1` through the output maximum | once |
| `floating` | `true` or `false` | once |
| `fullscreen` | `true` or `false` | once |
| `output` | connector name such as `DP-1` | once |
| `opacity` | decimal from `0.0` through `1.0` | continuously |

A `window` block accepts one or more identity patterns as arguments. When multiple arguments are given, the rule matches if any pattern matches the window's `app_id` or X11 class (OR logic). Pattern matching uses `fnmatch(3)`, supporting `*` (any string), `?` (any single character), and bracket expressions like `[0-9]`.

Punctuation inside an argument is not a delimiter. For example, `"app1;app2"` is treated as a single pattern searching for a literal semicolon. Use separate quoted arguments such as `"app1" "app2"` to specify alternatives.

The special wildcard `"*"` matches every window, including windows that do not report an `app_id` or X11 class. Any other pattern (including globs like `*term*`) requires the window to report an identity before evaluating the match.

If a block includes a `title` directive, both the identity criterion and the title criterion must match (AND logic). A block without `title` matches on identity alone. A block with `"*"` as its argument matches on `title` alone. A missing protocol title cannot satisfy a title criterion.

Every matching rule is applied in file order. For each key, the last matching rule that sets it wins. A wildcard block can provide a baseline for later rules to override.

## Placement behavior

`tag`, `floating`, `fullscreen`, and `output` apply once per window: when it opens, or the first time a later title, `app_id` or X11 class change makes a rule set that key. A key that already applied never applies again, so moving a window later does not cause a rule to move it back, and a title that keeps changing cannot keep moving it. A key that first applies after the window opened applies even if you already moved the window. A late `fullscreen false` does nothing, as it does at open. `opacity` is reevaluated while the window lives, so a title match can follow a title change. A matching rule replaces the active or inactive style opacity; the values are not multiplied.

An output rule without a tag uses the named output's focused tag. If the output is disconnected or the tag is above its maximum, Leme drops that placement key and opens the window normally. A rule cannot prevent a window from opening.

A child with a usable managed parent keeps the parent's output and tag. Matching `floating`, `fullscreen`, and `opacity` keys still apply, but placement keys do not override the parent relationship.

Applications that set their title or class after the window appears, such as Electron apps with a placeholder title or Spotify, get their placement when the matching name arrives. The window opens where it would without the rule first, then moves.

Fullscreen views are always fully opaque. See [appearance](appearance.md) for the reason.
