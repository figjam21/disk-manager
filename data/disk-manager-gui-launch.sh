#!/bin/sh
# pkexec strips almost all environment variables for security -- including
# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY/XDG_RUNTIME_DIR -- so a GUI launched as
# `Exec=pkexec disk-manager-gui` directly authenticates fine but then can't
# connect to the display and dies silently. This script runs first as the
# invoking user (so these variables still hold their real session values),
# then re-injects them explicitly into the elevated process via `env`, which
# is the standard workaround admin GUI tools (e.g. GParted) use for this.
exec pkexec env \
    DISPLAY="$DISPLAY" \
    XAUTHORITY="$XAUTHORITY" \
    WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    /usr/bin/disk-manager-gui
