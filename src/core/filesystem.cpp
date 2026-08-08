#include "dm/filesystem.hpp"
#include "dm/command.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>

namespace dm {

namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// /proc/mounts (and /etc/mtab) escape whitespace/backslash in paths using
// octal escapes: \040 space, \011 tab, \012 newline, \134 backslash.
std::string unescapeOctal(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i + 1] >= '0' && s[i + 1] <= '7' &&
            s[i + 2] >= '0' && s[i + 2] <= '7' &&
            s[i + 3] >= '0' && s[i + 3] <= '7') {
            int value = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out.push_back(static_cast<char>(value));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Returns the mkfs binary name and whether it is supported.
bool mkfsBinaryFor(const std::string& fsType, std::string& binaryOut) {
    if (fsType == "ext2") { binaryOut = "mkfs.ext2"; return true; }
    if (fsType == "ext3") { binaryOut = "mkfs.ext3"; return true; }
    if (fsType == "ext4") { binaryOut = "mkfs.ext4"; return true; }
    if (fsType == "xfs") { binaryOut = "mkfs.xfs"; return true; }
    if (fsType == "btrfs") { binaryOut = "mkfs.btrfs"; return true; }
    if (fsType == "vfat" || fsType == "fat32" || fsType == "fat") { binaryOut = "mkfs.vfat"; return true; }
    if (fsType == "ntfs") { binaryOut = "mkfs.ntfs"; return true; }
    return false;
}

CommandResult makeError(const std::string& message) {
    CommandResult res;
    res.exitCode = -1;
    res.stdErr = message;
    return res;
}

// Resolves a fstab "spec" column (raw device path, "UUID=...", or "LABEL=...")
// to a device path for comparison purposes. Returns "" if it can't be resolved.
std::string resolveSpec(const std::string& spec) {
    const std::string uuidPrefix = "UUID=";
    const std::string labelPrefix = "LABEL=";
    if (spec.rfind(uuidPrefix, 0) == 0) {
        std::string uuid = spec.substr(uuidPrefix.size());
        CommandResult res = run({"blkid", "-U", uuid});
        if (res.ok()) return trim(res.stdOut);
        return "";
    }
    if (spec.rfind(labelPrefix, 0) == 0) {
        std::string label = spec.substr(labelPrefix.size());
        CommandResult res = run({"blkid", "-L", label});
        if (res.ok()) return trim(res.stdOut);
        return "";
    }
    return spec;
}

bool specMatches(const std::string& spec, const std::string& devicePath) {
    if (spec == devicePath) return true;
    const std::string uuidPrefix = "UUID=";
    const std::string labelPrefix = "LABEL=";
    if (spec.rfind(uuidPrefix, 0) == 0 || spec.rfind(labelPrefix, 0) == 0) {
        std::string resolved = resolveSpec(spec);
        return !resolved.empty() && resolved == devicePath;
    }
    return false;
}

// Copies /etc/fstab to /etc/fstab.bak.<epoch-seconds>. Returns "" on success,
// or an error message on failure.
std::string backupFstab() {
    std::error_code ec;
    std::string backupPath = "/etc/fstab.bak." + std::to_string(static_cast<long long>(std::time(nullptr)));
    fs::copy_file("/etc/fstab", backupPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return "failed to back up /etc/fstab to " + backupPath + ": " + ec.message();
    }
    return "";
}

} // namespace

CommandResult FilesystemManager::makeFilesystem(const std::string& devicePath, const std::string& fsType,
                                                 const std::string& label) {
    std::string binary;
    if (!mkfsBinaryFor(fsType, binary)) {
        return makeError("unsupported filesystem type: " + fsType);
    }

    std::vector<std::string> argv = {binary};

    // Force flags so re-formatting an already-formatted device doesn't hang on
    // an interactive confirmation prompt.
    if (fsType == "ext2" || fsType == "ext3" || fsType == "ext4" ||
        fsType == "xfs" || fsType == "btrfs") {
        argv.push_back("-f");
    }

    if (!label.empty()) {
        if (fsType == "vfat" || fsType == "fat32" || fsType == "fat") {
            argv.push_back("-n");
            argv.push_back(label);
        } else {
            // ext2/ext3/ext4/xfs/btrfs/ntfs
            argv.push_back("-L");
            argv.push_back(label);
        }
    }

    argv.push_back(devicePath);

    return run(argv);
}

CommandResult FilesystemManager::mount(const std::string& devicePath, const std::string& mountpoint,
                                        const std::string& fsType, const std::string& options) {
    std::error_code ec;
    fs::create_directories(mountpoint, ec);
    if (ec) {
        return makeError("failed to create mountpoint " + mountpoint + ": " + ec.message());
    }

    std::vector<std::string> argv = {"mount"};
    if (!fsType.empty()) {
        argv.push_back("-t");
        argv.push_back(fsType);
    }
    if (!options.empty()) {
        argv.push_back("-o");
        argv.push_back(options);
    }
    argv.push_back(devicePath);
    argv.push_back(mountpoint);

    return run(argv);
}

CommandResult FilesystemManager::unmount(const std::string& mountpointOrDevice) {
    return run({"umount", mountpointOrDevice});
}

CommandResult FilesystemManager::resize(const std::string& devicePath, const std::string& fsType,
                                         const std::string& mountpoint) {
    if (fsType == "ext2" || fsType == "ext3" || fsType == "ext4") {
        return run({"resize2fs", devicePath});
    }
    if (fsType == "xfs") {
        if (mountpoint.empty()) {
            return makeError("xfs_growfs requires the filesystem to be mounted; no mountpoint given");
        }
        return run({"xfs_growfs", mountpoint});
    }
    if (fsType == "btrfs") {
        if (mountpoint.empty()) {
            return makeError("btrfs resize requires a mountpoint; none given");
        }
        return run({"btrfs", "filesystem", "resize", "max", mountpoint});
    }
    return makeError("unsupported filesystem for resize: " + fsType);
}

std::vector<MountEntry> FilesystemManager::listMounts() {
    std::vector<MountEntry> result;

    std::ifstream in("/proc/mounts");
    if (!in.is_open()) {
        return result;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string source, target, fsType, options, dump, pass;
        if (!(iss >> source >> target >> fsType >> options)) {
            continue;
        }
        iss >> dump >> pass; // optional, unused

        MountEntry entry;
        entry.source = unescapeOctal(source);
        entry.target = unescapeOctal(target);
        entry.fsType = fsType;
        entry.options = options;
        result.push_back(std::move(entry));
    }

    return result;
}

std::vector<FstabEntry> FilesystemManager::readFstab() {
    std::vector<FstabEntry> result;

    std::ifstream in("/etc/fstab");
    if (!in.is_open()) {
        return result;
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        std::istringstream iss(trimmed);
        std::string spec, mountpoint, fsType, options, dumpStr, passStr;
        if (!(iss >> spec >> mountpoint >> fsType >> options)) {
            // Malformed line with fewer than 4 columns; skip.
            continue;
        }

        FstabEntry entry;
        entry.spec = spec;
        entry.mountpoint = mountpoint;
        entry.fsType = fsType;
        entry.options = options;
        entry.dump = 0;
        entry.pass = 0;
        if (iss >> dumpStr) {
            try { entry.dump = std::stoi(dumpStr); } catch (...) { entry.dump = 0; }
        }
        if (iss >> passStr) {
            try { entry.pass = std::stoi(passStr); } catch (...) { entry.pass = 0; }
        }

        result.push_back(std::move(entry));
    }

    return result;
}

bool FilesystemManager::isInFstab(const std::string& devicePath) {
    for (const auto& entry : readFstab()) {
        if (specMatches(entry.spec, devicePath)) {
            return true;
        }
    }
    return false;
}

CommandResult FilesystemManager::addToFstab(const std::string& devicePath, const std::string& mountpoint,
                                             const std::string& fsType, const std::string& options,
                                             int dump, int pass) {
    std::string spec = devicePath;
    CommandResult blkidRes = run({"blkid", "-s", "UUID", "-o", "value", devicePath});
    if (blkidRes.ok()) {
        std::string uuid = trim(blkidRes.stdOut);
        if (!uuid.empty()) {
            spec = "UUID=" + uuid;
        }
    }

    std::string backupError = backupFstab();
    if (!backupError.empty()) {
        return makeError(backupError);
    }

    std::ofstream out("/etc/fstab", std::ios::app);
    if (!out.is_open()) {
        return makeError("failed to open /etc/fstab for writing (are you root?)");
    }

    out << spec << '\t' << mountpoint << '\t' << fsType << '\t' << options << '\t'
        << dump << '\t' << pass << '\n';

    if (!out) {
        return makeError("failed to write entry to /etc/fstab (are you root?)");
    }

    CommandResult result;
    result.exitCode = 0;
    return result;
}

CommandResult FilesystemManager::removeFromFstab(const std::string& devicePathOrMountpoint) {
    std::ifstream in("/etc/fstab");
    if (!in.is_open()) {
        return makeError("failed to read /etc/fstab");
    }

    std::vector<std::string> keptLines;
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            keptLines.push_back(line);
            continue;
        }

        std::istringstream iss(trimmed);
        std::string spec, mountpoint, fsType, options, dumpStr, passStr;
        if (!(iss >> spec >> mountpoint >> fsType >> options)) {
            // Malformed line; preserve untouched.
            keptLines.push_back(line);
            continue;
        }

        bool matches = specMatches(spec, devicePathOrMountpoint) ||
                       mountpoint == devicePathOrMountpoint;
        if (!matches) {
            keptLines.push_back(line);
        }
    }
    in.close();

    std::string backupError = backupFstab();
    if (!backupError.empty()) {
        return makeError(backupError);
    }

    std::ofstream out("/etc/fstab", std::ios::trunc);
    if (!out.is_open()) {
        return makeError("failed to open /etc/fstab for writing (are you root?)");
    }

    for (const auto& kept : keptLines) {
        out << kept << '\n';
    }

    if (!out) {
        return makeError("failed to write /etc/fstab (are you root?)");
    }

    CommandResult result;
    result.exitCode = 0;
    return result;
}

} // namespace dm
