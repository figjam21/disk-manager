#include "dm/mdadm.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace dm {

namespace {

std::string trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

// Parses the leading run of digits (ignoring leading spaces) in `s` as a uint64_t.
uint64_t parseLeadingUInt(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    size_t start = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    if (i == start) {
        return 0;
    }
    return std::stoull(s.substr(start, i - start));
}

int parseIntSafe(const std::string& s) {
    try {
        return std::stoi(trim(s));
    } catch (...) {
        return 0;
    }
}

// Extracts the "mdN" array name from a /proc/mdstat line if it starts one,
// e.g. "md0 : active raid1 sda1[0] sdb1[1]" -> "md0".
std::optional<std::string> extractArrayName(const std::string& line) {
    static const std::regex re(R"(^(md\d+)\s*:)");
    std::smatch m;
    if (std::regex_search(line, m, re)) {
        return m[1].str();
    }
    return std::nullopt;
}

} // namespace

bool MdadmManager::available() {
    return dm::commandExists("mdadm");
}

std::vector<RaidArray> MdadmManager::listArrays() {
    std::vector<RaidArray> result;
    std::ifstream in("/proc/mdstat");
    if (!in.is_open()) {
        return result;
    }
    std::string line;
    while (std::getline(in, line)) {
        auto name = extractArrayName(line);
        if (!name) {
            continue;
        }
        std::string mdPath = "/dev/" + *name;
        auto d = detail(mdPath);
        if (d) {
            result.push_back(*d);
        }
    }
    return result;
}

std::optional<RaidArray> MdadmManager::detail(const std::string& mdPath) {
    CommandResult res = run({"mdadm", "--detail", mdPath});
    if (!res.ok()) {
        return std::nullopt;
    }

    RaidArray arr;
    arr.device = mdPath;

    std::istringstream iss(res.stdOut);
    std::string line;
    bool inTable = false;
    while (std::getline(iss, line)) {
        // Strip trailing carriage return, if present.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!inTable && line.find("Number") != std::string::npos &&
            line.find("RaidDevice") != std::string::npos &&
            line.find("State") != std::string::npos) {
            inTable = true;
            continue;
        }

        if (inTable) {
            std::string trimmed = trim(line);
            if (trimmed.empty()) {
                continue;
            }
            std::vector<std::string> tokens = splitWs(line);
            if (tokens.size() < 4) {
                continue;
            }
            RaidMember member;
            try {
                member.number = std::stoi(tokens[0]);
                member.raidDevice = std::stoi(tokens[3]);
            } catch (...) {
                continue;
            }
            size_t stateEnd = tokens.size();
            if (stateEnd > 4 && tokens.back().rfind("/dev/", 0) == 0) {
                member.device = tokens.back();
                stateEnd = tokens.size() - 1;
            }
            std::string state;
            for (size_t i = 4; i < stateEnd; ++i) {
                if (!state.empty()) {
                    state += ' ';
                }
                state += tokens[i];
            }
            member.state = state;
            arr.members.push_back(member);
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));

        if (key == "Raid Level") {
            arr.level = value;
        } else if (key == "State") {
            arr.state = value;
        } else if (key == "UUID") {
            arr.uuid = value;
        } else if (key == "Active Devices") {
            arr.activeDevices = parseIntSafe(value);
        } else if (key == "Working Devices") {
            arr.workingDevices = parseIntSafe(value);
        } else if (key == "Failed Devices") {
            arr.failedDevices = parseIntSafe(value);
        } else if (key == "Spare Devices") {
            arr.spareDevices = parseIntSafe(value);
        } else if (key == "Raid Devices") {
            arr.raidDevices = parseIntSafe(value);
        } else if (key == "Total Devices") {
            arr.totalDevices = parseIntSafe(value);
        } else if (key == "Array Size") {
            uint64_t kib = parseLeadingUInt(value);
            arr.arraySizeBytes = kib * 1024ULL;
        }
    }

    arr.degraded = arr.failedDevices > 0 || arr.activeDevices < arr.raidDevices;
    return arr;
}

CommandResult MdadmManager::createArray(const std::string& mdPath, const std::string& level,
                                         const std::vector<std::string>& devices,
                                         const std::vector<std::string>& spares) {
    std::vector<std::string> argv = {"mdadm", "--create", mdPath, "--level=" + level,
                                      "--raid-devices=" + std::to_string(devices.size())};
    if (!spares.empty()) {
        argv.push_back("--spare-devices=" + std::to_string(spares.size()));
    }
    argv.push_back("--run");
    for (const auto& d : devices) {
        argv.push_back(d);
    }
    for (const auto& s : spares) {
        argv.push_back(s);
    }
    return run(argv);
}

CommandResult MdadmManager::assembleArray(const std::string& mdPath,
                                           const std::vector<std::string>& devices) {
    std::vector<std::string> argv = {"mdadm", "--assemble", mdPath};
    if (devices.empty()) {
        argv.push_back("--scan");
    } else {
        for (const auto& d : devices) {
            argv.push_back(d);
        }
    }
    return run(argv);
}

CommandResult MdadmManager::stopArray(const std::string& mdPath) {
    return run({"mdadm", "--stop", mdPath});
}

CommandResult MdadmManager::addDevice(const std::string& mdPath, const std::string& devicePath) {
    return run({"mdadm", mdPath, "--add", devicePath});
}

CommandResult MdadmManager::removeDevice(const std::string& mdPath, const std::string& devicePath) {
    return run({"mdadm", mdPath, "--remove", devicePath});
}

CommandResult MdadmManager::failDevice(const std::string& mdPath, const std::string& devicePath) {
    return run({"mdadm", mdPath, "--fail", devicePath});
}

CommandResult MdadmManager::replaceDevice(const std::string& mdPath, const std::string& oldDevicePath,
                                           const std::string& newDevicePath) {
    // Best-effort: the device may already be marked faulty, in which case --fail
    // itself fails even though we're already in the desired state. Ignore its result.
    failDevice(mdPath, oldDevicePath);

    CommandResult removeRes = removeDevice(mdPath, oldDevicePath);
    if (!removeRes.ok()) {
        return removeRes;
    }

    return addDevice(mdPath, newDevicePath);
}

CommandResult MdadmManager::growRaidDevices(const std::string& mdPath, int newRaidDevices) {
    return run({"mdadm", "--grow", mdPath, "--raid-devices=" + std::to_string(newRaidDevices)});
}

CommandResult MdadmManager::growSize(const std::string& mdPath, const std::string& size) {
    return run({"mdadm", "--grow", mdPath, "--size=" + size});
}

RebuildStatus MdadmManager::rebuildStatus(const std::string& mdPath) {
    RebuildStatus status;

    std::string name = mdPath;
    const std::string prefix = "/dev/";
    if (name.rfind(prefix, 0) == 0) {
        name = name.substr(prefix.size());
    }

    std::ifstream in("/proc/mdstat");
    if (!in.is_open()) {
        return status;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }

    const std::regex headerRe("^" + name + R"(\s*:)");
    static const std::regex progressRe(R"((resync|recovery|reshape|check)\s*=\s*([0-9]+(?:\.[0-9]+)?)%)");
    static const std::regex finishRe(R"(finish=(\S+))");
    static const std::regex speedRe(R"(speed=(\S+))");

    for (size_t i = 0; i < lines.size(); ++i) {
        if (!std::regex_search(lines[i], headerRe)) {
            continue;
        }
        size_t limit = std::min(lines.size(), i + 4);
        for (size_t j = i + 1; j < limit; ++j) {
            std::smatch m;
            if (std::regex_search(lines[j], m, progressRe)) {
                status.active = true;
                status.operation = m[1].str();
                status.percent = std::stod(m[2].str());
                std::smatch fm;
                if (std::regex_search(lines[j], fm, finishRe)) {
                    status.timeRemaining = fm[1].str();
                }
                std::smatch sm;
                if (std::regex_search(lines[j], sm, speedRe)) {
                    status.speed = sm[1].str();
                }
                return status;
            }
        }
        break;
    }

    return status;
}

} // namespace dm
