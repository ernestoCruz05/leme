# Configuration errors

Most mistakes are warnings: the bad line is skipped, or the key keeps its
default, and the rest of the file loads. Some mistakes reject the whole file:

- a syntax error, such as an unclosed quote or brace;
- an unknown or duplicated top-level block;
- an unknown variable or list, or a broken `for` or `if` header;
- a bad `binds` setup: no `common` mode, or a missing or cyclic group;
- a keymap that does not compile;
- outputs the hardware refuses, or a `maximum` below a tag in use;
- a syntax error in `leme.env`.

A rejected file at startup starts Leme on safe defaults. A rejected reload keeps
the running config.

## Safe defaults

| Key | Action |
| --- | --- |
| `SUPER+Return` | opens `$TERMINAL`, or the first of foot, alacritty, kitty, wezterm, ghostty and xterm that is installed |
| `SUPER+r` | `reload_config` |
| `SUPER+q` | quits Leme |
| `CTRL+ALT+F1` to `F12` | switches VT |
| `CTRL+ALT+space` | next keyboard layout |

## Checking a file

```sh
leme --config-check
leme --config-check ./draft.scfg
```

It checks the file a session would load, or the path given, prints every
problem with its line, and exits non-zero if there were any. `-c` is the short
form.

```text
warning: unknown directive `bordr_width` in `style`
  --> config.scfg:19:5
    |
 19 |     bordr_width 2
    |     ^^^^^^^^^^^
    |
    = help: a directive with a similar name exists: `border_width`

config.scfg: 1 warning
```

A problem inside a `for` loop or `if` gets a `= note:` line naming the
iteration or branch it came from.

## In a running session

The session log has every problem, one per line:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log
```

`timao` returns the latest ones:

```sh
timao eval '(query (get (config) "diagnostics"))'
```

The `config_errors` block is accepted but does nothing yet.
