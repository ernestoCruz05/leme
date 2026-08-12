# Locking and idle

Leme provides the locking and idle protocols. An external locker and idle program decide when to lock, when to sleep outputs, and when to unlock.

## Minimal setup

Recent versions of `swayidle` and `swaylock`, together with `wlopm`, are one
compatible combination. Run this inside a Leme session to lock after five
minutes and power outputs off after ten:

```sh
swayidle -w \
    timeout 300 'swaylock -f' \
    timeout 600 'wlopm --off "*"' \
        resume 'wlopm --on "*"'
```

Activity powers the outputs back on, but the lock screen remains until you
authenticate. Test `swaylock -f`, `wlopm --off '*'`, and `wlopm --on '*'`
individually before starting the combined command automatically. To start it
with Leme, save the command in an executable script and call that script by its
absolute path from the [`exec` block](../configuration/startup-and-environment.md).

The locker must support `ext-session-lock-v1`, and the idle program must support
`ext-idle-notify-v1`. Package versions that predate those protocols will not
work with this setup.

## Lock behavior

Leme implements `ext-session-lock-v1`, `ext-idle-notify-v1`, and `idle-inhibit-unstable-v1`.

Locking fails closed. Leme covers normal content immediately and sends input only to the lock surface. If the locker exits, an opaque blocker remains and another locker can take over. Normal windows do not reappear until an explicit unlock.

An idle inhibitor counts only while its surface is mapped and visible on an enabled output. A hidden tag does not keep the session awake.

## Output power

Leme implements `wlr-output-power-management-unstable-v1`, but it does not choose an idle timeout or start a locker. A client such as `wlopm` can turn a connector off:

```sh
wlopm
wlopm --off DP-1
wlopm --on DP-1
wlopm --off '*'
wlopm --on '*'
```

Quote `'*'` so the shell does not expand it. Powering an output off is not the same as unplugging it. The connector, tags, views, layer surfaces, and stored geometry remain tracked but dormant. A configuration reload does not wake it.

When an output is powered on, Leme restores its latest mode, scale, transform, and position. It does not overwrite temporary state on other outputs. If every physical output is powered off, Leme does not create a headless emergency output.

## Recommended order

For an idle policy that locks before powering down:

1. start a locker and wait until the lock is active;
2. on a later idle timeout, power outputs off;
3. power them on when activity or resume occurs;
4. unlock normally.

The wake transition remains isolated from desktop content. Capture during a lock sees only committed lock content or the opaque blocker.

See [screen sharing](screen-sharing.md) for capture behavior and [troubleshooting outputs](../troubleshooting/outputs.md) if a power request does not return an output.
