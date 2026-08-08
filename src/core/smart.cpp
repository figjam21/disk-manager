#include "dm/smart.hpp"
#include "dm/command.hpp"

#include <nlohmann/json.hpp>

namespace dm {

namespace {

using json = nlohmann::json;

// Pulls the raw numeric value of an ATA attribute out of its "raw" sub-object,
// falling back to parsing "raw.string" (e.g. "0", or "12345 (avg)") if "raw.value"
// is missing, since some smartctl/vendor combinations only populate one or the other.
std::optional<uint64_t> rawValueOf(const json& attr) {
    if (!attr.contains("raw") || !attr["raw"].is_object()) return std::nullopt;
    const json& raw = attr["raw"];
    if (raw.contains("value") && raw["value"].is_number()) {
        return raw["value"].get<uint64_t>();
    }
    if (raw.contains("string") && raw["string"].is_string()) {
        try {
            return static_cast<uint64_t>(std::stoull(raw["string"].get<std::string>()));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool nameContains(const std::string& name, const char* needle) {
    return name.find(needle) != std::string::npos;
}

void parseAtaAttributes(const json& j, SmartReport& report) {
    if (!j.contains("ata_smart_attributes")) return;
    const json& sa = j["ata_smart_attributes"];
    if (!sa.contains("table") || !sa["table"].is_array()) return;

    for (const auto& a : sa["table"]) {
        SmartAttribute attr;
        attr.id = a.value("id", 0);
        attr.name = a.value("name", std::string());
        attr.value = a.value("value", 0);
        attr.worst = a.value("worst", 0);
        attr.threshold = a.value("thresh", 0);
        if (a.contains("raw") && a["raw"].is_object()) {
            attr.rawValue = a["raw"].value("string", std::string());
        }
        attr.failed = !a.value("when_failed", std::string()).empty();

        auto raw = rawValueOf(a);

        bool isReallocated = attr.id == 5 || nameContains(attr.name, "Reallocated_Sector");
        bool isPending = attr.id == 197 || nameContains(attr.name, "Pending_Sector");
        bool isUncorrectable = attr.id == 198 || nameContains(attr.name, "Uncorrectable");

        if (isReallocated && !report.reallocatedSectors && raw) report.reallocatedSectors = raw;
        if (isPending && !report.pendingSectors && raw) report.pendingSectors = raw;
        if (isUncorrectable && !report.uncorrectableSectors && raw) report.uncorrectableSectors = raw;

        report.attributes.push_back(std::move(attr));
    }
}

void parseAta(const json& j, SmartReport& report) {
    if (j.contains("temperature") && j["temperature"].contains("current")) {
        report.temperatureC = j["temperature"].value("current", 0);
    }
    if (j.contains("power_on_time") && j["power_on_time"].contains("hours")) {
        report.powerOnHours = j["power_on_time"].value("hours", uint64_t{0});
    }
    if (j.contains("power_cycle_count") && j["power_cycle_count"].is_number()) {
        report.powerCycles = j["power_cycle_count"].get<uint64_t>();
    }
    parseAtaAttributes(j, report);
}

void parseNvme(const json& j, SmartReport& report) {
    if (!j.contains("nvme_smart_health_information_log")) return;
    const json& h = j["nvme_smart_health_information_log"];

    if (h.contains("temperature") && h["temperature"].is_number()) {
        report.temperatureC = h["temperature"].get<int>();
    }
    if (h.contains("power_on_hours") && h["power_on_hours"].is_number()) {
        report.powerOnHours = h["power_on_hours"].get<uint64_t>();
    }
    if (h.contains("power_cycles") && h["power_cycles"].is_number()) {
        report.powerCycles = h["power_cycles"].get<uint64_t>();
    }
    if (h.contains("percentage_used") && h["percentage_used"].is_number()) {
        report.nvmePercentageUsed = static_cast<uint8_t>(h["percentage_used"].get<unsigned>());
    }
    if (h.contains("media_errors") && h["media_errors"].is_number()) {
        report.nvmeMediaErrors = h["media_errors"].get<uint64_t>();
    }
    // "critical_warning" is available in the log (h.value("critical_warning", 0)) but the
    // header exposes no field to carry it; isAtRisk() does not need it since
    // media_errors/percentage_used already cover the risk heuristic.
}

} // namespace

bool SmartManager::smartctlAvailable() {
    return commandExists("smartctl");
}

SmartReport SmartManager::query(const std::string& devicePath) {
    SmartReport report;
    report.device = devicePath;

    CommandResult result = run({"smartctl", "-a", "-j", devicePath});

    if (result.stdOut.empty()) {
        report.supported = false;
        report.error = result.stdErr.empty()
            ? "smartctl produced no output"
            : "smartctl produced no output: " + result.stdErr;
        return report;
    }

    json j;
    try {
        j = json::parse(result.stdOut);
    } catch (const std::exception& e) {
        report.supported = false;
        report.error = std::string("failed to parse smartctl JSON output: ") + e.what();
        return report;
    }

    // smartctl's own top-level error messages (e.g. permission denied, device not found)
    // live under smartctl.messages[]; surface the first one if present so callers get a
    // useful reason even though we still attempt to parse whatever else is there.
    std::string smartctlMessage;
    if (j.contains("smartctl") && j["smartctl"].contains("messages") && j["smartctl"]["messages"].is_array()
        && !j["smartctl"]["messages"].empty()) {
        smartctlMessage = j["smartctl"]["messages"][0].value("string", std::string());
    }

    // When smartctl can't even identify/open the device (missing device, permission
    // denied, unsupported device type, ...) the JSON is just the "smartctl"/"local_time"
    // wrapper with no device-level keys at all. Treat that as an unsupported/failed
    // query rather than a healthy default-constructed report.
    bool hasDeviceData = j.contains("device") || j.contains("smart_support") ||
        j.contains("smart_status") || j.contains("ata_smart_attributes") ||
        j.contains("nvme_smart_health_information_log");

    if (!hasDeviceData) {
        report.supported = false;
        report.error = !smartctlMessage.empty() ? smartctlMessage : "smartctl could not identify the device";
        return report;
    }

    bool smartSupportAvailable = true; // assume supported unless told otherwise
    if (j.contains("smart_support") && j["smart_support"].contains("available")) {
        smartSupportAvailable = j["smart_support"].value("available", true);
    }

    bool hasAta = j.contains("ata_smart_attributes") || j.contains("smart_status");
    bool hasNvme = j.contains("nvme_smart_health_information_log");

    if (!smartSupportAvailable && !hasAta && !hasNvme) {
        report.supported = false;
        report.error = !smartctlMessage.empty() ? smartctlMessage : "device does not support SMART";
        return report;
    }

    report.supported = true;

    if (j.contains("model_family")) report.modelFamily = j.value("model_family", std::string());
    if (j.contains("firmware_version")) report.firmwareVersion = j.value("firmware_version", std::string());

    if (j.contains("smart_status") && j["smart_status"].contains("passed")) {
        report.healthy = j["smart_status"].value("passed", true);
    }

    std::string deviceType;
    if (j.contains("device") && j["device"].contains("type")) {
        deviceType = j["device"].value("type", std::string());
    }

    if (hasNvme || deviceType == "nvme") {
        parseNvme(j, report);
    } else {
        // Default to ATA parsing for "sat"/"ata" device types, and as a general
        // fallback when the device type is unrecognized but ATA-shaped data is present.
        parseAta(j, report);
    }

    if (!smartctlMessage.empty() && report.error.empty()) {
        // Non-fatal smartctl diagnostic (e.g. a bitmask exit status without a fatal
        // parse failure); keep it around for visibility without treating the whole
        // query as unsupported.
        report.error = smartctlMessage;
    }

    return report;
}

bool SmartManager::isAtRisk(const SmartReport& report) {
    if (!report.healthy) return true;

    if (report.reallocatedSectors && *report.reallocatedSectors > 0) return true;
    if (report.pendingSectors && *report.pendingSectors > 0) return true;
    if (report.uncorrectableSectors && *report.uncorrectableSectors > 0) return true;
    if (report.nvmeMediaErrors && *report.nvmeMediaErrors > 0) return true;
    if (report.nvmePercentageUsed && *report.nvmePercentageUsed > 90) return true;

    for (const auto& attr : report.attributes) {
        if (attr.failed) return true;
    }

    return false;
}

} // namespace dm
