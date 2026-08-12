# Writing `timao` scripts

`timao` uses the same command vocabulary as keybindings. It is useful for bars, launch scripts, and programs that need to switch modes around a game.

## Read one value

```sh
focused_output="$(timao get focused_output)" || exit 1
mode="$(timao get mode)" || exit 1
printf 'output=%s mode=%s\n' "$focused_output" "$mode"
```

A scalar query prints the raw value. `--json` prints the JSON representation instead. In Bash, a comma-separated projection can fill two variables with one query:

```sh
{ read -r output; read -r mode; } \
    < <(timao get focused_output,mode)
```

Collections use stable keys rather than numeric indexes:

```sh
timao get workspaces DP-1:3 layout
```

## Subscribe to changes

Subscribe to everything or name only the events a program needs:

```sh
timao sub layout keyboard_layout
```

The command runs until the client is killed. A bar can read each JSON event and update only the affected field. See the [control protocol](../reference/control-protocol.md) for event shapes.

## Select a mode while a program runs

This portable shell script selects `game` mode, runs the program, and restores
`common` mode when the program exits:

```sh
#!/bin/sh
set -eu

restore_mode() {
    timao mode common >/dev/null 2>&1 || true
}

timao mode game
trap restore_mode 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

game-binary "$@"
```

Do not use `exec` for the final command. The shell must remain running so its
exit trap can restore the mode. The mode must already exist in the scfg file.
The [keybinding reference](../configuration/keybindings.md) explains
`escape_exits` and inherited groups.

## Handle failures

`timao` exits with:

- `0` when Leme accepted the request or returned the query;
- `1` when Leme returned an error;
- `2` when the socket was unreachable or usage was invalid.

The socket path comes from `LEME_SOCKET` for programs spawned by Leme. Outside that environment, use the path described in the [control protocol](../reference/control-protocol.md).

Socket commands cannot spawn processes, quit Leme, or switch VTs. While the session is locked, commands fail and queries continue to answer.
