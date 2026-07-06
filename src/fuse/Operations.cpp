#include "fuse/Operations.hpp"

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "app/Config.hpp"
#include "util/Error.hpp"
#include "util/Logger.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace personalfs::fuse {

namespace {

int errnoResult()
{
    return -errno;
}

int exceptionResult()
{
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    } catch (...) {
        return -EIO;
    }
}

int fdFromHandle(std::uint64_t handle)
{
    if (handle > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return -1;
    }
    return static_cast<int>(handle);
}

struct CallerContext {
    uid_t uid;
    gid_t gid;
    mode_t umask;
};

CallerContext currentCallerContext()
{
    const auto* ctx = fuse_get_context();
    if (ctx == nullptr) {
        return {
            .uid = getuid(),
            .gid = getgid(),
            .umask = 0,
        };
    }

    return {
        .uid = ctx->uid,
        .gid = ctx->gid,
        .umask = ctx->umask,
    };
}

mode_t initialMode(mode_t mode, mode_t callerUmask)
{
    return (mode & ~callerUmask) & 07777;
}

bool callerCanAccess(const struct stat& st, int mask, const CallerContext& caller)
{
    if (mask == F_OK || caller.uid == 0) {
        return true;
    }

    int shift = 0;
    if (caller.uid == st.st_uid) {
        shift = 6;
    } else if (caller.gid == st.st_gid) {
        shift = 3;
    }

    int required = 0;
    if ((mask & R_OK) != 0) {
        required |= 4;
    }
    if ((mask & W_OK) != 0) {
        required |= 2;
    }
    if ((mask & X_OK) != 0) {
        required |= 1;
    }

    return (static_cast<int>((st.st_mode >> shift) & 7) & required) == required;
}

void presentRootOwnedAsCaller(struct stat& st, const CallerContext& caller)
{
    if (st.st_uid == 0 && st.st_gid == 0 && caller.uid != 0) {
        st.st_uid = caller.uid;
        st.st_gid = caller.gid;
    }
}

int openFlagsToAccessMask(int flags)
{
    switch (flags & O_ACCMODE) {
    case O_WRONLY:
        return W_OK;
    case O_RDWR:
        return R_OK | W_OK;
    case O_RDONLY:
    default:
        return R_OK;
    }
}

bool callerCanCreateIn(const std::filesystem::path& target, const CallerContext& caller)
{
    struct stat parent {};
    if (::lstat(target.parent_path().c_str(), &parent) != 0) {
        return false;
    }
    presentRootOwnedAsCaller(parent, caller);
    return callerCanAccess(parent, W_OK | X_OK, caller);
}

const char* printablePath(const char* path)
{
    return path == nullptr ? "<null>" : path;
}

std::string octalMode(mode_t mode)
{
    std::ostringstream out;
    out << std::oct << mode;
    return out.str();
}

constexpr std::uint64_t metadataRetryReserve = 1024ULL * 1024ULL;

} // namespace

Operations::Operations(storage::ImageManager& imageManager)
    : imageManager_(imageManager)
{
}

void Operations::log(const std::string& message) const
{
    if (imageManager_.verbose()) {
        util::Logger::info(message);
    }
}

int Operations::logResult(const std::string& operation, int result) const
{
    if (!imageManager_.verbose()) {
        return result;
    }

    std::string message = operation + " result=" + std::to_string(result);
    if (result < 0) {
        const int error = -result;
        message += " errno=" + std::to_string(error);
        message += " message=";
        message += std::strerror(error);
    }
    util::Logger::info(message);
    return result;
}

std::filesystem::path Operations::fullPath(const char* fusePath) const
{
    if (fusePath == nullptr || fusePath[0] != '/') {
        throw util::Error("invalid FUSE path");
    }

    std::filesystem::path result = imageManager_.paths().internalMount;
    std::filesystem::path relative{fusePath + 1};
    for (const auto& part : relative) {
        if (part == "..") {
            throw util::Error("parent traversal is not allowed");
        }
        if (part == ".") {
            continue;
        }
        result /= part;
    }
    return result;
}

int Operations::getattr(const char* path, struct stat* st) const
{
    log(std::string{"getattr: "} + printablePath(path));
    try {
        if (::lstat(fullPath(path).c_str(), st) != 0) {
            return logResult("getattr", errnoResult());
        }
        presentRootOwnedAsCaller(*st, currentCallerContext());
        return logResult("getattr", 0);
    } catch (...) {
        return logResult("getattr", exceptionResult());
    }
}

int Operations::access(const char* path, int mask) const
{
    try {
        const auto caller = currentCallerContext();
        log(std::string{"access: "} + printablePath(path) + " mask=" + std::to_string(mask)
            + " uid=" + std::to_string(caller.uid) + " gid=" + std::to_string(caller.gid));

        struct stat st {};
        if (::lstat(fullPath(path).c_str(), &st) != 0) {
            return logResult("access", errnoResult());
        }

        presentRootOwnedAsCaller(st, caller);
        return logResult("access", callerCanAccess(st, mask, caller) ? 0 : -EACCES);
    } catch (...) {
        return logResult("access", exceptionResult());
    }
}

int Operations::readlink(const char* path, char* buffer, std::size_t size) const
{
    log(std::string{"readlink: "} + printablePath(path));
    try {
        if (size == 0) {
            return -EINVAL;
        }
        const auto result = ::readlink(fullPath(path).c_str(), buffer, size - 1);
        if (result < 0) {
            return logResult("readlink", errnoResult());
        }
        buffer[result] = '\0';
        return logResult("readlink", 0);
    } catch (...) {
        return logResult("readlink", exceptionResult());
    }
}

int Operations::opendir(const char* path, std::uint64_t* handle) const
{
    log(std::string{"opendir: "} + printablePath(path));
    try {
        const int fd = ::open(fullPath(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return logResult("opendir", errnoResult());
        }
        *handle = static_cast<std::uint64_t>(fd);
        return logResult("opendir", 0);
    } catch (...) {
        return logResult("opendir", exceptionResult());
    }
}

int Operations::readdir(const char* path, void* buffer, void* fillerPtr) const
{
    log(std::string{"readdir: "} + printablePath(path));
    auto filler = reinterpret_cast<fuse_fill_dir_t>(fillerPtr);
    try {
        DIR* dir = ::opendir(fullPath(path).c_str());
        if (dir == nullptr) {
            return errnoResult();
        }

        while (dirent* entry = ::readdir(dir)) {
            struct stat st {};
            st.st_ino = entry->d_ino;
            st.st_mode = static_cast<mode_t>(entry->d_type) << 12U;
            if (filler(buffer, entry->d_name, &st, 0, static_cast<fuse_fill_dir_flags>(0)) != 0) {
                break;
            }
        }

        ::closedir(dir);
        return logResult("readdir", 0);
    } catch (...) {
        return logResult("readdir", exceptionResult());
    }
}

int Operations::releasedir(std::uint64_t handle) const
{
    log("releasedir: handle=" + std::to_string(handle));
    const int fd = fdFromHandle(handle);
    if (::close(fd) != 0) {
        return logResult("releasedir", errnoResult());
    }
    return logResult("releasedir", 0);
}

int Operations::fsyncdir(const char* path, std::uint64_t handle, bool datasync) const
{
    log(std::string{"fsyncdir: "} + printablePath(path) + " handle=" + std::to_string(handle)
        + " datasync=" + (datasync ? "true" : "false"));

        const int fd = fdFromHandle(handle);
        if (fd >= 0) {
            const int result = datasync ? ::fdatasync(fd) : ::fsync(fd);
            if (result != 0) {
                return logResult("fsyncdir", errnoResult());
            }
            return logResult("fsyncdir", 0);
    }

    try {
        const int opened = ::open(fullPath(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (opened < 0) {
            return logResult("fsyncdir", errnoResult());
        }
        const int result = datasync ? ::fdatasync(opened) : ::fsync(opened);
        const int savedErrno = errno;
        ::close(opened);
        if (result != 0) {
            errno = savedErrno;
            return logResult("fsyncdir", errnoResult());
        }
        return logResult("fsyncdir", 0);
    } catch (...) {
        return logResult("fsyncdir", exceptionResult());
    }
}

int Operations::mknod(const char* path, mode_t mode, dev_t rdev)
{
    log(std::string{"mknod: "} + printablePath(path) + " mode=" + octalMode(mode) + " rdev=" + std::to_string(rdev));
    try {
        const auto caller = currentCallerContext();
        const mode_t effectiveMode = (mode & S_IFMT) | initialMode(mode, caller.umask);
        const auto target = fullPath(path);
        if (!callerCanCreateIn(target, caller)) {
            return logResult("mknod", -EACCES);
        }
        imageManager_.ensureFreeForWrite(0);
        int result = 0;
        if (S_ISREG(mode)) {
            result = ::open(target.c_str(), O_CREAT | O_EXCL | O_WRONLY, effectiveMode);
            if (result < 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(metadataRetryReserve)) {
                result = ::open(target.c_str(), O_CREAT | O_EXCL | O_WRONLY, effectiveMode);
            }
            if (result >= 0) {
                if (::fchown(result, caller.uid, caller.gid) != 0) {
                    const int savedErrno = errno;
                    ::close(result);
                    errno = savedErrno;
                    return logResult("mknod", errnoResult());
                }
                ::close(result);
                result = 0;
            }
        } else {
            if ((S_ISCHR(mode) || S_ISBLK(mode)) && caller.uid != 0) {
                return logResult("mknod", -EPERM);
            }
            result = ::mknod(target.c_str(), effectiveMode, rdev);
            if (result != 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(metadataRetryReserve)) {
                result = ::mknod(target.c_str(), effectiveMode, rdev);
            }
            if (result == 0 && ::lchown(target.c_str(), caller.uid, caller.gid) != 0) {
                return logResult("mknod", errnoResult());
            }
        }
        if (result != 0) {
            return logResult("mknod", errnoResult());
        }
        return logResult("mknod", 0);
    } catch (...) {
        return logResult("mknod", exceptionResult());
    }
}

int Operations::mkdir(const char* path, mode_t mode)
{
    log(std::string{"mkdir: "} + printablePath(path) + " mode=" + octalMode(mode));
    try {
        const auto caller = currentCallerContext();
        const auto target = fullPath(path);
        if (!callerCanCreateIn(target, caller)) {
            return logResult("mkdir", -EACCES);
        }
        imageManager_.ensureFreeForWrite(0);
        int result = ::mkdir(target.c_str(), initialMode(mode, caller.umask));
        if (result != 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(metadataRetryReserve)) {
            result = ::mkdir(target.c_str(), initialMode(mode, caller.umask));
        }
        if (result != 0) {
            return logResult("mkdir", errnoResult());
        }
        if (::chown(target.c_str(), caller.uid, caller.gid) != 0) {
            return logResult("mkdir", errnoResult());
        }
        return logResult("mkdir", 0);
    } catch (...) {
        return logResult("mkdir", exceptionResult());
    }
}

int Operations::unlink(const char* path)
{
    log(std::string{"unlink: "} + printablePath(path));
    try {
        if (::unlink(fullPath(path).c_str()) != 0) {
            return errnoResult();
        }
        maybeShrinkAfterDelete();
        return 0;
    } catch (...) {
        return exceptionResult();
    }
}

int Operations::rmdir(const char* path)
{
    log(std::string{"rmdir: "} + printablePath(path));
    try {
        if (::rmdir(fullPath(path).c_str()) != 0) {
            return errnoResult();
        }
        maybeShrinkAfterDelete();
        return 0;
    } catch (...) {
        return exceptionResult();
    }
}

int Operations::symlink(const char* target, const char* linkPath)
{
    log(std::string{"symlink: "} + printablePath(linkPath) + " -> " + printablePath(target));
    try {
        const auto caller = currentCallerContext();
        const auto linkTarget = fullPath(linkPath);
        if (!callerCanCreateIn(linkTarget, caller)) {
            return logResult("symlink", -EACCES);
        }
        imageManager_.ensureFreeForWrite(static_cast<std::uint64_t>(std::strlen(target)));
        int result = ::symlink(target, linkTarget.c_str());
        if (result != 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(metadataRetryReserve)) {
            result = ::symlink(target, linkTarget.c_str());
        }
        if (result != 0) {
            return logResult("symlink", errnoResult());
        }
        if (::lchown(linkTarget.c_str(), caller.uid, caller.gid) != 0) {
            return logResult("symlink", errnoResult());
        }
        return logResult("symlink", 0);
    } catch (...) {
        return logResult("symlink", exceptionResult());
    }
}

int Operations::rename(const char* from, const char* to, unsigned int flags)
{
    log(std::string{"rename: "} + printablePath(from) + " -> " + printablePath(to) + " flags=" + std::to_string(flags));
    try {
        if (flags != 0) {
            return -EINVAL;
        }
        if (::rename(fullPath(from).c_str(), fullPath(to).c_str()) != 0) {
            return errnoResult();
        }
        return 0;
    } catch (...) {
        return exceptionResult();
    }
}

int Operations::link(const char* from, const char* to)
{
    log(std::string{"link: "} + printablePath(from) + " -> " + printablePath(to));
    try {
        const auto caller = currentCallerContext();
        const auto target = fullPath(to);
        if (!callerCanCreateIn(target, caller)) {
            return -EACCES;
        }
        imageManager_.ensureFreeForWrite(0);
        int result = ::link(fullPath(from).c_str(), target.c_str());
        if (result != 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(metadataRetryReserve)) {
            result = ::link(fullPath(from).c_str(), target.c_str());
        }
        if (result != 0) {
            return errnoResult();
        }
        return 0;
    } catch (...) {
        return exceptionResult();
    }
}

int Operations::chmod(const char* path, mode_t mode)
{
    log(std::string{"chmod: "} + printablePath(path) + " mode=" + octalMode(mode));
    try {
        if (::chmod(fullPath(path).c_str(), mode) != 0) {
            return errnoResult();
        }
        return 0;
    } catch (...) {
        return exceptionResult();
    }
}

int Operations::chown(const char* path, uid_t uid, gid_t gid)
{
    log(std::string{"chown: "} + printablePath(path) + " uid=" + std::to_string(uid) + " gid=" + std::to_string(gid));
    try {
        if (::lchown(fullPath(path).c_str(), uid, gid) != 0) {
            return errnoResult();
        }
        return 0;
    } catch (...) {
        return exceptionResult();
    }
}

int Operations::truncate(const char* path, off_t size, int fd)
{
    log(std::string{"truncate: "} + printablePath(path) + " size=" + std::to_string(size) + " fd=" + std::to_string(fd));
    try {
        if (size < 0) {
            return -EINVAL;
        }

        struct stat st {};
        const int statResult = fd >= 0 ? ::fstat(fd, &st) : ::lstat(fullPath(path).c_str(), &st);
        if (statResult != 0) {
            return errnoResult();
        }

        if (static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(st.st_size)) {
            imageManager_.ensureFreeForWrite(static_cast<std::uint64_t>(size - st.st_size));
        }

        int result = fd >= 0 ? ::ftruncate(fd, size) : ::truncate(fullPath(path).c_str(), size);
        if (result != 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(static_cast<std::uint64_t>(size))) {
            result = fd >= 0 ? ::ftruncate(fd, size) : ::truncate(fullPath(path).c_str(), size);
        }
        if (result != 0) {
            return errnoResult();
        }
        if (static_cast<std::uint64_t>(size) < static_cast<std::uint64_t>(st.st_size)) {
            maybeShrinkAfterDelete();
        }
        return 0;
    } catch (...) {
        return exceptionResult();
    }
}

int Operations::open(const char* path, int flags, std::uint64_t* handle) const
{
    log(std::string{"open: "} + printablePath(path) + " flags=" + std::to_string(flags));
    try {
        const auto target = fullPath(path);
        struct stat st {};
        if (::lstat(target.c_str(), &st) != 0) {
            return logResult("open", errnoResult());
        }
        const auto caller = currentCallerContext();
        presentRootOwnedAsCaller(st, caller);
        int mask = openFlagsToAccessMask(flags);
        if ((flags & O_TRUNC) != 0) {
            mask |= W_OK;
        }
        if (!callerCanAccess(st, mask, caller)) {
            return logResult("open", -EACCES);
        }

        const int fd = ::open(target.c_str(), flags);
        if (fd < 0) {
            return logResult("open", errnoResult());
        }
        *handle = static_cast<std::uint64_t>(fd);
        return logResult("open", 0);
    } catch (...) {
        return logResult("open", exceptionResult());
    }
}

int Operations::create(const char* path, mode_t mode, int flags, std::uint64_t* handle)
{
    log(std::string{"create: "} + printablePath(path) + " mode=" + octalMode(mode) + " flags=" + std::to_string(flags));
    try {
        const auto caller = currentCallerContext();
        const auto target = fullPath(path);
        if (!callerCanCreateIn(target, caller)) {
            return logResult("create", -EACCES);
        }
        imageManager_.ensureFreeForWrite(0);
        int fd = ::open(target.c_str(), flags | O_CREAT, initialMode(mode, caller.umask));
        if (fd < 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(metadataRetryReserve)) {
            fd = ::open(target.c_str(), flags | O_CREAT, initialMode(mode, caller.umask));
        }
        if (fd < 0) {
            return logResult("create", errnoResult());
        }
        if (::fchown(fd, caller.uid, caller.gid) != 0) {
            const int savedErrno = errno;
            ::close(fd);
            errno = savedErrno;
            return logResult("create", errnoResult());
        }
        *handle = static_cast<std::uint64_t>(fd);
        return logResult("create", 0);
    } catch (...) {
        return logResult("create", exceptionResult());
    }
}

int Operations::read(std::uint64_t handle, char* buffer, std::size_t size, off_t offset) const
{
    log("read: handle=" + std::to_string(handle) + " size=" + std::to_string(size) + " offset=" + std::to_string(offset));
    const int fd = fdFromHandle(handle);
    const ssize_t result = ::pread(fd, buffer, size, offset);
    if (result < 0) {
        return logResult("read", errnoResult());
    }
    return logResult("read", static_cast<int>(result));
}

int Operations::write(std::uint64_t handle, const char* buffer, std::size_t size, off_t offset)
{
    log("write: handle=" + std::to_string(handle) + " size=" + std::to_string(size) + " offset=" + std::to_string(offset));
    try {
        prepareWrite(handle, size, offset);
        const int fd = fdFromHandle(handle);
        ssize_t result = ::pwrite(fd, buffer, size, offset);
        if (result < 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(static_cast<std::uint64_t>(size))) {
            result = ::pwrite(fd, buffer, size, offset);
        }
        if (result < 0) {
            return logResult("write", errnoResult());
        }
        return logResult("write", static_cast<int>(result));
    } catch (...) {
        return logResult("write", exceptionResult());
    }
}

int Operations::release(std::uint64_t handle) const
{
    log("release: handle=" + std::to_string(handle));
    const int fd = fdFromHandle(handle);
    if (::close(fd) != 0) {
        return logResult("release", errnoResult());
    }
    return logResult("release", 0);
}

int Operations::fsync(std::uint64_t handle, bool datasync) const
{
    log("fsync: handle=" + std::to_string(handle) + " datasync=" + (datasync ? "true" : "false"));
    const int fd = fdFromHandle(handle);
    const int result = datasync ? ::fdatasync(fd) : ::fsync(fd);
    if (result != 0) {
        return logResult("fsync", errnoResult());
    }
    return logResult("fsync", 0);
}

int Operations::fallocate(const char* path, int mode, off_t offset, off_t length, int fd)
{
    log(std::string{"fallocate: "} + printablePath(path) + " mode=" + std::to_string(mode)
        + " offset=" + std::to_string(offset) + " length=" + std::to_string(length)
        + " fd=" + std::to_string(fd));

    try {
        if (offset < 0 || length < 0) {
            return logResult("fallocate", -EINVAL);
        }

        bool closeAfter = false;
        int targetFd = fd;
        if (targetFd < 0) {
            targetFd = ::open(fullPath(path).c_str(), O_RDWR);
            if (targetFd < 0) {
                return logResult("fallocate", errnoResult());
            }
            closeAfter = true;
        }

        struct stat st {};
        if (::fstat(targetFd, &st) != 0) {
            const int savedErrno = errno;
            if (closeAfter) {
                ::close(targetFd);
            }
            errno = savedErrno;
            return logResult("fallocate", errnoResult());
        }

        const auto endOffset = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(length);
        if (endOffset > static_cast<std::uint64_t>(st.st_size)) {
            imageManager_.ensureFreeForWrite(endOffset - static_cast<std::uint64_t>(st.st_size));
        } else {
            imageManager_.ensureFreeForWrite(static_cast<std::uint64_t>(length));
        }

        int result = ::fallocate(targetFd, mode, offset, length);
        if (result != 0 && errno == ENOSPC && imageManager_.growAfterNoSpace(static_cast<std::uint64_t>(length))) {
            result = ::fallocate(targetFd, mode, offset, length);
        }
        const int savedErrno = errno;
        if (closeAfter) {
            ::close(targetFd);
        }
        if (result != 0) {
            errno = savedErrno;
            log("fallocate failed: errno=" + std::to_string(savedErrno) + " message=" + std::strerror(savedErrno));
            return logResult("fallocate", errnoResult());
        }
        return logResult("fallocate", 0);
    } catch (...) {
        return logResult("fallocate", exceptionResult());
    }
}

int Operations::setxattr(const char* path, const char* name, const char* value, std::size_t size, int flags)
{
    log(std::string{"setxattr: "} + printablePath(path) + " name=" + (name == nullptr ? "<null>" : name)
        + " size=" + std::to_string(size) + " flags=" + std::to_string(flags));
    try {
        if (::lsetxattr(fullPath(path).c_str(), name, value, size, flags) != 0) {
            return logResult("setxattr", errnoResult());
        }
        return logResult("setxattr", 0);
    } catch (...) {
        return logResult("setxattr", exceptionResult());
    }
}

int Operations::getxattr(const char* path, const char* name, char* value, std::size_t size) const
{
    log(std::string{"getxattr: "} + printablePath(path) + " name=" + (name == nullptr ? "<null>" : name)
        + " size=" + std::to_string(size));
    try {
        const ssize_t result = ::lgetxattr(fullPath(path).c_str(), name, value, size);
        if (result < 0) {
            return logResult("getxattr", errnoResult());
        }
        return logResult("getxattr", static_cast<int>(result));
    } catch (...) {
        return logResult("getxattr", exceptionResult());
    }
}

int Operations::listxattr(const char* path, char* list, std::size_t size) const
{
    log(std::string{"listxattr: "} + printablePath(path) + " size=" + std::to_string(size));
    try {
        const ssize_t result = ::llistxattr(fullPath(path).c_str(), list, size);
        if (result < 0) {
            return logResult("listxattr", errnoResult());
        }
        return logResult("listxattr", static_cast<int>(result));
    } catch (...) {
        return logResult("listxattr", exceptionResult());
    }
}

int Operations::removexattr(const char* path, const char* name)
{
    log(std::string{"removexattr: "} + printablePath(path) + " name=" + (name == nullptr ? "<null>" : name));
    try {
        if (::lremovexattr(fullPath(path).c_str(), name) != 0) {
            return logResult("removexattr", errnoResult());
        }
        return logResult("removexattr", 0);
    } catch (...) {
        return logResult("removexattr", exceptionResult());
    }
}

int Operations::utimens(const char* path, const struct timespec tv[2], int fd)
{
    log(std::string{"utimens: "} + printablePath(path) + " fd=" + std::to_string(fd));
    try {
        const int result = fd >= 0
            ? ::futimens(fd, tv)
            : ::utimensat(AT_FDCWD, fullPath(path).c_str(), tv, AT_SYMLINK_NOFOLLOW);
        if (result != 0) {
            return logResult("utimens", errnoResult());
        }
        return logResult("utimens", 0);
    } catch (...) {
        return logResult("utimens", exceptionResult());
    }
}

int Operations::statfs(struct statvfs* st) const
{
    try {
        if (::statvfs(imageManager_.paths().internalMount.c_str(), st) != 0) {
            return logResult("statfs", errnoResult());
        }

        const auto info = imageManager_.spaceInfo();
        const auto parentInfo = imageManager_.parentSpaceInfo();
        const std::uint64_t fragmentSize = st->f_frsize != 0 ? st->f_frsize : st->f_bsize;
        const std::uint64_t quota = imageManager_.config().quotaSize;
        const std::uint64_t effectiveTotal = info.used >= quota
            ? quota
            : info.used + std::min(parentInfo.free, quota - info.used);
        const std::uint64_t fakeFreeBytes = effectiveTotal > info.used ? effectiveTotal - info.used : 0;
        const std::uint64_t fakeBlocks = effectiveTotal / fragmentSize;
        const std::uint64_t fakeFreeBlocks = fakeFreeBytes / fragmentSize;

        log("statfs: quota=" + app::formatSize(quota)
            + " effective_total=" + app::formatSize(effectiveTotal)
            + " reported_free=" + app::formatSize(fakeFreeBytes)
            + " btrfs_total=" + app::formatSize(info.total)
            + " btrfs_used=" + app::formatSize(info.used)
            + " btrfs_free=" + app::formatSize(info.free)
            + " parent_total=" + app::formatSize(parentInfo.total)
            + " parent_free=" + app::formatSize(parentInfo.free)
            + " fragment_size=" + std::to_string(fragmentSize)
            + " fake_blocks=" + std::to_string(fakeBlocks)
            + " fake_free_blocks=" + std::to_string(fakeFreeBlocks));

        st->f_blocks = fakeBlocks;
        st->f_bfree = fakeFreeBlocks;
        st->f_bavail = fakeFreeBlocks;
        return logResult("statfs", 0);
    } catch (...) {
        return logResult("statfs", exceptionResult());
    }
}

void Operations::prepareWrite(std::uint64_t handle, std::size_t size, off_t offset)
{
    const int fd = fdFromHandle(handle);
    if (fd < 0 || offset < 0) {
        throw util::Error("invalid write handle or offset");
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        throw util::Error(std::string{"fstat failed: "} + std::strerror(errno));
    }

    std::uint64_t bytesToAdd = 0;
    const auto endOffset = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(size);
    if (endOffset > static_cast<std::uint64_t>(st.st_size)) {
        bytesToAdd = endOffset - static_cast<std::uint64_t>(st.st_size);
    }
    imageManager_.ensureFreeForWrite(bytesToAdd);
}

void Operations::maybeShrinkAfterDelete()
{
    imageManager_.maybeShrink();
}

} // namespace personalfs::fuse
