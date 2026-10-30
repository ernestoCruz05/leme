# Layouts

Every tag has a layout. You can change the focused tag at runtime or set persistent defaults in the [`tags` configuration](../configuration/tags.md).

## Dwindle

Dwindle splits the tile containing the focused window along its longer side:
a wide tile splits side by side, a tall one top and bottom. The direction is
chosen when the window is added, so resizing never flips existing splits. A
tiled SUPER-drag previews a new position and reinserts the window when
released.

## Master-stack

Master-stack places the first `nmaster` windows in the master area. `mfact`
sets that area's share of the available width. Remaining windows occupy the
stack beside it. Use `move` and `resize` for adjustments; tiled SUPER-drag is
for dwindle tags.

## Desktop (experimental)

Desktop leaves windows freely placed. Use SUPER-drag to move them and
SUPER-right-drag to resize them. Existing windows keep their rectangles when
entering desktop layout; new windows use the floating size and centering policy.
Saved desktop positions are restored relative to the output and clamped to its
usable area.

Desktop focuses windows on hover without raising them. A click raises the window
and its related dialogs on the same tag and scene layer, keeping children above
their parents. This uses the parent relationships supplied by native Wayland and
XWayland clients. Fullscreen and sticky layer policies are unchanged.

The layout does not change a window's explicit floating preference.
`toggle_floating` changes that preference while the window remains freely placed.
Returning to a tiled layout retiles windows whose floating preference is off.
The public `floating` field continues to report the explicit preference.
Container members keep that preference too, but must be detached before it can
be changed.

Leme draws an icon grid behind windows and bottom-layer panels. It reads
the XDG desktop directory, falling back to `~/Desktop`. A desktop directory set
to the home directory disables the grid. Hidden entries are omitted. Each
output showing a desktop tag displays the same directory in its usable area;
entries that do not fit are not drawn.

The prototype uses 48-pixel icons in 96-pixel-wide cells with a 16-pixel inset,
all in logical pixels. Labels use up to two lines of `Sans 10`. Row height grows
as needed to fit the measured text and its shadow. SVG files can supply their own
artwork; `.desktop` files supply icon metadata only. Named SVG and PNG icons
come from Adwaita and its inherited themes, with hicolor as a fallback.

Icons stay in the cells they are dragged to. Positions are saved by file name as
a column and row in `$XDG_STATE_HOME/leme/desktop-positions`
(`~/.local/state/leme/desktop-positions` by default), and every output uses the
same arrangement. Entries without a saved position, or whose cell is taken or
outside the grid, fill the first free cells in file name order. Renaming through
the menu keeps an entry's cell. Trash and moving into a folder forget it, and an
external rename loses it. New folder puts the folder in the cell that was
right-clicked when that cell is free.

Left-click selects an entry and Ctrl+click adds or removes one. Clicking an
entry that is already selected keeps the selection until release, so it can be
dragged; a release without a drag then selects only that entry. Pressing on blank
space and dragging draws a selection box. Entries whose cells it touches are
selected while it moves. The box replaces the selection, or adds to it when Ctrl
was held at the press. A click on blank space without dragging clears the
selection.

Double-click opens an ordinary file or folder with its default application
through `xdg-open`, which requires xdg-utils at runtime, and Enter opens every
selected entry. Double-clicks must complete within 400 ms without moving more
than 4 logical pixels. Arrows select the nearest entry in that direction without
wrapping. Escape clears selection. Existing compositor bindings take priority.

Dragging a selected entry more than 4 logical pixels drags the whole selection.
Over a window, it is a normal Wayland drag that offers `text/uri-list` with the
copy action only, and the application decides what to do with the files. Over
the desktop, a folder that is not part of the drag is highlighted, and dropping
on it moves the entries into it. Elsewhere, the target cells are outlined:
the entry under the pointer goes to the cell under the pointer, and the others
keep their offsets from it. A cell that is outside the grid or taken by an entry
that is not moving falls back to the nearest free cell. Escape cancels the drag.

Clicking the grid or its blank space gives keyboard focus to the desktop.
Window-targeting commands then have no focused window. Hovering over a window
restores normal window focus and clears selection. Selection uses the active
border color and survives unrelated artwork updates. Renaming through the menu
keeps selection on the new name. External renames, removal and leaving the desktop
clear it. Covering windows and panels keep their input priority.

Launcher entries, regular files with execute permissions, and special files
cannot be opened from the grid. Symlink targets are checked too. Refusals and
opener startup failures are logged; errors after the opener starts belong to
that process. Leme does not execute launcher metadata or document contents;
the configured default applications still control what they do with a file.

Right-click blank desktop space for New folder or Open desktop folder. An entry's
menu has Open, Rename and Move to Trash. Right-clicking a selected entry acts on
the whole selection, and Rename is available only for a single entry.
Right-clicking an unselected entry selects only it first. These menus use the
same mouse and keyboard navigation as the titlebar menu. Open keeps the
restrictions above and shows an error if opening is refused or the opener cannot
start.

A `desktop_menu` block replaces one of these menus: `"blank"` for empty desktop
space, `"files"` for entries. Each `entry` has a label and a Leme command, and
`separator` draws a line between rows:

```scfg
desktop_menu "blank" {
    entry "New folder" grid_new_folder
    entry "Open terminal here" spawn foot --working-directory=%dir
    separator
    entry "Open desktop folder" grid_open
}
desktop_menu "files" {
    entry "Open" grid_open
    entry "Edit" spawn kate %files
    separator
    entry "Rename" grid_rename
    entry "Move to Trash" grid_trash
}
```

Without a block, a menu keeps the default rows described above. From a menu,
`grid_open`, `grid_rename`, `grid_trash` and `grid_new_folder` act on the
entries that were right-clicked, and a new folder goes in the clicked cell.
`grid_open` in the blank menu opens the desktop folder. Rename rows are greyed
out unless exactly one entry is selected. Other commands run as they would from
a binding.

In `spawn` arguments, `%dir` becomes the desktop folder, also inside a longer
argument such as `--working-directory=%dir`. `%files` becomes one argument per
selected entry, each a full path. It has to be an argument of its own and only
works in the `"files"` menu. Programs start directly, without a shell, in
Leme's working directory.

A menu holds up to 64 rows, and labels are 1 to 63 bytes. An empty block turns
that menu off, so right-clicking there does nothing. Rows the config check
rejects are reported and left out; the rest of the block still applies.
Reloading the config closes an open desktop menu.

`focus_desktop` gives the grid keyboard focus without a click, and
`desktop_menu` opens its menu from the keyboard. The `grid_*` commands open,
rename, trash and select entries, including ranges and select all. They apply
only while the grid has focus, so they can be bound to plain keys such as Delete
or F2. See the [command reference](../reference/commands.md).

New folder and Rename use a native name prompt. Rename selects the complete name,
including its extension. Type to replace it, use Left/Right, Home/End and
Backspace/Delete to edit, or Ctrl+A to select it all. Return submits; Tab and
Shift+Tab cycle the field and buttons. Escape or an outside click closes the
prompt. Text uses the active keyboard layout, including Shift, Caps Lock and
AltGr. Reserved compositor bindings take priority; other shortcut chords close
the prompt and continue through normal binding handling.

Names are limited to 1024 UTF-8 bytes in the prompt and to the filesystem's own
limit after encoding conversion. Empty names, `/`, control characters, `.` and
`..` are rejected. A name that cannot be represented in the field starts empty
with an explanation. There is no clipboard editing, compose support, IME or key
repeat. Dot-prefixed names are allowed but remain hidden from the grid.

Folder creation, rename, Trash and moving into a folder run in the compositor and
block it until they finish, as directory scanning does. Creation respects the process umask. Existing
destinations are never overwritten; unsupported atomic rename and missing desktop
directories are errors. Trash uses GIO's standard Trash handling and never falls
back to permanent deletion. Rename and Trash operate on symlinks themselves, not
their targets. They act on the entry's name, so a file replaced under that name
after the menu opened is the one renamed or trashed. Errors leave a name prompt
available for retry. Trash and moving into a folder stop at the first entry that
fails and show the error. A finished operation refreshes the grid even when
directory watching is unavailable.

There is no grid scrolling or built-in undo, and the desktop does not accept
drops from other applications.

Directory changes, saved launcher metadata, and edits to referenced artwork
update automatically after a 100 ms debounce. This includes artwork outside
the desktop directory and files replaced during a save. Artwork in a folder
that does not exist yet, or reached through a chain of symlinks, is picked up
by the next desktop change or by `reload_config`. Unchanged images stay
cached in memory. A successful `reload_config` also rereads the desktop directory
setting and clears the image cache.

Images are rasterized synchronously. Loading or refreshing artwork can pause
the compositor. There is no background rasterization, polling, or persistent
cache.

Tag changes involving desktop layout are instant. Swipes use the instant
fallback when their navigable tag ring contains a desktop tag.

### Containers

Hold Shift while dragging a desktop window onto another desktop window to make
a two-window dwindle container. Dropping onto a member of an existing container
adds the window to that container. Any visible desktop window on any output can
be a target, snapped or not. The preview follows the actual hit window; an
overlapping window or panel blocks targets behind it. Edge snapping is off while
the modifier is held, and a drag without it only moves or snaps the window.
Dialogs, hidden or fullscreen windows, sticky windows and scratchpads are not
grouping targets. A new container takes the target window's position, size and
placement.

The modifier is a desktop setting. It accepts `shift`, `ctrl`, `alt` or `super`
and defaults to `shift`:

```scfg
desktop {
    container_modifier ctrl
}
```

A configuration reload applies a new value; it is not a live setting. SUPER-left
drag also groups when the modifier is held with SUPER or added during the drag.

`container_join left|right|up|down` does the same from the keyboard. The focused
standalone window joins the nearest eligible window or container member in that
direction on its tag, on the side of the target that faces it. A member must be
detached before it can join another group.

A container has one compositor titlebar showing the focused member's title, and
an outer border in the border color. Members sit flush
against that border and against each other, with square corners and their own
borders; the focused member's border uses the active color. Nothing is drawn
behind the members, so translucent windows show what is behind the group.
Members have no individual compositor titlebars. Client-drawn titlebars remain
part of client content. Hover focus does not raise the group; clicking a member,
its dialog or the frame raises the complete group. Keyboard focus always belongs
to a client. Dialogs stay outside the membership and follow their member parent
for stacking and inherited minimization.

Move the group by its titlebar. Like a standalone window, a normal group can be
left partly outside the output. Its controls minimize or maximize the group, or
request closure of all current members. A refusing client can keep its window
open. `close_view` still closes only the focused client. Resize the outer frame
from its edges; native members resize with it while dragging, and XWayland
members catch up on release. Resizing a half or quarter clears placement at the
current size. A maximized group must be restored first. Members cannot resize
their own tiles through a standalone floating resize.

`container_layout dwindle|master_stack` selects the inner layout.
`resize` changes that layout's split or sizing parameter. In a dwindle group,
SUPER-right drag on a member moves the split at the nearest edge or corner, as on
tiled tags; master-stack groups have no pointer split resize.
`move left|right|up|down` swaps the focused member with its neighbor in that
direction and does nothing at the edge of the group. Dwindle splits the hit
member in the previewed direction. In master-stack, left/up inserts before the
hit member and right/down after it. The preview shows the resulting layout cell.

`container_detach` releases the focused member as a normal desktop window at its
current content location. SUPER-left drag does the same after moving more than
four logical pixels and leaving the source frame. Releasing inside does not
reorder members. Once detached, the drag is an ordinary window move: holding the
modifier lets it join another group, but not its source group during that drag.
Cancellation leaves the window standalone. `container_dissolve` releases all
members at their current cells. These commands have no default bindings.

A fullscreen member retains its membership and layout slot. Fullscreen hides the
frame and other members, and group placement/minimization is unavailable until it
ends. When only one member remains, it becomes a standalone window with the
container's placement, restore geometry and minimized state. Leaving desktop
layout dissolves containers and restores each member's floating preference.

Group drags can cross to enabled outputs showing desktop tags when
`cross_output_drag` permits it. The frame and its members move together, including
transient descendants. Moving one member with a tag/output command detaches it.
Output removal can preserve a group on a desktop fallback; a tiled fallback
dissolves it. Reconnecting an output does not recreate containers. Desktop tag
transitions remain instant.

With [`animation { move }`](../configuration/animation.md#desktop-animations),
placement and inner layout changes animate the frame and members together.
Creation, insertion, member removal, swaps and keyboard resizing use the same
settings. Closing members can fade without an individual
titlebar. Pointer moves and resizing stay tied to the cursor; output transfers
and dissolution are instant.

### Minimized windows

`minimize_view` hides the focused desktop window without closing it.
`restore_minimized` restores all minimized windows on the focused output's current
tag. The built-in defaults and example configuration bind these to SUPER+m and
SUPER+SHIFT+m. Existing user configurations are not changed.

Minimized windows retain their geometry and remain in shell window lists. Ordinary
focus navigation skips them. Switching tags preserves their state; leaving desktop
layout or moving to a tiling tag restores them. Fullscreen, sticky, scratchpad,
unmanaged and actively dragged windows cannot be minimized.

Minimizing a parent also hides its eligible dialogs on the same tag. The strip
shows their highest minimized ancestor. Restoring a child clears its minimizing
ancestors, but independently minimized siblings stay hidden. Restoring a parent
also leaves its independently minimized children hidden.

A black row at the bottom of the usable area shows application icons for minimized
windows. Click an icon to restore and focus that window. Arrow cells page through
overflow; missing artwork uses a generic application image or a drawn fallback.
The row does not resize ordinary windows. Placed windows, placed containers and
the file grid exclude its space.

A minimized container has one strip entry, using the remembered member's icon.
Member minimize/restore requests affect the whole group. Its dialogs inherit that
hiding, while independently minimized dialogs retain their own state. Activating
a hidden member or dialog restores the group and then focuses the requested
client. `restore_minimized` restores each group once.

With `animation { move }`, minimizing shrinks and fades a snapshot toward the
strip cell; restoring reverses that motion. Off-page entries use the appropriate
arrow cell. Without a strip cell, the snapshot shrinks to 60% of its size around
its center. A container's frame and members share one endpoint.

The strip stays below durable and top layers, and hides temporarily during fullscreen.
If enabled recovery cannot fit or cannot be built, new minimization is refused or
the affected active tag's windows are restored.

The built-in strip is enabled by default. To use only an external shell or commands
for recovery:

```scfg
desktop {
    minimized_strip false
}
```

The live setting is `desktop.minimized_strip`. Disabling it leaves windows
minimized. Client-side controls and external shells can request minimization;
their available buttons depend on the client. Leme does not detect whether an
external shell has a recovery widget.

### Titlebars and window placement

Desktop windows using server-side decorations have a titlebar with the title and
Minimize, Maximize/Restore and Close buttons, by default 32px tall with the
buttons on the right. Colors, fonts, sizes and which buttons sit on which side
are set with [desktop styling](../configuration/appearance.md#desktop-styling).
Drag the title area
to move the window. A click requires a matching release on the
same button without moving more than four logical pixels. Unavailable actions are
dimmed. Double-click the title area to toggle maximize, with presses no more than
400ms and four logical pixels apart.

Right-click the titlebar without modifiers to open the window menu. It includes
Minimize, Maximize/Restore, Fullscreen, Keep on all tags, Move to tag and Close.
Move to output appears when at least two outputs are enabled. A container's menu
has group placement and minimization, Layout, Detach, Dissolve group and Close all.
`window_menu` opens the focused window's menu from the keyboard.

Hover a submenu row for 150ms to open it, or use Right, Return or space. Up and
Down select enabled rows and wrap. Left or Escape closes a submenu; Escape in the
main menu closes the whole menu. Other keys close it and continue through normal
binding handling. A right press followed by a drag and release selects an item;
releasing where the menu opened leaves it open. Click outside to dismiss it.
Tall menus scroll with the wheel, and keyboard navigation keeps the selected row
visible.

Native client-side decorations are left to the client. X11 titlebar hints are
respected. Fullscreen hides the titlebar; sticky, scratchpad and tiled-layout
windows do not receive one. Small frames that cannot fit the controls retain
border-only decoration. Titlebars use the existing window fade animation and
appear in output capture, but not direct client-window capture.

Maximize fills the usable desktop area, excluding panels and the minimized strip.
A window can also occupy the left or right half, or one of four quarters of that
area. These placements share one saved normal geometry. Switching between them
does not overwrite it. Client size limits can disable individual placements.

Dragging a maximized or snapped titlebar more than four logical pixels restores
normal size and continues the move. The cursor keeps its proportional horizontal
position on the titlebar. SUPER-drag also restores placed windows. Keyboard move
and resize, and pointer resize, require normal placement.

During a titlebar or SUPER move, reaching an outer output edge shows a translucent
preview. The top edge maximizes, the left and right edges select halves, and the
corners select quarters. Release to apply the preview, or move away to dismiss it.
The edge band is two logical pixels wide; corners extend 48 logical pixels along
each edge. Shared edges are ignored at the cursor position, so dragging between
outputs does not snap there. The bottom edge has no action outside its corners.
A target that cannot fit the client's size limits has no preview. With
`animation { move }`, previews grow from the cursor and glide between targets.
Placement animates after release; moving and restoring under the pointer remain
instant.

Transfers between desktop tags or outputs retain placement; leaving desktop
restores normal geometry. Placement does not change floating preference or
activate a window. Minimized windows retain placement. Fullscreen takes precedence
and retains it too; placement requests are refused until fullscreen ends. Only
maximized placement is reported to clients as maximized. Halves and quarters are
reported to native clients through their tiled edges. The `snap` and `place`
commands set placements from the keyboard; see the
[command reference](../reference/commands.md).
## Persistent and runtime changes

A `set_layout` command changes the live tag. Runtime layout changes do not rewrite the scfg file. A configuration reload resets a tag only when its effective configured settings changed; unrelated reloads preserve runtime layout and resize adjustments.

Tag changes can remain instant or use an optional visual transition. See
[animation configuration](../configuration/animation.md).

The exact command forms are in the [command reference](../reference/commands.md).
