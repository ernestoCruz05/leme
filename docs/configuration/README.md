# Configuration

Leme reads one scfg file:

```text
$XDG_CONFIG_HOME/leme/config.scfg
```

When `XDG_CONFIG_HOME` is empty or unset, the path is `~/.config/leme/config.scfg`. A file named `config`, with no extension, is still read when `config.scfg` is absent. There is no `LEME_CONFIG` override.

At startup, a missing or invalid file selects safe defaults and logs the problem. `reload_config` parses and tests a replacement before applying it. If parsing, keyboard compilation, or output testing fails, the running configuration stays in place.

A valid reload applies the smallest recoverable changes it can. Some malformed directives fall back individually; fatal failures keep the previous configuration or use safe defaults at startup. See [configuration errors](config-errors.md) for the split.

## Examples

- [`config/leme.scfg`](../../config/leme.scfg) is the small starting point. Copy it before your first session.
- [`config/leme-full.scfg`](../../config/leme-full.scfg) is the complete annotated example. It includes hardware-specific output and pointer examples, modes, binding groups, window rules, publication, and startup commands.

The complete file is linked rather than pasted here. Change connector names, modes, pointer device names, and external programs before using those parts of it.

## Topics

| Topic | Use it when you want to... |
| --- | --- |
| [Syntax and variables](syntax-and-variables.md) | understand blocks, values, and `$name` expansion |
| [Tags](tags.md) | set tag counts, layouts, and adaptive-tag behavior |
| [Appearance](appearance.md) | set gaps, borders, and opacity |
| [Animation](animation.md) | configure window open and close effects or workspace transitions |
| [Window rules](window-rules.md) | place, float, fullscreen, or fade matching windows |
| [Scratchpads](scratchpads.md) | keep unnamed windows ready or configure named scratchpads |
| [Outputs](outputs.md) | configure modes, placement, cross-output behavior, or publication |
| [Keyboard](keyboard.md) | choose XKB layouts and variants |
| [Pointer](pointer.md) | set libinput defaults or exact-device overrides |
| [Cursor](cursor.md) | set the pointer theme and size, including for X11 clients |
| [Keybindings](keybindings.md) | define commands, modes, and reusable binding groups |
| [Startup and environment](startup-and-environment.md) | launch programs and set their child environment |
| [Configuration errors](config-errors.md) | find rejected settings in logs or through `timao` |

For the complete command list, see the [command reference](../reference/commands.md). For the behavior behind tags and layouts, see [concepts](../concepts/README.md).
