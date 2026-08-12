# Configuration errors

Leme reports configuration problems without discarding every valid setting.
Most malformed scalar settings are dropped, recorded, and replaced by their
built-in values. Invalid bindings are recorded and skipped without a
replacement. Duplicate directives are invalid; do not rely on which duplicate
value remains after recovery.

Some failures reject the complete load. These include invalid scfg syntax,
failed variable expansion, duplicate or unknown top-level blocks, structurally
invalid or duplicate named output and pointer blocks, unresolved binding-group
names or inheritance, no usable `common` mode, keyboard compilation failure,
reducing `maximum` below a materialized tag, and output configuration rejected
by the hardware.

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
config.scfg:6:19: unterminated double-quoted string
    border_active "#296bb8
                  ^
config.scfg: configuration rejected
```

Recoverable problems, the ones that would otherwise only reach the session
log, are listed with their line numbers:

```text
config.scfg:14: border_width requires one nonnegative integer
config.scfg: 1 problem found
```

## Find the error

`leme-session` writes diagnostics to:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log
```

Read the first reported error before the later ones. One mistake can cause
follow-up diagnostics.

The control interface publishes the latest diagnostics:

```sh
timao get config
```

The response includes the configuration path, line-numbered messages, and a
flag indicating whether the diagnostic list reached its limit.

## On-screen diagnostics

The `config_errors` block is accepted, but the current build does not draw an
on-screen report. Its `show`, `position`, and
`timeout` values have no visible effect. Use the session log or `timao get
config` until the banner listed on the [roadmap](../../ROADMAP.md) is available.

A successful reload is transactional. Leme parses, compiles, and tests the
replacement before changing the running session. See [startup and
environment](startup-and-environment.md) for entries that run only at startup.
