#!/usr/bin/env bash
# install.sh - installs everything disk-manager needs to build and run on
# Debian/Ubuntu: build toolchain + dev headers, plus the runtime tools the core
# library shells out to (mdadm, smartmontools, parted, filesystem utilities,
# enclosure LED control, data-recovery tools, fwupd).
#
# Usage: sudo ./install.sh [--no-build]
#   --no-build   only install packages, don't configure/build the project
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Error: this script installs system packages and kernel modules, run it with sudo." >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "Error: this script targets Debian/Ubuntu (apt-get not found)." >&2
    exit 1
fi

echo "==> Updating package lists"
apt-get update -qq

echo "==> Installing build toolchain"
apt-get install -y \
    build-essential cmake pkg-config git \
    libncurses-dev nlohmann-json3-dev libgtkmm-3.0-dev

echo "==> Installing RAID + storage management tools"
apt-get install -y \
    mdadm smartmontools parted gdisk \
    e2fsprogs xfsprogs btrfs-progs dosfstools

echo "==> Installing drive-locate (enclosure LED) support"
# ledmon/ledctl controls per-slot fault/locate LEDs on hardware that exposes SES;
# sg3-utils gives the lower-level sg_ses fallback. Neither is required -- on plain
# SATA/USB drives disk-manager falls back to a read-only activity-blink pattern.
apt-get install -y sg3-utils ledmon || true

echo "==> Installing data-recovery tools"
apt-get install -y gddrescue testdisk

echo "==> Installing firmware update support"
apt-get install -y fwupd

echo "==> Loading RAID kernel modules"
modprobe linear   || true
modprobe raid0    || true
modprobe raid1    || true
modprobe raid456  || true
modprobe raid10   || true

MODULES_FILE=/etc/modules-load.d/disk-manager-raid.conf
echo "==> Persisting RAID modules across reboots ($MODULES_FILE)"
cat > "$MODULES_FILE" <<'EOF'
linear
raid0
raid1
raid456
raid10
EOF

echo "==> Enabling background services (smartd, mdadm monitor, fwupd)"
systemctl enable --now smartmontools.service 2>/dev/null || systemctl enable --now smartd.service 2>/dev/null || true
systemctl enable --now mdmonitor.service 2>/dev/null || true
systemctl enable --now fwupd.service 2>/dev/null || true

echo "==> Assembling any RAID arrays described in mdadm.conf (no-op if none)"
mdadm --assemble --scan || true

if [[ "${1:-}" != "--no-build" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    echo "==> Configuring and building disk-manager"
    cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$SCRIPT_DIR/build" -j"$(nproc)"
    echo
    echo "Build complete:"
    echo "  ${SCRIPT_DIR}/build/disk-manager-tui"
    [[ -x "${SCRIPT_DIR}/build/disk-manager-gui" ]] && echo "  ${SCRIPT_DIR}/build/disk-manager-gui"
fi

echo
echo "==> Done. RAID array management, partitioning, and mounting require root:"
echo "      sudo ./build/disk-manager-tui"
