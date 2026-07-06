#pragma once

#include <string>

namespace personalfs::util {

class Logger {
public:
    static void info(const std::string& message);
    static void error(const std::string& message);
};

} // namespace personalfs::util

