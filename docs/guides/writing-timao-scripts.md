# Writing `timao` scripts

`timao` uses a prefix language. It parses and checks the forms in the whole source before execution, so a syntax error at the end of a file prevents earlier actions from running. Errors found during execution can follow successful actions. Those earlier effects are not rolled back.

[`scripts/dev-session.timao`](../../scripts/dev-session.timao) sets up a workspace
by reusing or launching applications, waiting for their windows, and moving them
to a selected output and tag. It defaults to a read-only plan. See the
[script documentation](../../scripts/README.md) for options and limitations.

## Read state

```sh
timao eval '(query (count (views)))'
timao --raw eval '(query (get (session) "mode"))'
timao eval '(query (get (session) "keyboard_layout"))'
timao eval '(query (get (config) "diagnostics"))'
```

Use `query`, `act` and `watch` to send expressions to the compositor. Inside a remote item scope, `.urgent` reads a field of the current item. Use `get` to read a local object. The language does not support `event.value` syntax, anonymous functions, shell substitution or implicit host calls.

```sh
timao eval '(query (where (views) .urgent))'
timao eval '(act (command "focus_tag" (list "2")))'
```

Local forms include `do`, `let`, `if`, `and`, `or`, `for-each`, and `try` with a `catch` handler. `def` and `defn` are top-level definitions. Values include null, booleans, finite numbers, strings, lists and objects.

Bindings and function captures are immutable. Functions can call themselves, but do not see definitions introduced later. Predicates, including those passed to `await`, cannot perform host operations. The language does not load imports or startup hooks.

## Stream changes

A default printer is convenient at the shell:

```sh
timao eval '(watch (count (views)))'
```

For a custom handler, save this as a file and use `timao run FILE`:

```lisp
(defn show-value (event)
  (if (or (= (get event "event") "snapshot")
          (= (get event "event") "change")
          (= (get event "event") "reset"))
      (emit (get event "value"))
      null))
(def w (watch (count (views))))
(on w show-value)
```

Handlers receive events for suspension and resets as well as value changes. Check the `event` field before reading `value`. A registered handler keeps the script running. `cancel` accepts the watch handle or its local `watch:N` ID; canceling the last registration lets the script exit when its body has finished. Unhandled file/eval errors stop its registrations. REPL errors affect the current form or handler.

The script body and handlers run serially. Network reads can continue during a request or `await`, but another registered handler will not run inside the current one. Handlers follow delivery order among eligible events on that connection; there is no global ordering across subscriptions.

`await` takes an unattached watch, a predicate function and a timeout in milliseconds. The predicate receives an event with a value, must return a boolean, and cannot perform host operations. Once waiting begins, `await` cancels its watch on success, timeout or error. The deadline includes time spent in the predicate.

## Arguments, output and launch

```lisp
(emit args)
```

With `timao run FILE 'one argument' two`, `args` is the immutable list of those two strings. File return values are silent unless emitted. A byte-zero shebang is accepted in files. Use stdin source with `timao run -`; it cannot simultaneously serve as unrelated application input.

```lisp
(launch (list "application" "literal argument"))
```

`launch` copies the argument array and uses direct or PATH-based exec. It creates a detached session with standard input, output and error connected to `/dev/null`. It does not invoke a shell unless the argument array explicitly names one.

The result acknowledges exec, not window creation or continued application health. Timao does not supervise or wait for the application to exit, close it when the script ends, or associate a future window with that process. Setup failure and `launch_outcome_unknown` are separate errors. Do not automatically retry an unknown launch outcome.

## Recovery and migration

Replace the old `timao get` entrypoint with explicit query expressions, and `timao sub` with a watch. Stdout now defaults to JSON, not ad-hoc scalar lines; request `--raw` only when its restricted scalar representation is appropriate. Sensitive roots are unavailable while locked; use safe `status` information for lock state rather than assuming all queries remain readable.

Timao attempts to restore acknowledged watches after retryable connection failures while a handler, printer or await still uses them. Queries and actions are not replayed. Preserve the request/instance and diagnostic `details` when reporting partial or uncertain actions. Treat `outcome_unknown` as an action that may have happened, not a request to retry.

A `catch` handler receives the remote error payload when available; local errors provide `code` and `message`. It does not receive all the source and call-site information printed by the CLI. There is no rethrow or exit primitive. If a script catches an error and finishes normally, its exit status can be 0. Scripts that report failures through `emit` need callers to inspect those values.

See the [`timao` reference](../reference/timao.md) for options, limits, output and exit status, and [control protocol](../reference/control-protocol.md) for framing and native identities.
