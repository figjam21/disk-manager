#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "dm/command.hpp"

namespace dm {

struct PartitionEntry {
    int number = 0;
    std::string path;      // e.g. "/dev/sda1"
    uint64_t startBytes = 0;
    uint64_t endBytes = 0;
    uint64_t sizeBytes = 0;
    std::string fsType;
    std::string name;      // GPT partition name, if any
    std::string flags;     // comma-separated, e.g. "boot,esp"
};

struct PartitionTable {
    std::string device;
    std::string label;     // "gpt", "msdos", "loop", or "" if unpartitioned
    uint64_t sizeBytes = 0;
    std::vector<PartitionEntry> partitions;
};

class PartitionManager {
public:
    bool partedAvailable();

    // Parses `parted -s -m <disk> unit B print free`.
    PartitionTable read(const std::string& diskPath);

    // Destructive: replaces the partition table. fsType is used only to pick a
    // sensible default alignment; it does not format anything.
    CommandResult createTable(const std::string& diskPath, const std::string& label /* "gpt" or "msdos" */);

    // startBytes/endBytes are absolute byte offsets into the disk (parted's "unit B"
    // addressing). Pass endBytes == 0 to mean "rest of the disk".
    CommandResult createPartition(const std::string& diskPath, const std::string& fsType,
                                   uint64_t startBytes, uint64_t endBytes);

    CommandResult deletePartition(const std::string& diskPath, int number);

    // Grows or shrinks partition `number` so it ends at newEndBytes. Does not touch
    // the filesystem inside it -- call FilesystemManager::resize afterwards.
    CommandResult resizePartition(const std::string& diskPath, int number, uint64_t newEndBytes);

    CommandResult setFlag(const std::string& diskPath, int number, const std::string& flag, bool on);
};

} // namespace dm
