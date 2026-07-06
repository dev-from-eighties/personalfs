#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace personalfs::app {

enum class ResizeMode {
    Static,
    Fibonacci,
};

struct Config {
    std::filesystem::path storageRoot{"pfs-data"};
    std::string imageName{"personal.img"};
    std::string runtimeMountName{"mounted-img"};

    std::uint64_t quotaSize{100ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t initialSize{1ULL * 1024ULL * 1024ULL * 1024ULL};
    ResizeMode resizeMode{ResizeMode::Fibonacci};
    std::uint64_t staticStep{1ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t fibonacciMaxStep{32ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t growFreeThreshold{256ULL * 1024ULL * 1024ULL};

    bool adaptiveShrink{true};
    bool shrinkOnUnmount{true};
    std::uint64_t shrinkFreeThreshold{2ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t shrinkMinReclaim{1ULL * 1024ULL * 1024ULL * 1024ULL};
};

Config loadConfig(const std::filesystem::path& path);
void saveConfig(const std::filesystem::path& path, const Config& config);
std::uint64_t parseSize(const std::string& value);
std::string formatSize(std::uint64_t bytes);
ResizeMode parseResizeMode(const std::string& value);
std::string formatResizeMode(ResizeMode mode);

} // namespace personalfs::app
