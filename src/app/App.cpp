#define FUSE_USE_VERSION 31

#include "app/App.hpp"

#include "app/Config.hpp"
#include "fuse/FuseMain.hpp"
#include "storage/BackendPaths.hpp"
#include "storage/ImageManager.hpp"
#include "util/Error.hpp"
#include "util/Logger.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/mount.h>
#include <thread>
#include <utility>
#include <vector>

namespace personalfs::app {

namespace {

void usage()
{
    util::Logger::info("Usage:");
    util::Logger::info("  personalfs initialize <workdir> --quota <size> [--initial-size <size>]");
    util::Logger::info("  personalfs mount <workdir> <mountpoint> [--allow-other | -o allow_other] [--verbose]");
    util::Logger::info("  personalfs shrink <workdir>");
}

std::string shellQuote(const std::filesystem::path& path)
{
    std::string quoted{"'"};
    for (char ch : path.string()) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += '\'';
    return quoted;
}

class MountSignalHandler {
public:
    explicit MountSignalHandler(std::filesystem::path mountPoint)
        : mountPoint_(std::move(mountPoint))
    {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);
        sigaddset(&signals_, SIGHUP);
        sigaddset(&signals_, SIGUSR1);

        if (pthread_sigmask(SIG_BLOCK, &signals_, &previousMask_) != 0) {
            util::Logger::error("ERROR: mount: could not install signal mask");
            return;
        }

        active_ = true;
        thread_ = std::thread{[this] { waitForSignals(); }};
    }

    ~MountSignalHandler()
    {
        if (thread_.joinable()) {
            pthread_kill(thread_.native_handle(), SIGUSR1);
            thread_.join();
        }

        if (active_) {
            pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr);
        }
    }

    MountSignalHandler(const MountSignalHandler&) = delete;
    MountSignalHandler& operator=(const MountSignalHandler&) = delete;

private:
    void waitForSignals()
    {
        for (;;) {
            int signal = 0;
            if (sigwait(&signals_, &signal) != 0) {
                return;
            }
            if (signal == SIGUSR1) {
                return;
            }

            util::Logger::info("mount: received signal, unmounting " + mountPoint_.string());
            requestUnmount();
            return;
        }
    }

    void requestUnmount() const
    {
        if (::umount2(mountPoint_.c_str(), MNT_DETACH) == 0) {
            return;
        }

        const std::string command = "fusermount3 -uz " + shellQuote(mountPoint_) + " >/dev/null 2>&1";
        const int ignoredStatus = std::system(command.c_str());
        (void)ignoredStatus;
    }

    std::filesystem::path mountPoint_;
    sigset_t signals_{};
    sigset_t previousMask_{};
    std::thread thread_;
    bool active_{false};
};

class ImageMountGuard {
public:
    explicit ImageMountGuard(storage::ImageManager& imageManager)
        : imageManager_(imageManager)
    {
        imageManager_.mountImage();
        mounted_ = true;
    }

    ~ImageMountGuard()
    {
        if (!mounted_) {
            return;
        }

        try {
            imageManager_.unmountImage();
        } catch (const std::exception& error) {
            util::Logger::error(std::string{"ERROR: could not unmount internal image: "} + error.what());
        }
    }

    ImageMountGuard(const ImageMountGuard&) = delete;
    ImageMountGuard& operator=(const ImageMountGuard&) = delete;

private:
    storage::ImageManager& imageManager_;
    bool mounted_{false};
};

Config loadWorkdirConfig(const std::filesystem::path& workdir)
{
    Config defaults;
    const auto defaultPaths = storage::makeBackendPaths(workdir, defaults);
    return loadConfig(defaultPaths.config);
}

int initializeCommand(const std::vector<std::string_view>& args)
{
    if (args.size() < 2) {
        util::Logger::error("initialize requires a workdir");
        return 1;
    }

    Config config;
    bool quotaWasSet = false;
    std::filesystem::path workdir = std::filesystem::absolute(std::filesystem::path{std::string{args[1]}});

    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--quota" && i + 1 < args.size()) {
            config.quotaSize = parseSize(std::string{args[++i]});
            quotaWasSet = true;
        } else if (args[i] == "--initial-size" && i + 1 < args.size()) {
            config.initialSize = parseSize(std::string{args[++i]});
        } else {
            util::Logger::error("unknown initialize argument: " + std::string{args[i]});
            return 1;
        }
    }

    if (!quotaWasSet) {
        util::Logger::error("initialize requires --quota <size>");
        return 1;
    }

    storage::ImageManager::initializeWorkdir(workdir, config);
    util::Logger::info("OK");
    return 0;
}

int shrinkCommand(const std::vector<std::string_view>& args)
{
    if (args.size() != 2) {
        util::Logger::error("shrink requires exactly 1 argument: workdir");
        return 1;
    }

    std::filesystem::path workdir = std::filesystem::absolute(std::filesystem::path{std::string{args[1]}});
    auto config = loadWorkdirConfig(workdir);
    auto paths = storage::makeBackendPaths(workdir, config);
    storage::ImageManager imageManager{config, paths};
    ImageMountGuard mountedImage{imageManager};
    imageManager.shrinkToFit();
    util::Logger::info("OK");
    return 0;
}

struct MountOptions {
    bool allowOther{false};
    bool verbose{false};
};

std::optional<MountOptions> parseMountOptions(const std::vector<std::string_view>& args)
{
    MountOptions options;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--allow-other") {
            options.allowOther = true;
        } else if (args[i] == "--verbose") {
            options.verbose = true;
        } else if (args[i] == "-o" && i + 1 < args.size() && args[i + 1] == "allow_other") {
            options.allowOther = true;
            ++i;
        } else {
            util::Logger::error("unknown mount argument: " + std::string{args[i]});
            return std::nullopt;
        }
    }
    return options;
}

int mountCommand(char** argv, const std::vector<std::string_view>& args)
{
    if (args.size() < 3) {
        util::Logger::error("mount requires: workdir mountpoint [--allow-other | -o allow_other] [--verbose]");
        return 1;
    }

    const auto options = parseMountOptions(args);
    if (!options.has_value()) {
        return 1;
    }

    std::filesystem::path workdir = std::filesystem::absolute(std::filesystem::path{std::string{args[1]}});
    std::filesystem::path mountPoint = std::filesystem::absolute(std::filesystem::path{std::string{args[2]}});
    auto config = loadWorkdirConfig(workdir);
    auto paths = storage::makeBackendPaths(workdir, config);
    storage::ImageManager imageManager{config, paths};
    imageManager.setVerbose(options->verbose);
    ImageMountGuard mountedImage{imageManager};

    std::string programName{argv[0] != nullptr ? argv[0] : "personalfs"};
    std::string foreground{"-f"};
    std::string singleThreaded{"-s"};
    std::string mountArg = mountPoint.string();
    std::string optionPrefix{"-o"};
    std::string allowOtherOption{"allow_other"};
    std::vector<char*> fuseArgs{
        programName.data(),
        foreground.data(),
        singleThreaded.data(),
        mountArg.data(),
    };
    if (options->allowOther) {
        fuseArgs.push_back(optionPrefix.data());
        fuseArgs.push_back(allowOtherOption.data());
    }

    MountSignalHandler signalHandler{mountPoint};
    const int result = fuse::runFuse(static_cast<int>(fuseArgs.size()), fuseArgs.data(), imageManager);
    if (config.shrinkOnUnmount) {
        imageManager.shrinkToFit();
    }
    return result;
}

} // namespace

int App::run(int argc, char** argv)
{
    try {
        std::vector<std::string_view> args;
        args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
        for (int i = 1; i < argc; ++i) {
            args.push_back(argv[i]);
        }

        if (args.empty()) {
            usage();
            return 1;
        }

        if (args[0] == "initialize") {
            return initializeCommand(args);
        }
        if (args[0] == "mount") {
            return mountCommand(argv, args);
        }
        if (args[0] == "shrink") {
            return shrinkCommand(args);
        }

        util::Logger::error("unknown command: " + std::string{args[0]});
        usage();
        return 1;
    } catch (const util::Error& error) {
        util::Logger::error(std::string{"ERROR: "} + error.what());
        return 1;
    } catch (const std::exception& error) {
        util::Logger::error(std::string{"ERROR: "} + error.what());
        return 1;
    }
}

} // namespace personalfs::app
