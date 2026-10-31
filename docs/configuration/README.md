# Configuration

Leme reads `$XDG_CONFIG_HOME/leme/config.scfg`, or
`~/.config/leme/config.scfg` when `XDG_CONFIG_HOME` is unset. A file named
`config` in the same directory is used when `config.scfg` does not exist.

`leme --config-check [PATH]` checks a file without starting a session.
`reload_config` applies changes, and a file that fails to load leaves the
running config in place. See [configuration errors](config-errors.md).

## Examples

- [`config/leme.scfg`](https://github.com/ernestoCruz05/leme/blob/main/config/leme.scfg)
  is a small starting point.
- [`config/leme-full.scfg`](https://github.com/ernestoCruz05/leme/blob/main/config/leme-full.scfg)
  shows most blocks, some commented out. Change its connector names, device
  names and programs before copying from it.

## Topics

| Topic | Covers |
| --- | --- |
| [Syntax and variables](syntax-and-variables.md) | blocks, `vars`, loops, conditions, environment |
| [Tags](tags.md) | tag counts, layouts, per-tag settings |
| [Desktop layout](desktop.md) | the desktop layout, its file grid, menus and styling |
| [Appearance](appearance.md) | gaps, borders, opacity, rounding, blur |
| [Animation](animation.md) | window, layout and workspace animations |
| [Window rules](window-rules.md) | per-app tag, output, floating and opacity |
| [Scratchpads](scratchpads.md) | the scratchpad pool and named scratchpads |
| [Outputs](outputs.md) | modes, placement, scale, behavior across outputs |
| [Keyboard](keyboard.md) | XKB layouts and repeat |
| [Pointer](pointer.md) | libinput settings, per device |
| [Gestures](gestures.md) | touchpad workspace swipes |
| [Cursor](cursor.md) | cursor theme and size |
| [Keybindings](keybindings.md) | binds, modes, binding groups |
| [Startup and environment](startup-and-environment.md) | `exec` and `env` |
| [Configuration errors](config-errors.md) | reading diagnostics, reload behavior |

All commands are in the [command reference](../reference/commands.md).
