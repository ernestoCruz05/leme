# `timao`

`timao` is Leme's command-line client. Programs launched by Leme receive the socket path in `LEME_SOCKET`.

## Commands

Use the same verbs as scfg bindings:

```sh
timao set_layout accordion
timao switch_layout
timao toggle_floating
timao cycle_keyboard_layout
timao mode game
```

The socket refuses `spawn`, `quit`, and `switch_vt`. While the session is locked, commands fail and queries continue to answer.

## Queries

`get` walks the state tree using whitespace-separated tokens:

```sh
timao get mode
timao get focused_output
timao get keyboard_layout active
timao get keyboard_layout active,available
timao get workspaces DP-1:3 layout
timao get config
```

One field prints a scalar. A comma-separated list prints one line per field in the requested order. `--json` forces the raw JSON value.

Collections use stable keys rather than numeric indexes. Workspace ids include the connector, such as `DP-1:3`. A keyboard layout with a variant is reported as `name(variant)`.

The state tree includes the focused output, mode, keyboard layouts, workspaces, focused view, and the latest configuration diagnostics. `focused_view` is `null` when no view is focused.

## Events

Subscribe to every event or name only the fields a program needs:

```sh
timao sub
timao sub keyboard_layout mode
timao sub layout
```

The command streams until it is killed. Events are coalesced by a reconciliation pass, so a burst of changes produces one event for each field that actually changed.

## Exit status

- `0`: Leme accepted the command or returned the query;
- `1`: Leme returned an error;
- `2`: the socket was unreachable or usage was invalid.

See [writing `timao` scripts](../guides/writing-timao-scripts.md) for shell patterns and the [control protocol](control-protocol.md) for wire details.
