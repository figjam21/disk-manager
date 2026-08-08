#pragma once
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>

namespace dm {

enum class LocateMethod {
    None,           // no way to identify this drive found
    Enclosure,      // per-slot fault/locate LED via ledctl (SES-backed enclosures)
    ActivityBlink,  // fallback: read-only I/O pattern to flicker the activity LED
};

// Helps a human physically find a drive in a chassis: either lights a dedicated
// enclosure "locate" LED (ledctl, on hardware that supports SES), or -- on plain
// SATA/USB drives with no addressable LED -- drives a read-only access pattern that
// makes the drive's activity light flicker in a recognizable on/off rhythm. The
// activity-blink fallback never writes to the device.
class LocateManager {
public:
    ~LocateManager();

    LocateMethod bestMethodFor(const std::string& devicePath);

    // Starts locating devicePath. durationSeconds == 0 means "until stop() is
    // called". Returns false if a locate is already active for this device or no
    // method is available. Non-blocking.
    bool start(const std::string& devicePath, int durationSeconds = 0);

    void stop(const std::string& devicePath);

    void stopAll();

    bool isActive(const std::string& devicePath);

private:
    bool startEnclosure(const std::string& devicePath);
    void stopEnclosure(const std::string& devicePath);
    void blinkLoop(std::string devicePath, int durationSeconds);

    struct ActiveLocate {
        std::atomic<bool>* stopFlag;
        std::thread thread;
        LocateMethod method;
    };
    std::mutex mutex_;
    std::map<std::string, ActiveLocate> active_;
};

} // namespace dm
