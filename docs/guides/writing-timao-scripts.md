# Writing `timao` scripts

`timao` scripts are a small prefix language. The whole file is checked before
anything runs, so a syntax error at the end stops the actions at the start.
Actions that ran before a runtime error are not undone.

[`scripts/dev-session.timao`](https://github.com/ernestoCruz05/leme/blob/main/scripts/dev-session.timao)
is a full example: it opens or reuses apps, waits for their windows and moves
them to an output and tag. See the
[scripts README](https://github.com/ernestoCruz05/leme/blob/main/scripts/README.md).

## Reading and acting

```sh
timao eval '(query (count (views)))'
timao eval '(query (where (views) .urgent))'
timao eval '(act (command "focus_tag" (list "2")))'
```

`query` reads, `act` changes, `watch` streams. Inside them, `.field` reads a
field of the current item. `get` reads a field of a local value.

| Forms | Use |
| --- | --- |
| `def`, `defn` | top-level values and functions |
| `let`, `do`, `if`, `and`, `or`, `for-each` | local logic |
| `try` with `catch` | handle errors |
| `query`, `act`, `watch` | talk to Leme |
| `on`, `cancel`, `await` | handle watch events |
| `emit` | print a value |
| `launch` | start a program |

Values are null, booleans, numbers, strings, lists and objects. Bindings cannot
be changed after they are made.

## Watching

`timao eval '(watch (count (views)))'` prints every change. In a script, attach
a handler with `on`:

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

Handlers also get `suspended` and `error` events, so check `event` before
reading `value`. The script keeps running while a handler is attached;
`cancel` removes it. Handlers run one at a time.

`(await WATCH PREDICATE TIMEOUT_MS)` waits until the predicate returns true for
an event, then cancels the watch.

## Arguments and programs

`args` is the list of arguments after the file name in
`timao run FILE ARG ...`. A file prints nothing unless it uses `emit`.

```lisp
(emit args)
(launch (list "foot" "--app-id" "notes"))
```

`launch` starts a program detached, without a shell, with its output going to
`/dev/null`. It only confirms the program started, not that a window opened.

A `catch` handler gets the error with `code` and `message`. A script that
catches every error exits with status 0, so check what it emits.
