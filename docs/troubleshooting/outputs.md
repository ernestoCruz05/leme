# Output troubleshooting

## Output configuration fails

Connector names are exact. List them and their advertised modes from inside
the session:

```sh
wlr-randr
```

You can also inspect the compositor log for each active connector.

Remember:

- a refresh value requires a matching `mode`;
- Leme accepts a requested refresh only when the nearest advertised value is within 1 Hz;
- scale must be from `0.5` through `4.0`;
- transform must be `normal`, `90`, `180`, or `270`.

Remove a hardware-specific `output` block to recover. Leme then uses each output's preferred mode at scale 1 and normal transform. A failed runtime reload keeps the last working output state.

Output-management clients can request temporary positions and modes. Reload restores scfg policy. Overlapping output geometry and requests that disable every output are rejected.

## Output does not power off or return

Confirm that Leme advertises the power-management global:

```sh
wayland-info | grep zwlr_output_power_manager_v1
```

Use a client such as `wlopm` to inspect and change the state:

```sh
wlopm
wlopm --off DP-1
wlopm
wlopm --on DP-1
wlopm
```

Quote the all-output wildcard:

```sh
wlopm --off '*'
wlopm --on '*'
```

Power-off is not unplug. The output remains tracked with dormant tags and views, and a reload does not wake it. Powering every physical output off does not create a headless fallback.

If ON fails, Leme leaves the target off rather than disturbing another output. Correct the configured mode or non-overlapping geometry, reload while the output remains off, and retry.

For capture recovery, verify that the output is back on before checking portals and PipeWire. A direct capture command such as `grim -o DP-1 capture.png` separates compositor capture from transport problems.
