# Installation

## Requirements

Build Leme with:

- a C17 compiler;
- Meson and Ninja;
- wlroots 0.20.x;
- Wayland server 1.22 or newer;
- xkbcommon 1.5 or newer;
- libinput;
- Pixman;
- `wayland-scanner`, which ships inside the `wayland` package on most
  distributions and as `dev-util/wayland-scanner` on Gentoo.

## Build

From the repository root:

```sh
meson setup build
meson compile -C build
```

Add `--wipe` to `meson setup` to reconfigure from scratch.

## Install

For a system installation under `/usr`:

```sh
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

The install contains:

```text
/usr/bin/leme
/usr/bin/leme-session
/usr/bin/timao
/usr/share/wayland-sessions/leme.desktop
/usr/share/xdg-desktop-portal/leme-portals.conf
```

Check which build is installed with:

```sh
leme --version
```

Read [minimal configuration](minimal-config.md) before copying a config file. Read [first session](first-session.md) before selecting Leme from a display manager or launching it from a TTY.

## Visual effects

Some appearance settings need more than the scene graph a stock wlroots
provides, so they are gated behind a build option. A build without it accepts
those settings, validates them, and ignores them; nothing else changes.

| Setting | Block |
| --- | --- |
| `corner_radius` | [`style`](../configuration/appearance.md) |
| `blur` | [`style`](../configuration/appearance.md) |

Build and install as above, with two added flags:

```sh
meson setup build --prefix=/usr -Deffects=true
meson compile -C build
sudo meson install -C build --skip-subprojects
```

Without `--skip-subprojects`, wlroots' own headers and `wlroots-0.20.pc` land
in the prefix, and anything else on the machine that builds against wlroots
would find Leme's patched copy instead of the system one. The `leme` binary
is unaffected either way, because the patched renderer is linked into it.

The option compiles wlroots from source, pinned to the revision in
`subprojects/wlroots.wrap`, with the rendering patch in
`subprojects/packagefiles/`, and links it statically. The cost is that a
wlroots release needs the patch rebased rather than just a version bump, so
this build follows wlroots on Leme's schedule instead of the distribution's.

## Configuration file location

Leme reads `$XDG_CONFIG_HOME/leme/config.scfg`. The
[configuration reference](../configuration/README.md) covers the fallbacks and
reload behavior.
