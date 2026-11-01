# Installation

## Requirements

- wlroots 0.20.x (`wlroots-0.20`)
- Wayland server 1.22 or newer, and `wayland-scanner`
- xkbcommon 1.5 or newer
- libinput, libdrm, libudev, Pixman
- librsvg 2.46 or newer, Cairo, Pango, GLib with GIO
- a C17 compiler, Meson, Ninja, pkg-config

yyjson 0.12 or newer is used if installed. Otherwise Meson downloads the pinned
copy on the first setup.

## Install script

```sh
curl -fsSL https://raw.githubusercontent.com/ernestoCruz05/leme/main/install.sh | bash
```

The script checks these requirements and lists anything missing by its
pkg-config name. In a terminal it shows a menu to choose
[effects](#rounded-corners-and-blur), the source directory (`~/.local/src/leme`
by default), the install prefix and the version, and writes nothing until you
pick Install. It builds in `build-release` with `-Dwerror=false`, logs the
build to `~/.cache/leme-install.log` and installs to `/usr`, asking for sudo
only for that step. Running it again updates the source and reinstalls. From a
clone, `./install.sh` builds the checked-out code instead.

On NixOS the script points to the [flake](nixos.md) instead. It also stops
before building when the prefix is read-only, as `/usr` is on Fedora
Silverblue; use `--prefix ~/.local` there and start Leme from a TTY.

| Option | Effect |
| --- | --- |
| `--effects`, `--no-effects` | answers the effects question |
| `--dir PATH` | where to clone the source |
| `--prefix PATH` | where to install, `/usr` by default |
| `--main` | builds the main branch instead of the latest release |
| `--yes` | takes the defaults without asking |
| `--plain` | asks line by line instead of showing the menu |

To uninstall, run `sudo ninja -C ~/.local/src/leme/build-release uninstall`.

## Build and install

```sh
meson setup build --buildtype=release --prefix=/usr
ninja -C build
sudo ninja -C build install
```

This installs `leme`, `leme-session`, `timao`, a `leme.desktop` session for
display managers, a portal config and `leme-session.target` for systemd.

Leme builds with `-Werror`. If a newer compiler stops the build on a warning,
add `-Dwerror=false`. If `leme --version` shows an older build, check for a
copy in `/usr/local/bin`.

## Rounded corners and blur

`corner_radius` and `blur` need the effects build, which compiles a patched
wlroots 0.20.2 into Leme instead of using the system one:

```sh
meson setup build-effects --buildtype=release --prefix=/usr -Deffects=true
ninja -C build-effects
sudo ninja -C build-effects install
```

That wlroots needs Meson 1.3, Wayland 1.24.0, wayland-protocols 1.47, libdrm
2.4.129, xkbcommon 1.8.0 and Pixman 0.46.0 or newer, plus the wlroots backend
dependencies. Check Meson's summary that DRM, libinput, GLES2 and XWayland are
enabled.

## Runtime packages

Leme starts without these, but most desktops want them:

| Package | For |
| --- | --- |
| `xdg-desktop-portal`, `xdg-desktop-portal-gtk` | file choosers and other portals |
| `xdg-desktop-portal-luminous` | screenshots and screen sharing with a window picker |
| `xdg-desktop-portal-wlr` | screen sharing of whole outputs, where luminous is not packaged |
| `pipewire`, `wireplumber` | screen sharing streams |
| XWayland | X11 apps, see [XWayland](../guides/xwayland.md) |
| Adwaita icon theme, `xdg-utils` | icons and opening files in the desktop layout |

Log in again after installing a portal backend.

On NixOS, see [NixOS](nixos.md). Then read
[minimal configuration](minimal-config.md) and
[first session](first-session.md).
