#pragma once
#include "storage/Store.hpp"
namespace personalfs
{
int mount(Store &store, const std::string &mountpoint, const std::vector<std::string> &options);
}
