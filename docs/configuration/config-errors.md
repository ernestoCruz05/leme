# Configuration errors

Leme reports configuration problems without discarding every valid setting.
Most malformed scalar settings are dropped, recorded, and replaced by their
built-in values. Unset or empty environment variable reads (`$(env.NAME)`) record a
recoverable diagnostic and substitute `none`. Invalid bindings are recorded and skipped
without a replacement. Duplicate directives are invalid; do not rely on which duplicate
value remains after recovery.

Some failures reject the complete load. These include invalid scfg syntax,
malformed template loop or condition headers, variable and list declaration errors,
unknown selected variables or lists, template limit violations, duplicate or unknown
top-level blocks, structurally invalid or duplicate named output and pointer blocks,
unresolved binding-group names or inheritance, no usable `common` mode, keyboard
compilation failure, reducing `maximum` below a materialized tag, and output
configuration rejected by the hardware.

Inactive conditional branches suppress variable expansion and semantic diagnostics, but
cannot suppress lexer or parser syntax errors.

At startup, a rejected load uses safe defaults. During reload, the running
configuration remains in place.

## Check before you reload

```sh
leme --config-check
```

Validates the configuration Leme would load, reports every problem it finds,
and exits non-zero if there were any. The short form is `leme -c`. Pass a path
to check a file before installing it:

```sh
leme --config-check ./draft.scfg
```

A given path is used exactly as written. Without one, the check resolves the
same file a session would load, so the two never disagree.

Failures that reject the load are shown with the offending line and a caret:

```text
error: unterminated double-quoted string
  --> config.scfg:6:19
   |
 6 |     border_active "#296bb8
   |                   ^
config.scfg: configuration rejected
```

Recoverable problems, the ones that would otherwise only reach the session
log, are listed with their line numbers:

```text
warning: border_width requires one nonnegative integer
  --> config.scfg:14:18
   |
14 |     border_width nonsense
   |                  ^^^^^^^^

config.scfg: 1 warning
```

## Reading expansion notes

When a problem occurs inside a template loop or condition, `leme --config-check` adds an expansion trail note explaining how the offending directive was generated:

```text
warning: width requires a finite decimal from 0.1 through 1.0
  --> config.scfg:14:15
    |
 14 |         width 9.0
    |               ^^^
    |
    = note: expanded from `for pad`, iterations "term" and "editor"

warning: unknown directive `bordr_width` in `style`
  --> config.scfg:19:5
    |
 19 |     bordr_width 2
    |     ^^^^^^^^^^^
    |
    = help: a directive with a similar name exists: `border_width`

config.scfg: 2 warnings
```

A `= note:` line names the loop iteration or branch that generated the directive. When the same problem occurs across multiple iterations of a loop, a single diagnostic note covers them together.

The session log carries the compact one-line form. Query published diagnostics with `timao eval '(query (get (config) "diagnostics"))'`.

## Environment file diagnostics

When loading an optional `leme.env` file beside the configuration, syntax errors and budget overruns reject the configuration load:

- `expected NAME=value`: a non-comment line contains no `=` separator.
- `expected an entry name before '='`: missing key name before `=`.
- `invalid character in entry name`: key contains characters other than letters, digits, and underscores.
- `entry name must not begin with a digit`: key begins with a digit.
- `unexpected 'export' prefix; write NAME=value`: line begins with `export `.
- `file exceeds 65536 bytes`: the file exceeds the 65536-byte budget.
- `line exceeds 4096 bytes`: a single line exceeds the 4096-byte line budget.
- `more than 256 entries`: the file contains more than 256 entries.

Duplicate entries within `leme.env` are recoverable warnings:

- `duplicate entry NAME; last value wins`: a key was defined multiple times; the last value takes precedence.

## Find the error

`leme-session` writes diagnostics to:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log
```

Read the first reported error before the later ones. One mistake can cause
follow-up diagnostics.

The control interface publishes the latest diagnostics:

```sh
timao eval '(query (get (config) "diagnostics"))'
```

The response includes line-numbered diagnostics and truncation information.
Query the configuration path separately with `timao eval '(query (get (config) "path"))'`.

## On-screen diagnostics

The `config_errors` block is accepted, but the current build does not draw an
on-screen report. Its `show`, `position`, and
`timeout` values have no visible effect. Use the session log or the diagnostics
query above until the banner listed on the [roadmap](../../ROADMAP.md) is available.

A successful reload is transactional. Leme parses, compiles, and tests the
replacement before changing the running session. See [startup and
environment](startup-and-environment.md) for entries that run only at startup.
