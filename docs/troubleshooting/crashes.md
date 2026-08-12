# Crash reports

A useful report includes:

- the Git commit or package version;
- whether the build used sanitizers;
- the smallest configuration that still reproduces the crash;
- exact reproduction steps, including native Wayland or XWayland clients;
- the complete Leme stderr or session log from startup through the crash;
- relevant kernel lines;
- a backtrace or core dump when available;
- output, GPU, driver, input device, login manager, and session-service details.

Collect recent kernel lines with whichever command your system permits:

```sh
dmesg --ctime | grep -Ei 'leme|segfault' | tail -50
journalctl -k -b | grep -Ei 'leme|segfault' | tail -50
```

Redact tokens, usernames, and unrelated environment values before sharing logs. Keep timestamps and the order of errors intact.
