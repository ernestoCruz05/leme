# Minimal configuration

The repository ships a small starting point at [`config/leme.scfg`](../../config/leme.scfg). Copy it to Leme's user configuration path:

```sh
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
mkdir -p "$config_home/leme"
cp config/leme.scfg "$config_home/leme/config"
```

The file sets up a usable starting session:

- creates three initial tags and allows up to nine;
- selects the dwindle layout;
- uses the US keyboard layout;
- defines `SUPER` as the modifier and `foot` as the terminal;
- binds terminal launch, closing, tag navigation, reload, and quit with
  `SUPER+SHIFT+e`.

Replace `foot` with the terminal command you use. The `spawn` command receives an argument vector directly, so it does not expand shell variables or shell syntax.

The minimal file leaves output selection, pointer settings, appearance, window rules, startup commands, and extra binding modes at their defaults. See the [configuration reference](../configuration/README.md) when you need one of those settings.

For a complete annotated example, see [`config/leme-full.scfg`](../../config/leme-full.scfg). It includes hardware-specific output and pointer examples, binding groups, modes, rules, publication policy, and startup commands. Adapt those values before using them.
