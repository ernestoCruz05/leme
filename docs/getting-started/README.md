# Getting started

Leme is early alpha software. It can run a usable Wayland session, but
configuration and behavior may change before a stable release. Physical
multi-monitor behavior has received limited hardware coverage. Start with a
nested session, then verify your own outputs, input devices, and login stack
before relying on Leme for a primary session.

Follow this order:

1. [Install and build Leme](installation.md).
2. [Run a nested or headless session](nested-session.md).
3. [Copy and understand the minimal configuration](minimal-config.md).
4. [Start a normal session](first-session.md).
5. Continue with the [configuration reference](../configuration/README.md).

A normal login session should provide a writable `XDG_RUNTIME_DIR`. Leme must run as the logged-in user. Distribution-specific setup belongs in the [Gentoo/OpenRC guide](../guides/gentoo-openrc.md), not in the basic path.
