#include "util/Error.hpp"

namespace personalfs::util {

Error::Error(const std::string& message)
    : std::runtime_error(message)
{
}

} // namespace personalfs::util

