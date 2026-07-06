#include "util/Logger.hpp"

#include <iostream>

namespace personalfs::util {

void Logger::info(const std::string& message)
{
    std::cout << message << '\n';
}

void Logger::error(const std::string& message)
{
    std::cerr << message << '\n';
}

} // namespace personalfs::util

