#include "dm/partition.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace dm {

namespace {

// Splits `s` on `delim`, keeping empty fields (so column positions line up
// with parted's machine-readable output even when trailing fields are
// missing).
std::vector<std::string> splitFields(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Strips a trailing ';' (parted terminates every record line with one).
std::string stripSemicolon(const std::string& s) {
    if (!s.empty() && s.back() == ';') return s.substr(0, s.size() - 1);
    return s;
}

// Strips a trailing 'B' unit suffix (e.g. "1048576B" -> "1048576").
std::string stripByteSuffix(const std::string& s) {
    if (!s.empty() && (s.back() == 'B' || s.back() == 'b')) return s.substr(0, s.size() - 1);
    return s;
}

uint64_t parseBytes(const std::string& field) {
    std::string stripped = stripByteSuffix(field);
    if (stripped.empty()) return 0;
    try {
        return std::stoull(stripped);
    } catch (...) {
        return 0;
    }
}

// Builds the partition device node path for partition `number` of `diskPath`,
// respecting the sdX vs nvmeXnY/mmcblkX naming convention split.
std::string partitionPath(const std::string& diskPath, const std::string& number) {
    if (!diskPath.empty() && std::isdigit(static_cast<unsigned char>(diskPath.back()))) {
        return diskPath + "p" + number;
    }
    return diskPath + number;
}

} // namespace

bool PartitionManager::partedAvailable() {
    return commandExists("parted");
}

PartitionTable PartitionManager::read(const std::string& diskPath) {
    PartitionTable table;
    table.device = diskPath;

    CommandResult result = run({"parted", "-s", "-m", diskPath, "unit", "B", "print", "free"});

    std::istringstream stream(result.stdOut);
    std::string line;
    bool sawSummary = false;

    while (std::getline(stream, line)) {
        // Lines may be terminated with '\r\n'; trim any stray '\r'.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line == "BYT;") continue; // unit marker, always first line

        std::string body = stripSemicolon(line);
        std::vector<std::string> fields = splitFields(body, ':');

        if (!sawSummary) {
            // Disk summary line:
            // path:sizeB:transport:logicalSectorSize:physicalSectorSize:label:model:flags
            sawSummary = true;
            if (fields.size() > 0) table.device = fields[0];
            if (fields.size() > 1) table.sizeBytes = parseBytes(fields[1]);
            if (fields.size() > 5) table.label = fields[5];
            continue;
        }

        // Partition-or-freespace line:
        // number:startB:endB:sizeB:fstype:name:flags
        std::string number = fields.size() > 0 ? fields[0] : "";
        std::string fsType = fields.size() > 4 ? fields[4] : "";
        if (fsType == "free") continue; // free space, not a real partition

        PartitionEntry entry;
        entry.number = 0;
        if (!number.empty()) {
            try {
                entry.number = std::stoi(number);
            } catch (...) {
                entry.number = 0;
            }
        }
        entry.startBytes = fields.size() > 1 ? parseBytes(fields[1]) : 0;
        entry.endBytes = fields.size() > 2 ? parseBytes(fields[2]) : 0;
        entry.sizeBytes = fields.size() > 3 ? parseBytes(fields[3]) : 0;
        entry.fsType = fsType;
        entry.name = fields.size() > 5 ? fields[5] : "";
        entry.flags = fields.size() > 6 ? fields[6] : "";
        entry.path = partitionPath(diskPath, number);

        table.partitions.push_back(std::move(entry));
    }

    return table;
}

CommandResult PartitionManager::createTable(const std::string& diskPath, const std::string& label) {
    return run({"parted", "-s", diskPath, "mklabel", label});
}

CommandResult PartitionManager::createPartition(const std::string& diskPath, const std::string& fsType,
                                                 uint64_t startBytes, uint64_t endBytes) {
    std::string end = (endBytes == 0) ? "100%" : (std::to_string(endBytes) + "B");
    std::vector<std::string> argv = {"parted", "-s", diskPath, "unit", "B", "mkpart", "primary"};
    if (!fsType.empty()) argv.push_back(fsType);
    argv.push_back(std::to_string(startBytes) + "B");
    argv.push_back(end);
    return run(argv);
}

CommandResult PartitionManager::deletePartition(const std::string& diskPath, int number) {
    return run({"parted", "-s", diskPath, "rm", std::to_string(number)});
}

CommandResult PartitionManager::resizePartition(const std::string& diskPath, int number, uint64_t newEndBytes) {
    return run({"parted", "-s", diskPath, "resizepart", std::to_string(number), std::to_string(newEndBytes) + "B"});
}

CommandResult PartitionManager::setFlag(const std::string& diskPath, int number, const std::string& flag, bool on) {
    return run({"parted", "-s", diskPath, "set", std::to_string(number), flag, on ? "on" : "off"});
}

} // namespace dm
