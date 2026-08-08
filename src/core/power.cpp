#include "dm/power.hpp"

#include <algorithm>
#include <cctype>

namespace dm {

namespace {
std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}
} // namespace

bool PowerManager::hdparmAvailable() {
    return commandExists("hdparm");
}

bool PowerManager::supportsStandby(const std::string& transport) {
    std::string t = toLower(transport);
    return t != "nvme"; // NVMe has no platters/heads for ATA STANDBY to park
}

CommandResult PowerManager::standby(const std::string& devicePath) {
    return run({"hdparm", "-y", devicePath});
}

} // namespace dm
