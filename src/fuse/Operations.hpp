#pragma once

#include "storage/ImageManager.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

namespace personalfs::fuse {

class Operations {
public:
    explicit Operations(storage::ImageManager& imageManager);

    std::filesystem::path fullPath(const char* fusePath) const;
    int getattr(const char* path, struct stat* st) const;
    int access(const char* path, int mask) const;
    int readlink(const char* path, char* buffer, std::size_t size) const;
    int opendir(const char* path, std::uint64_t* handle) const;
    int readdir(const char* path, void* buffer, void* filler) const;
    int releasedir(std::uint64_t handle) const;
    int fsyncdir(const char* path, std::uint64_t handle, bool datasync) const;
    int mknod(const char* path, mode_t mode, dev_t rdev);
    int mkdir(const char* path, mode_t mode);
    int unlink(const char* path);
    int rmdir(const char* path);
    int symlink(const char* target, const char* linkPath);
    int rename(const char* from, const char* to, unsigned int flags);
    int link(const char* from, const char* to);
    int chmod(const char* path, mode_t mode);
    int chown(const char* path, uid_t uid, gid_t gid);
    int truncate(const char* path, off_t size, int fd);
    int open(const char* path, int flags, std::uint64_t* handle) const;
    int create(const char* path, mode_t mode, int flags, std::uint64_t* handle);
    int read(std::uint64_t handle, char* buffer, std::size_t size, off_t offset) const;
    int write(std::uint64_t handle, const char* buffer, std::size_t size, off_t offset);
    int release(std::uint64_t handle) const;
    int fsync(std::uint64_t handle, bool datasync) const;
    int fallocate(const char* path, int mode, off_t offset, off_t length, int fd);
    int setxattr(const char* path, const char* name, const char* value, std::size_t size, int flags);
    int getxattr(const char* path, const char* name, char* value, std::size_t size) const;
    int listxattr(const char* path, char* list, std::size_t size) const;
    int removexattr(const char* path, const char* name);
    int utimens(const char* path, const struct timespec tv[2], int fd);
    int statfs(struct statvfs* st) const;

private:
    void log(const std::string& message) const;
    int logResult(const std::string& operation, int result) const;
    void prepareWrite(std::uint64_t handle, std::size_t size, off_t offset);
    void maybeShrinkAfterDelete();

    storage::ImageManager& imageManager_;
};

} // namespace personalfs::fuse
