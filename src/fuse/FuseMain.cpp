#define FUSE_USE_VERSION 35
#include "fuse/FuseMain.hpp"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <sys/xattr.h>
#include <unistd.h>
namespace personalfs
{
namespace
{
Store &store()
{
    return *static_cast<Store *>(fuse_get_context()->private_data);
}
template <class F> int call(F &&f)
{
    try
    {
        return f();
    }
    catch (const std::system_error &e)
    {
        store().recordError(e.code().value(), e.what());
        return -e.code().value();
    }
    catch (const std::bad_alloc &)
    {
        return -ENOMEM;
    }
    catch (...)
    {
        return -EIO;
    }
}
Handle &handle(fuse_file_info *fi)
{
    return *reinterpret_cast<Handle *>(fi->fh);
}
Id id(const char *path, fuse_file_info *fi)
{
    return fi ? handle(fi).id : store().resolve(path);
}
int getAttr(const char *path, struct stat *st, fuse_file_info *fi)
{
    return call(
        [&]
        {
            *st = store().getattr(id(path, fi), fi ? handle(fi).fd.get() : -1);
            return 0;
        });
}
int openFile(const char *path, fuse_file_info *fi)
{
    return call(
        [&]
        {
            fi->fh =
                reinterpret_cast<std::uint64_t>(store().open(store().resolve(path), fi->flags).release());
            return 0;
        });
}
int createFile(const char *path, mode_t mode, fuse_file_info *fi)
{
    return call(
        [&]
        {
            auto c = fuse_get_context();
            auto h = store().createOpen(path, mode, c->uid, c->gid, fi->flags);
            fi->fh = reinterpret_cast<std::uint64_t>(h.release());
            return 0;
        });
}
int releaseFile(const char *, fuse_file_info *fi)
{
    return call(
        [&]
        {
            store().release(std::unique_ptr<Handle>(&handle(fi)));
            fi->fh = 0;
            return 0;
        });
}
int readFile(const char *, char *b, size_t n, off_t off, fuse_file_info *fi)
{
    return call([&] { return static_cast<int>(store().read(handle(fi), b, n, off)); });
}
int writeFile(const char *, const char *b, size_t n, off_t off, fuse_file_info *fi)
{
    return call([&] { return static_cast<int>(store().write(handle(fi), b, n, off)); });
}
int syncFile(const char *, int dataOnly, fuse_file_info *fi)
{
    return call(
        [&]
        {
            store().sync(handle(fi), dataOnly);
            return 0;
        });
}
int flushFile(const char *, fuse_file_info *fi)
{
    return call(
        [&]
        {
            int fd = ::dup(handle(fi).fd.get());
            if (fd < 0)
                fail(errno, "flush dup");
            if (::close(fd))
                fail(errno, "flush close");
            return 0;
        });
}
int makeDir(const char *path, mode_t mode)
{
    return call(
        [&]
        {
            auto c = fuse_get_context();
            store().create(path, Kind::Directory, mode, c->uid, c->gid);
            return 0;
        });
}
int unlinkFile(const char *path)
{
    return call(
        [&]
        {
            store().remove(path, false);
            return 0;
        });
}
int removeDir(const char *path)
{
    return call(
        [&]
        {
            store().remove(path, true);
            return 0;
        });
}
int renameEntry(const char *a, const char *b, unsigned flags)
{
    return call(
        [&]
        {
            store().rename(a, b, flags);
            return 0;
        });
}
int truncateFile(const char *path, off_t size, fuse_file_info *fi)
{
    return call(
        [&]
        {
            store().truncate(id(path, fi), size, fi ? handle(fi).fd.get() : -1);
            return 0;
        });
}
int openDir(const char *path, fuse_file_info *fi)
{
    return call(
        [&]
        {
            fi->fh = reinterpret_cast<std::uint64_t>(store().openDirectory(store().resolve(path)).release());
            return 0;
        });
}
int readDir(const char *, void *buffer, fuse_fill_dir_t filler, off_t offset, fuse_file_info *fi,
            fuse_readdir_flags)
{
    return call(
        [&]
        {
            if (offset < 0)
                fail(EINVAL, "readdir cookie");
            auto &d = handle(fi);
            for (std::size_t i = offset; i < d.snapshot.size(); ++i)
            {
                struct stat st{};
                st.st_ino = d.snapshot[i].second.id + 1;
                st.st_mode = typeMode(d.snapshot[i].second.kind);
                if (filler(buffer, d.snapshot[i].first.c_str(), &st, i + 1,
                           static_cast<fuse_fill_dir_flags>(0)))
                    break;
            }
            return 0;
        });
}
int releaseDir(const char *path, fuse_file_info *fi)
{
    return releaseFile(path, fi);
}
int syncDir(const char *, int, fuse_file_info *fi)
{
    return call(
        [&]
        {
            store().syncDirectory(handle(fi).id);
            return 0;
        });
}
int statFs(const char *, struct statvfs *st)
{
    return call(
        [&]
        {
            *st = store().statfs();
            return 0;
        });
}
int symbolicLink(const char *target, const char *path)
{
    return call(
        [&]
        {
            auto c = fuse_get_context();
            store().create(path, Kind::Symlink, 0777, c->uid, c->gid, target);
            return 0;
        });
}
int readLink(const char *path, char *buffer, size_t size)
{
    return call(
        [&]
        {
            auto target = store().readlink(store().resolve(path));
            if (!size)
                fail(EINVAL, "readlink buffer");
            auto n = std::min(target.size(), size - 1);
            std::memcpy(buffer, target.data(), n);
            buffer[n] = 0;
            return 0;
        });
}
void owner(Id obj)
{
    auto c = fuse_get_context();
    if (c->uid && c->uid != store().metadata(obj).uid)
        fail(EPERM, "owner required");
}
int chmodFile(const char *path, mode_t mode, fuse_file_info *fi)
{
    return call(
        [&]
        {
            auto obj = id(path, fi);
            owner(obj);
            store().attributes(obj, mode, {}, {});
            return 0;
        });
}
int chownFile(const char *path, uid_t uid, gid_t gid, fuse_file_info *fi)
{
    return call(
        [&]
        {
            auto obj = id(path, fi);
            auto c = fuse_get_context();
            auto m = store().metadata(obj);
            if (c->uid)
            {
                owner(obj);
                if (uid != static_cast<uid_t>(-1) && uid != m.uid)
                    fail(EPERM, "changing owner requires root");
                if (gid != static_cast<gid_t>(-1) && gid != m.gid && gid != c->gid)
                {
                    int count = fuse_getgroups(0, nullptr);
                    if (count < 0)
                        fail(EPERM, "cannot verify groups");
                    std::vector<gid_t> groups(count);
                    if (fuse_getgroups(count, groups.data()) < 0 ||
                        std::find(groups.begin(), groups.end(), gid) == groups.end())
                        fail(EPERM, "group membership required");
                }
            }
            store().attributes(obj, {}, uid == static_cast<uid_t>(-1) ? std::optional<uid_t>{} : uid,
                               gid == static_cast<gid_t>(-1) ? std::optional<gid_t>{} : gid);
            return 0;
        });
}
int utimensFile(const char *path, const timespec times[2], fuse_file_info *fi)
{
    return call(
        [&]
        {
            store().times(id(path, fi), times);
            return 0;
        });
}
int getXattr(const char *path, const char *name, char *value, size_t size)
{
    return call(
        [&]
        {
            if (std::strcmp(path, "/") || std::strcmp(name, "user.personalfs.stats"))
                fail(EOPNOTSUPP, "xattrs unsupported");
            auto json = store().statistics();
            if (!size)
                return static_cast<int>(json.size());
            if (size < json.size())
                fail(ERANGE, "stats buffer");
            std::memcpy(value, json.data(), json.size());
            return static_cast<int>(json.size());
        });
}
void *init(fuse_conn_info *conn, fuse_config *config)
{
    config->use_ino = 1;
    config->entry_timeout = 0;
    config->attr_timeout = 0;
    config->negative_timeout = 0;
    config->nullpath_ok = 1;
    config->hard_remove = 1;
    conn->want &= ~FUSE_CAP_WRITEBACK_CACHE;
    return &store();
}
} // namespace
int mount(Store &backend, const std::string &mountpoint, const std::vector<std::string> &options)
{
    fuse_operations ops{};
    ops.init = init;
    ops.getattr = getAttr;
    ops.open = openFile;
    ops.create = createFile;
    ops.read = readFile;
    ops.write = writeFile;
    ops.release = releaseFile;
    ops.flush = flushFile;
    ops.fsync = syncFile;
    ops.mkdir = makeDir;
    ops.unlink = unlinkFile;
    ops.rmdir = removeDir;
    ops.rename = renameEntry;
    ops.truncate = truncateFile;
    ops.opendir = openDir;
    ops.readdir = readDir;
    ops.releasedir = releaseDir;
    ops.fsyncdir = syncDir;
    ops.statfs = statFs;
    ops.symlink = symbolicLink;
    ops.readlink = readLink;
    ops.chmod = chmodFile;
    ops.chown = chownFile;
    ops.utimens = utimensFile;
    ops.getxattr = getXattr;
    ops.link = [](const char *, const char *) { return -EOPNOTSUPP; };
    ops.mknod = [](const char *, mode_t, dev_t) { return -EOPNOTSUPP; };
    ops.setxattr = [](const char *, const char *, const char *, size_t, int) { return -EOPNOTSUPP; };
    ops.removexattr = [](const char *, const char *) { return -EOPNOTSUPP; };
    ops.listxattr = [](const char *, char *, size_t) { return -EOPNOTSUPP; };
    ops.fallocate = [](const char *, int, off_t, off_t, fuse_file_info *) { return -EOPNOTSUPP; };
    std::vector<std::string> args{"personalfs", mountpoint, "-o", "default_permissions,fsname=personalfs-v2"};
    args.insert(args.end(), options.begin(), options.end());
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(a.data());
    auto result = fuse_main(argv.size(), argv.data(), &ops, &backend);
    if (result == 0)
        backend.shutdown();
    return result;
}
} // namespace personalfs
