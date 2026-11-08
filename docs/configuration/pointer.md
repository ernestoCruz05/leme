# Pointer

A `pointer` block sets libinput options for every pointer device. A block with
a device name overrides them for that device only.

```scfg
pointer {
    accel_profile adaptive
    accel_speed 0.0
    natural_scroll false
    left_handed false
    tap true
    focus_follows_mouse true
}

pointer "Razer Razer DeathAdder V2" {
    accel_profile flat
}
```

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `accel_profile` | `adaptive`, `flat` | `adaptive` | pointer acceleration |
| `accel_speed` | `-1.0` to `1.0` | `0.0` | pointer speed |
| `natural_scroll` | `true`, `false` | `false` | reverses scrolling |
| `left_handed` | `true`, `false` | `false` | swaps the buttons |
| `tap` | `true`, `false` | `true` | tap to click on touchpads |
| `focus_follows_mouse` | `true`, `false` | `true` | focus the window under the pointer instead of the clicked one |

`focus_follows_mouse` only goes in the unnamed block. Device names must match
exactly. The session log lists them on `leme: configured pointer` lines. A
device ignores the keys it does not support.

## Touchpad and mouse options

A key you leave out keeps the device's own default.

```scfg
pointer {
    disable_while_typing true
    click_method clickfinger
    scroll_method two_finger
    tap_button_map lrm
    scroll_factor 1.5
}

pointer "Razer Razer DeathAdder V2" {
    scroll_method on_button_down
    scroll_button middle
}
```

| Key | Values | Effect |
| --- | --- | --- |
| `disable_while_typing` | `true`, `false` | ignores the touchpad while you type |
| `click_method` | `none`, `button_areas`, `clickfinger` | `button_areas` picks the button by where you press, `clickfinger` by how many fingers are down |
| `scroll_method` | `none`, `two_finger`, `edge`, `on_button_down` | how scrolling starts |
| `scroll_button` | `left`, `right`, `middle`, `side`, `extra`, `forward`, `back`, `task` | the button `on_button_down` holds to scroll |
| `middle_emulation` | `true`, `false` | pressing left and right together clicks the middle button |
| `tap_button_map` | `lrm`, `lmr` | which buttons one, two and three finger taps click |
| `tap_drag` | `true`, `false` | tap, then touch again and move to drag |
| `drag_lock` | `none`, `timeout`, `sticky` | keeps a tap drag going after you lift, until a timeout or the next tap |
| `drag_fingers` | `0`, `3`, `4` | moving that many fingers drags as if the button were held; `0` turns it off |
| `scroll_factor` | above `0`, up to `100` | multiplies scroll distance; `1.0` when left out |

`drag_fingers` stays off on a finger count that a [swipe](gestures.md) or a
[swipe binding](keybindings.md#gestures) uses, and the session log says so.
`scroll_factor` also applies to virtual pointers.
