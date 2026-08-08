#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace dm {

// Result of a completed (argv-exec'd, never shell-interpolated) external command.
struct CommandResult {
    int exitCode = -1;
    std::string stdOut;
    std::string stdErr;
    bool ok() const { return exitCode == 0; }
};

// Runs argv[0] with argv[1:] as literal arguments (fork/execvp, no shell), captures
// combined output. Safe against injection since arguments are never concatenated
// into a shell string.
CommandResult run(const std::vector<std::string>& argv);

// Like run(), but invokes onLine(text) for every line (delimited by '\n' or '\r',
// since progress tools like ddrescue/dd rewrite the current line with '\r') as it is
// produced, rather than waiting for exit. Blocks the calling thread until the child
// exits or *cancel is set to true, in which case the child is SIGTERM'd. Returns the
// child's exit code, or -1 if it was cancelled/could not be started.
int runStreaming(const std::vector<std::string>& argv,
                  const std::function<void(const std::string& line)>& onLine,
                  std::atomic<bool>* cancel = nullptr);

// True if `name` resolves via PATH (checked with `which`-equivalent access() probing).
bool commandExists(const std::string& name);

bool isRoot();

} // namespace dm
