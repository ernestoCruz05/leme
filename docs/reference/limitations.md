# Limitations

These limitations describe the current build. Planned and deferred work is tracked in the [roadmap](../../ROADMAP.md).

- Leme has no persistent VRR or adaptive-sync policy.
- Touch, tablet, gesture, text-input, and input-method support is not implemented.
- Color management, HDR, gamma control, and DRM leasing are not implemented.
- Views do not return to their original monitor when a disconnected output is plugged back in.
- Portal window sharing needs `xdg-desktop-portal-luminous`. `xdg-desktop-portal-wlr` shares whole outputs unless it is given a custom chooser.
- Leme has no maximized or minimized state.
- Terminal swallowing is not implemented.
- Physical multi-monitor support is new and still needs broader hardware testing.

