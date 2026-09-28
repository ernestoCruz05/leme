# Performance

Blur, rounded corners, translucency and animations all cost rendering time.
To see how much on your hardware, start Leme with `LEME_RENDER_TIMING` set to a
report interval in seconds, from 1 through 60:

```sh
LEME_RENDER_TIMING=5 leme-session
```

For each output that draws frames, Leme then writes a line like this to the
[session log](startup.md#start-with-the-log) at every interval:

```text
leme: render timing eDP-1: 298 frames in 5.0 s; cpu avg 0.310 ms max 1.204 ms; gpu avg 0.552 ms p95 0.970 ms max 1.041 ms (298 timed)
```

- `frames` counts the frames Leme drew in that interval. An idle output draws
  nothing and writes no line.
- `cpu` is the time Leme spent preparing and submitting each frame.
- `gpu` is the time the graphics card spent on each frame, with the 95th
  percentile and the slowest frame. `timed` is how many frames produced a
  measurement. A frame shown without compositing, or one whose measurement the
  driver did not deliver, has none.

A frame has to be ready within the refresh interval to be shown on time: about
6 ms at 165 Hz and 16.7 ms at 60 Hz. Compare `p95` and `max` against that, not
only the average.

To compare settings, change one `style` value, reload, and repeat the same
activity, such as switching tags a few times, for one interval.

The GPU times come from the driver's timer queries and are estimates. They
depend on the clock the card is running at, so a lightly loaded card reports
higher times than the same work would take at full speed. With the software
renderer (`WLR_RENDERER=pixman`) the report shows `gpu n/a`. Builds without
`-Deffects=true` use the stock wlroots timer, which discards every measurement
on drivers that report timer interruptions on each frame, such as NVIDIA's; the
report then shows `gpu n/a` as well.

An invalid value is reported in the log and leaves the reports off.
