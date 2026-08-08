#include "dm/locate.hpp"
#include "dm/command.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <random>
#include <utility>
#include <vector>

namespace dm {

namespace {
constexpr std::chrono::milliseconds kOnReadSleep{15};
constexpr std::chrono::milliseconds kOffPhaseSleep{400};
constexpr std::chrono::milliseconds kEnclosurePollInterval{100};
constexpr int kReadsPerOnPhase = 6;
constexpr size_t kReadChunkBytes = 4096;
constexpr long long kMaxScanBytes = 200LL * 1024 * 1024;
constexpr long long kFallbackDeviceSize = 100LL * 1024 * 1024;
} // namespace

LocateManager::~LocateManager() {
    stopAll();
}

LocateMethod LocateManager::bestMethodFor(const std::string& devicePath) {
    // We can't cheaply verify that this specific enclosure/slot supports SES
    // without a deeper probe, so the presence of ledctl on PATH is used as a
    // best-effort signal that the enclosure method is available.
    if (dm::commandExists("ledctl")) {
        return LocateMethod::Enclosure;
    }
    if (access(devicePath.c_str(), R_OK) == 0) {
        return LocateMethod::ActivityBlink;
    }
    return LocateMethod::None;
}

bool LocateManager::startEnclosure(const std::string& devicePath) {
    return dm::run({"ledctl", "locate=" + devicePath}).ok();
}

void LocateManager::stopEnclosure(const std::string& devicePath) {
    // Best-effort cleanup; ignore the result.
    dm::run({"ledctl", "locate_off=" + devicePath});
}

void LocateManager::blinkLoop(std::string devicePath, int durationSeconds) {
    // GAP IN include/dm/locate.hpp: blinkLoop() is declared taking only
    // (devicePath, durationSeconds) with no way to receive the per-locate
    // stop flag, even though it must poll it to know when to exit. Since the
    // header can't be modified, we recover the flag here by looking it up in
    // our own active_ map (which already stores it in ActiveLocate::stopFlag)
    // right as this thread starts. This is safe by construction: start()
    // inserts the map entry for devicePath *before* launching this thread,
    // and stop() does not erase that entry until *after* it has joined this
    // very thread -- so the entry, and the stopFlag pointer inside it, are
    // guaranteed to exist for the entire lifetime of this function.
    std::atomic<bool>* stopFlag = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_.find(devicePath);
        if (it != active_.end()) {
            stopFlag = it->second.stopFlag;
        }
    }
    if (stopFlag == nullptr) {
        return;
    }

    int fd = open(devicePath.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }

    long long size = static_cast<long long>(lseek(fd, 0, SEEK_END));
    if (size <= 0) {
        size = kFallbackDeviceSize;
    }
    lseek(fd, 0, SEEK_SET);

    long long scanRange = size < kMaxScanBytes ? size : kMaxScanBytes;
    if (scanRange <= static_cast<long long>(kReadChunkBytes)) {
        scanRange = static_cast<long long>(kReadChunkBytes) + 1;
    }

    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<long long> dist(
        0, scanRange - static_cast<long long>(kReadChunkBytes) - 1);

    std::vector<char> buf(kReadChunkBytes);
    const auto begin = std::chrono::steady_clock::now();

    auto timeUp = [&]() {
        if (durationSeconds <= 0) {
            return false;
        }
        return std::chrono::steady_clock::now() - begin >=
               std::chrono::seconds(durationSeconds);
    };

    while (!stopFlag->load() && !timeUp()) {
        // "On" phase: a handful of small, rapid, read-only reads at random
        // offsets so the drive's stock activity LED flickers.
        for (int i = 0; i < kReadsPerOnPhase && !stopFlag->load(); ++i) {
            long long offset = dist(rng);
            // Read-only; result is intentionally unused (this is purely to
            // create disk activity for the LED, never to write). GCC's
            // warn_unused_result isn't silenced by a (void) cast, so capture
            // it in a named, explicitly-ignored variable instead.
            [[maybe_unused]] ssize_t nread = pread(fd, buf.data(), buf.size(), static_cast<off_t>(offset));
            std::this_thread::sleep_for(kOnReadSleep);
        }

        if (stopFlag->load() || timeUp()) {
            break;
        }

        // "Off" phase: a pause with no I/O so the flicker reads as a
        // recognizable on/off rhythm rather than a solid burst.
        std::this_thread::sleep_for(kOffPhaseSleep);
    }

    close(fd);
}

bool LocateManager::start(const std::string& devicePath, int durationSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_.count(devicePath) > 0) {
        return false;
    }

    LocateMethod method = bestMethodFor(devicePath);
    if (method == LocateMethod::None) {
        return false;
    }

    if (method == LocateMethod::Enclosure) {
        if (startEnclosure(devicePath)) {
            auto* stopFlag = new std::atomic<bool>(false);
            auto [it, inserted] = active_.emplace(
                devicePath, ActiveLocate{stopFlag, std::thread(), LocateMethod::Enclosure});
            (void)inserted;
            it->second.thread = std::thread([this, devicePath, durationSeconds, stopFlag]() {
                const auto begin = std::chrono::steady_clock::now();
                while (!stopFlag->load()) {
                    if (durationSeconds > 0 &&
                        std::chrono::steady_clock::now() - begin >=
                            std::chrono::seconds(durationSeconds)) {
                        break;
                    }
                    std::this_thread::sleep_for(kEnclosurePollInterval);
                }
                // Turn the enclosure LED back off, whether we stopped because
                // of an explicit stop() or because the duration elapsed.
                stopEnclosure(devicePath);
            });
            return true;
        }
        // ledctl exists but this invocation failed -- the tool being present
        // doesn't guarantee this exact drive lives in a supported enclosure.
        // Fall through to the read-only activity-blink fallback below rather
        // than giving up entirely.
    }

    // ActivityBlink: either chosen directly by bestMethodFor(), or reached as
    // a fallback from a failed enclosure locate attempt above.
    if (access(devicePath.c_str(), R_OK) != 0) {
        return false;
    }
    auto* stopFlag = new std::atomic<bool>(false);
    auto [it, inserted] = active_.emplace(
        devicePath, ActiveLocate{stopFlag, std::thread(), LocateMethod::ActivityBlink});
    (void)inserted;
    it->second.thread = std::thread(&LocateManager::blinkLoop, this, devicePath, durationSeconds);
    return true;
}

void LocateManager::stop(const std::string& devicePath) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = active_.find(devicePath);
    if (it == active_.end()) {
        return;
    }

    std::atomic<bool>* stopFlag = it->second.stopFlag;
    *stopFlag = true;
    // Move the thread handle out so we don't need to keep `it` alive (or hold
    // the lock) across the blocking join below.
    std::thread worker = std::move(it->second.thread);
    lock.unlock();

    if (worker.joinable()) {
        worker.join();
    }

    // Only erase (and free the flag) now that the worker has fully exited --
    // this is also what makes the map lookup inside blinkLoop() safe, since
    // the entry is guaranteed to still be present for the worker's entire
    // lifetime.
    lock.lock();
    active_.erase(devicePath);
    lock.unlock();
    delete stopFlag;
}

void LocateManager::stopAll() {
    std::vector<std::string> devices;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : active_) {
            devices.push_back(entry.first);
        }
    }
    for (const auto& devicePath : devices) {
        stop(devicePath);
    }
}

bool LocateManager::isActive(const std::string& devicePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.count(devicePath) > 0;
}

} // namespace dm
