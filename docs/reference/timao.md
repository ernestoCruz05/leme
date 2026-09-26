# `timao`

`timao` is Leme's control client and scripting runtime. It writes JSON by default. Help and version commands do not connect to the compositor, and invocations do not load startup scripts or modules.

## Invocation

```sh
timao --help
timao --version
timao toggle_floating
timao set_layout accordion
timao eval '(query (views))'
timao --raw eval '(query (get (session) "mode"))'
timao run script.timao 'literal argument'
timao run - 'literal argument'
timao repl
```

Global options precede the entrypoint: `--socket PATH`, `--json`, `--raw`, `--human`, `--help`, `--version`, and `--`. Conflicting output formats are errors. There is no implicit REPL. `eval` takes exactly one source argument; `run` takes a filename (`-` means stdin) followed by immutable script `args`. Paths and arguments are literal, not shell-expanded by timao.

The client uses `--socket` when supplied. Otherwise it uses a nonempty `LEME_SOCKET`, falling back to `$XDG_RUNTIME_DIR/leme-$WAYLAND_DISPLAY.sock` if that variable is unset or empty. It requires the socket and peer to belong to the current effective user and the socket permissions to be `0600`. On failure, it does not remove the socket or search for another compositor.

## Commands

Command names and argument syntax are documented in the [command reference](commands.md):

- `focus_next_tag`, `focus_previous_tag`, `focus_last_tag`, `focus_previous_view`
- `focus_tag`, `focus`, `focus_output`
- `switch_layout`, `set_layout`, `remove_empty_tag`, `mode`
- `toggle_floating`, `toggle_sticky`, `toggle_fullscreen`, `close_view`
- `move`, `move_view_to_tag`, `move_view_to_output`, `resize`
- `reload_config`, `cycle_keyboard_layout`
- `scratchpad_send`, `scratchpad_retrieve`, `scratchpad_toggle`

These commands use the compositor's command adapter. `spawn`, `quit` and `switch_vt` are not available as simple CLI commands. Scripts can start a detached process with `launch`. A command can succeed with a no-op result, for example when a requested state is already set.

## Queries and watches

```sh
timao eval '(query (tags))'
timao eval '(query (outputs))'
timao eval '(query (get (config) "path"))'
timao eval '(query (get (config) "diagnostics"))'
timao eval '(watch (count (views)))'
```

Use `query` and `watch` expressions for state access. `timao get` and `timao sub` are not supported entrypoints. Request the fields you need: returning a whole `config` or `runtime` value can exceed clone or response limits even when a single field would fit.

When eval returns an unattached watch, timao installs a default printer and keeps running. A definition returns null, so `(def w (watch ...))` alone does not install a printer. A watch with a user handler keeps that handler instead. A `watch:N` ID belongs to the current timao process; it is not the server's subscription token and cannot cancel another client's watch.

Timao attempts recovery after retryable connection failures for acknowledged watches still used by a handler, printer or await. Successful recovery starts a new generation and supplies a reset event. Queries and actions are never replayed. Watches report current values and may combine intermediate changes into one event.

Locking suspends sensitive watches and discards pending sensitive data. Unlocking produces a fresh reset; data already delivered to a client cannot be recalled. See the [control protocol](control-protocol.md) for recovery and locking rules.

## Output and interaction

JSON plus LF is the default on stdout, including terminals. Successful simple commands print their action value, not an IPC envelope. Eval and REPL display ordinary results; files print only explicit `emit` output. Watch events include local identity and generation. Callable/watch descriptions, diagnostics, prompts and editing UI use stderr.

`--raw` accepts scalars and flat scalar arrays. It rejects embedded LF/CR/NUL, objects and nested collections, and does not preserve the type information available in JSON. Simple action results and default watch events cannot use raw output; a watch handler can emit scalar values explicitly. `--human` produces escaped output with presentation limits. Use JSON for machine processing.

Diagnostics use JSON in every output mode and include remote details when available. If diagnostic formatting runs out of space or memory, the fallback reports that details are unavailable.

The REPL supports multiline parser-based input, UTF-8 scalar editing and bounded memory-only history. It is not a grapheme-cluster editor. Non-terminal input is line-fed without prompts. Errors are contained to the current form or handler; failed handlers are removed while independent registrations can continue/recover. Ctrl-C clears input or interrupts active work; earlier effects are not rolled back. EOF cancels registrations and exits. If cancellation interrupts an already partly written record, the invocation ends rather than joining subsequent output onto a corrupt record; that final record may be truncated.

## Limits and exit status

The runtime has a 64 MiB parent memory budget, a 32 MiB interpreter limit and 2 MiB client queues. Source is limited to 1 MiB and watches to 32. These limits track allocations made through the runtime's accounting system; they do not cap process RSS.

Request and negotiation waits have deadlines. Output waits can be interrupted by cancellation. Shutdown allows a shared 250 ms grace period for cleanup, though a delayed helper may remain unreaped when it ends.

- `0`: successful finite completion or REPL EOF.
- `1`: unhandled runtime, remote, resource, launch or output failure.
- `2`: usage, source/preparation or initial connection failure.
- `130` / `143`: noninteractive SIGINT / SIGTERM interruption.

A script can catch an error and finish with status 0. For scripts that emit failure records, callers must inspect those records as well as the exit status.

An `outcome_unknown` diagnostic means an action may have happened. Do not replay it automatically. Formatting failures and cancellation do not undo actions. A detached launch acknowledgement confirms exec; it does not confirm window creation or continued application health.

See [writing scripts](../guides/writing-timao-scripts.md) for the language boundary and examples.
