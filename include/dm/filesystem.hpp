#pragma once
#include <string>
#include <vector>
#include "dm/command.hpp"

namespace dm {

struct MountEntry {
    std::string source;   // device path
    std::string target;   // mountpoint
    std::string fsType;
    std::string options;
};

struct FstabEntry {
    std::string spec;     // "UUID=..." or device path, as written in fstab
    std::string mountpoint;
    std::string fsType;
    std::string options;
    int dump = 0;
    int pass = 0;
};

class FilesystemManager {
public:
    CommandResult makeFilesystem(const std::string& devicePath, const std::string& fsType,
                                  const std::string& label = "");

    CommandResult mount(const std::string& devicePath, const std::string& mountpoint,
                         const std::string& fsType = "", const std::string& options = "defaults");

    // Accepts either a mountpoint or a device path.
    CommandResult unmount(const std::string& mountpointOrDevice);

    // Grows the filesystem to fill its underlying block device (partition or md
    // array). Dispatches to resize2fs / xfs_growfs / `btrfs filesystem resize max`
    // based on fsType. XFS requires the filesystem to be mounted; mountpoint may be
    // left empty for ext*, but is required for xfs and btrfs.
    CommandResult resize(const std::string& devicePath, const std::string& fsType,
                          const std::string& mountpoint = "");

    // Live view from /proc/mounts.
    std::vector<MountEntry> listMounts();

    std::vector<FstabEntry> readFstab();

    bool isInFstab(const std::string& devicePath);

    // Appends an entry (preferring UUID= if the device has one) after backing up
    // /etc/fstab to /etc/fstab.bak.<unix-time>. Never rewrites existing lines.
    CommandResult addToFstab(const std::string& devicePath, const std::string& mountpoint,
                              const std::string& fsType, const std::string& options = "defaults",
                              int dump = 0, int pass = 2);

    // Removes any line whose spec resolves to devicePath, or whose mountpoint
    // matches. Backs up /etc/fstab first, same as addToFstab.
    CommandResult removeFromFstab(const std::string& devicePathOrMountpoint);
};

} // namespace dm
