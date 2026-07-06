#include "storage/BackendPaths.hpp"

namespace personalfs::storage {

BackendPaths makeBackendPaths(const std::filesystem::path& workdir, const app::Config& config)
{
    BackendPaths paths;
    paths.workdir = workdir;
    paths.root = workdir / config.storageRoot;
    paths.config = paths.root / "config.toml";
    paths.image = paths.root / config.imageName;
    paths.internalMount = paths.root / config.runtimeMountName;
    return paths;
}

} // namespace personalfs::storage

