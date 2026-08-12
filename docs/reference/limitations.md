# Limitations

These limitations describe the current build. Planned and deferred work is tracked in the [roadmap](../../ROADMAP.md).

- Leme has no persistent VRR or adaptive-sync policy.
- Touch, tablet, gesture, text-input, input-method, and virtual-keyboard support is not implemented.
- Color management, HDR, gamma control, and DRM leasing are not implemented.
- Views do not return to their original monitor when a disconnected output is plugged back in.
- Portal screen sharing offers whole-output sources. Direct clients can request per-window capture, but portals do not use that source.
- Leme has no maximized or minimized state.
- Sticky floating windows and terminal swallowing are not implemented.
- Physical multi-monitor support is new and still needs broader hardware testing.

An item marked planned or deferred on the roadmap is not part of the current build.
