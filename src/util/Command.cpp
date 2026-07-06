#include "util/Command.hpp"

#include "util/Error.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace personalfs::util {

CommandResult runCommand(const std::vector<std::string>& args, bool captureOutput)
{
    if (args.empty()) {
        throw Error("empty command");
    }

    int pipeFd[2] = {-1, -1};
    if (captureOutput && pipe(pipeFd) != 0) {
        throw Error(std::string{"pipe failed: "} + std::strerror(errno));
    }

    const pid_t pid = fork();
    if (pid < 0) {
        if (pipeFd[0] >= 0) {
            close(pipeFd[0]);
            close(pipeFd[1]);
        }
        throw Error(std::string{"fork failed: "} + std::strerror(errno));
    }

    if (pid == 0) {
        if (captureOutput) {
            close(pipeFd[0]);
            dup2(pipeFd[1], STDOUT_FILENO);
            dup2(pipeFd[1], STDERR_FILENO);
            close(pipeFd[1]);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        std::fprintf(stderr, "execvp failed for %s: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }

    std::string output;
    if (captureOutput) {
        close(pipeFd[1]);
        char buffer[4096];
        for (;;) {
            const ssize_t count = read(pipeFd[0], buffer, sizeof(buffer));
            if (count > 0) {
                output.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            close(pipeFd[0]);
            throw Error(std::string{"read command output failed: "} + std::strerror(errno));
        }
        close(pipeFd[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw Error(std::string{"waitpid failed: "} + std::strerror(errno));
        }
    }

    if (WIFEXITED(status)) {
        return {.exitCode = WEXITSTATUS(status), .output = output};
    }
    if (WIFSIGNALED(status)) {
        return {.exitCode = 128 + WTERMSIG(status), .output = output};
    }
    return {.exitCode = 1, .output = output};
}

} // namespace personalfs::util
