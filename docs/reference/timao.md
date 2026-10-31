# `timao`

`timao` talks to a running Leme over its control socket. It runs commands,
evaluates expressions and watches state. Output is JSON.

```sh
timao toggle_floating
timao set_layout master_stack
timao get session focused_output name
timao watch tags
timao query '(count (views))'
timao act '(set-minimized (where (views) .minimized) false)'
timao watch '(get (session) "mode")'
```

| Form | Effect |
| --- | --- |
| `timao COMMAND [ARG ...]` | runs a [command](commands.md), except `spawn`, `quit` and `switch_vt` |
| `timao get ROOT [FIELD ...]` | prints a value once |
| `timao watch ROOT [FIELD ...]` | prints a value every time it changes |
| `timao query EXPRESSION` | evaluates an expression once |
| `timao act EXPRESSION` | runs an action expression |
| `timao watch EXPRESSION` | evaluates an expression now and on every change |

`ROOT` is `views`, `tags`, `outputs`, `inputs`, `session`, `config`, `runtime`
or `status`. Each `FIELD` reads one member and follows references, so
`session focused_output name` is the focused output's name. Ask for the fields
you need: a whole `config` or `runtime` can be too big for one reply.

`watch` takes an expression when its argument starts with `(`.

| Option | Effect |
| --- | --- |
| `--socket PATH` | socket to use |
| `--json` | JSON output, the default |
| `--raw` | plain scalars, one per line |

Options go before the form. Without `--socket`, `timao` uses `LEME_SOCKET`,
then `$XDG_RUNTIME_DIR/leme-$WAYLAND_DISPLAY.sock`. The socket must belong to
you and have mode `0600`.

## Expressions

`query`, `act` and `watch` take one expression in prefix form. Leme reads and
checks it; `timao` sends the text as it is.

```sh
timao query '(tags)'
timao query '(get (config) "diagnostics")'
timao query '(where (views) .minimized)'
timao query '(map (where (views) (= .output.name "eDP-1")) (object "app" .app_id "title" .title "tag" .tag.number))'
timao act '(container-join (get (session) "focused_view") "left")'
timao act '(command "focus_tag" (list "2"))'
```

| Syntax | Meaning |
| --- | --- |
| `(NAME ARG ...)` | calls an operator |
| `.field`, `.a.b` | reads a field of the current item |
| `"text"` | a string, with JSON escapes |
| `12`, `-1.5` | a number |
| `true`, `false`, `null` | literals |
| `; text` | a comment to the end of the line |

Names are only valid as operators. `.field` works inside the item arguments of
`where`, `select`, `map` and `sort-by`. Build lists with `(list ...)` and
objects with `(object KEY VALUE ...)`.

| Kind | Operators |
| --- | --- |
| roots | `views` `tags` `outputs` `inputs` `session` `config` `runtime` `status` |
| lookup | `view` `tag` `output` `input` `by-id` `get` `item` |
| collections | `where` `select` `map` `sort-by` `limit` `first` `count` `contains` `list` `object` |
| comparison | `=` `!=` `<` `<=` `>` `>=` |
| arithmetic | `+` `-` `*` `/` |
| logic | `and` `or` `not` `if` |

An `act` expression has one action at its root:

| Action | Effect |
| --- | --- |
| `set-minimized VIEWS BOOL` | minimizes or restores desktop windows, on any tag |
| `container-layout VIEW LAYOUT` | `dwindle` or `master_stack` inside the view's container |
| `container-join VIEW DIR` | groups the view with its neighbor |
| `container-detach VIEW` | takes the view out of its container |
| `container-dissolve VIEW` | breaks up the view's container |
| `set-config PATH VALUE` | changes a setting until the next reload |
| `command NAME ARGS` | runs a [command](commands.md) with a list of string arguments |

## Watching

`watch` prints a `snapshot` line, then one line per change. Each line is a
watch event from the [control protocol](control-protocol.md#watches), with the
result in `value`. With `--raw`, `watch` prints only the values. `timao`
exits when the watch ends with an error or the compositor closes the socket.

## Live settings

`set-config` changes style, cursor, pointer, gesture, output, publication and
`desktop.minimized_strip` settings without editing the file. A reload puts the
file's values back.

```sh
timao get config effective style gap_outer
timao act '(set-config (list "style" "gap_outer" "top") 20)'
timao act '(set-config (list "style" "smart_gaps") true)'
```

## Window fields for bars

View records have `minimized` and `maximized` booleans, and a `container` text
that is the same for every member of a container and `null` otherwise:

```sh
timao get views | jq 'map(select(.container != null)) | group_by(.container)'
```

Minimized windows stay in `views`. Container members report `maximized` as
false even when the container is maximized.

## Errors

Errors go to stderr, one JSON object per line. An error from Leme has `code`,
`message` and `phase`, plus `expr_path` when it points into the expression. A
syntax error has `expr_path` `/source` and `offset`, the byte where reading
stopped:

```sh
$ timao query '(count (views)'
{"code":"invalid_request","message":"unclosed list","phase":"decode","expr_path":"/source","offset":0}
```

Errors from `timao` itself use the codes `usage`, `connection`, `permission`,
`raw_output`, `output`, `protocol` and `memory`.

## Exit status

| Status | Meaning |
| --- | --- |
| `0` | success |
| `1` | the expression or command failed while running, or the connection or output failed |
| `2` | bad usage, no connection, or Leme refused the request before running it |

`timao` does not catch signals. SIGINT and SIGTERM end it, which a shell
reports as `130` and `143`.
