# Troubleshooting

Start with the [session log](startup.md#start-with-the-log), then choose the symptom that matches what you see.

- [Startup](startup.md) covers logs, `XDG_RUNTIME_DIR`, D-Bus, and login-session checks.
- [Outputs](outputs.md) covers connector names, modes, power requests, and recovery from bad geometry.
- [Input](input.md) covers seat access, VT switching, pointer names, and nested-session limits.
- [Portals and capture](portals-and-capture.md) separates compositor capture from portal and PipeWire failures.
- [XWayland](xwayland.md) covers lazy startup, missing binaries, and native fallback.
- [Crashes](crashes.md) lists the information needed for a useful report.

Do not run Leme as root or change device permissions globally as a workaround. Those steps hide the login-session problem and can make the system less safe.
