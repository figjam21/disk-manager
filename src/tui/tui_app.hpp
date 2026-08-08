#pragma once
#include "dm/device.hpp"
#include "dm/smart.hpp"
#include "dm/partition.hpp"
#include "dm/filesystem.hpp"
#include "dm/mdadm.hpp"
#include "dm/locate.hpp"
#include "dm/firmware.hpp"
#include "dm/recovery.hpp"

namespace dm {

class TuiApp {
public:
    void run();

private:
    DeviceManager devices_;
    SmartManager smart_;
    PartitionManager partitions_;
    FilesystemManager filesystems_;
    MdadmManager mdadm_;
    LocateManager locate_;
    FirmwareManager firmware_;
    RecoveryManager recovery_;

    void screenDisks();
    void screenDiskDetail(const std::string& diskPath);
    void screenSmartDetail(const std::string& diskPath);
    void screenPartitions(const std::string& diskPath);
    void screenCreatePartition(const std::string& diskPath);
    void screenMountManage(const BlockDevice& dev);

    void screenArrays();
    void screenArrayDetail(const std::string& mdPath);
    void screenAssembleDiscovered(const DiscoveredArray& disc);
    void screenCreateArray();
    void screenReplaceDrive(const std::string& mdPath);
    void screenGrowArray(const std::string& mdPath);

    void screenFirmware();
    void screenRecovery();
};

} // namespace dm
