#include "storage/Store.hpp"
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace personalfs;
namespace
{
void require(bool b, const char *message)
{
    if (!b)
        throw std::runtime_error(message);
}
template <class F> void error(int code, F f)
{
    try
    {
        f();
    }
    catch (const std::system_error &e)
    {
        if (e.code().value() == code)
            return;
        throw;
    }
    throw std::runtime_error("expected error not returned");
}
struct Temp
{
    std::string path;
    Temp()
    {
        char p[] = "/tmp/personalfs-v2-test-XXXXXX";
        auto s = ::mkdtemp(p);
        if (!s)
            fail(errno, "mkdtemp");
        path = s;
        Store::init(path, 2);
    }
    ~Temp()
    {
        std::filesystem::remove_all(path);
    }
};
Id file(Store &s, const std::string &p)
{
    return s.create(p, Kind::File, 0640, ::getuid(), ::getgid());
}
Id dir(Store &s, const std::string &p)
{
    return s.create(p, Kind::Directory, 0750, ::getuid(), ::getgid());
}
void contents(Store &s, Id id, const std::string &text)
{
    auto h = s.open(id, O_RDWR);
    require(s.write(*h, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size()), "write size");
    s.sync(*h, false);
    std::string out(text.size(), ' ');
    require(s.read(*h, out.data(), out.size(), 0) == static_cast<ssize_t>(out.size()) && out == text,
            "data roundtrip");
    s.release(std::move(h));
}
void format()
{
    auto super = Super{}.encode();
    require(Super::decode(super).pageSize == 4096, "superblock roundtrip");
    for (std::size_t n = 0; n < super.size(); ++n)
        error(EIO, [&] { Super::decode(Bytes(super.begin(), super.begin() + n)); });
    super[17] ^= 4;
    error(EIO, [&] { Super::decode(super); });
    Bytes test{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    require(crc32c(test) == 0xe3069283, "CRC32C test vector");
}
void functional()
{
    Temp t;
    Id surviving;
    {
        Store s(t.path);
        auto a = dir(s, "/a"), b = dir(s, "/b");
        auto f = file(s, "/a/file");
        contents(s, f, "native bytes without header");
        std::ifstream raw(t.path + "/" + s.physicalName(f));
        std::string text((std::istreambuf_iterator<char>(raw)), {});
        require(text == "native bytes without header", "raw object contents");
        auto h = s.open(f, O_RDWR);
        s.rename("/a/file", "/b/new");
        require(s.resolve("/b/new") == f, "stable ID after rename");
        error(ENOENT, [&] { s.resolve("/a/file"); });
        s.remove("/b/new", false);
        require(s.getattr(f, h->fd.get()).st_nlink == 0, "open unlink nlink");
        char data[64]{};
        require(s.read(*h, data, 64, 0) ==
                    static_cast<ssize_t>(std::string("native bytes without header").size()),
                "open unlink data");
        s.write(*h, "X", 1, 0);
        s.sync(*h, true);
        s.release(std::move(h));
        error(ENOENT, [&] { s.metadata(f); });
        auto child = dir(s, "/a/sub");
        dir(s, "/a/sub/deep");
        error(EINVAL, [&] { s.rename("/a", "/a/sub/deep/loop"); });
        error(ENOTEMPTY, [&] { s.remove("/a", true); });
        s.rename("/a/sub", "/b/sub");
        require(s.metadata(child).parent == b, "directory parent update");
        auto target = file(s, "/b/target");
        auto opened = s.open(target, O_RDONLY);
        surviving = file(s, "/a/source");
        contents(s, surviving, "replacement");
        error(EEXIST, [&] { s.rename("/a/source", "/b/target", 1); });
        s.rename("/a/source", "/b/target");
        require(s.resolve("/b/target") == surviving, "replace identity");
        s.release(std::move(opened));
        s.truncate(surviving, 4);
        require(s.getattr(surviving).st_size == 4, "truncate");
        auto large = file(s, "/large-offset");
        auto largeHandle = s.open(large, O_RDWR);
        const off_t end = 30LL * 1024 * 1024 * 1024;
        s.write(*largeHandle, "z", 1, end - 1);
        require(s.getattr(large).st_size == end, "64-bit offset beyond 30 GiB");
        char last;
        require(s.read(*largeHandle, &last, 1, end - 1) == 1 && last == 'z', "64-bit offset data");
        s.remove("/large-offset", false);
        s.release(std::move(largeHandle));
        s.attributes(surviving, 0600, {}, {});
        require((s.getattr(surviving).st_mode & 0777) == 0600, "chmod");
        timespec times[2]{{100, 123}, {200, 456}};
        s.times(surviving, times);
        require(s.getattr(surviving).st_mtim.tv_nsec == 456, "nanosecond timestamps");
        auto sy = s.create("/link", Kind::Symlink, 0777, ::getuid(), ::getgid(), "b/target");
        require(s.readlink(sy) == "b/target", "symlink");
        std::string name = "/a/";
        name.push_back(static_cast<char>(0xff));
        file(s, name);
        require(s.resolve(name) > 0, "byte names");
        auto snapshot = s.list(a);
        file(s, "/a/later");
        require(!snapshot.contains("later"), "snapshot isolation");
        auto empty = dir(s, "/open-directory");
        auto directoryHandle = s.openDirectory(empty);
        s.remove("/open-directory", true);
        require(directoryHandle->snapshot.size() == 2, "unlinked directory snapshot retained");
        s.syncDirectory(empty);
        s.release(std::move(directoryHandle));
        error(ENOENT, [&] { s.metadata(empty); });
        s.compact(a);
        file(s, "/a/after-compact");
        require(s.fsck().find("true") != std::string::npos, "fsck");
        error(EWOULDBLOCK, [&] { Store second(t.path); });
        require(s.physicalName(1) == "0000000000000000/1.obj" &&
                    s.physicalName(3) == "0000000000000001/3.obj",
                "page mapping");
    }
    {
        Store s(t.path);
        require(s.resolve("/b/target") == surviving, "restart namespace");
        s.fsck();
    }
}
void deepAndCache()
{
    Temp t;
    Store s(t.path, Store::Mode::ReadWrite, 1024);
    std::string p;
    for (int i = 0; i < 80; ++i)
    {
        p += "/" + std::string(60, 'x');
        dir(s, p);
    }
    file(s, p + "/file");
    require(s.resolve(p + "/file") > 0, "deep path resolution");
    auto large = dir(s, "/large");
    for (int i = 0; i < 100; ++i)
        file(s, "/large/f" + std::to_string(i));
    require(s.resolve("/large/f99") > 0, "cache bypass lookup");
    require(s.list(large).size() == 100, "oversized listing");
    s.fsck();
    auto stats = s.statistics();
    require(stats.find("cache_bypasses") != stats.npos, "cache bypass measured");
    for (auto &e : std::filesystem::recursive_directory_iterator(t.path))
    {
        auto relative = e.path().lexically_relative(t.path);
        require(std::distance(relative.begin(), relative.end()) <= 2, "fixed physical depth");
    }
}
void concurrent()
{
    Temp t;
    Store s(t.path);
    dir(s, "/a");
    dir(s, "/b");
    auto id = file(s, "/a/item");
    auto h = s.open(id, O_RDWR);
    std::exception_ptr problem;
    std::mutex mutex;
    auto run = [&](auto fn)
    {
        try
        {
            fn();
        }
        catch (...)
        {
            std::lock_guard l(mutex);
            problem = std::current_exception();
        }
    };
    std::thread mover(
        [&]
        {
            run(
                [&]
                {
                    for (int i = 0; i < 30; ++i)
                    {
                        s.rename("/a/item", "/b/item");
                        s.rename("/b/item", "/a/item");
                    }
                });
        });
    std::thread writer(
        [&]
        {
            run(
                [&]
                {
                    std::string bytes(8192, 'a');
                    for (int i = 0; i < 200; ++i)
                        s.write(*h, bytes.data(), bytes.size(), i * 8192);
                    s.sync(*h, false);
                });
        });
    std::thread creator(
        [&]
        {
            run(
                [&]
                {
                    for (int i = 0; i < 30; ++i)
                    {
                        auto p = "/b/f" + std::to_string(i);
                        file(s, p);
                        s.remove(p, false);
                    }
                });
        });
    mover.join();
    writer.join();
    creator.join();
    if (problem)
        std::rethrow_exception(problem);
    require(s.getattr(id).st_size == 200 * 8192, "concurrent data size");
    s.release(std::move(h));
    s.fsck();
}
void fastPathAndBudget()
{
    Temp t;
    Store s(t.path, Store::Mode::ReadWrite, 1);
    auto id = file(s, "/data");
    auto handle = s.open(id, O_RDWR);
    auto before = s.metrics.counters["openat_calls"];
    for (int i = 0; i < 100; ++i)
        s.write(*handle, "x", 1, i);
    require(s.metrics.counters["openat_calls"] == before, "data write performed namespace I/O");
    s.list(0);
    auto stats = s.statistics();
    require(stats.find("\"cache_bytes\":0") != std::string::npos, "tiny cache budget exceeded");
    s.release(std::move(handle));
    s.shutdown();
    require(std::filesystem::exists(t.path + "/stats.json"), "shutdown statistics");
}
bool crash(const std::string &point, const std::string &operation, bool allowCompletion = false)
{
    Temp t;
    Id src, dst;
    {
        Store s(t.path);
        dir(s, "/a");
        dir(s, "/b");
        src = file(s, "/a/src");
        dst = file(s, "/b/dst");
        contents(s, src, "source");
        contents(s, dst, "destination");
    }
    pid_t pid = ::fork();
    if (pid < 0)
        fail(errno, "fork");
    if (pid == 0)
    {
        try
        {
            Store s(t.path);
            if (point.starts_with('#'))
                ::setenv("PERSONALFS_KILL_AFTER", point.c_str() + 1, 1);
            else
                ::setenv("PERSONALFS_FAULT", point.c_str(), 1);
            if (operation == "rename")
                s.rename("/a/src", "/b/dst");
            else if (operation == "create")
                file(s, "/a/new");
            else if (operation == "unlink")
                s.remove("/a/src", false);
            else
                s.compact(s.resolve("/a"));
            ::_exit(0);
        }
        catch (...)
        {
            ::_exit(10);
        }
    }
    int status;
    ::waitpid(pid, &status, 0);
    bool completed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    require((WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) || (allowCompletion && completed),
            "fault boundary not reached");
    {
        Store recovered(t.path);
        recovered.fsck();
        if (operation == "rename")
        {
            auto entries = recovered.list(recovered.resolve("/a"));
            if (entries.contains("src"))
            {
                require(recovered.resolve("/a/src") == src && recovered.resolve("/b/dst") == dst,
                        "rename old state");
            }
            else
                require(recovered.resolve("/b/dst") == src, "rename REDO identity");
        }
        if (operation == "create")
        {
            auto es = recovered.list(recovered.resolve("/a"));
            if (es.contains("new"))
                require(es.at("new").id > dst, "reserved ID not reused");
        }
    }
    {
        Store repeat(t.path);
        repeat.fsck();
    }
    return completed;
}
void corruption()
{
    Temp t;
    Id d;
    {
        Store s(t.path);
        d = dir(s, "/d");
        file(s, "/d/f");
    }
    std::string path;
    {
        Store s(t.path, Store::Mode::Check);
        path = t.path + "/" + s.physicalName(d);
    }
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(directoryHeaderSize + 15);
        f.put('!');
    }
    error(EIO,
          [&]
          {
              Store s(t.path, Store::Mode::Check);
              s.fsck();
          });
}
void pendingReadOnly()
{
    Temp t;
    pid_t pid = ::fork();
    if (!pid)
    {
        Store s(t.path);
        ::setenv("PERSONALFS_FAULT", "begin", 1);
        file(s, "/pending");
        ::_exit(0);
    }
    int status;
    ::waitpid(pid, &status, 0);
    error(EUCLEAN, [&] { Store s(t.path, Store::Mode::Check); });
    {
        Store s(t.path);
        require(s.resolve("/pending") > 0, "pending redo");
        s.fsck();
    }
}
void ioErrors()
{
    for (int code : {ENOSPC, EDQUOT, EIO, EACCES})
    {
        Temp t;
        {
            Store s(t.path);
            auto id = file(s, "/file");
            auto h = s.open(id, O_RDWR);
            auto text = std::to_string(code);
            ::setenv("PERSONALFS_FAIL_ERRNO", text.c_str(), 1);
            ::setenv("PERSONALFS_FAIL_AT", "data-write", 1);
            error(code, [&] { s.write(*h, "x", 1, 0); });
            ::unsetenv("PERSONALFS_FAIL_AT");
            require(s.getattr(id).st_size == 0, "failed write not reported as success");
            ::setenv("PERSONALFS_FAIL_AT", "fsync", 1);
            error(code, [&] { s.sync(*h, false); });
            ::unsetenv("PERSONALFS_FAIL_AT");
            s.release(std::move(h));
            ::setenv("PERSONALFS_FAIL_AT", "directory-written", 1);
            error(code, [&] { file(s, "/redo"); });
            ::unsetenv("PERSONALFS_FAIL_AT");
            error(EIO, [&] { s.resolve("/redo"); });
            error(EIO, [&] { file(s, "/blocked"); });
        }
        {
            Store s(t.path);
            require(s.resolve("/redo") > 0, "failed metadata transaction redone");
            s.fsck();
        }
        ::unsetenv("PERSONALFS_FAIL_ERRNO");
    }
}
void partialTail()
{
    Temp t;
    std::string path;
    {
        Store s(t.path);
        auto id = dir(s, "/a");
        path = t.path + "/" + s.physicalName(id);
    }
    auto pid = ::fork();
    if (!pid)
    {
        Store s(t.path);
        ::setenv("PERSONALFS_FAULT", "directory-written", 1);
        file(s, "/a/new");
        ::_exit(0);
    }
    int status;
    ::waitpid(pid, &status, 0);
    auto size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size - 7);
    {
        Store s(t.path);
        require(s.resolve("/a/new") > 0, "partial unconfirmed tail redone");
        s.fsck();
    }
    size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size - 7);
    error(EIO,
          [&]
          {
              Store s(t.path, Store::Mode::Check);
              s.fsck();
          });
}
void symlinkDefense()
{
    Temp t;
    {
        Store s(t.path);
        file(s, "/file");
    }
    auto page = t.path + "/0000000000000000";
    std::filesystem::rename(page, page + "-saved");
    std::filesystem::create_directory_symlink(page + "-saved", page);
    try
    {
        Store s(t.path);
        s.resolve("/file");
        throw std::runtime_error("physical symlink accepted");
    }
    catch (const std::system_error &e)
    {
        require(e.code().value() == ENOTDIR || e.code().value() == ELOOP, "physical symlink rejected");
    }
}
void repeatedRecovery()
{
    Temp t;
    Id source;
    {
        Store s(t.path);
        dir(s, "/a");
        dir(s, "/b");
        source = file(s, "/a/source");
        file(s, "/b/destination");
    }
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        auto pid = ::fork();
        if (pid == 0)
        {
            ::setenv("PERSONALFS_FAULT", "action-3", 1);
            Store s(t.path);
            s.rename("/a/source", "/b/destination");
            ::_exit(10);
        }
        int status;
        ::waitpid(pid, &status, 0);
        require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "repeated recovery fault");
    }
    Store s(t.path);
    require(s.resolve("/b/destination") == source, "repeated REDO preserved identity");
    error(ENOENT, [&] { s.resolve("/a/source"); });
    s.fsck();
}
void tornRecord(bool committed)
{
    Temp t;
    auto pid = ::fork();
    if (pid == 0)
    {
        Store s(t.path);
        ::setenv("PERSONALFS_FAULT", committed ? "commit" : "directory-written", 1);
        file(s, "/new");
        ::_exit(10);
    }
    int status;
    ::waitpid(pid, &status, 0);
    std::fstream f(t.path + "/0.obj", std::ios::binary | std::ios::in | std::ios::out);
    f.seekg(-1, std::ios::end);
    char value;
    f.get(value);
    f.seekp(-1, std::ios::end);
    f.put(value ^ 1);
    f.close();
    if (committed)
        error(EIO, [&] { Store s(t.path); });
    else
    {
        Store s(t.path);
        require(s.resolve("/new") != 0, "torn uncommitted suffix restored");
        s.fsck();
    }
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--exhaustive")
        {
            for (auto operation : {"create", "rename", "unlink", "compact"})
            {
                bool finished = false;
                for (unsigned boundary = 1; boundary < 512; ++boundary)
                {
                    if (crash("#" + std::to_string(boundary), operation, true))
                    {
                        std::cout << operation << ": " << boundary - 1 << " interruption boundaries passed\n";
                        finished = true;
                        break;
                    }
                }
                require(finished, "exhaustive fault sweep did not reach completion");
            }
            return 0;
        }
        format();
        functional();
        deepAndCache();
        concurrent();
        fastPathAndBudget();
        pendingReadOnly();
        corruption();
        ioErrors();
        partialTail();
        symlinkDefense();
        repeatedRecovery();
        tornRecord(false);
        tornRecord(true);
        for (auto point : {"id-reserved", "temp-created", "temp-written", "temp-synced", "temp-published",
                           "begin", "action-1", "action-2", "action-3", "action-4", "action-5", "action-6",
                           "directory-written", "commit", "journal-clean", "journal.temp-synced",
                           "journal.temp-published", "1.meta.temp-synced", "1.meta.temp-published"})
            crash(point, "rename");
        for (auto point : {"id-reserved", "begin", "object-created", "action-1", "action-2", "action-3",
                           "action-4", "commit", "journal-clean"})
            crash(point, "create");
        for (auto point : {"begin", "directory-written", "commit", "journal-clean", "gc-object",
                           "gc-object-synced", "gc-meta"})
            crash(point, "unlink");
        crash("compact-synced", "compact");
        crash("compact-published", "compact");
        std::cout << "v2 format, namespace, data, cache, concurrency, I/O errors, integrity and 38 SIGKILL "
                     "scenarios passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
