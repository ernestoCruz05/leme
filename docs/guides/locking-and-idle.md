# Locking and idle

Leme does not lock or sleep on its own. A locker with `ext-session-lock-v1`
and an idle daemon with `ext-idle-notify-v1` decide when. With `swayidle`,
`swaylock` and `wlopm`, this locks after five minutes and turns the outputs off
after ten:

```sh
swayidle -w \
    timeout 300 'swaylock -f' \
    timeout 600 'wlopm --off "*"' \
        resume 'wlopm --on "*"'
```

Test each command alone first. To start it with Leme, put it in a script and
run that from [`exec`](../configuration/startup-and-environment.md).

If the locker crashes, the screen stays covered until a new locker unlocks it.
Idle inhibitors, such as a playing video, only count while visible.

## Output power

```sh
wlopm --off DP-1
wlopm --on '*'
```

Quote `'*'` so the shell leaves it alone. An output that is off keeps its tags
and windows, comes back with its last mode and position, and is not woken by a
reload. `output_power NAME on|off|toggle` does the same from a binding.
