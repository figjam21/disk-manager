#include "dm/firmware.hpp"
#include "dm/command.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <sstream>

namespace dm {

namespace {

using json = nlohmann::json;

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool containsCi(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

// Heuristic: fwupd has no notion of "this is a storage device" beyond the plugin
// that drives it and its human-readable name, and neither is a fully reliable
// signal on its own (e.g. some SATA/SAS disks are only reachable via a generic
// "scsi"/"ata" plugin whose exact string varies by fwupd version). We check a
// small allow-list of plugin names plus a name-substring fallback; this will
// both miss some storage devices and, in principle, could false-positive on a
// non-storage device with a coincidental name.
bool looksLikeStorageDevice(const json& dev) {
    std::string plugin = toLower(dev.value("Plugin", std::string()));
    if (plugin == "nvme" || plugin == "ata_id" || plugin == "block" ||
        plugin == "ata" || plugin == "scsi") {
        return true;
    }
    std::string name = dev.value("Name", std::string());
    return containsCi(name, "ssd") || containsCi(name, "nvme") ||
           containsCi(name, "hdd") || containsCi(name, "disk");
}

// Parses `udevadm info --query=property --name=<path>` output (plain "KEY=value"
// lines) and returns the requested keys' values, or empty string if not present.
struct UdevProps {
    std::string idModel;
    std::string idVendor;
};

UdevProps queryUdevProps(const std::string& devicePath) {
    UdevProps props;
    CommandResult result = run({"udevadm", "info", "--query=property", "--name=" + devicePath});
    if (!result.ok()) return props;

    std::istringstream stream(result.stdOut);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip a trailing '\r' in case output is CRLF-terminated.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "ID_MODEL") {
            props.idModel = value;
        } else if (key == "ID_VENDOR") {
            props.idVendor = value;
        }
    }
    return props;
}

// udev encodes spaces in ID_MODEL/ID_VENDOR as underscores; undo that before doing
// substring comparisons against fwupd's free-text device names.
std::string udevDecode(const std::string& s) {
    std::string out = s;
    std::replace(out.begin(), out.end(), '_', ' ');
    return out;
}

} // namespace

bool FirmwareManager::fwupdAvailable() {
    return commandExists("fwupdmgr");
}

CommandResult FirmwareManager::refreshMetadata() {
    return run({"fwupdmgr", "refresh", "-y"});
}

CommandResult FirmwareManager::applyUpdate(const std::string& fwupdDeviceId) {
    return run({"fwupdmgr", "update", fwupdDeviceId, "-y"});
}

std::vector<FirmwareInfo> FirmwareManager::listStorageDevices(
    const std::vector<std::string>& blockDevicePaths) {
    std::vector<FirmwareInfo> result;

    if (!fwupdAvailable()) {
        return result;
    }

    CommandResult devicesResult = run({"fwupdmgr", "get-devices", "--json"});
    if (!devicesResult.ok() || devicesResult.stdOut.empty()) {
        return result;
    }

    json devicesJson;
    try {
        devicesJson = json::parse(devicesResult.stdOut);
    } catch (const std::exception&) {
        return result;
    }

    if (!devicesJson.contains("Devices") || !devicesJson["Devices"].is_array()) {
        return result;
    }

    for (const auto& dev : devicesJson["Devices"]) {
        if (!looksLikeStorageDevice(dev)) continue;

        FirmwareInfo info;
        info.fwupdDeviceId = dev.value("DeviceId", std::string());
        info.vendor = dev.value("Vendor", std::string());
        info.name = dev.value("Name", std::string());
        info.currentVersion = dev.value("Version", std::string());
        info.managedByFwupd = true;
        result.push_back(std::move(info));
    }

    if (result.empty()) {
        return result;
    }

    // Best-effort: cross-reference get-updates --json to flag which of the storage
    // devices we already found have a pending update, and what version it is.
    CommandResult updatesResult = run({"fwupdmgr", "get-updates", "--json"});
    if (updatesResult.ok() && !updatesResult.stdOut.empty()) {
        try {
            json updatesJson = json::parse(updatesResult.stdOut);
            if (updatesJson.contains("Devices") && updatesJson["Devices"].is_array()) {
                for (const auto& dev : updatesJson["Devices"]) {
                    std::string id = dev.value("DeviceId", std::string());
                    if (id.empty()) continue;
                    if (!dev.contains("Releases") || !dev["Releases"].is_array() ||
                        dev["Releases"].empty()) {
                        continue;
                    }
                    std::string latest = dev["Releases"][0].value("Version", std::string());
                    if (latest.empty()) continue;

                    for (auto& info : result) {
                        if (info.fwupdDeviceId == id) {
                            info.updateAvailable = true;
                            info.latestVersion = latest;
                            break;
                        }
                    }
                }
            }
        } catch (const std::exception&) {
            // Non-fatal: just skip update cross-referencing.
        }
    }

    // Best-effort mapping of fwupd devices to kernel block device paths. fwupd
    // identifies devices by GUID, not by /dev/sdX node, so there is no reliable
    // way to do this; we fall back to a case-insensitive substring match between
    // udev's ID_MODEL (decoded) and the fwupd device's free-text Name. This can
    // both miss real matches (differing strings) and, in theory, mismatch on
    // generic/ambiguous model strings, so devicePath is left empty rather than
    // guessed when nothing lines up confidently.
    for (const auto& path : blockDevicePaths) {
        UdevProps props = queryUdevProps(path);
        std::string model = udevDecode(props.idModel);
        std::string vendor = udevDecode(props.idVendor);
        if (model.empty() && vendor.empty()) continue;

        for (auto& info : result) {
            if (!info.devicePath.empty()) continue; // already matched to another path

            bool matched = false;
            if (!model.empty()) {
                matched = containsCi(info.name, model) || containsCi(model, info.name);
            }
            if (!matched && !vendor.empty()) {
                matched = containsCi(info.name, vendor) || containsCi(vendor, info.name);
            }
            if (matched) {
                info.devicePath = path;
                break;
            }
        }
    }

    return result;
}

} // namespace dm
