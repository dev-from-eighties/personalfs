#pragma once

#include <string>
#include <vector>

namespace personalfs::util {

struct CommandResult {
    int exitCode{0};
    std::string output;
};

CommandResult runCommand(const std::vector<std::string>& args, bool captureOutput = false);

} // namespace personalfs::util

