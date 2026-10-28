# Gestures

Touchpad gestures allow direct manipulation and flick navigation across workspace tags.

```scfg
gestures {
    workspace_switch {
        mode single
        fingers 3
        distance 300
        threshold 0.5
        deceleration 0.997
        velocity_window 150
        occupied false
    }
}
```

## Workspace Switch

The `workspace_switch` sub-block configures multi-finger horizontal swipe gestures for tag switching:

| Property | Values | Default | Description |
| --- | --- | --- | --- |
| `mode` | `single`, `scrub`, `free` | `single` | Controls dragging freedom and release destinations; see below. |
| `fingers` | integer, `0` or $\ge 3$ | `3` | Number of fingers required. `0` disables workspace swipe gestures. Two-finger swipes are not delivered by libinput and are rejected. |
| `distance` | decimal $> 0.0$ | `300.0` | Gesture-motion units equivalent to traversing one full tag. |
| `threshold` | decimal between `0.0` and `1.0` | `0.5` | Position threshold for choosing between neighboring tags at release. |
| `deceleration` | decimal between `0.0` and `1.0` | `0.997` | Deceleration factor used to project resting position from release velocity; destinations are restricted by `mode`. |
| `velocity_window` | integer $> 0$ | `150` | Moving average window (in milliseconds) for computing swipe velocity at gesture end. |
| `occupied` | `true`, `false` | `false` | Swipe only through tags that have a window, plus the current tag. |

## Navigation Modes

- **`single`** (default): one gesture can select the starting tag or either neighbor. Dragging farther meets a small rubber-band limit; a fast flick cannot skip another tag.
- **`scrub`**: drag through multiple tags, but release selects only one of the two tags currently in view. Lifting exactly on a tag keeps that tag. Release velocity helps choose between the visible pair and starts the spring, but cannot fling to another tag beyond that pair.
- **`free`**: drag through multiple tags and let release momentum coast farther. Motion continues through the existing tag ring, including wrap.

By default the swipe moves through the navigable tags: pinned tags, occupied tags, and the empty candidate. With `occupied true` it skips every tag without a window, like `focus_next_tag occupied`, but keeps the tag you start on so you can swipe away from an empty one. With windows on tags 1 and 3, swiping from tag 2 reaches 1 or 3, and from there only 1 and 3 remain. When no other tag has a window, the swipe does nothing. While some navigable tag is empty, a swipe that starts during a keyboard tag switch finishes that animation first instead of taking it over.

All modes share the same direct tracking and spring settling. Continuous navigation reuses a bounded snapshot cache rather than capturing every workspace. Explicit gesture cancellation returns to the initially focused tag; in continuous modes it uses that tag's nearest equivalent ring position rather than retracing every completed lap.

## Tracking and Release

Direction-recognition movement is not replayed when the workspace starts moving. While fingers remain down, movement follows displacement directly—there is no spring chasing your fingers. Pausing before lifting reduces release momentum.

Animated release starts a spring at the last displayed position with the measured release velocity. Its defaults are damping ratio `1.0`, stiffness `1000`, and epsilon `0.0001`; an explicit `animation.workspace.spring` overrides them. Workspace easing still controls keyboard transitions, not gesture release. Disabled/unavailable animations and initial snapshot-allocation failures remain nonanimated.

Compatible animation takeovers preserve the current position. An in-progress long keyboard jump can contain nonadjacent snapshot imagery; Leme finishes that transition and restarts recognition rather than reinterpreting it as a different workspace pair. The same fallback applies when changing bounds during spring overscroll would reveal different imagery.

On supported libinput devices, Leme temporarily selects flat acceleration at neutral speed and restores the saved settings when the gesture ends or is cancelled. Flat gain is device-specific, not raw input; unsupported devices retain their usual deltas.

## Scroll Direction

The swipe direction follows the touchpad device's `natural_scroll` setting configured under `pointer`. When natural scrolling is active, swiping left moves forward to higher tag numbers, matching natural touch manipulation.
