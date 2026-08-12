# Input troubleshooting

## Input or VT access fails

Check that the login session is active on the expected seat and TTY:

```sh
loginctl show-session "$XDG_SESSION_ID" \
  -p Type -p Class -p Active -p State -p Seat -p TTY
```

Leme should run as the logged-in user in an active session. Device access comes from wlroots/libseat and the login stack. Do not change `/dev/dri` or `/dev/input` permissions globally.

`switch_vt` needs a real seat session and works only for VT numbers 1 through 12. It is unavailable when Leme runs nested or headlessly. On seatd systems, check that the seatd service and user access match the distribution's setup.

## A pointer override does not apply

Named `pointer` blocks match the complete libinput device name. Read the names
Leme configured from the session log:

```sh
state_home="${XDG_STATE_HOME:-$HOME/.local/state}"
grep 'leme: configured pointer ' "$state_home/leme/session.log"
```

Copy the complete name; substring matching is not used.

```scfg
pointer "Exact device name" {
    accel_profile flat
}
```

A named block overrides only the properties it contains. Unsupported properties are skipped for that device.
