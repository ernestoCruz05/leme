# Timao scripts

## Development session

`dev-session.timao` sets up applications on a selected monitor and tag. Run it
from the repository root with a monitor name:

```sh
timao run scripts/dev-session.timao eDP-1 | jq .
```

Without `--apply`, it reports which windows it would reuse and which applications
it would launch. It does not change windows, layout, or focus.

### Configuration

Edit the values and application lists at the start of the script:

| Setting | Default |
| --- | --- |
| Tag | 4 |
| Layout | dwindle |
| Window wait after launch | 15 seconds per application |
| Editor | foot running nvim, app ID `leme-dev-editor` |
| Shell | foot, app ID `leme-dev-shell` |
| Browser | Firefox |

Commands are argument arrays, not shell strings. Change the command and expected
app ID together. The first application is the primary window and must have the
unique role `editor`. The tag must be within the output's configured tag range.

### Apply

```sh
timao run scripts/dev-session.timao eDP-1 --apply | jq .
```

This sets the tag's layout, reuses or launches each application, disables
fullscreen and floating on its selected window, and moves it to the destination
tag. It then selects the output and tag and focuses the primary window.

An existing Firefox window can be moved. When several eligible windows have the
same app ID, the most recently focused is selected. Only tag-owned windows whose
`parent` is null are eligible, excluding windows with a published parent,
scratchpads and sticky windows. The script moves one selected window per
application. The layout change can rearrange other windows on the destination
tag.

The `--demo` profile uses three dedicated terminals instead of the development
applications:

```sh
timao run scripts/dev-session.timao eDP-1 --demo | jq .
timao run scripts/dev-session.timao eDP-1 --demo --apply | jq .
```

The apply command still changes the destination tag's layout and focus. Running
it again reuses eligible windows that remain open.

### Watch

```sh
timao run scripts/dev-session.timao eDP-1 --demo --apply --watch | jq .
```

After setup, `workspace-watch` records report the tag's windows, titles, focus,
and visibility. The watch does not move windows back or relaunch applications.
Ctrl+C stops observation without closing applications or undoing setup.

Locking suspends sensitive observation. Unlocking supplies a fresh reset. If the
target is removed or the compositor restarts, its old identity is not reused for
a replacement. The watch reports `target_present: false` when the captured tag
no longer exists. Watch recovery does not rerun setup.

### Failures

The top-level `try`/`catch` reports caught setup errors as `session-failed` and
stops setup. Earlier changes are not rolled back. Actions and launches are not
retried automatically, including when their outcome is unknown. The script logs
action results and caught error payloads; uncaught errors use timao's stderr
diagnostics.

A successful exec does not guarantee a window. The script subscribes before
launching, rechecks for a window, and waits after launch for up to the configured
timeout. This timeout does not cover subscription setup or the exec handshake.
App IDs are matching criteria, not proof of process ownership. Concurrent invocations can race and
launch duplicates; run one instance at a time.

Destination IDs are captured once, then resolved against each action's current
snapshot. A stale target fails rather than selecting a replacement by name.
`session-ready` reports completion of setup calls, not continued application
health. Applications remain open when the script exits.

The language has no rethrow or exit primitive. A caught setup error is reported
in JSON but exits with status 0. Check for `session-ready` or `session-failed`
rather than relying only on exit status. Usage and disabled-output messages also
exit 0. Uncaught runtime, cancellation and handler failures use timao's normal
nonzero statuses. In the examples piped to `jq`, the shell normally reports
`jq`'s exit status, not timao's.

Generic `by-id` results have type `any` and cannot directly serve as trusted
action targets. The script uses `view ID` and filtered `tags` or `outputs`
collections to retain snapshot entity types.

## Output windows

`output-windows.timao` lists windows on a monitor, including hidden tags:

```sh
timao run scripts/output-windows.timao eDP-1 | jq .
```

It follows `.output.name` and `.tag.number` without requiring IDs as arguments.
