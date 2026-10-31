# Control protocol

The control socket speaks JSON, one object per line. [`timao`](timao.md) uses
it, and so can any other client. Bars and panels can also use the
[Wayland protocols](wayland-protocols.md) instead.

The socket is `$XDG_RUNTIME_DIR/leme-$WAYLAND_DISPLAY.sock`, mode `0600`, and
its path is in `LEME_SOCKET`. Check that you own it before connecting.

## Requests

Start with `hello`. Its reply has the compositor's `instance`, which every
later request carries:

```json
{"version":1,"id":"h:1","op":"hello"}
{"version":1,"id":"q:1","instance":"INSTANCE","op":"query","expr":{"call":"count","args":[{"call":"views","args":[]}]}}
{"version":1,"id":"q:2","instance":"INSTANCE","op":"query","source":"(count (views))"}
```

| `op` | Effect |
| --- | --- |
| `hello` | starts the session and returns `instance`, capabilities and limits |
| `query` | evaluates a read-only expression |
| `act` | checks an action plan, then applies it |
| `watch` | evaluates an expression now and again on every change |
| `unwatch` | stops a watch by its `subscription` |

`query`, `act` and `watch` carry one expression, either as `expr` or as
`source`. `expr` is a tree of `{"literal": VALUE}`, `{"call": NAME, "args":
[...]}` and `{"field": [...]}`. `source` is the same expression as text, in
the [syntax `timao` uses](timao.md#expressions). The roots are `views`, `tags`,
`outputs`, `inputs`, `session`, `config`, `runtime` and `status`. `act` accepts
the [commands](commands.md) except `spawn`, `quit` and `switch_vt`.

A reply has `type: "reply"`, the request `id`, `ok` and either `value` or
`error`. An error has `code`, `message` and `phase`, plus `expr_path` when it
points into the expression and `offset` when it points at a byte: into the
frame for a JSON error, into `source` for a syntax error. An `act` that fails partway leaves the earlier actions applied, and the
error says which ones ran.

## Watches

`watch` replies first, then sends `type: "event"` records with its
`subscription` and an increasing `sequence`:

| `event` | Carries |
| --- | --- |
| `snapshot`, `change` | the full current `value` |
| `reset` | a fresh `value`, for example after unlock |
| `suspended` | nothing; the session is locked |
| `error` | the error that ended the watch |

Changes are coalesced: a connection gets at most one update per 16 ms, always
with the latest value. Locking suspends watches on private data and refuses
private queries and actions until unlock.

## Limits

| Limit | Default |
| --- | --- |
| clients | 16 |
| request size | 64 KiB |
| response size | 1 MiB |
| JSON depth | 64 |
| expression nodes | 4096 |
| expression nesting | 64 levels of the tree; a call takes two |
| outstanding requests per client | 16 |
| watches per client | 32 |
| action targets | 256 |

Ask for the fields you need. A whole root can go over the response limit when
one field of it fits.
