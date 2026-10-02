# JSON control protocol

The control socket carries UTF-8 JSON records, each containing one object followed by LF. It supports queries, actions and watches. Native toplevel/workspace publication, panels and session locking use Wayland protocols.

## Endpoint and negotiation

Leme creates `$XDG_RUNTIME_DIR/leme-$WAYLAND_DISPLAY.sock` with mode `0600` and exports its path as `LEME_SOCKET`. Clients must validate the socket owner, permissions and peer credentials, then send a HELLO request before other operations.

```json
{"version":1,"id":"h:1","op":"hello"}
```

HELLO returns a reply carrying the current opaque compositor `instance`, version/capability/limit information, and a null revision. Ordinary requests include that instance. IDs and instance tokens are opaque strings, not timestamps or persistent entity handles. Never reuse an entity reference from another compositor instance.

```json
{"version":1,"id":"q:1","instance":"INSTANCE-FROM-HELLO","op":"query","expr":{"call":"count","args":[{"call":"views","args":[]}]}}
```

Successful replies have `type: "reply"`, the original `id`, `instance`, `revision`, `ok: true` and `value`. Failures have `ok: false` and a structured `error` instead of `value`. The error phase identifies where the request failed: decoding, validation, evaluation, preflight or execution. Preserve its details, including which actions were applied, failed or left unattempted. A failed request may have applied some actions.

## Expressions and actions

Expressions use explicit forms such as `{"literal": VALUE}`, `{"call": NAME, "args": [...]}` and item-relative `{"field": ["path", "segments"]}`. Operators and fields are validated before effects. Roots include views, tags, outputs, inputs, session, config, runtime and status. Published views include urgency and explicit ownership information. Remote item scopes and typed entity references are not arbitrary pointer or object access. `get` and field paths read through a reference: a member the reference itself doesn't carry is read from the entity it names, so `(get (get (session) "focused_output") "name")` returns the focused output's name.

`query` is read-only. `act` evaluates and preflights an action plan before applying it. The command adapter accepts the command names in the [client reference](timao.md); it does not expose compositor `spawn`, `quit` or `switch_vt`. If execution fails after some actions have run, those effects remain. The client does not replay queries or actions.

Project only the data needed. Large whole roots and deeply nested/materialized values can exceed clone, work or response limits; a small query of a field may fit when its entire root does not.

## WATCH and UNWATCH

`watch` takes an expression without side effects and returns a subscription owned by the connection. Its acknowledgement precedes the initial snapshot, though both records may arrive in one read. `unwatch` takes that subscription's native token. Another connection cannot cancel it.

```json
{"version":1,"id":"w:1","instance":"INSTANCE-FROM-HELLO","op":"watch","expr":{"call":"count","args":[{"call":"views","args":[]}]}}
{"version":1,"id":"u:1","instance":"INSTANCE-FROM-HELLO","op":"unwatch","subscription":"sub:1"}
```

Native event records have `type: "event"`, `event`, `subscription`, `sequence`, `instance` and `revision`:

- `snapshot`, `change`: a complete `value`.
- `reset`: a complete fresh `value`, with `reason: "unlock"` on native unlock resets.
- `suspended`: `reason: "session_locked"`, null revision, no value or error.
- `error`: a terminal structured error, not a value.

Sequences are canonical positive uint64 decimal strings; they increase, but gaps are allowed. Watches coalesce coherent current snapshots using full value equality. They are not a lossless event journal. Safe-root watches have null revisions.

The timao client normalizes these into local `watch:N` identities and generations. After recovery its reset reason can be `reconnect` or `instance_changed`; those client-local events are not new native wire kinds. Local watch IDs never address another process's registrations.

## Locking, connection loss and bounds

Sensitive queries/actions are refused while locked. Sensitive watches discard pending private values and suspend; safe status observations can continue. Unlock publishes a fresh baseline, not stale deltas. Already delivered or executing data cannot be recalled. A connection may be closed if sensitive output has already started.

Timao attempts recovery after retryable connection failures for watches that were acknowledged, remain uncancelled, and still have a handler, printer or active await. It negotiates HELLO, reads safe status, and registers those watches in creation order. Reconnect attempts use a backoff capped at 8 seconds. Protocol, capability or resource failures can terminate a watch instead.

Queries and actions are never replayed. If the client sent the final LF of an ACT but received no authoritative reply, the action's outcome may be unknown. Do not assume it failed or retry it automatically.

The default limits are 16 clients, 65,536 request bytes and 1,048,576 response bytes excluding LF, JSON depth 64, 4,096 expression nodes, field-path depth 16, 16 outstanding requests, 32 watches, 256 action targets and 100,000 work units. Requests processed in the same IPC scheduler turn share a 5 ms semantic deadline.

Each connection has an 8 MiB memory account, including a 2 MiB output queue limit. Snapshots have a 32 MiB limit. The public model and IPC accounts share a 64 MiB parent budget; these are not separate 64 MiB allowances for each client. Limit checks can reject requests or close connections. The accounting does not measure process RSS, and the semantic deadline does not bound the duration of every syscall or domain operation.

## Snapshot caching

Leme attempts to cache runtime metadata before opening the control socket.
Runtime and config roots have separate sealed builders. Leme can reuse an
unchanged sensitive snapshot after checking the lock state and request budget.
Config mutations and feature changes update the metadata generation keys. Other
state changes can reuse metadata whose keys still match.

A fresh sensitive capture includes all roots so Leme can compare it with the
previous baseline. The revision advances when values change, not merely because
the cache was invalidated. Cached config references are checked against the
current entity indexes. Queries with different root selections retain separate
projections of an unchanged baseline.

Lock transitions discard the model's sensitive baseline and config cache. Safe
runtime metadata may remain cached. Cached values count toward memory limits,
and captures still use the request's work and deadline limits.

For CLI output formats and script usage, see the [`timao` reference](timao.md) and [scripting guide](../guides/writing-timao-scripts.md).
