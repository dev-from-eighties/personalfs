#pragma once

#include "app/Config.hpp"
#include "storage/BackendPaths.hpp"

#include <cstdint>
#include <mutex>

namespace personalfs::storage {

struct SpaceInfo {
    std::uint64_t total{0};
    std::uint64_t free{0};
    std::uint64_t used{0};
};

class ImageManager {
public:
    ImageManager(app::Config config, BackendPaths paths);

    static void initializeWorkdir(const std::filesystem::path& workdir, const app::Config& config);

    void mountImage();
    void unmountImage();
    void ensureFreeForWrite(std::uint64_t bytesToAdd);
    bool growAfterNoSpace(std::uint64_t bytesNeeded);
    void maybeShrink();
    void shrinkToFit();
    void setVerbose(bool verbose);

    const app::Config& config() const;
    const BackendPaths& paths() const;
    SpaceInfo spaceInfo() const;
    SpaceInfo parentSpaceInfo() const;
    std::uint64_t imageSize() const;
    bool isMounted() const;
    bool verbose() const;

private:
    std::uint64_t alignUpMiB(std::uint64_t value) const;
    std::uint64_t resizeStepForSize(std::uint64_t currentSize) const;
    std::uint64_t fibonacciStepForSize(std::uint64_t currentSize) const;
    std::uint64_t growthTarget(std::uint64_t currentSize, std::uint64_t usedAfterWrite) const;
    std::uint64_t shrinkTarget(std::uint64_t currentSize, std::uint64_t used) const;
    void growTo(std::uint64_t targetSize);
    void resizeBtrfsToMax();
    void resizeLoopDevice();
    void shrinkTo(std::uint64_t targetSize);

    app::Config config_;
    BackendPaths paths_;
    mutable std::mutex mutex_;
    bool verbose_{false};
};

} // namespace personalfs::storage
