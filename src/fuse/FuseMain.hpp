#pragma once

#include "app/Config.hpp"
#include "storage/BackendPaths.hpp"
#include "storage/ImageManager.hpp"

namespace personalfs::fuse {

int runFuse(int argc, char** argv, storage::ImageManager& imageManager);

} // namespace personalfs::fuse

