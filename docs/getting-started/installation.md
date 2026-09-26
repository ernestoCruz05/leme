# Installation

Want rounded corners or blur? Read [Visual effects](#visual-effects) before
building or installing. The standard build commands below leave these effects
disabled; the effects build uses its own patched wlroots.

## Requirements

For the standard build, install the development packages for:

- wlroots 0.20.x, with pkg-config name `wlroots-0.20`;
- Wayland server 1.22 or newer;
- xkbcommon 1.5 or newer;
- libinput;
- Pixman.

You also need a C17 compiler, Meson, Ninja, Git, pkg-config or pkgconf, and
`wayland-scanner`. The scanner ships in the Wayland package on many distributions
and as `dev-util/wayland-scanner` on Gentoo. Your wlroots package may require
newer versions of its dependencies than Leme's direct checks above.

The standard build accepts wlroots versions from 0.20.0 up to, but not including,
0.21.0. wlroots 0.19 and earlier are not supported. Check the installed version:

```sh
pkg-config --modversion wlroots-0.20
```

Both build variants use the yyjson revision pinned in
`subprojects/yyjson.wrap`. Meson fetches it during setup, so the first build needs
network access unless that source is already present. A system yyjson package
does not replace this subproject.

## Build

From the repository root:

```sh
meson setup build --buildtype=release --prefix=/usr
meson compile -C build
```

For an existing build directory, change the options explicitly and rebuild:

```sh
meson configure build -Dbuildtype=release -Dprefix=/usr
meson compile -C build
```

Use `meson setup --wipe build --buildtype=release --prefix=/usr` to discard the
cached build configuration, for example after changing compilers.

## Install

With the `/usr` prefix configured above:

```sh
sudo meson install -C build
```

The install contains:

```text
/usr/bin/leme
/usr/bin/leme-session
/usr/bin/timao
/usr/share/wayland-sessions/leme.desktop
/usr/share/xdg-desktop-portal/leme-portals.conf
/usr/lib/systemd/user/leme-session.target
```

Check which executables your shell resolves and which build is installed:

```sh
command -v leme timao
leme --version
timao --version
```

An older copy under `/usr/local/bin` can take precedence over `/usr/bin` in
`PATH`. Check both paths if the version or behavior does not match the build you
just installed.

Read [minimal configuration](minimal-config.md) before copying a config file.
Read [first session](first-session.md) before selecting Leme from a display
manager or launching it from a TTY.

## NixOS

See the [NixOS installation instructions](nixos.md). The flake's package builds
with effects and supplies the pinned wlroots and yyjson sources before Meson
runs. Meson downloads are disabled in that build.

## Visual effects

The `style` settings `corner_radius` and `blur` require the effects build and a
supported renderer. A build without effects accepts these settings but ignores
them. See [appearance](../configuration/appearance.md).

With `-Deffects=true`, Meson builds wlroots **0.20.2** from
`subprojects/wlroots.wrap`, applies
`subprojects/packagefiles/wlroots-rounded.patch`, and links it statically into
Leme. This build does not use the system wlroots library. Do not change the wrap
to a different wlroots release without checking and rebasing the patch.

Building the pinned wlroots requires at least:

| Dependency | Version |
| --- | --- |
| Meson | 1.3 |
| Wayland server and scanner | 1.24.0 |
| wayland-protocols | 1.47 |
| libdrm | 2.4.129 |
| xkbcommon | 1.8.0 |
| Pixman | 0.43.0 |

Install the additional wlroots build dependencies for the backends and renderers
you need. These include libinput, libudev/libseat, graphics libraries and display
information packages for a direct DRM session. Check Meson's feature summary
for DRM, libinput, GLES2 and XWayland support rather than assuming they were all
enabled.

Use a separate build directory to keep the variants distinct:

```sh
meson setup build-effects --buildtype=release --prefix=/usr -Deffects=true
meson compile -C build-effects
sudo meson install -C build-effects
```

For an existing effects build:

```sh
meson configure build-effects -Dbuildtype=release -Dprefix=/usr -Deffects=true
meson compile -C build-effects
sudo meson install -C build-effects
```

The patched wlroots library, headers and pkg-config file are not installed.
Leme, Timao and the session files use the configured prefix. The effects build
still needs its other runtime dependencies; linking wlroots statically does not
make Leme a fully static executable.

## Configuration file location

Leme reads `$XDG_CONFIG_HOME/leme/config.scfg`, or
`~/.config/leme/config.scfg` when `XDG_CONFIG_HOME` is unset. The
[configuration reference](../configuration/README.md) covers the fallbacks and
reload behavior.
