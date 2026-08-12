# Pointer

An unnamed `pointer` block sets global libinput defaults:

```scfg
pointer {
    accel_profile adaptive
    accel_speed 0.0
    natural_scroll false
    left_handed false
    tap true
}
```

| Property | Values | Default |
| --- | --- | --- |
| `accel_profile` | `adaptive` or `flat` | `adaptive` |
| `accel_speed` | decimal from `-1.0` through `1.0` | `0.0` |
| `natural_scroll` | `true` or `false` | `false` |
| `left_handed` | `true` or `false` | `false` |
| `tap` | `true` or `false` | `true` |

A named block matches the exact libinput device name and overrides only the fields it contains:

```scfg
pointer "Razer Razer DeathAdder V2" {
    accel_profile flat
    accel_speed 0.0
}
```

Unsupported properties are skipped for that device. A valid reload applies new pointer settings without moving the cursor or cancelling an active grab. Copy the device name exactly; named blocks do not use substring matching.

Leme does not apply a device-specific override unless the configuration contains
a named `pointer` block.
