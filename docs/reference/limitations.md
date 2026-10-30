# Limitations

These limitations describe the current build. Planned and deferred work is tracked in the [roadmap](../../ROADMAP.md).

- Leme has no persistent VRR or adaptive-sync policy.
- Touch, tablet, gesture, text-input, and input-method support is not implemented.
- Color management, HDR, gamma control, and DRM leasing are not implemented.
- Portal window sharing needs `xdg-desktop-portal-luminous`. `xdg-desktop-portal-wlr` shares whole outputs unless it is given a custom chooser.
- Maximization, minimization and titlebar controls are limited to the experimental [desktop layout](../concepts/layouts.md#desktop-experimental).
- The file grid has no scrolling and does not accept drops from other applications. Drags from the grid into applications offer copies only. Every output uses the same icon arrangement.
- Desktop name prompts accept up to 1024 UTF-8 bytes, subject to the filesystem's filename limit. They have no clipboard editing, compose support, IME, mouse text selection or key repeat.
- Desktop file operations, directory scanning, opening checks and artwork loading run synchronously in the compositor, so a slow filesystem pauses it. Rename and moving into a folder require atomic no-overwrite support; unavailable Trash is an error, with no permanent-delete fallback. There is no built-in undo. Operations act on the entry's name, so a file replaced under that name in the meantime is the one affected.
- Desktop containers cannot nest or merge with another whole container. Layout changes dissolve them, as does returning evacuated members to a reconnected home output. Dialogs, sticky windows and scratchpads cannot be members. Dragging inside a group does not reorder members; `move` swaps them instead. Pointer split resizing works only in dwindle groups.
- Containers suppress compositor titlebars on members, but cannot remove client-drawn titlebars. They use the existing tiled sizing policy; there is no container-specific client-size constraint solver or independent member animation.
- Desktop containers keep square corners even when `corner_radius` is set. Menu, prompt and minimized strip sizes are fixed; [desktop styling](../configuration/appearance.md#desktop-styling) only changes their colors and fonts.
- Public view records expose only a nullable text container ID. There is no container entity, root, reference traversal or `by-id` lookup.
- Terminal swallowing is not implemented.
- Physical multi-monitor support is new and still needs broader hardware testing.

