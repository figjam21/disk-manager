#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include "dm/command.hpp"

namespace dm {

struct RescueProgress {
    uint64_t totalBytes = 0;
    uint64_t rescuedBytes = 0;
    uint64_t errorBytes = 0;
    int errorCount = 0;
    double percent = 0.0;
    std::string currentStatus; // ddrescue's single-letter status, spelled out
    bool finished = false;
};

// Wraps ddrescue for imaging a failing block device (works on a single disk, a
// partition, or a whole /dev/mdX RAID array -- ddrescue only cares that it's a
// block device to read from) to a destination image file or device, and testdisk /
// photorec for partition-table and file-level recovery afterwards.
class RecoveryManager {
public:
    bool ddrescueAvailable();
    bool testdiskAvailable();  // also implies photorec, same package
    bool ddrescueguiAvailable();

    // Runs ddrescue in the foreground of the calling thread (intended to be called
    // from a worker thread by the UI layer), reporting progress by polling
    // mapFile on a timer rather than screen-scraping ddrescue's redraw-in-place
    // output. mapFile is created if it doesn't exist and makes the run resumable if
    // interrupted -- callers should always pass a persistent path for it, never a
    // temp file, so a failing-drive imaging session can be resumed later.
    // Returns ddrescue's exit code, or -1 if cancelled via *cancel.
    int rescue(const std::string& source, const std::string& destImage, const std::string& mapFile,
               const std::function<void(const RescueProgress&)>& onProgress,
               std::atomic<bool>* cancel);

    // These tools have their own full-screen (n)curses UI and expect a real
    // controlling terminal, so this just execs them directly rather than trying to
    // wrap their output -- the caller (TUI) should suspend its own screen first via
    // endwin()/refresh(), and the GUI should run them in a spawned terminal emulator.
    CommandResult launchTestdisk(const std::string& target);
    CommandResult launchPhotorec(const std::string& target, const std::string& outputDir);
};

} // namespace dm
