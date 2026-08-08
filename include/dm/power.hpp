#pragma once
#include <string>
#include "dm/command.hpp"

namespace dm {

// Spins a drive down rather than cutting its power outright (Linux has no
// portable way to remove electrical power from a fixed SATA/SAS bay -- only
// hot-swap enclosures with their own controls can do that). ATA standby is the
// safe, reversible equivalent: the drive parks its heads and stops spinning, then
// transparently spins back up and services the next I/O request on its own, no
// user action required to "power back on".
class PowerManager {
public:
    bool hdparmAvailable();

    // True for devices where ATA standby doesn't apply (NVMe has no spinning
    // platters or heads to park) -- callers should refuse the action rather than
    // letting hdparm fail with a confusing error.
    bool supportsStandby(const std::string& transport);

    // `hdparm -y <devicePath>` -- immediate ATA STANDBY. Safe to call on a device
    // with no open filesystems; callers should unmount first if anything is
    // mounted, since parking heads under active I/O just means the next access
    // blocks briefly while the drive spins back up.
    CommandResult standby(const std::string& devicePath);
};

} // namespace dm
