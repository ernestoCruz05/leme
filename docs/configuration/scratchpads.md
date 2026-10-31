# Scratchpads

A scratchpad is a window kept off the tags that you show and hide with a key.
There is one pool shared by every output, and one scratchpad shows at a time,
above the windows of whatever tag is on screen.

```scfg
scratchpad "drop" {
    identity "foot-drop"
    spawn foot --app-id foot-drop
    width 0.6
    height 0.6
}

binds "common" {
    SUPER+minus scratchpad_send
    SUPER+equal scratchpad_toggle
    SUPER+SHIFT+minus scratchpad_retrieve
    SUPER+d scratchpad_toggle drop
}
```

## Commands

| Command | Effect |
| --- | --- |
| `scratchpad_send` | moves the focused window into the pool |
| `scratchpad_toggle` | shows or hides the newest window sent to the pool |
| `scratchpad_toggle NAME` | shows or hides a named scratchpad, starting it if needed |
| `scratchpad_retrieve` | puts the shown scratchpad, or the newest one, back on the focused tag |

Toggling a scratchpad that is shown on another output moves it to the focused
one.

## Named scratchpads

A `scratchpad "NAME"` block ties a name to an app. Its toggle uses a running
window with that identity, or starts `spawn` when there is none.

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `identity` | an exact `app_id` or X11 class | required | which window belongs to it |
| `spawn` | a program and its arguments | required | started when no window matches |
| `width` | `0.1` to `1.0` | `0.6` | fraction of the output width |
| `height` | `0.1` to `1.0` | `0.6` | fraction of the output height |
| `spawn_timeout` | `1000` to `600000` ms | `10000` | how long to wait for the started app's window |

Names and identities must be unique, and names cannot contain whitespace. A
named scratchpad is centered at its size every time it shows. Raise
`spawn_timeout` for apps that start slowly; a window that opens after it runs
out is a normal window.

Sticky windows cannot be sent to the pool, and scratchpads cannot be made
sticky.
