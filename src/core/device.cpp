#include "dm/device.hpp"
#include "dm/command.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>

namespace dm {

namespace {

using json = nlohmann::json;

std::string jsonToString(const json& j) {
    if (j.is_null()) return "";
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>() ? "1" : "0";
    if (j.is_number_integer()) return std::to_string(j.get<long long>());
    if (j.is_number_unsigned()) return std::to_string(j.get<unsigned long long>());
    if (j.is_number_float()) return std::to_string(j.get<double>());
    return "";
}

uint64_t jsonToU64(const json& j) {
    if (j.is_null()) return 0;
    if (j.is_number_unsigned()) return j.get<uint64_t>();
    if (j.is_number_integer()) {
        long long v = j.get<long long>();
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    }
    if (j.is_number_float()) {
        double v = j.get<double>();
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    }
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        if (s.empty()) return 0;
        try {
            return static_cast<uint64_t>(std::stoull(s));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

bool jsonToBool(const json& j) {
    if (j.is_null()) return false;
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number_integer()) return j.get<long long>() != 0;
    if (j.is_number_unsigned()) return j.get<unsigned long long>() != 0;
    if (j.is_number_float()) return j.get<double>() != 0.0;
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        return s == "1" || s == "true" || s == "yes";
    }
    return false;
}

std::string getStr(const json& node, const char* key) {
    if (!node.contains(key)) return "";
    return jsonToString(node.at(key));
}

// Recursively walk an lsblk device node (and its "children"), appending a
// flat BlockDevice for every disk/partition/etc. encountered. Whole-disk
// entries get their childPaths populated with the direct children's paths.
void walk(const json& node, std::vector<BlockDevice>& out) {
    BlockDevice dev;
    dev.name = getStr(node, "name");
    dev.path = getStr(node, "path");
    dev.kname = getStr(node, "kname");
    dev.pkname = getStr(node, "pkname");
    dev.type = getStr(node, "type");
    dev.model = getStr(node, "model");
    dev.serial = getStr(node, "serial");
    dev.transport = getStr(node, "tran");
    dev.fsType = getStr(node, "fstype");
    dev.uuid = getStr(node, "uuid");
    dev.label = getStr(node, "label");
    dev.mountpoint = getStr(node, "mountpoint");
    if (node.contains("size")) dev.sizeBytes = jsonToU64(node.at("size"));
    if (node.contains("rota")) dev.rotational = jsonToBool(node.at("rota"));
    if (node.contains("rm")) dev.removable = jsonToBool(node.at("rm"));

    if (node.contains("children") && node.at("children").is_array()) {
        for (const auto& child : node.at("children")) {
            dev.childPaths.push_back(getStr(child, "path"));
        }
    }

    out.push_back(std::move(dev));

    if (node.contains("children") && node.at("children").is_array()) {
        for (const auto& child : node.at("children")) {
            walk(child, out);
        }
    }
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string basename(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

} // namespace

std::vector<BlockDevice> DeviceManager::listAll() {
    std::vector<BlockDevice> result;

    CommandResult res = run({"lsblk", "-J", "-b", "-o",
                              "NAME,PATH,KNAME,PKNAME,TYPE,MODEL,SERIAL,TRAN,ROTA,"
                              "FSTYPE,UUID,LABEL,MOUNTPOINT,SIZE,RM"});
    if (!res.ok()) {
        return result;
    }

    json root;
    try {
        root = json::parse(res.stdOut);
    } catch (...) {
        return result;
    }

    if (!root.contains("blockdevices") || !root.at("blockdevices").is_array()) {
        return result;
    }

    for (const auto& node : root.at("blockdevices")) {
        walk(node, result);
    }

    return result;
}

std::vector<BlockDevice> DeviceManager::listDisks() {
    std::vector<BlockDevice> all = listAll();
    std::vector<BlockDevice> disks;
    for (auto& dev : all) {
        if (dev.type == "disk") {
            disks.push_back(std::move(dev));
        }
    }
    return disks;
}

std::optional<BlockDevice> DeviceManager::find(const std::string& path) {
    std::vector<BlockDevice> all = listAll();
    for (auto& dev : all) {
        if (dev.path == path) {
            return dev;
        }
    }
    return std::nullopt;
}

std::string DeviceManager::rootDiskName() {
    CommandResult findmntRes = run({"findmnt", "-no", "SOURCE", "/"});
    if (!findmntRes.ok()) {
        return "";
    }

    std::string source = trim(findmntRes.stdOut);
    if (source.empty()) {
        return "";
    }

    CommandResult lsblkRes = run({"lsblk", "-no", "PKNAME", source});
    if (lsblkRes.ok()) {
        std::string pkname = trim(lsblkRes.stdOut);
        if (!pkname.empty()) {
            return pkname;
        }
    }

    return basename(source);
}

} // namespace dm
