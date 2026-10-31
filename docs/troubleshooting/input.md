# Input troubleshooting

## No input or VT switching

```sh
loginctl show-session "$XDG_SESSION_ID" \
  -p Type -p Class -p Active -p State -p Seat -p TTY
```

The session must be active on a seat, and Leme must run as you. Device access
comes from the login stack through libseat, so fix that rather than changing
`/dev/dri` or `/dev/input` permissions. `switch_vt` only works on VTs 1 to 12
and not in a nested or headless session.

## A pointer block does not apply

Named `pointer` blocks need the full device name. The log lists them:

```sh
grep 'leme: configured pointer ' "${XDG_STATE_HOME:-$HOME/.local/state}/leme/session.log"
```

Copy the name as it is. See [pointer](../configuration/pointer.md).
