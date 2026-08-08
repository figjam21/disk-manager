#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace dm {

// One row from `lsblk`: a whole disk, a partition, or an md/lvm virtual device.
struct BlockDevice {
    std::string name;        // e.g. "sda1"
    std::string path;        // e.g. "/dev/sda1"
    std::string kname;       // kernel name, usually == name
    std::string pkname;      // parent kernel device name, empty for whole disks
    std::string type;        // "disk", "part", "raid1", "raid5", "lvm", ...
    std::string model;       // whole disks only
    std::string serial;      // whole disks only
    std::string transport;   // "sata", "nvme", "usb", "sas", ...
    std::string fsType;      // filesystem type if formatted
    std::string uuid;        // filesystem UUID
    std::string label;       // filesystem label
    std::string mountpoint;
    uint64_t sizeBytes = 0;
    bool rotational = false;
    bool removable = false;
    std::vector<std::string> childPaths; // partitions, for whole disks
};

class DeviceManager {
public:
    // Flat list of every disk and partition lsblk knows about.
    std::vector<BlockDevice> listAll();

    // Only entries with type == "disk" (physical/virtual whole disks, not partitions).
    std::vector<BlockDevice> listDisks();

    std::optional<BlockDevice> find(const std::string& path);

    // Kernel name (e.g. "sda") of the disk backing the running system's root
    // filesystem, or empty if it can't be determined. Callers must not treat an
    // empty result as "safe to touch anything" -- it means "exclude nothing
    // automatically here", not "there is no root disk".
    std::string rootDiskName();
};

} // namespace dm
