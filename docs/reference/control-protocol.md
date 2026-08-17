# Control protocol

Leme exposes a Unix socket for scripts and desktop shells. It covers state that is not already published by a Wayland protocol.

Tags and windows remain in Wayland protocols:

| Need | Protocol |
| --- | --- |
| tag list, names, active and urgent state | `ext-workspace-v1` |
| window list, titles, app ids, focus, close, fullscreen | `wlr-foreign-toplevel-management-v1` |
| panels and bars | `wlr-layer-shell-v1` |
| lock surfaces | `ext-session-lock-v1` |

## Socket

The socket is at `$XDG_RUNTIME_DIR/leme-$WAYLAND_DISPLAY.sock` and has mode `0600`. Leme exports its path as `LEME_SOCKET` to child processes.

At most 16 clients can connect. A request line longer than 1024 bytes is rejected and the connection closes. A client that stops reading is disconnected when its output buffer fills; it cannot stall the compositor.

Access control comes from filesystem ownership and mode. There is no second authentication handshake.

## Requests

Each request is one whitespace-separated line. The verbs are the same tokens accepted in a `binds` block:

```text
set_layout accordion
switch_layout
toggle_floating
toggle_sticky
scratchpad_send
scratchpad_toggle
scratchpad_toggle drop
scratchpad_retrieve
mode game
```

The socket refuses these commands:

| Command | Reason |
| --- | --- |
| `spawn` | prevents process execution through the control interface |
| `quit` | prevents a client from terminating the session |
| `switch_vt` | reserves VT switching for local bindings and the seat session |

Replies are one JSON object per request:

```json
{"ok":true}
{"ok":true,"value":"accordion"}
{"ok":false,"error":"invalid command frobnicate"}
{"ok":false,"error":"refused: spawn"}
{"ok":false,"error":"session locked"}
```

## Queries

`get` walks the state tree. The final token can be a comma-separated field list:

```text
get
get mode
get focused_output
get focused_output,mode
get keyboard_layout active,available
get workspaces DP-1:3 layout
get config
```

Each successful query response is an object with `"ok":true` and a `value`
member. A single field puts its value in `value`. A comma list puts an object
keyed by the requested fields in `value`. Collections use stable keys rather
than indexes because adaptive tags can materialize and disappear.

The state tree contains focused output, mode, keyboard layouts, published workspaces, focused-view state, and configuration diagnostics. It does not contain window titles or content.

### Focused view

`get focused_view` puts the focused-view projection in `value`. The projection
has `floating` first, `scratchpad` second, and `sticky` third:

```json
{"ok":true,"value":{"floating":true,"scratchpad":false,"sticky":true}}
```

A focused scratchpad has `"scratchpad":true` and `"sticky":false`:

```json
{"ok":true,"value":{"floating":true,"scratchpad":true,"sticky":false}}
```

With no focused view, the complete response is:

```json
{"ok":true,"value":null}
```

Each focused-view field is also available as a query:

```text
get focused_view floating
get focused_view scratchpad
get focused_view sticky
get focused_view floating,scratchpad,sticky
```

Each query puts a JSON boolean in `value`, or `null` when no view is focused:

| Request | True response | False response | No focused view |
| --- | --- | --- | --- |
| `get focused_view floating` | `{"ok":true,"value":true}` | `{"ok":true,"value":false}` | `{"ok":true,"value":null}` |
| `get focused_view scratchpad` | `{"ok":true,"value":true}` | `{"ok":true,"value":false}` | `{"ok":true,"value":null}` |
| `get focused_view sticky` | `{"ok":true,"value":true}` | `{"ok":true,"value":false}` | `{"ok":true,"value":null}` |

The comma query puts an object in `value` in `floating`, `scratchpad`, `sticky` order.
For a focused scratchpad and with no focused view, respectively, the responses
are:

```json
{"ok":true,"value":{"floating":true,"scratchpad":true,"sticky":false}}
{"ok":true,"value":{"floating":null,"scratchpad":null,"sticky":null}}
```

## Events

`subscribe` with no arguments delivers all events. Naming fields filters the stream:

```text
subscribe
subscribe keyboard_layout mode
```

| Event | Shape |
| --- | --- |
| `layout` | `{"event":"layout","workspace":"DP-1:3","layout":"accordion"}` |
| `keyboard_layout` | `{"event":"keyboard_layout","layout":"us"}` |
| `mode` | `{"event":"mode","mode":"resize"}` |
| `focused_output` | `{"event":"focused_output","output":"HDMI-A-1"}` |
| `view` | `{"event":"view","floating":true,"scratchpad":false,"sticky":true}` |
| `config` | `{"event":"config","diagnostics":3,"truncated":false}` |

Events are generated from reconciled state. A client that never subscribes
receives none. Event JSON is not wrapped in a query response envelope.

The `view` event has all three fields in canonical order. When no view is
focused, all values are null:

```json
{"event":"view","floating":null,"scratchpad":null,"sticky":null}
```

## Locked sessions and stale sockets

While locked, commands return `session locked` and events stop. Queries continue to answer because this state tree carries no window content or titles. Unlocking rebuilds the published state.

A compositor killed without cleanup can leave its socket behind. The next Leme instance tests it first. A refused connection is treated as stale and reclaimed. A live connection is left alone, and Leme starts without the control interface rather than taking the socket. Failure to create the socket does not abort compositor startup.

See [`timao`](timao.md) for the client interface.
