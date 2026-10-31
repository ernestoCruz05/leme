# `timao`

`timao` talks to a running Leme over its control socket. It runs commands,
reads and watches state, and runs scripts. Output is JSON.

```sh
timao toggle_floating
timao set_layout master_stack
timao get session focused_output name
timao watch tags
timao eval '(query (views))'
timao run script.timao 'an argument'
timao repl
```

| Form | Effect |
| --- | --- |
| `timao COMMAND [ARG ...]` | runs a [command](commands.md), except `spawn`, `quit` and `switch_vt` |
| `timao get ROOT [FIELD ...]` | prints a value once |
| `timao watch ROOT [FIELD ...]` | prints a value every time it changes |
| `timao eval SOURCE` | runs one expression |
| `timao run FILE [ARG ...]` | runs a script file, `-` for stdin |
| `timao repl` | interactive prompt |

`ROOT` is `views`, `tags`, `outputs`, `inputs`, `session`, `config`, `runtime`
or `status`. Each `FIELD` reads one member and follows references, so
`session focused_output name` is the focused output's name. Ask for the fields
you need: a whole `config` or `runtime` can be too big for one reply.

| Option | Effect |
| --- | --- |
| `--socket PATH` | socket to use |
| `--json` | JSON output, the default |
| `--raw` | plain scalars, one per line |
| `--human` | readable output, not for scripts |

Without `--socket`, `timao` uses `LEME_SOCKET`, then
`$XDG_RUNTIME_DIR/leme-$WAYLAND_DISPLAY.sock`. The socket must belong to you and
have mode `0600`.

## Queries and actions

```sh
timao eval '(query (tags))'
timao eval '(query (get (config) "diagnostics"))'
timao eval '(query (where (views) .minimized))'
timao eval '(watch (count (views)))'
timao eval '(act (set-minimized (where (views) .minimized) false))'
timao eval '(act (container-join (get (session) "focused_view") "left"))'
```

`query` reads, `watch` streams and `act` changes something. The language is in
[writing `timao` scripts](../guides/writing-timao-scripts.md).

| Action | Effect |
| --- | --- |
| `set-minimized VIEWS BOOL` | minimizes or restores desktop windows, on any tag |
| `container-layout VIEW LAYOUT` | `dwindle` or `master_stack` inside the view's container |
| `container-join VIEW DIR` | groups the view with its neighbor |
| `container-detach VIEW` | takes the view out of its container |
| `container-dissolve VIEW` | breaks up the view's container |
| `set-config PATH VALUE` | changes a setting until the next reload |

## Live settings

`set-config` changes style, cursor, pointer, gesture, output, publication and
`desktop.minimized_strip` settings without editing the file. A reload puts the
file's values back.

```sh
timao get config effective style gap_outer
timao eval '(act (set-config (list "style" "gap_outer" "top") 20))'
timao eval '(act (set-config (list "style" "smart_gaps") true))'
```

## Window fields for bars

View records have `minimized` and `maximized` booleans, and a `container` text
that is the same for every member of a container and `null` otherwise:

```sh
timao get views | jq 'map(select(.container != null)) | group_by(.container)'
```

Minimized windows stay in `views`. Container members report `maximized` as
false even when the container is maximized.

## Exit status

| Status | Meaning |
| --- | --- |
| `0` | success |
| `1` | the command, script or connection failed while running |
| `2` | bad usage, a script that does not parse, or no connection |
| `130`, `143` | interrupted by SIGINT or SIGTERM |
