#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace dm {

struct SmartAttribute {
    int id = 0;
    std::string name;
    int value = 0;
    int worst = 0;
    int threshold = 0;
    std::string rawValue;
    bool failed = false; // smartctl "when_failed" is non-empty
};

struct SmartReport {
    std::string device;
    bool supported = false;   // device responded to SMART queries at all
    bool healthy = true;      // overall-health self-assessment (PASSED)
    std::string modelFamily;
    std::string firmwareVersion;
    std::optional<int> temperatureC;
    std::optional<uint64_t> powerOnHours;
    std::optional<uint64_t> powerCycles;
    std::optional<uint64_t> reallocatedSectors; // ATA attribute 5
    std::optional<uint64_t> pendingSectors;     // ATA attribute 197
    std::optional<uint64_t> uncorrectableSectors; // ATA attribute 198
    std::optional<uint8_t> nvmePercentageUsed;    // NVMe life-used indicator
    std::optional<uint64_t> nvmeMediaErrors;
    std::vector<SmartAttribute> attributes; // ATA only; empty for NVMe
    std::string error;        // set if smartctl could not be run/parsed
};

class SmartManager {
public:
    bool smartctlAvailable();

    // Runs `smartctl -a -j <devicePath>` and parses both ATA and NVMe report shapes.
    SmartReport query(const std::string& devicePath);

    // Heuristic beyond the device's own PASSED/FAILED verdict: flags drives with any
    // reallocated/pending/uncorrectable sectors, high NVMe percentage-used, or a
    // failed attribute, since many failing consumer drives keep reporting PASSED
    // until very late. This is a heuristic, not a guarantee -- absence of risk here
    // does not mean the drive is healthy, only that these particular signals are clean.
    bool isAtRisk(const SmartReport& report);
};

} // namespace dm
