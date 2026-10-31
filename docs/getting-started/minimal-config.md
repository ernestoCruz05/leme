# Minimal configuration

[`config/leme.scfg`](https://github.com/ernestoCruz05/leme/blob/main/config/leme.scfg)
is a small starting config. Copy it into place:

```sh
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
mkdir -p "$config_home/leme"
cp config/leme.scfg "$config_home/leme/config.scfg"
```

It sets three tags out of nine, dwindle, the `us` layout and `foot` as the
terminal. Change `term` if you use another one. Its bindings:

| Keys | Action |
| --- | --- |
| `SUPER+Return` | terminal |
| `SUPER+q` | close the window |
| `SUPER+arrows` | focus in that direction |
| `SUPER+SHIFT+arrows` | move the window |
| `SUPER+1` to `9` | go to that tag |
| `SUPER+SHIFT+1` to `9` | move the window to that tag |
| `SUPER+v` | float or tile |
| `SUPER+f` | fullscreen |
| `SUPER+w` | next layout: dwindle, master-stack, desktop |
| `SUPER+m`, `SUPER+SHIFT+m` | minimize, restore minimized (desktop layout) |
| `SUPER+r` | reload the config |
| `SUPER+SHIFT+e` | quit |

[`config/leme-full.scfg`](https://github.com/ernestoCruz05/leme/blob/main/config/leme-full.scfg)
shows most blocks, and the [configuration reference](../configuration/README.md)
covers each one.
