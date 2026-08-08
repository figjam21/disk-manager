#include "dm/recovery.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace dm {

namespace {

// Parses a GNU ddrescue map file into a RescueProgress snapshot. Returns a
// default-constructed (finished = false) RescueProgress if the file can't be
// opened yet -- ddrescue may not have created it the instant it starts.
RescueProgress parseMapFile(const std::string& mapFile, uint64_t knownTotalBytes) {
    RescueProgress progress;

    std::ifstream in(mapFile);
    if (!in.is_open()) {
        return progress;
    }

    std::string line;
    std::string lastPhaseComment;
    uint64_t sumAllSizes = 0;

    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        if (line[0] == '#') {
            if (line.rfind("# Mapfile.", 0) == 0 || line.rfind("# Command line:", 0) == 0 ||
                line.rfind("# Start time:", 0) == 0 || line.rfind("# Current time:", 0) == 0) {
                continue;
            }
            // Candidate phase-description comment (e.g. "# Copying non-tried
            // blocks... Pass 1 (forwards)", "# Finished", or the column
            // header "#      pos        size  status" -- harmless if that one
            // ends up captured transiently since a later real phase comment,
            // if present, will overwrite it).
            std::string comment = line.substr(1);
            size_t start = comment.find_first_not_of(" \t");
            comment = (start == std::string::npos) ? std::string() : comment.substr(start);
            lastPhaseComment = comment;
            continue;
        }

        std::istringstream iss(line);
        std::string posStr, sizeStr, statusStr;
        if (!(iss >> posStr >> sizeStr >> statusStr)) {
            continue;
        }

        uint64_t size = 0;
        try {
            size = std::stoull(sizeStr, nullptr, 16);
        } catch (...) {
            continue;
        }

        sumAllSizes += size;
        char status = statusStr.empty() ? '?' : statusStr[0];
        if (status == '+') {
            progress.rescuedBytes += size;
        } else if (status == '-') {
            progress.errorBytes += size;
            progress.errorCount += 1;
        }
    }

    progress.currentStatus = lastPhaseComment;
    progress.totalBytes = knownTotalBytes > 0 ? knownTotalBytes : sumAllSizes;
    progress.percent = progress.totalBytes > 0
                            ? (100.0 * static_cast<double>(progress.rescuedBytes) /
                               static_cast<double>(progress.totalBytes))
                            : 0.0;
    return progress;
}

// Runs argv via fork()+execvp() with stdio left untouched (inherited from
// this process), for interactive full-screen (n)curses tools that need a
// real controlling terminal. stdOut/stdErr on the returned CommandResult are
// always empty since nothing is captured -- that's expected here.
CommandResult execInherited(const std::vector<std::string>& argv, const std::string& chdirTo = std::string()) {
    CommandResult result;

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        cargv.push_back(const_cast<char*>(arg.c_str()));
    }
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        result.exitCode = -1;
        return result;
    }

    if (pid == 0) {
        // Child: no dup2 calls, stdio stays inherited from the parent so the
        // curses UI can talk to the real controlling terminal.
        if (!chdirTo.empty() && chdir(chdirTo.c_str()) != 0) {
            _exit(127);
        }
        execvp(cargv[0], cargv.data());
        _exit(127); // execvp only returns on failure
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    } else {
        result.exitCode = -1;
    }
    return result;
}

} // namespace

bool RecoveryManager::ddrescueAvailable() {
    return dm::commandExists("ddrescue");
}

bool RecoveryManager::testdiskAvailable() {
    return dm::commandExists("testdisk");
}

bool RecoveryManager::ddrescueguiAvailable() {
    return dm::commandExists("ddrescueview");
}

int RecoveryManager::rescue(const std::string& source, const std::string& destImage, const std::string& mapFile,
                             const std::function<void(const RescueProgress&)>& onProgress,
                             std::atomic<bool>* cancel) {
    uint64_t totalBytes = 0;
    int fd = open(source.c_str(), O_RDONLY);
    if (fd >= 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz > 0) {
            totalBytes = static_cast<uint64_t>(sz);
        }
        close(fd);
    }

    std::atomic<bool> pollerDone{false};
    std::thread poller([&]() {
        while (!pollerDone.load()) {
            if (onProgress) {
                onProgress(parseMapFile(mapFile, totalBytes));
            }
            // Sleep ~1s total, but in small slices so we notice pollerDone
            // promptly once ddrescue exits instead of over-shooting by up to
            // a full second.
            for (int i = 0; i < 10 && !pollerDone.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    int exitCode = dm::runStreaming({"ddrescue", "-d", "-r3", source, destImage, mapFile},
                                     [](const std::string&) {}, cancel);

    pollerDone = true;
    poller.join();

    if (onProgress) {
        RescueProgress finalProgress = parseMapFile(mapFile, totalBytes);
        finalProgress.finished = true;
        onProgress(finalProgress);
    }

    return exitCode;
}

CommandResult RecoveryManager::launchTestdisk(const std::string& target) {
    std::vector<std::string> argv =
        target.empty() ? std::vector<std::string>{"testdisk"} : std::vector<std::string>{"testdisk", target};
    return execInherited(argv);
}

CommandResult RecoveryManager::launchPhotorec(const std::string& target, const std::string& outputDir) {
    std::vector<std::string> argv =
        target.empty() ? std::vector<std::string>{"photorec"} : std::vector<std::string>{"photorec", target};
    return execInherited(argv, outputDir);
}

} // namespace dm
