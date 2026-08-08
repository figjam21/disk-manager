#include "dm/command.hpp"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <poll.h>
#include <signal.h>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace dm {

namespace {

// Builds a raw argv array (execvp wants char* not const char*) that stays valid for
// the lifetime of the vector<std::string> backing it.
std::vector<char*> toArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    return argv;
}

struct Pipes {
    int outFd[2];
    int errFd[2];
};

} // namespace

bool commandExists(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0;
    const char* pathEnv = getenv("PATH");
    std::string path = pathEnv ? pathEnv : "/usr/sbin:/usr/bin:/sbin:/bin";
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string dir = path.substr(start, end - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return true;
        }
        start = end + 1;
    }
    return false;
}

bool isRoot() { return geteuid() == 0; }

CommandResult run(const std::vector<std::string>& argv) {
    CommandResult result;
    if (argv.empty()) { result.stdErr = "empty argv"; return result; }

    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        result.stdErr = "pipe() failed";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.stdErr = "fork() failed";
        return result;
    }
    if (pid == 0) {
        // Child
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        auto cArgv = toArgv(argv);
        execvp(cArgv[0], cArgv.data());
        _exit(127); // execvp only returns on failure
    }

    // Parent
    close(outPipe[1]);
    close(errPipe[1]);

    struct pollfd fds[2];
    fds[0] = {outPipe[0], POLLIN, 0};
    fds[1] = {errPipe[0], POLLIN, 0};
    bool outOpen = true, errOpen = true;
    char buf[4096];
    while (outOpen || errOpen) {
        fds[0].fd = outOpen ? outPipe[0] : -1;
        fds[1].fd = errOpen ? errPipe[0] : -1;
        int rc = poll(fds, 2, -1);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(outPipe[0], buf, sizeof(buf));
            if (n > 0) result.stdOut.append(buf, n);
            else outOpen = false;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(errPipe[0], buf, sizeof(buf));
            if (n > 0) result.stdErr.append(buf, n);
            else errOpen = false;
        }
    }
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

int runStreaming(const std::vector<std::string>& argv,
                  const std::function<void(const std::string& line)>& onLine,
                  std::atomic<bool>* cancel) {
    if (argv.empty()) return -1;

    int outPipe[2];
    if (pipe(outPipe) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(outPipe[0]); close(outPipe[1]); return -1; }
    if (pid == 0) {
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(outPipe[1], STDERR_FILENO);
        close(outPipe[0]); close(outPipe[1]);
        auto cArgv = toArgv(argv);
        execvp(cArgv[0], cArgv.data());
        _exit(127);
    }

    close(outPipe[1]);
    std::string carry;
    char buf[4096];
    bool killedForCancel = false;
    while (true) {
        if (cancel && cancel->load()) {
            kill(pid, SIGTERM);
            killedForCancel = true;
            break;
        }
        struct pollfd pfd { outPipe[0], POLLIN, 0 };
        int rc = poll(&pfd, 1, 200); // 200ms so we can notice cancellation promptly
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if (rc == 0) continue; // timeout, re-check cancel
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(outPipe[0], buf, sizeof(buf));
            if (n <= 0) break;
            carry.append(buf, n);
            size_t pos;
            while ((pos = carry.find_first_of("\r\n")) != std::string::npos) {
                std::string line = carry.substr(0, pos);
                carry.erase(0, pos + 1);
                if (!line.empty()) onLine(line);
            }
        }
    }
    close(outPipe[0]);
    if (!carry.empty()) onLine(carry);

    int status = 0;
    waitpid(pid, &status, 0);
    if (killedForCancel) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace dm
