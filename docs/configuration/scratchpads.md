# Scratchpads

Scratchpads keep a window available without putting it on a tag. Leme has one
compositor-wide pool, shared by every output, and shows at most one pool member
at a time.

## Named scratchpads

Declare a named scratchpad with an exact application identity and an argument
vector to start it:

```scfg
scratchpad "drop" {
    identity "foot-drop"
    spawn foot --app-id foot-drop
    width 0.6
    height 0.6
}
```

The name and `identity` must be nonempty, exact, case-sensitive, and unique.
A name cannot contain any of the six ASCII whitespace characters: space, tab,
newline, carriage return, form feed, or vertical tab. This keeps the name safe
for both bindings and whitespace-separated `timao` commands.

`identity` matches a Wayland app-id or X11 class exactly. It does not match a
window title or a pattern. `spawn` and `identity` are required. `spawn` is an
argument vector, not a shell command, so shell expansion is not performed.

`width` and `height` are fractions of the focused output's usable area. They
default to `0.6` and accept values from `0.1` through `1.0`, inclusive. Named
scratchpads use these dimensions and are centered whenever they are shown.

Malformed scratchpad blocks, duplicate names or identities, and invalid
dimensions are skipped with configuration diagnostics. A binding for a name
that has no valid scratchpad definition is also skipped with a diagnostic. See
[configuration errors](config-errors.md) for diagnostics and reload failures.

## Commands and bindings

Use the standard commands for the unnamed pool:

```scfg
binds "common" {
    SUPER+minus scratchpad_send
    SUPER+equal scratchpad_toggle
    SUPER+SHIFT+minus scratchpad_retrieve
}
```

- `scratchpad_send` sends the focused managed view to the pool. A normal tagged
  view becomes unnamed, and a tiled or fullscreen view becomes floating first.
  Sending a shown scratchpad keeps its named claim.
- `scratchpad_toggle` shows or hides the newest unnamed member. Named members
  are not part of this rotation.
- `scratchpad_retrieve` returns the shown member to the focused tag. When none
  is shown, it returns the newest unnamed member instead.

Use `scratchpad_toggle NAME` for a named definition:

```scfg
binds "common" {
    SUPER+d scratchpad_toggle drop
}
```

A named toggle looks for one view in this order:

1. its currently shown claimed member;
2. its hidden claimed member;
3. the newest hidden unnamed pool member with the configured identity;
4. the most recently focused tagged view with that identity;
5. a new process started from `spawn`.

A named definition claims one member. Other windows with the same identity
remain ordinary windows. Repeating a named toggle while its process is pending
updates the destination output without starting another process.

## Showing, focus, and outputs

Showing or retrieving a scratchpad focuses it. Sending or showing one promotes
it in the global pool. Showing a requested member hides any other shown member.
Requesting a shown member on the focused output hides it. Requesting it from a
different focused output moves it there and keeps it shown.

An unnamed scratchpad keeps its floating frame. Moving it to another output
preserves its size and relative position where possible, then keeps the frame
inside that output's usable area. A named scratchpad is resized and centered on
each show. A currently shown scratchpad supports normal floating move and
resize. Output geometry changes keep its frame inside the usable area.

A shown scratchpad remains visible when its output changes tags, above the
windows on those tags. Hiding a focused scratchpad restores the most recent
eligible view on the focused tag, or clears keyboard focus when there is none.
Hiding a nonfocused member leaves focus unchanged.

If the displayed output is powered off, disabled, or removed, Leme hides the
scratchpad and leaves it in the pool. Scratchpad commands refuse while the
session is locked. Locking keeps the logical shown state, and the scratchpad
returns after unlock.

## Reload and failures

A reload keeps a named claim only when the name and identity still match a
valid definition. Removing, renaming, or changing that identity clears the
claim without closing or moving the existing member. Updated dimensions apply
the next time a named scratchpad is shown. Retrieving a named member also
clears its claim.

If no matching view exists, a named toggle starts its configured process and
waits up to 10 seconds for a matching managed view. A spawn failure or timeout
clears the pending request and records a diagnostic. An unknown named command
fails without starting a process. Configuration diagnostics are available in
the session log and through `timao get config`.

See the [command reference](../reference/commands.md) for all command forms
and [`timao`](../reference/timao.md) for control-socket use.
