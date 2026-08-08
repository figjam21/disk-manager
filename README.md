# disk-manager

A Linux disk and RAID management tool with both an ncurses TUI and a gtkmm desktop
GUI, built on top of the standard Linux storage stack (`mdadm`, `smartctl`, `parted`,
`fwupdmgr`, `ddrescue`, `testdisk`/`photorec`) rather than reimplementing any of it.

## Features

- **Disks & SMART** — enumerate disks/partitions, read SMART health (ATA and NVMe),
  flag at-risk drives before they fail outright.
- **Partitions & mount points** — create/delete/resize partitions, format, mount,
  grow filesystems, manage persistent `/etc/fstab` entries.
- **RAID arrays** (mdadm/software RAID) — view status and rebuild progress,
  create arrays (RAID 0/1/5/6/10), assemble/stop, add/remove/fail members, grow,
  resize the array and its filesystem.
- **Guided drive replacement** — pick a failing member (SMART-flagged in the UI),
  optionally light its locate LED or blink its activity light to confirm you've
  got the right physical drive, then fail/remove/add-replacement in one flow with
  live rebuild progress.
- **Drive locate** — lights a per-slot enclosure fault/locate LED via `ledctl`
  where the hardware supports it (SES); on plain SATA/USB drives with no
  addressable LED, falls back to a read-only access pattern that makes the
  drive's stock activity light flicker. Never writes to the device.
- **Firmware** — wraps `fwupdmgr` (the Linux Vendor Firmware Service) to show
  current firmware versions and apply vendor-signed updates. Deliberately does
  not implement any custom flashing logic — that's the only approach safe enough
  to automate.
- **Data recovery** — image a failing disk, partition, or RAID array with
  `ddrescue` (resumable via a persistent mapfile, live progress), or launch
  `testdisk`/`photorec` for partition-table and file-level recovery.

## Install (.deb package, recommended)

```bash
sudo ./install.sh          # build toolchain + runtime deps
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
cd build && cpack -G DEB
sudo dpkg -i disk-manager_*_amd64.deb
```

This installs both binaries to `/usr/bin`, registers a **Disk Manager** entry
(with icon) in your desktop's application menu that launches the GUI elevated
via `pkexec`, and a **Disk Manager (Terminal)** entry that opens the TUI in a
terminal via `sudo`. Debian/Ubuntu only (the runtime tool dependencies are apt
package names).

## Build without packaging

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
```

`disk-manager-gui` is only built if `libgtkmm-3.0-dev` is present; the TUI has no
such dependency and always builds.

## Run

```bash
sudo disk-manager-tui     # if installed via the .deb
# or, from a build tree:
sudo ./build/disk-manager-tui
# or the GUI:
sudo disk-manager-gui
```

Root is required for anything that touches disks (partitioning, formatting,
mounting, RAID management, firmware updates). Without root, both UIs still work
read-only for browsing disks and SMART data.

## Safety notes

- Every destructive action (formatting, deleting/resizing partitions, creating a
  RAID array, stopping/failing array members) requires an explicit confirmation;
  array creation additionally requires typing a confirmation phrase, since it's
  the single most destructive action in the app.
- `/etc/fstab` is backed up to `/etc/fstab.bak.<timestamp>` before any edit.
- The system disk (backing `/`) is excluded from the RAID-creation disk picker.
- ddrescue imaging never writes to its source device.

## Layout

```
include/dm/      public API of the core library (dm_core)
src/core/        implementation: device/smart/partition/filesystem/mdadm/locate/firmware/recovery
src/tui/         ncurses frontend
src/gui/         gtkmm frontend
data/            .desktop entries and the app icon (hicolor theme, all sizes + scalable SVG)
packaging/       postinst/postrm maintainer scripts (desktop/icon cache refresh)
install.sh       Debian/Ubuntu dependency installer + build
```
