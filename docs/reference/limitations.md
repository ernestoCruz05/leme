# Limitations

- Adaptive sync has no fullscreen-only mode.
- No touch, tablet, text input or input methods.
- Touchpad edge bindings need a touchpad that reports its size, and do not work
  when Leme runs inside another compositor.
- No color management, HDR or DRM leasing.
- Rounded corners and blur only render with the GLES2 and pixman renderers,
  not with `WLR_RENDERER=vulkan`.
- No terminal swallowing.
- Multi-monitor support still needs testing on more hardware.
- A disabled output stays off when the last other monitor is unplugged.
- Portal window sharing needs `xdg-desktop-portal-luminous`;
  `xdg-desktop-portal-wlr` shares whole outputs unless given a custom chooser.
- Titlebars, maximize and minimize only exist in the experimental
  [desktop layout](../configuration/desktop.md).

## Desktop layout

- The file grid does not scroll, does not accept drops from other apps, and
  only drags copies into apps. Every output shows the same arrangement.
- File operations, folder scans and icon loading run inside the compositor, so
  a slow filesystem pauses it. There is no undo, and no permanent delete when
  Trash is unavailable.
- Name prompts take up to 1024 bytes, with no clipboard, compose, input method,
  mouse selection or key repeat.
- Containers cannot nest or merge. Client-drawn titlebars stay inside a
  container, and pointer split resizing only works in dwindle containers.
- Containers keep square corners with `corner_radius`. Menu, prompt and strip
  sizes are fixed.
- `timao` exposes a container only as a text id on its members.
