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
