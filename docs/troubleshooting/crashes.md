# Crash reports

Report crashes at
[github.com/ernestoCruz05/leme/issues](https://github.com/ernestoCruz05/leme/issues)
with:

- `leme --version`;
- the smallest config that still crashes;
- the steps, and which apps were involved, Wayland or X11;
- the full session log, from a `--debug` session if you can reproduce it;
- a backtrace or core dump if you have one;
- kernel lines from around the crash;
- your GPU, driver, outputs, input devices and login manager.

```sh
dmesg --ctime | grep -Ei 'leme|segfault' | tail -50
journalctl -k -b | grep -Ei 'leme|segfault' | tail -50
```

Remove tokens, usernames and unrelated environment values from logs, but keep
the timestamps and the order of errors.
