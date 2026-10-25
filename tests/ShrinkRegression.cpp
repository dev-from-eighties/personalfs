#include "storage/ImageManager.hpp"
#include "fuse/Operations.hpp"
#include "util/Command.hpp"
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr std::uint64_t GiB = 1024ULL * 1024 * 1024;
std::uint64_t deviceSize = 8 * GiB;
bool failResize = false;
bool pretendResize = false;
bool partialFailure = false;
bool failQuery = false;
int resizeCalls = 0;
int refreshCalls = 0;
int syncCalls = 0;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
extern "C" int __wrap_statvfs(const char*, struct statvfs* st)
{
    *st = {};
    st->f_frsize = 4096;
    st->f_blocks = deviceSize / 4096;
    st->f_bavail = (deviceSize - 2 * GiB) / 4096;
    return 0;
}
extern "C" int __wrap_ioctl(int, unsigned long request, ...)
{
    if (failQuery) return -1;
    va_list args;
    va_start(args, request);
    void* data = va_arg(args, void*);
    va_end(args);
    if (request == BTRFS_IOC_FS_INFO) {
        static_cast<btrfs_ioctl_fs_info_args*>(data)->num_devices = 1;
    } else if (request == BTRFS_IOC_DEV_INFO) {
        static_cast<btrfs_ioctl_dev_info_args*>(data)->total_bytes = deviceSize;
    } else return -1;
    return 0;
}
namespace personalfs::util {
CommandResult runCommand(const std::vector<std::string>& args, bool)
{
    if (args[0] == "findmnt") return {0, "/dev/loop-test\n"};
    if (args[0] == "losetup") { ++refreshCalls; return {}; }
    if (args.size() > 2 && args[2] == "sync") { ++syncCalls; return {}; }
    if (args.size() > 3 && args[2] == "resize") {
        ++resizeCalls;
        const auto target = std::stoull(args[3]) * 1024 * 1024;
        check(target < deviceSize, "shrink attempted to grow Btrfs");
        if (failResize) {
            if (partialFailure) deviceSize = target;
            return {1, "injected ENOSPC"};
        }
        if (!pretendResize) deviceSize = target;
        return {};
    }
    throw std::runtime_error("unexpected command: " + args[0]);
}
}
int main()
{
    char name[] = "/tmp/personalfs-shrink-XXXXXX";
    const char* directory = ::mkdtemp(name);
    if (!directory) return 1;
    const std::filesystem::path root(directory);
    try {
        personalfs::app::Config config;
        config.resizeMode = personalfs::app::ResizeMode::Static;
        config.staticStep = GiB;
        personalfs::storage::BackendPaths paths{root, root, root / "config", root / "image", root / "mount"};
        std::filesystem::create_directory(paths.internalMount);
        std::ofstream(paths.image).close();
        std::filesystem::resize_file(paths.image, 12 * GiB); // sparse, no large allocation
        personalfs::storage::ImageManager manager(config, paths);
        personalfs::fuse::Operations ops(manager);
        manager.maybeShrink();
        check(deviceSize == 7 * GiB, "target must be based on actual Btrfs size");
        check(manager.imageSize() == deviceSize, "backing file was not reconciled");
        check(refreshCalls == 1 && syncCalls == 2, "missing synchronization or loop refresh");

        failResize = true;
        std::ofstream(paths.internalMount / "file") << "payload";
        check(ops.unlink("/file") == 0, "maintenance error leaked into successful unlink");
        check(!std::filesystem::exists(paths.internalMount / "file"), "file was not deleted");
        check(ops.unlink("/missing") == -ENOENT, "real unlink error was lost");
        std::filesystem::create_directory(paths.internalMount / "dir");
        check(ops.rmdir("/dir") == 0, "maintenance error leaked into rmdir");
        std::ofstream(paths.internalMount / "truncate") << "payload";
        check(ops.truncate("/truncate", 0, -1) == 0, "maintenance error leaked into truncate");
        check(manager.imageSize() == 7 * GiB, "failed resize truncated backing file");

        failResize = false;
        pretendResize = true;
        bool rejected = false;
        try { manager.maybeShrink(); } catch (...) { rejected = true; }
        check(rejected && manager.imageSize() == 7 * GiB, "unverified resize truncated image");
        pretendResize = false;
        failQuery = true;
        const auto previousCalls = resizeCalls;
        rejected = false;
        try { manager.maybeShrink(); } catch (...) { rejected = true; }
        check(rejected && resizeCalls == previousCalls, "failed size query allowed resize");
        failQuery = false;

        // A failed command may leave Btrfs smaller while the image stays large.
        failResize = true;
        partialFailure = true;
        rejected = false;
        try { manager.maybeShrink(); } catch (...) { rejected = true; }
        check(rejected && deviceSize == 6 * GiB && manager.imageSize() == 7 * GiB,
              "partial failure must preserve backing file");
        failResize = false;
        partialFailure = false;
        manager.maybeShrink();
        check(deviceSize == 5 * GiB && manager.imageSize() == deviceSize,
              "retry after partial failure did not reconcile sizes");
        const auto callsBeforeTail = resizeCalls;

        // If the filesystem cannot shrink below initial_size, reclaim only the
        // surplus backing-file tail without issuing a Btrfs resize.
        config.initialSize = deviceSize;
        personalfs::storage::ImageManager tailOnly(config, paths);
        std::filesystem::resize_file(paths.image, 12 * GiB);
        tailOnly.shrinkToFit();
        check(tailOnly.imageSize() == deviceSize && resizeCalls == callsBeforeTail,
              "tail-only recovery unnecessarily resized Btrfs");
        std::filesystem::remove_all(root);
        std::cout << "Shrink regression tests passed\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
