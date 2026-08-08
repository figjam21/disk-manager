#pragma once
#include <string>
#include <vector>
#include "dm/command.hpp"

namespace dm {

struct FirmwareInfo {
    std::string devicePath;     // best-effort match to a BlockDevice::path, may be empty
    std::string fwupdDeviceId;  // fwupd's internal id, empty if not fwupd-managed
    std::string vendor;
    std::string name;
    std::string currentVersion;
    std::string latestVersion;  // empty if no update available or unknown
    bool updateAvailable = false;
    bool managedByFwupd = false;
};

// Wraps `fwupdmgr` (the Linux Vendor Firmware Service client) rather than talking to
// drives directly: fwupd only ever applies updates the vendor has published and
// signed for that exact model, which is the only firmware-flashing approach safe
// enough to automate. Drives fwupd doesn't manage show up read-only (current
// version via smartctl/nvme identify) with no update path offered.
class FirmwareManager {
public:
    bool fwupdAvailable();

    // Cross-references `fwupdmgr get-devices --json` against the given block device
    // paths (matched heuristically by model/vendor string, since fwupd does not
    // expose a kernel device node mapping) and, for storage-class fwupd devices,
    // `fwupdmgr get-updates --json` for available updates.
    std::vector<FirmwareInfo> listStorageDevices(const std::vector<std::string>& blockDevicePaths);

    CommandResult refreshMetadata(); // fwupdmgr refresh

    // Applies a pending update for the given fwupd device id. Callers must obtain an
    // explicit typed confirmation from the user before calling this -- firmware
    // updates can be interrupted by power loss and are not reversible.
    CommandResult applyUpdate(const std::string& fwupdDeviceId);
};

} // namespace dm
