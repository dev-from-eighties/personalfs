#include "storage/ImageManager.hpp"

#include "util/Command.hpp"
#include "util/Error.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace personalfs::storage {

namespace {

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;

std::string trimOutput(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    while (!value.empty() && (value.front() == '\n' || value.front() == '\r' || value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    return value;
}

void requireOk(const std::vector<std::string>& command, const std::string& action)
{
    const auto result = util::runCommand(command, true);
    if (result.exitCode != 0) {
        throw util::Error(action + " failed: " + trimOutput(result.output));
    }
}

std::string sizeForBtrfs(std::uint64_t bytes)
{
    const auto roundedMiB = std::max<std::uint64_t>(1, bytes / mib);
    return std::to_string(roundedMiB) + "M";
}

std::uint64_t addClamped(std::uint64_t left, std::uint64_t right, std::uint64_t limit)
{
    if (left >= limit) {
        return limit;
    }
    return right > limit - left ? limit : left + right;
}

std::uint64_t btrfsShrinkReserve(std::uint64_t used, std::uint64_t configuredReserve)
{
    const auto proportionalReserve = used / 10;
    return std::max({configuredReserve, proportionalReserve, gib});
}

} // namespace

ImageManager::ImageManager(app::Config config, BackendPaths paths)
    : config_(std::move(config))
    , paths_(std::move(paths))
{
}

void ImageManager::initializeWorkdir(const std::filesystem::path& workdir, const app::Config& config)
{
    const auto paths = makeBackendPaths(workdir, config);
    std::error_code error;
    if (!std::filesystem::exists(workdir, error)) {
        std::filesystem::create_directories(workdir, error);
        if (error) {
            throw util::Error("could not create workdir: " + error.message());
        }
    }
    if (!std::filesystem::is_directory(workdir, error)) {
        throw util::Error("workdir is not a directory: " + workdir.string());
    }
    if (std::filesystem::exists(paths.root, error)) {
        throw util::Error("workdir is already initialized: " + paths.root.string());
    }

    requireOk({"mkfs.btrfs", "--version"}, "mkfs.btrfs availability");

    std::filesystem::create_directories(paths.internalMount, error);
    if (error) {
        throw util::Error("could not create backend directories: " + error.message());
    }

    app::saveConfig(paths.config, config);
    {
        std::ofstream image{paths.image, std::ios::binary | std::ios::trunc};
        if (!image) {
            throw util::Error("could not create image file: " + paths.image.string());
        }
    }

    std::filesystem::resize_file(paths.image, config.initialSize, error);
    if (error) {
        throw util::Error("could not create image file: " + error.message());
    }

    requireOk({"mkfs.btrfs", "-f", paths.image.string()}, "mkfs.btrfs");
}

void ImageManager::mountImage()
{
    std::scoped_lock lock{mutex_};
    if (isMounted()) {
        return;
    }

    const auto result = util::runCommand({"mount", "-o", "loop", paths_.image.string(), paths_.internalMount.string()}, true);
    if (result.exitCode == 0) {
        return;
    }

    std::string message = "mount image failed: " + trimOutput(result.output);
    if (::geteuid() != 0) {
        message += "\nHint: mounting the internal btrfs image through a loop device requires root/CAP_SYS_ADMIN. ";
        message += "Run PersonalFS from a service/root context, or configure a privileged helper for the internal image mount.";
    }
    throw util::Error(message);
}

void ImageManager::unmountImage()
{
    std::scoped_lock lock{mutex_};
    if (!isMounted()) {
        return;
    }
    requireOk({"umount", paths_.internalMount.string()}, "umount image");
}

bool ImageManager::isMounted() const
{
    const auto result = util::runCommand({"findmnt", "-T", paths_.internalMount.string(), "-n", "-o", "TARGET"}, true);
    return result.exitCode == 0 && trimOutput(result.output) == paths_.internalMount.string();
}

SpaceInfo ImageManager::spaceInfo() const
{
    struct statvfs st {};
    if (::statvfs(paths_.internalMount.c_str(), &st) != 0) {
        throw util::Error(std::string{"statvfs failed: "} + std::strerror(errno));
    }
    const std::uint64_t blockSize = st.f_frsize != 0 ? st.f_frsize : st.f_bsize;
    SpaceInfo info;
    info.total = static_cast<std::uint64_t>(st.f_blocks) * blockSize;
    info.free = static_cast<std::uint64_t>(st.f_bavail) * blockSize;
    info.used = info.total > info.free ? info.total - info.free : 0;
    return info;
}

SpaceInfo ImageManager::parentSpaceInfo() const
{
    struct statvfs st {};
    const auto parentPath = paths_.image.parent_path().empty() ? paths_.root : paths_.image.parent_path();
    if (::statvfs(parentPath.c_str(), &st) != 0) {
        throw util::Error(std::string{"parent statvfs failed: "} + std::strerror(errno));
    }

    const std::uint64_t blockSize = st.f_frsize != 0 ? st.f_frsize : st.f_bsize;
    SpaceInfo info;
    info.total = static_cast<std::uint64_t>(st.f_blocks) * blockSize;
    info.free = static_cast<std::uint64_t>(st.f_bavail) * blockSize;
    info.used = info.total > info.free ? info.total - info.free : 0;
    return info;
}

std::uint64_t ImageManager::imageSize() const
{
    std::error_code error;
    const auto size = std::filesystem::file_size(paths_.image, error);
    if (error) {
        throw util::Error("could not stat image file: " + error.message());
    }
    return size;
}

std::uint64_t ImageManager::btrfsDeviceSize() const
{
    const int fd = ::open(paths_.internalMount.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        throw util::Error(std::string{"could not open btrfs mount: "} + std::strerror(errno));
    }
    struct btrfs_ioctl_fs_info_args fs {};
    struct btrfs_ioctl_dev_info_args device {};
    device.devid = 1; // Images created by PersonalFS contain a single device, id 1.
    const bool valid = ::ioctl(fd, BTRFS_IOC_FS_INFO, &fs) == 0
        && fs.num_devices == 1
        && ::ioctl(fd, BTRFS_IOC_DEV_INFO, &device) == 0
        && device.total_bytes > 0;
    ::close(fd);
    if (!valid) {
        throw util::Error("could not determine single-device btrfs size; refusing to shrink image");
    }
    return device.total_bytes;
}

void ImageManager::setVerbose(bool verbose)
{
    verbose_ = verbose;
}

const app::Config& ImageManager::config() const
{
    return config_;
}

const BackendPaths& ImageManager::paths() const
{
    return paths_;
}

bool ImageManager::verbose() const
{
    return verbose_;
}

std::uint64_t ImageManager::alignUpMiB(std::uint64_t value) const
{
    const std::uint64_t remainder = value % mib;
    if (remainder == 0) {
        return value;
    }
    return value + mib - remainder;
}

std::uint64_t ImageManager::resizeStepForSize(std::uint64_t currentSize) const
{
    if (config_.resizeMode == app::ResizeMode::Fibonacci) {
        return fibonacciStepForSize(currentSize);
    }
    return std::max<std::uint64_t>(mib, alignUpMiB(config_.staticStep));
}

std::uint64_t ImageManager::fibonacciStepForSize(std::uint64_t currentSize) const
{
    const auto maxStep = std::max<std::uint64_t>(mib, alignUpMiB(config_.fibonacciMaxStep));
    const auto baseStep = std::min(maxStep, std::max<std::uint64_t>(mib, alignUpMiB(config_.initialSize)));

    std::uint64_t previous = baseStep;
    std::uint64_t current = baseStep;
    while (current < currentSize && current < maxStep) {
        const auto next = addClamped(current, previous, maxStep);
        previous = current;
        current = next;
    }
    return current;
}

std::uint64_t ImageManager::growthTarget(std::uint64_t currentSize, std::uint64_t usedAfterWrite) const
{
    const auto wanted = std::max(config_.initialSize, alignUpMiB(usedAfterWrite + config_.growFreeThreshold));
    const auto step = resizeStepForSize(currentSize);
    auto stepped = addClamped(currentSize, step, config_.quotaSize);

    if (config_.resizeMode == app::ResizeMode::Static && wanted > stepped) {
        const auto remainder = wanted % step;
        stepped = remainder == 0 ? wanted : wanted + step - remainder;
    } else if (wanted > stepped) {
        stepped = wanted;
    }

    return std::min(config_.quotaSize, alignUpMiB(stepped));
}

std::uint64_t ImageManager::shrinkTarget(std::uint64_t currentSize, std::uint64_t used) const
{
    const auto reserve = btrfsShrinkReserve(used, config_.growFreeThreshold);
    const auto minimum = std::max(config_.initialSize, alignUpMiB(used + reserve));
    if (minimum >= currentSize) {
        return currentSize;
    }

    const auto step = resizeStepForSize(currentSize);
    const auto stepped = currentSize > step ? currentSize - step : minimum;
    return std::max(minimum, alignUpMiB(stepped));
}

void ImageManager::ensureFreeForWrite(std::uint64_t bytesToAdd)
{
    std::scoped_lock lock{mutex_};
    const auto current = spaceInfo();
    const auto currentImageSize = imageSize();

    if (verbose_) {
        util::Logger::info(
            "space-check: requested=" + app::formatSize(bytesToAdd)
            + " btrfs_total=" + app::formatSize(current.total)
            + " btrfs_used=" + app::formatSize(current.used)
            + " btrfs_free=" + app::formatSize(current.free)
            + " image_size=" + app::formatSize(currentImageSize)
            + " quota=" + app::formatSize(config_.quotaSize));
    }

    if (current.free >= config_.growFreeThreshold && current.free >= bytesToAdd) {
        if (verbose_) {
            util::Logger::info("space-check: no grow needed");
        }
        return;
    }

    const auto target = growthTarget(currentImageSize, current.used + bytesToAdd);
    if (verbose_) {
        util::Logger::info(
            "space-check: grow target=" + app::formatSize(target)
            + " current_image=" + app::formatSize(currentImageSize));
    }
    if (target <= currentImageSize) {
        if (verbose_) {
            util::Logger::info("space-check: target is not larger than current image");
        }
        return;
    }
    growTo(target);
}

bool ImageManager::growAfterNoSpace(std::uint64_t bytesNeeded)
{
    std::scoped_lock lock{mutex_};
    const auto current = spaceInfo();
    const auto currentImageSize = imageSize();
    auto target = growthTarget(currentImageSize, current.used + bytesNeeded);

    if (target <= currentImageSize && currentImageSize < config_.quotaSize) {
        target = addClamped(currentImageSize, resizeStepForSize(currentImageSize), config_.quotaSize);
    }

    if (verbose_) {
        util::Logger::info(
            "enospc-grow: requested=" + app::formatSize(bytesNeeded)
            + " btrfs_total=" + app::formatSize(current.total)
            + " btrfs_used=" + app::formatSize(current.used)
            + " btrfs_free=" + app::formatSize(current.free)
            + " image_size=" + app::formatSize(currentImageSize)
            + " target=" + app::formatSize(target)
            + " quota=" + app::formatSize(config_.quotaSize));
    }

    if (target <= currentImageSize) {
        return false;
    }

    growTo(target);
    return true;
}

void ImageManager::maybeShrink()
{
    if (!config_.adaptiveShrink) {
        if (verbose_) {
            util::Logger::info("shrink-check: adaptive shrink disabled");
        }
        return;
    }

    std::scoped_lock lock{mutex_};
    requireOk({"btrfs", "filesystem", "sync", paths_.internalMount.string()}, "btrfs sync before shrink");
    const auto current = spaceInfo();
    if (verbose_) {
        const auto reserve = btrfsShrinkReserve(current.used, config_.growFreeThreshold);
        util::Logger::info(
            "shrink-check: btrfs_used=" + app::formatSize(current.used)
            + " btrfs_free=" + app::formatSize(current.free)
            + " threshold=" + app::formatSize(config_.shrinkFreeThreshold)
            + " reserve=" + app::formatSize(reserve));
    }
    if (current.free < config_.shrinkFreeThreshold) {
        return;
    }

    const auto currentImageSize = imageSize();
    const auto target = shrinkTarget(btrfsDeviceSize(), current.used);
    if (target >= currentImageSize || currentImageSize - target < config_.shrinkMinReclaim) {
        return;
    }
    shrinkTo(target);
}

void ImageManager::shrinkToFit()
{
    std::scoped_lock lock{mutex_};
    requireOk({"btrfs", "filesystem", "sync", paths_.internalMount.string()}, "btrfs sync before shrink");
    const auto current = spaceInfo();
    const auto currentImageSize = imageSize();
    const auto target = shrinkTarget(btrfsDeviceSize(), current.used);
    if (verbose_) {
        const auto reserve = btrfsShrinkReserve(current.used, config_.growFreeThreshold);
        util::Logger::info(
            "shrink-to-fit: btrfs_used=" + app::formatSize(current.used)
            + " btrfs_free=" + app::formatSize(current.free)
            + " current_image=" + app::formatSize(currentImageSize)
            + " target=" + app::formatSize(target)
            + " reserve=" + app::formatSize(reserve));
    }
    if (target < currentImageSize) {
        try {
            shrinkTo(target);
        } catch (const std::exception& error) {
            util::Logger::error(std::string{"WARNING: shrink-to-fit skipped: "} + error.what());
        }
    }
}

void ImageManager::growTo(std::uint64_t targetSize)
{
    targetSize = std::min(config_.quotaSize, alignUpMiB(targetSize));
    util::Logger::info("personalfs: growing image to " + app::formatSize(targetSize));

    std::error_code error;
    std::filesystem::resize_file(paths_.image, targetSize, error);
    if (error) {
        throw util::Error("could not grow image file: " + error.message());
    }

    resizeLoopDevice();
    resizeBtrfsToMax();
}

void ImageManager::resizeLoopDevice()
{
    const auto result = util::runCommand({"findmnt", "-T", paths_.internalMount.string(), "-n", "-o", "SOURCE"}, true);
    if (result.exitCode != 0) {
        throw util::Error("could not find mounted loop device: " + trimOutput(result.output));
    }

    const auto source = trimOutput(result.output);
    if (source.rfind("/dev/loop", 0) == 0) {
        requireOk({"losetup", "-c", source}, "losetup resize");
    }
}

void ImageManager::resizeBtrfsToMax()
{
    requireOk({"btrfs", "filesystem", "resize", "max", paths_.internalMount.string()}, "btrfs grow");
}

void ImageManager::shrinkTo(std::uint64_t targetSize)
{
    // The backing file can be larger after a failed growth. Never interpret its
    // length (or statvfs capacity) as the current Btrfs device size.
    const auto deviceSize = btrfsDeviceSize();
    targetSize = std::min(deviceSize, std::max(config_.initialSize, alignUpMiB(targetSize)));
    if (targetSize >= imageSize()) {
        return;
    }
    util::Logger::info("personalfs: shrinking image to " + app::formatSize(targetSize));

    if (targetSize < deviceSize) {
        requireOk({"btrfs", "filesystem", "resize", sizeForBtrfs(targetSize), paths_.internalMount.string()}, "btrfs shrink");
    }
    // Verify the committed device boundary before truncating its backing file.
    requireOk({"btrfs", "filesystem", "sync", paths_.internalMount.string()}, "btrfs sync after shrink");
    if (btrfsDeviceSize() > targetSize) {
        throw util::Error("btrfs device still exceeds shrink target; refusing to truncate image");
    }
    std::error_code error;
    std::filesystem::resize_file(paths_.image, targetSize, error);
    if (error) {
        throw util::Error("could not shrink image file: " + error.message());
    }
    resizeLoopDevice();
}

} // namespace personalfs::storage
