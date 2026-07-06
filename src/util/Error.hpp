#pragma once

#include <stdexcept>
#include <string>

namespace personalfs::util {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message);
};

} // namespace personalfs::util

