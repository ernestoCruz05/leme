# Getting started

Start with a nested session, then verify your own outputs, input devices, and
login stack before relying on Leme for a primary session.

1. [Install and build Leme](installation.md).
2. [Run a nested or headless session](nested-session.md).
3. [Copy and understand the minimal configuration](minimal-config.md).
4. [Start a normal session](first-session.md).
5. Continue with the [configuration reference](../configuration/README.md).

A normal login session should provide a writable `XDG_RUNTIME_DIR`. Leme must run as the logged-in user. Distribution-specific setup belongs in the [Gentoo/OpenRC guide](../guides/gentoo-openrc.md), not in the basic path.
