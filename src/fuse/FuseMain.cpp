#include "fuse/FuseMain.hpp"

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "fuse/Operations.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace personalfs::fuse {

namespace {

Operations& operations()
{
    return *static_cast<Operations*>(fuse_get_context()->private_data);
}

int pfs_getattr(const char* path, struct stat* st, struct fuse_file_info* fi)
{
    (void)fi;
    return operations().getattr(path, st);
}

int pfs_access(const char* path, int mask)
{
    return operations().access(path, mask);
}

int pfs_readlink(const char* path, char* buffer, std::size_t size)
{
    return operations().readlink(path, buffer, size);
}

int pfs_opendir(const char* path, struct fuse_file_info* fi)
{
    std::uint64_t handle = 0;
    const int result = operations().opendir(path, &handle);
    if (result == 0) {
        fi->fh = handle;
    }
    return result;
}

int pfs_readdir(const char* path, void* buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;
    return operations().readdir(path, buffer, reinterpret_cast<void*>(filler));
}

int pfs_releasedir(const char* path, struct fuse_file_info* fi)
{
    (void)path;
    return operations().releasedir(fi->fh);
}

int pfs_fsyncdir(const char* path, int datasync, struct fuse_file_info* fi)
{
    return operations().fsyncdir(path, fi != nullptr ? fi->fh : 0, datasync != 0);
}

int pfs_mknod(const char* path, mode_t mode, dev_t rdev)
{
    return operations().mknod(path, mode, rdev);
}

int pfs_mkdir(const char* path, mode_t mode)
{
    return operations().mkdir(path, mode);
}

int pfs_unlink(const char* path)
{
    return operations().unlink(path);
}

int pfs_rmdir(const char* path)
{
    return operations().rmdir(path);
}

int pfs_symlink(const char* target, const char* linkPath)
{
    return operations().symlink(target, linkPath);
}

int pfs_rename(const char* from, const char* to, unsigned int flags)
{
    return operations().rename(from, to, flags);
}

int pfs_link(const char* from, const char* to)
{
    return operations().link(from, to);
}

int pfs_chmod(const char* path, mode_t mode, struct fuse_file_info* fi)
{
    (void)fi;
    return operations().chmod(path, mode);
}

int pfs_chown(const char* path, uid_t uid, gid_t gid, struct fuse_file_info* fi)
{
    (void)fi;
    return operations().chown(path, uid, gid);
}

int pfs_truncate(const char* path, off_t size, struct fuse_file_info* fi)
{
    return operations().truncate(path, size, fi != nullptr ? static_cast<int>(fi->fh) : -1);
}

int pfs_open(const char* path, struct fuse_file_info* fi)
{
    std::uint64_t handle = 0;
    const int result = operations().open(path, fi->flags, &handle);
    if (result == 0) {
        fi->fh = handle;
    }
    return result;
}

int pfs_create(const char* path, mode_t mode, struct fuse_file_info* fi)
{
    std::uint64_t handle = 0;
    const int result = operations().create(path, mode, fi->flags, &handle);
    if (result == 0) {
        fi->fh = handle;
    }
    return result;
}

int pfs_read(const char* path, char* buffer, std::size_t size, off_t offset, struct fuse_file_info* fi)
{
    (void)path;
    return operations().read(fi->fh, buffer, size, offset);
}

int pfs_write(const char* path, const char* buffer, std::size_t size, off_t offset, struct fuse_file_info* fi)
{
    (void)path;
    return operations().write(fi->fh, buffer, size, offset);
}

int pfs_release(const char* path, struct fuse_file_info* fi)
{
    (void)path;
    return operations().release(fi->fh);
}

int pfs_fsync(const char* path, int datasync, struct fuse_file_info* fi)
{
    (void)path;
    return operations().fsync(fi->fh, datasync != 0);
}

int pfs_fallocate(const char* path, int mode, off_t offset, off_t length, struct fuse_file_info* fi)
{
    return operations().fallocate(path, mode, offset, length, fi != nullptr ? static_cast<int>(fi->fh) : -1);
}

int pfs_setxattr(const char* path, const char* name, const char* value, std::size_t size, int flags)
{
    return operations().setxattr(path, name, value, size, flags);
}

int pfs_getxattr(const char* path, const char* name, char* value, std::size_t size)
{
    return operations().getxattr(path, name, value, size);
}

int pfs_listxattr(const char* path, char* list, std::size_t size)
{
    return operations().listxattr(path, list, size);
}

int pfs_removexattr(const char* path, const char* name)
{
    return operations().removexattr(path, name);
}

int pfs_utimens(const char* path, const struct timespec tv[2], struct fuse_file_info* fi)
{
    return operations().utimens(path, tv, fi != nullptr ? static_cast<int>(fi->fh) : -1);
}

int pfs_statfs(const char* path, struct statvfs* st)
{
    (void)path;
    return operations().statfs(st);
}

} // namespace

int runFuse(int argc, char** argv, storage::ImageManager& imageManager)
{
    Operations ops{imageManager};
    fuse_operations callbacks {};
    callbacks.getattr = pfs_getattr;
    callbacks.access = pfs_access;
    callbacks.readlink = pfs_readlink;
    callbacks.opendir = pfs_opendir;
    callbacks.readdir = pfs_readdir;
    callbacks.releasedir = pfs_releasedir;
    callbacks.fsyncdir = pfs_fsyncdir;
    callbacks.mknod = pfs_mknod;
    callbacks.mkdir = pfs_mkdir;
    callbacks.unlink = pfs_unlink;
    callbacks.rmdir = pfs_rmdir;
    callbacks.symlink = pfs_symlink;
    callbacks.rename = pfs_rename;
    callbacks.link = pfs_link;
    callbacks.chmod = pfs_chmod;
    callbacks.chown = pfs_chown;
    callbacks.truncate = pfs_truncate;
    callbacks.open = pfs_open;
    callbacks.create = pfs_create;
    callbacks.read = pfs_read;
    callbacks.write = pfs_write;
    callbacks.release = pfs_release;
    callbacks.fsync = pfs_fsync;
    callbacks.fallocate = pfs_fallocate;
    callbacks.setxattr = pfs_setxattr;
    callbacks.getxattr = pfs_getxattr;
    callbacks.listxattr = pfs_listxattr;
    callbacks.removexattr = pfs_removexattr;
    callbacks.utimens = pfs_utimens;
    callbacks.statfs = pfs_statfs;

    return fuse_main(argc, argv, &callbacks, &ops);
}

} // namespace personalfs::fuse
