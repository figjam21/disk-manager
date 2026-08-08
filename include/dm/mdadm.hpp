#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "dm/command.hpp"

namespace dm {

struct RaidMember {
    std::string device;  // e.g. "/dev/sdb1"
    int number = -1;
    int raidDevice = -1;
    std::string state;   // "active sync", "spare", "faulty", "rebuilding", ...
};

struct RaidArray {
    std::string device;  // e.g. "/dev/md0"
    std::string level;   // "raid0", "raid1", "raid5", "raid6", "raid10"
    std::string state;   // "clean", "active", "degraded", "resyncing", ...
    std::string uuid;
    uint64_t arraySizeBytes = 0;
    int raidDevices = 0;
    int totalDevices = 0;
    int activeDevices = 0;
    int workingDevices = 0;
    int failedDevices = 0;
    int spareDevices = 0;
    bool degraded = false;
    std::vector<RaidMember> members;
};

// Snapshot of an in-progress resync/recovery/reshape, from /proc/mdstat.
struct RebuildStatus {
    bool active = false;
    std::string operation; // "resync", "recovery", "reshape", "check"
    double percent = 0.0;
    std::string speed;         // e.g. "45123K/sec"
    std::string timeRemaining; // e.g. "12.3min", as printed by mdstat
};

// An array found by examining on-disk RAID superblocks directly, whether or not it
// is currently assembled/running. Surfaces arrays mdadm.conf doesn't know about --
// e.g. disks moved from another machine, or an array that was previously stopped.
struct DiscoveredArray {
    std::string preferredDevice; // mdadm's suggested name, e.g. "/dev/md/0"
    std::string uuid;
    std::string level;           // may be empty on older mdadm without --verbose support
    std::string metadataVersion;
    int numDevices = 0;
    std::vector<std::string> devices; // member device paths, empty if --verbose gave none
    bool active = false;              // true if a currently-running array has this UUID
    std::string activeDevice;         // the running /dev/mdX, set only when active
};

class MdadmManager {
public:
    bool available();

    // Enumerates /proc/mdstat and details every array found there.
    std::vector<RaidArray> listArrays();

    std::optional<RaidArray> detail(const std::string& mdPath);

    // mdadm --create /dev/mdX --level=<level> --raid-devices=N [--spare-devices=M] devices... spares...
    // level is given without the "raid" prefix stripped, e.g. "1", "5", "6", "10", "0".
    CommandResult createArray(const std::string& mdPath, const std::string& level,
                               const std::vector<std::string>& devices,
                               const std::vector<std::string>& spares = {});

    // mdadm --assemble; if devices is empty, uses --scan instead of an explicit list.
    CommandResult assembleArray(const std::string& mdPath, const std::vector<std::string>& devices = {});

    CommandResult stopArray(const std::string& mdPath);

    CommandResult addDevice(const std::string& mdPath, const std::string& devicePath);
    CommandResult removeDevice(const std::string& mdPath, const std::string& devicePath);
    CommandResult failDevice(const std::string& mdPath, const std::string& devicePath);

    // High-level replace workflow: fail + remove oldDevicePath (if not already
    // faulty/removed), then add newDevicePath. Stops at the first failure and
    // reports it via CommandResult.stdErr; caller should surface partial progress.
    CommandResult replaceDevice(const std::string& mdPath, const std::string& oldDevicePath,
                                 const std::string& newDevicePath);

    // mdadm --grow --raid-devices=N. The new device(s) must already have been added
    // (as spares) via addDevice first.
    CommandResult growRaidDevices(const std::string& mdPath, int newRaidDevices);

    // mdadm --grow --size=<sizeKb|"max">
    CommandResult growSize(const std::string& mdPath, const std::string& size = "max");

    RebuildStatus rebuildStatus(const std::string& mdPath);

    // Scans every block device's RAID superblock (`mdadm --examine --scan
    // --verbose`) to find arrays whether or not they're currently assembled --
    // read-only, does not activate or modify anything. Cross-references
    // listArrays() so each result's `active`/`activeDevice` reflects whether it's
    // already running (matched by UUID).
    std::vector<DiscoveredArray> scanForArrays();
};

} // namespace dm
