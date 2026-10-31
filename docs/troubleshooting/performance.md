# Performance

`LEME_RENDER_TIMING` makes Leme log how long frames take, every 1 to 60
seconds:

```sh
LEME_RENDER_TIMING=5 leme-session
```

Each output that draws writes a line like this to the
[session log](startup.md#start-with-the-log):

```text
leme: render timing eDP-1: 298 frames in 5.0 s; cpu avg 0.310 ms max 1.204 ms; gpu avg 0.552 ms p95 0.970 ms max 1.041 ms (298 timed)
```

| Field | Meaning |
| --- | --- |
| `frames` | frames drawn in the interval; an idle output writes no line |
| `cpu` | time Leme spent preparing each frame |
| `gpu` | time the GPU spent, with the 95th percentile and the slowest frame |
| `timed` | frames that got a GPU measurement |

A frame has to be ready within one refresh, about 6 ms at 165 Hz and 16.7 ms at
60 Hz, so look at `p95` and `max`. To compare settings, change one `style`
value, reload, and repeat the same thing, such as switching tags, for one
interval.

GPU times are estimates and read high when the GPU is idling at a low clock.
They show `gpu n/a` with `WLR_RENDERER=pixman`, and on drivers such as NVIDIA's
in builds without `-Deffects=true`.
