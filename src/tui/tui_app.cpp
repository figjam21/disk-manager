#include "tui_app.hpp"
#include "ui_helpers.hpp"
#include <ncurses.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dm {

namespace {

std::string human(uint64_t n) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10 ? 2 : 1) << v << units[u];
    return os.str();
}

std::string healthGlyph(const SmartReport& r, bool atRisk) {
    if (!r.supported) return "?";
    if (atRisk) return "!";
    return r.healthy ? "OK" : "FAIL";
}

std::string join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

// Mountpoint(s) and RAID membership for a disk, checking the disk itself and
// every partition on it. RAID membership covers both currently-assembled arrays
// (from listArrays()) and arrays found on-disk but not assembled (from
// scanForArrays()) -- a disk can be a "member" either way.
struct DiskExtra { std::string mounted; std::string raid; };

// `allDevices` must be a single already-fetched DeviceManager::listAll() result,
// reused across every disk in the caller's loop -- DeviceManager::find() re-runs
// `lsblk` from scratch on every call, so calling it once per partition per disk
// here turns a single screen refresh into dozens of redundant lsblk invocations
// (this was the actual cause of a "very slow to launch" GUI bug: not the
// display/pkexec/sudo plumbing, just this).
DiskExtra computeDiskExtra(const BlockDevice& d, const std::vector<BlockDevice>& allDevices,
                            const std::vector<RaidArray>& activeArrays,
                            const std::vector<DiscoveredArray>& discoveredArrays) {
    std::vector<std::string> names = {d.path};
    for (auto& c : d.childPaths) names.push_back(c);
    auto contains = [&](const std::string& p) { return std::find(names.begin(), names.end(), p) != names.end(); };

    std::vector<std::string> mounted;
    for (auto& n : names) {
        auto it = std::find_if(allDevices.begin(), allDevices.end(),
                                [&](const BlockDevice& bd) { return bd.path == n; });
        if (it != allDevices.end() && !it->mountpoint.empty()) mounted.push_back(it->mountpoint);
    }

    std::vector<std::string> raidHits;
    for (auto& arr : activeArrays) {
        for (auto& m : arr.members) {
            if (contains(m.device)) { raidHits.push_back(arr.device); break; }
        }
    }
    for (auto& disc : discoveredArrays) {
        if (disc.active) continue; // already covered via activeArrays above
        bool hit = std::any_of(disc.devices.begin(), disc.devices.end(), contains);
        if (hit) raidHits.push_back(disc.preferredDevice + " (not assembled)");
    }

    DiskExtra extra;
    extra.mounted = mounted.empty() ? "-" : join(mounted, ", ");
    extra.raid = raidHits.empty() ? "-" : join(raidHits, ", ");
    return extra;
}

} // namespace

void TuiApp::run() {
    if (!isRoot()) {
        ui::showMessage("Warning",
            "Not running as root.\n\n"
            "Viewing disks/SMART data works read-only, but partitioning, formatting,\n"
            "mounting, RAID management, firmware updates, and recovery all require root.\n"
            "Re-run with sudo for full functionality.");
    }
    static const std::string title =
#ifdef DM_VERSION
        "disk-manager " DM_VERSION;
#else
        "disk-manager";
#endif
    while (true) {
        int choice = ui::runMenu(title, {
            "Disks & SMART",
            "RAID Arrays",
            "Create RAID Array",
            "Firmware Updates",
            "Data Recovery",
            "Quit"
        });
        switch (choice) {
            case 0: screenDisks(); break;
            case 1: screenArrays(); break;
            case 2: screenCreateArray(); break;
            case 3: screenFirmware(); break;
            case 4: screenRecovery(); break;
            default: return;
        }
    }
}

// ---------------------------------------------------------------- Disks & SMART

void TuiApp::screenDisks() {
    while (true) {
        // Fetched once and reused for every disk below -- each of listDisks()/
        // find() would otherwise re-run `lsblk` from scratch on every call.
        auto allDevices = devices_.listAll();
        std::vector<BlockDevice> disks;
        for (auto& d : allDevices) if (d.type == "disk") disks.push_back(d);
        if (disks.empty()) { ui::showMessage("Disks", "No disks found."); return; }
        std::string rootDisk = devices_.rootDiskName();
        auto activeArrays = mdadm_.available() ? mdadm_.listArrays() : std::vector<RaidArray>{};
        auto discoveredArrays = mdadm_.available() ? mdadm_.scanForArrays() : std::vector<DiscoveredArray>{};
        std::vector<std::string> items;
        for (auto& d : disks) {
            auto report = smart_.query(d.path);
            bool risk = smart_.isAtRisk(report);
            auto extra = computeDiskExtra(d, allDevices, activeArrays, discoveredArrays);
            std::ostringstream os;
            os << d.path << "  " << std::setw(8) << human(d.sizeBytes)
               << "  [" << healthGlyph(report, risk) << "]"
               << "  " << (d.model.empty() ? "(unknown model)" : d.model)
               << "  mounted:" << extra.mounted
               << "  raid:" << extra.raid
               << (d.kname == rootDisk ? "  (system disk)" : "");
            items.push_back(os.str());
        }
        items.push_back("Back");
        int sel = ui::runMenu("Disks", items, "Enter: manage disk   q: back");
        if (sel < 0 || sel == (int)disks.size()) return;
        screenDiskDetail(disks[sel].path);
    }
}

void TuiApp::screenDiskDetail(const std::string& diskPath) {
    while (true) {
        auto devOpt = devices_.find(diskPath);
        if (!devOpt) return;
        int choice = ui::runMenu(diskPath, {
            "View SMART detail",
            "Partitions & filesystems",
            "Locate this drive (blink activity light)",
            "Power down (standby)",
            "Back"
        });
        if (choice < 0 || choice == 4) return;
        switch (choice) {
            case 0: screenSmartDetail(diskPath); break;
            case 1: screenPartitions(diskPath); break;
            case 2: {
                if (locate_.isActive(diskPath)) {
                    locate_.stop(diskPath);
                    ui::showMessage("Locate", "Stopped locating " + diskPath);
                } else {
                    auto method = locate_.bestMethodFor(diskPath);
                    if (method == LocateMethod::None) {
                        ui::showMessage("Locate", "No way to identify this drive was found\n(no enclosure LED support and drive not readable).");
                        break;
                    }
                    locate_.start(diskPath, 0);
                    std::string how = method == LocateMethod::Enclosure
                        ? "Lighting the enclosure locate LED for this slot."
                        : "No addressable LED found -- driving repeated read-only\naccesses so the drive's activity light flickers.\nWatch for the blinking drive, then come back here\nand select this option again to stop.";
                    ui::showMessage("Locate " + diskPath, how);
                }
                break;
            }
            case 3: screenPowerDown(*devOpt); break;
        }
    }
}

void TuiApp::screenPowerDown(const BlockDevice& dev) {
    if (!power_.hdparmAvailable()) { ui::showMessage("Power down", "hdparm is not installed. Run install.sh first."); return; }

    if (dev.kname == devices_.rootDiskName()) {
        ui::showMessage("Power down", "Refusing to power down the system disk.");
        return;
    }
    if (!power_.supportsStandby(dev.transport)) {
        ui::showMessage("Power down", dev.path + " is " + (dev.transport.empty() ? "not a spinning ATA/SATA disk" : dev.transport) +
            " -- ATA standby doesn't apply (no platters/heads to park).");
        return;
    }

    // Refuse if this disk (or any partition on it) is a member of a currently
    // active RAID array -- spinning it down mid-array risks the array treating
    // it as failed.
    std::vector<std::string> namesToCheck = {dev.path};
    for (auto& c : dev.childPaths) namesToCheck.push_back(c);
    for (auto& arr : mdadm_.listArrays()) {
        for (auto& m : arr.members) {
            if (std::find(namesToCheck.begin(), namesToCheck.end(), m.device) != namesToCheck.end()) {
                ui::showMessage("Power down",
                    dev.path + " has a member in active RAID array " + arr.device + " (" + m.device + ").\n"
                    "Powering it down could make the array treat it as failed. Stop or\n"
                    "otherwise take the array down first if you really need to do this.");
                return;
            }
        }
    }

    // Collect anything currently mounted under this disk or its partitions.
    std::vector<std::string> mounted;
    for (auto& name : namesToCheck) {
        auto d = devices_.find(name);
        if (d && !d->mountpoint.empty()) mounted.push_back(d->mountpoint);
    }
    if (!mounted.empty()) {
        std::ostringstream os;
        os << dev.path << " has mounted filesystems:\n";
        for (auto& mp : mounted) os << "  " << mp << "\n";
        os << "\nUnmount them and continue?";
        if (!ui::promptYesNo(os.str())) return;
        for (auto& mp : mounted) {
            auto res = filesystems_.unmount(mp);
            if (!res.ok()) { ui::showText("Unmount failed", "Could not unmount " + mp + ":\n" + res.stdErr); return; }
        }
    }

    if (!ui::promptYesNo("Spin down " + dev.path + " now?\nIt will spin back up automatically on its next access.")) return;
    auto res = power_.standby(dev.path);
    ui::showText("Power down", res.ok() ? dev.path + " is now in standby." : ("Failed:\n" + res.stdErr));
}

void TuiApp::screenSmartDetail(const std::string& diskPath) {
    auto r = smart_.query(diskPath);
    std::ostringstream os;
    if (!r.supported) {
        os << "SMART not available for " << diskPath << "\n";
        if (!r.error.empty()) os << "Error: " << r.error << "\n";
        ui::showText("SMART: " + diskPath, os.str());
        return;
    }
    os << "Overall health: " << (r.healthy ? "PASSED" : "FAILED") << "\n";
    os << "At-risk heuristic: " << (smart_.isAtRisk(r) ? "YES -- consider replacing" : "no") << "\n";
    if (!r.modelFamily.empty()) os << "Model family: " << r.modelFamily << "\n";
    if (!r.firmwareVersion.empty()) os << "Firmware: " << r.firmwareVersion << "\n";
    if (r.temperatureC) os << "Temperature: " << *r.temperatureC << " C\n";
    if (r.powerOnHours) os << "Power-on hours: " << *r.powerOnHours << "\n";
    if (r.powerCycles) os << "Power cycles: " << *r.powerCycles << "\n";
    if (r.reallocatedSectors) os << "Reallocated sectors: " << *r.reallocatedSectors << "\n";
    if (r.pendingSectors) os << "Pending sectors: " << *r.pendingSectors << "\n";
    if (r.uncorrectableSectors) os << "Uncorrectable sectors: " << *r.uncorrectableSectors << "\n";
    if (r.nvmePercentageUsed) os << "NVMe percentage used: " << (int)*r.nvmePercentageUsed << "%\n";
    if (r.nvmeMediaErrors) os << "NVMe media errors: " << *r.nvmeMediaErrors << "\n";
    if (!r.attributes.empty()) {
        os << "\nATA attributes:\n";
        os << std::left << std::setw(4) << "ID" << std::setw(26) << "Name" << std::setw(6) << "Val" << std::setw(6) << "Wrst" << std::setw(6) << "Thr" << "Raw\n";
        for (auto& a : r.attributes) {
            os << std::left << std::setw(4) << a.id << std::setw(26) << a.name
               << std::setw(6) << a.value << std::setw(6) << a.worst << std::setw(6) << a.threshold
               << a.rawValue << (a.failed ? "  [FAILED]" : "") << "\n";
        }
    }
    ui::showText("SMART: " + diskPath, os.str());
}

// ---------------------------------------------------------------- Partitions & mounts

void TuiApp::screenPartitions(const std::string& diskPath) {
    while (true) {
        auto table = partitions_.read(diskPath);
        std::vector<std::string> items;
        for (auto& p : table.partitions) {
            std::ostringstream os;
            os << p.path << "  " << std::setw(8) << human(p.sizeBytes) << "  "
               << (p.fsType.empty() ? "(no fs)" : p.fsType)
               << (p.flags.empty() ? "" : "  [" + p.flags + "]");
            items.push_back(os.str());
        }
        items.push_back("Create new partition (in free space)");
        items.push_back("Back");
        std::ostringstream title;
        title << diskPath << "  (" << table.label << ", " << human(table.sizeBytes) << ")";
        int sel = ui::runMenu(title.str(), items);
        int nPart = (int)table.partitions.size();
        if (sel < 0 || sel == nPart + 1) return;
        if (sel == nPart) { screenCreatePartition(diskPath); continue; }

        auto& part = table.partitions[sel];
        auto devOpt = devices_.find(part.path);
        int action = ui::runMenu(part.path, {
            "Mount / manage mountpoint",
            "Create filesystem (format)",
            "Resize (grow/shrink partition)",
            "Toggle boot flag",
            "Delete partition",
            "Back"
        });
        if (action == 0 && devOpt) {
            screenMountManage(*devOpt);
        } else if (action == 1) {
            bool cancelled;
            std::string fsType = ui::promptInput("Filesystem type (ext4, xfs, btrfs, vfat, ntfs):", cancelled, "ext4");
            if (cancelled || fsType.empty()) continue;
            if (!ui::promptConfirmPhrase("This ERASES ALL DATA on " + part.path + " (" + human(part.sizeBytes) + ").")) continue;
            auto res = filesystems_.makeFilesystem(part.path, fsType);
            ui::showText("mkfs " + part.path, res.ok() ? "Filesystem created." : ("Failed:\n" + res.stdErr));
        } else if (action == 2) {
            bool cancelled;
            std::string endStr = ui::promptInput("New end offset in MiB (from start of disk):", cancelled);
            if (cancelled || endStr.empty()) continue;
            uint64_t endBytes = 0;
            try { endBytes = std::stoull(endStr) * 1024ULL * 1024ULL; } catch (...) { continue; }
            if (!ui::promptConfirmPhrase("Resizing " + part.path + " does not touch the filesystem inside it.\nGrow the filesystem afterwards from the mount-management screen.")) continue;
            auto res = partitions_.resizePartition(diskPath, part.number, endBytes);
            ui::showText("Resize " + part.path, res.ok() ? "Partition table updated." : ("Failed:\n" + res.stdErr));
        } else if (action == 3) {
            bool turnOn = part.flags.find("boot") == std::string::npos;
            auto res = partitions_.setFlag(diskPath, part.number, "boot", turnOn);
            ui::showText("Flag", res.ok() ? "Done." : ("Failed:\n" + res.stdErr));
        } else if (action == 4) {
            if (!ui::promptConfirmPhrase("This DELETES partition " + part.path + " and everything on it.")) continue;
            auto res = partitions_.deletePartition(diskPath, part.number);
            ui::showText("Delete", res.ok() ? "Deleted." : ("Failed:\n" + res.stdErr));
        }
    }
}

void TuiApp::screenCreatePartition(const std::string& diskPath) {
    auto table = partitions_.read(diskPath);
    if (table.label.empty() || table.label == "loop") {
        if (!ui::promptYesNo("Disk has no partition table (or is unpartitioned). Create a GPT table now?\nThis erases any existing data on " + diskPath + ".")) return;
        auto res = partitions_.createTable(diskPath, "gpt");
        if (!res.ok()) { ui::showText("Create table failed", res.stdErr); return; }
    }
    bool cancelled;
    std::string fsType = ui::promptInput("Filesystem type hint for new partition (ext4, xfs, fat32, linux-swap, or blank):", cancelled, "ext4");
    if (cancelled) return;
    std::string startStr = ui::promptInput("Start offset in MiB:", cancelled, "1");
    if (cancelled || startStr.empty()) return;
    std::string endStr = ui::promptInput("End offset in MiB (blank = rest of disk):", cancelled);
    if (cancelled) return;
    uint64_t startBytes = 0, endBytes = 0;
    try { startBytes = std::stoull(startStr) * 1024ULL * 1024ULL; } catch (...) { return; }
    if (!endStr.empty()) { try { endBytes = std::stoull(endStr) * 1024ULL * 1024ULL; } catch (...) { return; } }
    auto res = partitions_.createPartition(diskPath, fsType, startBytes, endBytes);
    ui::showText("Create partition", res.ok() ? "Partition created." : ("Failed:\n" + res.stdErr));
}

void TuiApp::screenMountManage(const BlockDevice& dev) {
    while (true) {
        bool inFstab = filesystems_.isInFstab(dev.path);
        std::ostringstream status;
        status << "Device: " << dev.path << "\nFilesystem: " << (dev.fsType.empty() ? "(none)" : dev.fsType)
               << "\nCurrently mounted at: " << (dev.mountpoint.empty() ? "(not mounted)" : dev.mountpoint)
               << "\nIn /etc/fstab: " << (inFstab ? "yes" : "no");
        int choice = ui::runMenu("Mount management", {
            "Mount now",
            "Unmount",
            "Grow filesystem to fill partition",
            "Add persistent entry to /etc/fstab",
            "Remove entry from /etc/fstab",
            "Back"
        }, status.str());
        if (choice < 0 || choice == 5) return;
        bool cancelled;
        switch (choice) {
            case 0: {
                std::string mp = ui::promptInput("Mountpoint path:", cancelled, "/mnt/" + dev.name);
                if (cancelled || mp.empty()) break;
                auto res = filesystems_.mount(dev.path, mp, dev.fsType);
                ui::showText("Mount", res.ok() ? "Mounted at " + mp : ("Failed:\n" + res.stdErr));
                break;
            }
            case 1: {
                auto res = filesystems_.unmount(dev.mountpoint.empty() ? dev.path : dev.mountpoint);
                ui::showText("Unmount", res.ok() ? "Unmounted." : ("Failed:\n" + res.stdErr));
                break;
            }
            case 2: {
                auto res = filesystems_.resize(dev.path, dev.fsType, dev.mountpoint);
                ui::showText("Resize filesystem", res.ok() ? "Filesystem grown." : ("Failed:\n" + res.stdErr));
                break;
            }
            case 3: {
                std::string mp = ui::promptInput("Mountpoint for fstab entry:", cancelled, dev.mountpoint.empty() ? ("/mnt/" + dev.name) : dev.mountpoint);
                if (cancelled || mp.empty()) break;
                std::string opts = ui::promptInput("Mount options:", cancelled, "defaults");
                if (cancelled) break;
                auto res = filesystems_.addToFstab(dev.path, mp, dev.fsType, opts);
                ui::showText("fstab", res.ok() ? "Added (backup of /etc/fstab saved)." : ("Failed:\n" + res.stdErr));
                break;
            }
            case 4: {
                auto res = filesystems_.removeFromFstab(dev.path);
                ui::showText("fstab", res.ok() ? "Removed (backup of /etc/fstab saved)." : ("Failed:\n" + res.stdErr));
                break;
            }
        }
    }
}

// ---------------------------------------------------------------- RAID arrays

void TuiApp::screenArrays() {
    while (true) {
        if (!mdadm_.available()) { ui::showMessage("RAID", "mdadm is not installed. Run install.sh first."); return; }

        auto arrays = mdadm_.listArrays();
        // Scans every disk's RAID superblock, not just what's currently assembled --
        // this is what surfaces arrays after a reboot without mdadm.conf, disks
        // moved from another machine, or an array someone stopped earlier.
        auto discovered = mdadm_.scanForArrays();
        std::vector<DiscoveredArray> inactive;
        for (auto& d : discovered) if (!d.active) inactive.push_back(d);

        if (arrays.empty() && inactive.empty()) {
            ui::showMessage("RAID Arrays", "No active RAID arrays, and no RAID superblocks found on any disk.");
            return;
        }

        std::vector<std::string> items;
        for (auto& a : arrays) {
            std::ostringstream os;
            os << a.device << "  " << a.level << "  " << human(a.arraySizeBytes)
               << "  " << a.state << (a.degraded ? "  [DEGRADED]" : "");
            items.push_back(os.str());
        }
        int nActive = (int)arrays.size();
        for (auto& d : inactive) {
            std::ostringstream os;
            os << d.preferredDevice << "  " << (d.level.empty() ? "raid?" : d.level)
               << "  " << d.numDevices << " member(s)  [NOT ASSEMBLED]";
            items.push_back(os.str());
        }
        items.push_back("Back");

        int sel = ui::runMenu("RAID Arrays", items, "Enter: manage/assemble   q: back");
        if (sel < 0 || sel == (int)items.size() - 1) return;
        if (sel < nActive) {
            screenArrayDetail(arrays[sel].device);
        } else {
            screenAssembleDiscovered(inactive[sel - nActive]);
        }
    }
}

void TuiApp::screenAssembleDiscovered(const DiscoveredArray& disc) {
    std::ostringstream os;
    os << "Found on-disk but not currently assembled:\n\n"
       << "Suggested name: " << disc.preferredDevice << "\n"
       << "Level: " << (disc.level.empty() ? "(unknown)" : disc.level) << "\n"
       << "Metadata version: " << disc.metadataVersion << "\n"
       << "UUID: " << disc.uuid << "\n"
       << "Members (" << disc.devices.size() << "):\n";
    for (auto& dev : disc.devices) os << "  " << dev << "\n";
    if (!ui::promptYesNo(os.str() + "\nAssemble this array now?")) return;
    auto res = mdadm_.assembleArray(disc.preferredDevice, disc.devices);
    ui::showText("Assemble", res.ok() ? "Assembled as " + disc.preferredDevice + "." : ("Failed:\n" + res.stdErr));
}

void TuiApp::screenArrayDetail(const std::string& mdPath) {
    while (true) {
        auto detOpt = mdadm_.detail(mdPath);
        if (!detOpt) return;
        auto& d = *detOpt;
        std::ostringstream summary;
        summary << "Level: " << d.level << "   State: " << d.state << (d.degraded ? " [DEGRADED]" : "") << "\n";
        summary << "Size: " << human(d.arraySizeBytes) << "   Active/Working/Failed/Spare: "
                << d.activeDevices << "/" << d.workingDevices << "/" << d.failedDevices << "/" << d.spareDevices << "\n";
        for (auto& m : d.members) summary << "  " << m.device << "  " << m.state << "\n";
        auto rebuild = mdadm_.rebuildStatus(mdPath);
        if (rebuild.active) {
            summary << rebuild.operation << ": " << std::fixed << std::setprecision(1) << rebuild.percent
                    << "%  speed=" << rebuild.speed << "  eta=" << rebuild.timeRemaining << "\n";
        }
        int choice = ui::runMenu(mdPath, {
            "Mount / manage mountpoint",
            "Replace a drive",
            "Add spare device",
            "Fail + remove a device",
            "Grow (add capacity)",
            "Resize filesystem to fill array",
            "Stop array",
            "Back"
        }, summary.str());
        if (choice < 0 || choice == 7) return;
        bool cancelled;
        switch (choice) {
            case 0: {
                auto devOpt = devices_.find(mdPath);
                if (devOpt) screenMountManage(*devOpt);
                else ui::showMessage("Mount", "Could not read device info for " + mdPath);
                break;
            }
            case 1: screenReplaceDrive(mdPath); break;
            case 2: {
                std::string dev = ui::promptInput("Device path to add as spare (e.g. /dev/sdd1):", cancelled);
                if (cancelled || dev.empty()) break;
                auto res = mdadm_.addDevice(mdPath, dev);
                ui::showText("Add device", res.ok() ? "Added." : ("Failed:\n" + res.stdErr));
                break;
            }
            case 3: {
                std::string dev = ui::promptInput("Device path to fail and remove:", cancelled);
                if (cancelled || dev.empty()) break;
                if (!ui::promptConfirmPhrase("This marks " + dev + " faulty and removes it from " + mdPath + ".")) break;
                mdadm_.failDevice(mdPath, dev);
                auto res = mdadm_.removeDevice(mdPath, dev);
                ui::showText("Remove device", res.ok() ? "Removed." : ("Failed:\n" + res.stdErr));
                break;
            }
            case 4: screenGrowArray(mdPath); break;
            case 5: {
                auto devOpt = devices_.find(mdPath);
                std::string fsType = devOpt ? devOpt->fsType : "";
                std::string mp = devOpt ? devOpt->mountpoint : "";
                if (fsType.empty()) { ui::showMessage("Resize", "No filesystem detected on " + mdPath); break; }
                auto res = filesystems_.resize(mdPath, fsType, mp);
                ui::showText("Resize filesystem", res.ok() ? "Grown." : ("Failed:\n" + res.stdErr));
                break;
            }
            case 6: {
                if (!ui::promptConfirmPhrase("This stops (deactivates) " + mdPath + ". Data is not lost, but it must be re-assembled to use again.")) break;
                auto res = mdadm_.stopArray(mdPath);
                ui::showText("Stop", res.ok() ? "Stopped." : ("Failed:\n" + res.stdErr));
                if (res.ok()) return;
                break;
            }
        }
    }
}

void TuiApp::screenCreateArray() {
    if (!mdadm_.available()) { ui::showMessage("RAID", "mdadm is not installed. Run install.sh first."); return; }
    auto disks = devices_.listDisks();
    std::string rootDisk = devices_.rootDiskName();
    std::vector<std::string> items;
    std::vector<std::string> paths;
    for (auto& d : disks) {
        if (d.kname == rootDisk) continue; // never offer the system disk
        std::ostringstream os;
        os << d.path << "  " << human(d.sizeBytes) << "  " << (d.model.empty() ? "" : d.model)
           << (d.mountpoint.empty() ? "" : "  [mounted at " + d.mountpoint + "]");
        items.push_back(os.str());
        paths.push_back(d.path);
    }
    if (items.size() < 2) { ui::showMessage("Create array", "Need at least 2 non-system disks to create a RAID array."); return; }
    bool cancelled;
    auto picked = ui::runChecklist("Select member disks (space to toggle, enter to confirm)", items, "", cancelled);
    if (cancelled || picked.size() < 2) return;

    std::string level = "1";
    int lvlIdx = ui::runMenu("RAID level", {"0 (striping, no redundancy)", "1 (mirror)", "5 (parity, N-1 usable)", "6 (dual parity, N-2 usable)", "10 (mirrored stripes)"});
    switch (lvlIdx) {
        case 0: level = "0"; break;
        case 1: level = "1"; break;
        case 2: level = "5"; break;
        case 3: level = "6"; break;
        case 4: level = "10"; break;
        default: return;
    }
    std::string mdPath = ui::promptInput("New array device name:", cancelled, "/dev/md0");
    if (cancelled || mdPath.empty()) return;

    std::vector<std::string> members;
    std::ostringstream warn;
    warn << "About to create RAID" << level << " " << mdPath << " from:\n";
    for (int i : picked) { members.push_back(paths[i]); warn << "  " << paths[i] << "\n"; }
    warn << "\nTHIS DESTROYS ALL DATA on these disks.";
    if (!ui::promptConfirmPhrase(warn.str())) return;

    ui::showProgress("Creating " + mdPath, [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
        update(10, "running mdadm --create...");
        auto res = mdadm_.createArray(mdPath, level, members);
        update(100, res.ok() ? "done" : ("failed: " + res.stdErr));
    });
    ui::showText("Create array", "mdadm --create issued for " + mdPath + ".\nCheck RAID Arrays screen for sync progress.");
}

void TuiApp::screenReplaceDrive(const std::string& mdPath) {
    auto detOpt = mdadm_.detail(mdPath);
    if (!detOpt) return;
    std::vector<std::string> items;
    std::vector<std::string> memberPaths;
    for (auto& m : detOpt->members) {
        auto report = smart_.query(m.device);
        bool risk = smart_.isAtRisk(report);
        items.push_back(m.device + "  " + m.state + (risk ? "  [SMART AT-RISK]" : ""));
        memberPaths.push_back(m.device);
    }
    items.push_back("Cancel");
    int sel = ui::runMenu("Select failing/failed member to replace", items);
    if (sel < 0 || sel == (int)memberPaths.size()) return;
    std::string oldDev = memberPaths[sel];

    if (locate_.bestMethodFor(oldDev) != LocateMethod::None) {
        if (ui::promptYesNo("Blink the activity light on " + oldDev + " now to confirm you have\nthe right physical drive before pulling anything?")) {
            locate_.start(oldDev, 15);
            ui::showMessage("Locating", "Watch the drive bay for 15 seconds of activity...");
        }
    }

    bool cancelled;
    std::string newDev = ui::promptInput("Path of the REPLACEMENT drive/partition (already inserted):", cancelled);
    if (cancelled || newDev.empty()) return;
    if (!ui::promptConfirmPhrase("Replacing " + oldDev + " with " + newDev + " in " + mdPath + ".\nAny data currently on " + newDev + " will be destroyed.")) return;

    auto res = mdadm_.replaceDevice(mdPath, oldDev, newDev);
    if (!res.ok()) { ui::showText("Replace drive", "Failed:\n" + res.stdErr); return; }

    ui::showProgress("Rebuilding " + mdPath, [&](std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& update) {
        while (!cancel.load()) {
            auto rb = mdadm_.rebuildStatus(mdPath);
            if (!rb.active) { update(100, "rebuild complete or not reported by mdstat"); return; }
            update(rb.percent, rb.operation + "  speed=" + rb.speed + "  eta=" + rb.timeRemaining);
            struct timespec ts{0, 500'000'000L};
            nanosleep(&ts, nullptr);
        }
        update(0, "monitoring cancelled (rebuild continues in the background)");
    });
}

void TuiApp::screenGrowArray(const std::string& mdPath) {
    bool cancelled;
    std::string newDev = ui::promptInput("Device to add before growing (blank to skip, if already added):", cancelled);
    if (cancelled) return;
    if (!newDev.empty()) {
        auto res = mdadm_.addDevice(mdPath, newDev);
        if (!res.ok()) { ui::showText("Add device", "Failed:\n" + res.stdErr); return; }
    }
    std::string countStr = ui::promptInput("New total number of raid-devices:", cancelled);
    if (cancelled || countStr.empty()) return;
    int count = 0;
    try { count = std::stoi(countStr); } catch (...) { return; }
    if (!ui::promptConfirmPhrase("Growing " + mdPath + " to " + countStr + " devices. This can take a long time.")) return;
    auto res = mdadm_.growRaidDevices(mdPath, count);
    ui::showText("Grow", res.ok() ? "Grow started; check array detail for progress." : ("Failed:\n" + res.stdErr));
}

// ---------------------------------------------------------------- Firmware

void TuiApp::screenFirmware() {
    if (!firmware_.fwupdAvailable()) { ui::showMessage("Firmware", "fwupdmgr is not installed. Run install.sh first."); return; }
    auto disks = devices_.listDisks();
    std::vector<std::string> paths;
    for (auto& d : disks) paths.push_back(d.path);

    if (ui::promptYesNo("Refresh firmware metadata from the network first?")) {
        ui::showProgress("Refreshing metadata", [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
            update(50, "fwupdmgr refresh...");
            auto res = firmware_.refreshMetadata();
            update(100, res.ok() ? "done" : "failed (continuing with cached metadata)");
        });
    }

    auto infos = firmware_.listStorageDevices(paths);
    if (infos.empty()) { ui::showMessage("Firmware", "No fwupd-manageable storage devices found on this system."); return; }
    while (true) {
        std::vector<std::string> items;
        for (auto& f : infos) {
            std::ostringstream os;
            os << (f.devicePath.empty() ? "(unmatched device)" : f.devicePath) << "  " << f.vendor << " " << f.name
               << "  current=" << f.currentVersion
               << (f.updateAvailable ? ("  UPDATE AVAILABLE -> " + f.latestVersion) : "");
            items.push_back(os.str());
        }
        items.push_back("Back");
        int sel = ui::runMenu("Firmware", items);
        if (sel < 0 || sel == (int)infos.size()) return;
        auto& f = infos[sel];
        if (!f.updateAvailable) { ui::showMessage("Firmware", "No update available for " + f.name + "."); continue; }
        std::ostringstream warn;
        warn << "Updating firmware on " << f.name << " (" << f.currentVersion << " -> " << f.latestVersion << ").\n"
             << "Do not power off or remove this drive during the update -- an\n"
             << "interrupted firmware write can permanently damage the drive.";
        if (!ui::promptConfirmPhrase(warn.str())) continue;
        auto res = firmware_.applyUpdate(f.fwupdDeviceId);
        ui::showText("Firmware update", res.ok() ? "Update applied (a reboot may be required)." : ("Failed:\n" + res.stdErr));
    }
}

// ---------------------------------------------------------------- Data recovery

void TuiApp::screenRecovery() {
    int choice = ui::runMenu("Data Recovery", {
        "Image a failing disk/partition/array with ddrescue",
        "Launch testdisk (partition table recovery)",
        "Launch photorec (file carving / recovery)",
        "Back"
    });
    if (choice < 0 || choice == 3) return;
    bool cancelled;
    if (choice == 0) {
        if (!recovery_.ddrescueAvailable()) { ui::showMessage("Recovery", "ddrescue is not installed. Run install.sh first."); return; }
        std::string source = ui::promptInput("Source device (disk, partition, or /dev/mdX):", cancelled);
        if (cancelled || source.empty()) return;
        std::string dest = ui::promptInput("Destination image file path:", cancelled);
        if (cancelled || dest.empty()) return;
        std::string mapFile = ui::promptInput("Mapfile path (for resuming if interrupted):", cancelled, dest + ".mapfile");
        if (cancelled || mapFile.empty()) return;
        if (!ui::promptConfirmPhrase("Imaging " + source + " -> " + dest + ".\nThis reads " + source + " but never writes to it.")) return;

        ui::showProgress("ddrescue " + source, [&](std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& update) {
            recovery_.rescue(source, dest, mapFile, [&](const RescueProgress& p) {
                std::ostringstream os;
                os << p.currentStatus << "  rescued=" << human(p.rescuedBytes) << "/" << human(p.totalBytes)
                   << "  errors=" << p.errorCount;
                update(p.percent, os.str());
            }, &cancel);
        });
        ui::showText("ddrescue", "Finished (or cancelled). Mapfile kept at:\n" + mapFile + "\nRe-run with the same mapfile to resume.");
    } else if (choice == 1) {
        if (!recovery_.testdiskAvailable()) { ui::showMessage("Recovery", "testdisk is not installed. Run install.sh first."); return; }
        std::string target = ui::promptInput("Target device (blank = let testdisk choose):", cancelled);
        if (cancelled) return;
        endwin();
        recovery_.launchTestdisk(target);
        refresh();
    } else if (choice == 2) {
        if (!recovery_.testdiskAvailable()) { ui::showMessage("Recovery", "photorec is not installed. Run install.sh first."); return; }
        std::string target = ui::promptInput("Target device (blank = let photorec choose):", cancelled);
        if (cancelled) return;
        std::string outDir = ui::promptInput("Output directory for recovered files:", cancelled, "/root/recovered");
        if (cancelled) return;
        endwin();
        recovery_.launchPhotorec(target, outDir);
        refresh();
    }
}

} // namespace dm
