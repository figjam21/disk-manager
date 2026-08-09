#!/bin/sh
# Launches disk-manager-gui as root from a terminal. Runs through a terminal +
# sudo rather than pkexec because pkexec strips the display environment
# (DISPLAY/XAUTHORITY/WAYLAND_DISPLAY/XDG_RUNTIME_DIR) from the elevated
# process, and on at least some Wayland compositors even re-injecting those
# variables explicitly isn't enough to get a root GTK client actually mapped.
# Running inside the user's own terminal sidesteps that: it's a normal,
# already-connected client the whole time, sudo just changes its UID.
sudo --preserve-env=DISPLAY,XAUTHORITY,WAYLAND_DISPLAY,XDG_RUNTIME_DIR /usr/bin/disk-manager-gui
status=$?

# Only linger (and explain why) on a non-zero exit -- a normal, successful run
# should just close the terminal along with the app, not demand a keypress
# every time.
if [ "$status" -ne 0 ]; then
    echo "disk-manager-gui exited with status $status"
    echo "Press Enter to close this window..."
    read -r _dummy
fi
exit "$status"
