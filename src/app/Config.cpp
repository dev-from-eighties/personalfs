#include "app/Config.hpp"

#include "util/Error.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace personalfs::app {

namespace {

std::string trim(std::string value)
{
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); }).base(), value.end());
    return value;
}

std::string unquote(std::string value)
{
    value = trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool parseBool(std::string value)
{
    value = trim(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "true" || value == "1" || value == "yes") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no") {
        return false;
    }
    throw util::Error("invalid boolean value: " + value);
}

void validate(const Config& config)
{
    if (config.quotaSize == 0) {
        throw util::Error("quota_size must be greater than zero");
    }
    if (config.initialSize == 0) {
        throw util::Error("initial_size must be greater than zero");
    }
    if (config.initialSize > config.quotaSize) {
        throw util::Error("initial_size cannot be greater than quota_size");
    }
    if (config.staticStep == 0) {
        throw util::Error("static_step must be greater than zero");
    }
    if (config.fibonacciMaxStep == 0) {
        throw util::Error("fibonacci_max_step must be greater than zero");
    }
    if (config.growFreeThreshold >= config.quotaSize) {
        throw util::Error("grow_free_threshold must be smaller than quota_size");
    }
}

} // namespace

std::uint64_t parseSize(const std::string& rawValue)
{
    std::string value = unquote(rawValue);
    if (value.empty()) {
        throw util::Error("empty size value");
    }

    char suffix = '\0';
    if (!std::isdigit(static_cast<unsigned char>(value.back()))) {
        suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(value.back())));
        value.pop_back();
    }

    value = trim(std::move(value));
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        throw util::Error("invalid size value: " + rawValue);
    }

    const auto number = std::stoull(value);
    std::uint64_t multiplier = 1;
    switch (suffix) {
    case '\0':
    case 'B':
        multiplier = 1;
        break;
    case 'K':
        multiplier = 1024ULL;
        break;
    case 'M':
        multiplier = 1024ULL * 1024ULL;
        break;
    case 'G':
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        break;
    case 'T':
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        break;
    default:
        throw util::Error("unknown size suffix: " + std::string{suffix});
    }
    return number * multiplier;
}

std::string formatSize(std::uint64_t bytes)
{
    constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t mib = 1024ULL * 1024ULL;
    constexpr std::uint64_t kib = 1024ULL;
    if (bytes % gib == 0) {
        return std::to_string(bytes / gib) + "G";
    }
    if (bytes % mib == 0) {
        return std::to_string(bytes / mib) + "M";
    }
    if (bytes % kib == 0) {
        return std::to_string(bytes / kib) + "K";
    }
    return std::to_string(bytes) + "B";
}

ResizeMode parseResizeMode(const std::string& rawValue)
{
    std::string value = unquote(rawValue);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "static") {
        return ResizeMode::Static;
    }
    if (value == "fibonacci") {
        return ResizeMode::Fibonacci;
    }
    throw util::Error("invalid resize_mode: " + value);
}

std::string formatResizeMode(ResizeMode mode)
{
    switch (mode) {
    case ResizeMode::Static:
        return "static";
    case ResizeMode::Fibonacci:
        return "fibonacci";
    }
    return "static";
}

Config loadConfig(const std::filesystem::path& path)
{
    Config config;
    std::ifstream input{path};
    if (!input) {
        throw util::Error("could not open config file: " + path.string());
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw util::Error("invalid config line: " + line);
        }

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "storage_root") {
            config.storageRoot = unquote(value);
        } else if (key == "image_name") {
            config.imageName = unquote(value);
        } else if (key == "runtime_mount_name") {
            config.runtimeMountName = unquote(value);
        } else if (key == "quota_size") {
            config.quotaSize = parseSize(value);
        } else if (key == "initial_size") {
            config.initialSize = parseSize(value);
        } else if (key == "resize_mode") {
            config.resizeMode = parseResizeMode(value);
        } else if (key == "static_step") {
            config.staticStep = parseSize(value);
        } else if (key == "fibonacci_max_step") {
            config.fibonacciMaxStep = parseSize(value);
        } else if (key == "grow_step") {
            config.staticStep = parseSize(value);
        } else if (key == "grow_free_threshold") {
            config.growFreeThreshold = parseSize(value);
        } else if (key == "adaptive_shrink") {
            config.adaptiveShrink = parseBool(value);
        } else if (key == "shrink_on_unmount") {
            config.shrinkOnUnmount = parseBool(value);
        } else if (key == "shrink_free_threshold") {
            config.shrinkFreeThreshold = parseSize(value);
        } else if (key == "shrink_min_reclaim") {
            config.shrinkMinReclaim = parseSize(value);
        } else {
            throw util::Error("unknown config key: " + key);
        }
    }

    validate(config);
    return config;
}

void saveConfig(const std::filesystem::path& path, const Config& config)
{
    validate(config);
    std::ofstream output{path};
    if (!output) {
        throw util::Error("could not write config file: " + path.string());
    }

    output << "storage_root = \"" << config.storageRoot.string() << "\"\n";
    output << "image_name = \"" << config.imageName << "\"\n";
    output << "runtime_mount_name = \"" << config.runtimeMountName << "\"\n\n";
    output << "quota_size = \"" << formatSize(config.quotaSize) << "\"\n";
    output << "initial_size = \"" << formatSize(config.initialSize) << "\"\n";
    output << "resize_mode = \"" << formatResizeMode(config.resizeMode) << "\"\n";
    output << "static_step = \"" << formatSize(config.staticStep) << "\"\n";
    output << "fibonacci_max_step = \"" << formatSize(config.fibonacciMaxStep) << "\"\n";
    output << "grow_free_threshold = \"" << formatSize(config.growFreeThreshold) << "\"\n\n";
    output << "adaptive_shrink = " << (config.adaptiveShrink ? "true" : "false") << "\n";
    output << "shrink_on_unmount = " << (config.shrinkOnUnmount ? "true" : "false") << "\n";
    output << "shrink_free_threshold = \"" << formatSize(config.shrinkFreeThreshold) << "\"\n";
    output << "shrink_min_reclaim = \"" << formatSize(config.shrinkMinReclaim) << "\"\n";
}

} // namespace personalfs::app
