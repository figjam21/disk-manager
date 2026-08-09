#!/bin/sh
# Diagnostic launcher: the plain `Terminal=true` + `sudo ...` Exec line was
# reportedly opening a completely blank terminal window with no visible
# prompt or error at all, which means something is going wrong before we
# even get useful output to debug from. This wrapper prints every step
# explicitly and refuses to auto-close, so whatever is actually happening
# becomes visible instead of disappearing with the window.
echo "=== disk-manager-gui launcher starting ==="
echo "whoami: $(whoami)"
echo "DISPLAY=$DISPLAY"
echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
echo "XAUTHORITY=$XAUTHORITY"
echo "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"
echo "sudo path: $(command -v sudo || echo NOT FOUND)"
echo "--- running: sudo --preserve-env=DISPLAY,XAUTHORITY,WAYLAND_DISPLAY,XDG_RUNTIME_DIR /usr/bin/disk-manager-gui ---"
sudo --preserve-env=DISPLAY,XAUTHORITY,WAYLAND_DISPLAY,XDG_RUNTIME_DIR /usr/bin/disk-manager-gui
status=$?
echo "--- exited with status $status ---"
echo
echo "Press Enter to close this window..."
read _dummy
