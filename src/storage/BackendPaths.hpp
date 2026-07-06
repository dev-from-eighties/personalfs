#pragma once

#include "app/Config.hpp"

#include <filesystem>

namespace personalfs::storage {

struct BackendPaths {
    std::filesystem::path workdir;
    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path image;
    std::filesystem::path internalMount;
};

BackendPaths makeBackendPaths(const std::filesystem::path& workdir, const app::Config& config);

} // namespace personalfs::storage

