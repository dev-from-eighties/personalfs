#include "storage/Store.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>
namespace personalfs
{
namespace
{
int checked(int result, const char *operation)
{
    if (result < 0)
        fail(errno, operation);
    return result;
}
void writeAll(int fd, const Bytes &bytes, off_t offset = 0, Metrics *metrics = nullptr)
{
    std::size_t done = 0;
    while (done < bytes.size())
    {
        if (metrics)
        {
            metrics->count("metadata_pwrite_calls");
            metrics->sample("metadata_write_size", bytes.size() - done);
        }
        auto n = ::pwrite(fd, bytes.data() + done, bytes.size() - done, offset + done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            fail(n < 0 ? errno : EIO, "pwrite metadata");
        done += n;
    }
}
Bytes readAt(int fd, std::size_t count, off_t offset, Metrics *metrics = nullptr)
{
    Bytes b(count);
    std::size_t done = 0;
    while (done < count)
    {
        if (metrics)
        {
            metrics->count("metadata_pread_calls");
            metrics->sample("metadata_read_size", count - done);
        }
        auto n = ::pread(fd, b.data() + done, count - done, offset + done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            fail(errno, "pread metadata");
        if (!n)
            break;
        done += n;
    }
    b.resize(done);
    return b;
}
std::uint64_t fileSize(int fd)
{
    struct stat s{};
    checked(::fstat(fd, &s), "fstat");
    check(S_ISREG(s.st_mode), "expected regular physical file");
    return s.st_size;
}
std::vector<std::string> names(int fd, Metrics *metrics = nullptr)
{
    if (metrics)
        metrics->count("openat_calls");
    Fd copy(checked(::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC), "open directory enumeration"));
    DIR *d = ::fdopendir(::dup(copy.get()));
    if (!d)
        fail(errno, "fdopendir");
    std::vector<std::string> result;
    errno = 0;
    while (auto e = ::readdir(d))
    {
        std::string n = e->d_name;
        if (n != "." && n != "..")
            result.push_back(n);
    }
    int error = errno;
    ::closedir(d);
    if (error)
        fail(error, "readdir physical");
    return result;
}
std::string pageName(Id id, std::uint32_t pageSize)
{
    std::ostringstream o;
    o << std::hex << std::setw(16) << std::setfill('0') << ((id - 1) / pageSize);
    return o.str();
}
void validName(const std::string &n)
{
    if (n.empty() || n == "." || n == ".." || n.find('/') != n.npos || n.find('\0') != n.npos)
        fail(EINVAL, "invalid name");
    if (n.size() > 255)
        fail(ENAMETOOLONG, "virtual name exceeds NAME_MAX");
}
} // namespace
Fd::~Fd()
{
    if (fd_ >= 0)
        ::close(fd_);
}
Fd::Fd(Fd &&o) noexcept : fd_(o.fd_)
{
    o.fd_ = -1;
}
Fd &Fd::operator=(Fd &&o) noexcept
{
    if (this != &o)
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}
void fault(const char *point)
{
#ifdef PERSONALFS_TESTING
    static std::uint64_t boundary = 0;
    const char *count = std::getenv("PERSONALFS_KILL_AFTER");
    if (count && ++boundary == std::stoull(count))
    {
        ::kill(::getpid(), SIGKILL);
        ::_exit(99);
    }
    const char *p = std::getenv("PERSONALFS_FAULT");
    if (p && std::string(p) == point)
    {
        ::kill(::getpid(), SIGKILL);
        ::_exit(99);
    }
    p = std::getenv("PERSONALFS_FAIL_AT");
    if (p && std::string(p) == point)
    {
        const char *code = std::getenv("PERSONALFS_FAIL_ERRNO");
        fail(code ? std::stoi(code) : EIO, "injected I/O failure");
    }
#else
    (void)point;
#endif
}
void Metrics::count(const std::string &name, std::uint64_t amount) noexcept
{
    try
    {
        std::lock_guard l(mutex);
        counters[name] += amount;
    }
    catch (...)
    {
    } // Instrumentation must never change filesystem results.
}
void Metrics::sample(const std::string &name, std::uint64_t ns) noexcept
{
    try
    {
        std::lock_guard l(mutex);
        auto &h = histograms[name];
        ++h.count;
        h.totalNs += ns;
        h.maxNs = std::max(h.maxNs, ns);
        ++h.bins[std::min<unsigned>(63, std::bit_width(ns))];
        if (ns > 1000000000 && !name.ends_with("_size"))
            ++counters["slow_operations_over_1s"];
    }
    catch (...)
    {
    }
}
std::string Metrics::json() const
{
    std::lock_guard l(mutex);
    std::ostringstream o;
    o << "{\"timestamp_ns\":";
    auto t = now();
    o << std::uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec << ",\"counters\":{";
    bool first = true;
    for (auto &[k, v] : counters)
    {
        if (!first)
            o << ',';
        first = false;
        o << '"' << k << "\":" << v;
    }
    o << "},\"histograms\":{";
    first = true;
    for (auto &[k, h] : histograms)
    {
        if (!first)
            o << ',';
        first = false;
        o << '"' << k << "\":{\"unit\":\"" << (k.ends_with("_size") ? "bytes" : "ns")
          << "\",\"count\":" << h.count << ",\"total\":" << h.totalNs << ",\"max\":" << h.maxNs;
        for (auto p : {50, 95, 99})
        {
            std::uint64_t n = 0, bound = 0;
            for (unsigned i = 0; i < 64; ++i)
            {
                n += h.bins[i];
                if (n >= (h.count * p + 99) / 100)
                {
                    bound = i == 63 ? UINT64_MAX : (1ULL << i);
                    break;
                }
            }
            o << ",\"p" << p << "_upper\":" << bound;
        }
        o << '}';
    }
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    auto value = [&](const std::string &key)
    {
        auto i = counters.find(key);
        return i == counters.end() ? 0 : i->second;
    };
    o << "},\"elapsed_seconds\":" << seconds
      << ",\"read_mib_s\":" << value("read_bytes") / 1048576.0 / seconds
      << ",\"write_mib_s\":" << value("write_bytes") / 1048576.0 / seconds << ",\"operations_per_second\":{";
    first = true;
    for (auto &[name, h] : histograms)
    {
        if (name.ends_with("_size"))
            continue;
        if (!first)
            o << ',';
        first = false;
        o << '"' << name << "\":" << h.count / seconds;
    }
    o << "}}";
    return o.str();
}
Timer::~Timer()
{
    metrics_.sample(
        name_, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_)
                   .count());
}
void Store::syncFd(int fd, bool dataOnly)
{
    Timer t(metrics, dataOnly ? "fdatasync" : "fsync");
    metrics.count(dataOnly ? "fdatasync_calls" : "fsync_calls");
    fault("fsync");
    checked(dataOnly ? ::fdatasync(fd) : ::fsync(fd), "sync");
    fault("fsync-complete");
}
int Store::openAt(int directory, const char *name, int flags, mode_t mode)
{
    metrics.count("openat_calls");
    auto start = std::chrono::steady_clock::now();
    int result = ::openat(directory, name, flags, mode);
    int error = errno;
    metrics.sample("openat", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
    errno = error;
    return result;
}
Bytes Store::readFile(int dir, const std::string &name, std::size_t limit)
{

    Fd f(checked(openAt(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW), "open metadata"));
    auto size = fileSize(f.get());
    check(size <= limit, "oversized metadata");
    auto b = readAt(f.get(), size, 0, &metrics);
    check(b.size() == size, "short metadata read");
    return b;
}
void Store::atomicFile(int dir, const std::string &name, const Bytes &bytes)
{
    Timer timer(metrics, name == "journal"      ? "journal_publish"
                         : name == "superblock" ? "allocator_publish"
                                                : "metadata_publish");
    if (name == "journal")
        metrics.count("journal_bytes_written", bytes.size());
    auto stamp = now();
    auto tmp = ".pfs-" + std::to_string(::getpid()) + "-" + std::to_string(stamp.tv_nsec) + "-" +
               std::to_string(++tempCounter_) + ".tmp";
    Fd f(checked(openAt(staging_.get(), tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600),
                 "create metadata temporary"));
    try
    {
        fault("temp-created");
        writeAll(f.get(), bytes, 0, &metrics);
        fault("temp-written");
        syncFd(f.get());
        fault("temp-synced");
        fault((name + ".temp-synced").c_str());
        checked(::renameat(staging_.get(), tmp.c_str(), dir, name.c_str()), "publish metadata");
        metrics.count("renameat_calls");
        fault("temp-published");
        fault((name + ".temp-published").c_str());
        syncFd(dir);
        syncFd(staging_.get());
    }
    catch (...)
    {
        ::unlinkat(staging_.get(), tmp.c_str(), 0);
        throw;
    }
}
void Store::init(const std::string &path, std::uint32_t pageSize)
{
    if (pageSize < 1 || pageSize > 16384)
        fail(EINVAL, "objects-per-page must be between 1 and 16384");
    if (::mkdir(path.c_str(), 0700) && errno != EEXIST)
        fail(errno, "create store (parent must exist)");
    Fd root(checked(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), "open store"));
    struct stat st{};
    checked(::fstat(root.get(), &st), "stat store");
    if (st.st_uid != ::geteuid() || (st.st_mode & 0077))
        fail(EACCES, "store must be owned by daemon user with permissions 0700");
    if (!names(root.get()).empty())
        fail(EEXIST, "init requires an empty store");
    auto absolute = std::filesystem::canonical(path).string();
    long maxPath = ::fpathconf(root.get(), _PC_PATH_MAX), maxName = ::fpathconf(root.get(), _PC_NAME_MAX);
    if ((maxPath > 0 && absolute.size() + 1 + 16 + 1 + 20 + 5 + 32 >= static_cast<std::size_t>(maxPath)) ||
        (maxName > 0 && maxName < 64))
        fail(ENAMETOOLONG,
             "store is too deep or backend component limit too small; choose a shorter protected path");
    // Probe the complete page depth and longest transient component on the actual backend.
    const char *probePage = "ffffffffffffffff";
    checked(::mkdirat(root.get(), probePage, 0700), "backend page limit probe");
    Fd probeDirectory(
        checked(::openat(root.get(), probePage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                "open backend probe page"));
    std::string probe(64, 'p');
    try
    {
        Fd probeFd(checked(
            ::openat(probeDirectory.get(), probe.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600),
            "backend name/path limit probe; choose a shorter protected store path"));
        checked(::unlinkat(probeDirectory.get(), probe.c_str(), 0), "remove probe");
        checked(::unlinkat(root.get(), probePage, AT_REMOVEDIR), "remove probe page");
    }
    catch (...)
    {
        ::unlinkat(probeDirectory.get(), probe.c_str(), 0);
        ::unlinkat(root.get(), probePage, AT_REMOVEDIR);
        throw;
    }
    auto put = [&](const char *name, const Bytes &b)
    {
        Fd fd(checked(::openat(root.get(), name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600),
                      "initialize file"));
        writeAll(fd.get(), b);
        checked(::fsync(fd.get()), "sync initialization");
    };
    Meta m;
    m.uid = ::getuid();
    m.gid = ::getgid();
    m.atime = m.mtime = m.ctime = now();
    checked(::mkdirat(root.get(), "staging", 0700), "initialize staging");
    checked(::mkdirat(root.get(), "pending", 0700), "initialize pending");
    put("lock", {});
    put("0.obj", directoryHeader(0));
    put("0.meta", m.encode());
    put("journal", {});
    Super s;
    s.pageSize = pageSize;
    put("superblock", s.encode());
    checked(::fsync(root.get()), "sync new store");
    Fd parent(checked(
        ::open(std::filesystem::path(absolute).parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC),
        "open store parent"));
    checked(::fsync(parent.get()), "sync store parent");
}
Store::Store(const std::string &path, Mode mode, std::size_t cacheBytes)
    : writable_(mode == Mode::ReadWrite), cacheLimit_(cacheBytes)
{
    root_ = Fd(checked(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), "open store"));
    struct stat st{};
    checked(::fstat(root_.get(), &st), "stat store");
    if (st.st_uid != ::geteuid() || (st.st_mode & 0077))
        fail(EACCES, "store must be owned by daemon user with permissions 0700");
    lock_ = Fd(checked(openAt(root_.get(), "lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC), "open store lock"));
    checked(::flock(lock_.get(), LOCK_EX | LOCK_NB), "store already in use");
    super_ = Super::decode(readFile(root_.get(), "superblock", 4096));
    if (writable_)
    {
        const bool migrating = super_.minorVersion == 0;
        for (const char *name : {"staging", "pending"})
        {
            if (migrating && ::mkdirat(root_.get(), name, 0700) && errno != EEXIST)
                fail(errno, "create recovery directory");
        }
        if (migrating)
            syncFd(root_.get());
        staging_ = Fd(checked(openAt(root_.get(), "staging", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), "open staging"));
        pending_ = Fd(checked(openAt(root_.get(), "pending", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), "open pending"));
        recover();
        if (migrating)
        {
            Timer timer(metrics, "migration");
            std::cerr << "personalfs: upgrading store 2.0 -> 2.1; one-time object cleanup may take time\n";
            cleanup(); // Only legacy stores require global discovery, once.
            fault("migration-cleaned");
            super_.minorVersion = 1;
            saveSuper(); // Publish the format gate only after durable cleanup.
            fault("migration-published");
            std::cerr << "personalfs: store upgrade to 2.1 completed\n";
        }
        cleanupPending();
    }
    else
    {
        auto journal = readFile(root_.get(), "journal", 16 * 1024 * 1024);
        if (!journal.empty())
        {
            decodeTransaction(journal);
            fail(EUCLEAN, "pending journal; run fsck --repair");
        }
    }
    auto rootMeta = metadata(0);
    check(rootMeta.kind == Kind::Directory && rootMeta.parent == 0 && rootMeta.nlink == 1, "invalid root");
    // Directory logs are validated on access, not scanned at mount.
}
Store::~Store() = default;
void Store::guard()
{
    if (!writable_)
        fail(EROFS, "read-only inspection");
    if (poisoned_)
        fail(EIO, "transaction failed; close and recover store before further mutations");
}
void Store::saveSuper()
{
    atomicFile(root_.get(), "superblock", super_.encode());
}
std::shared_ptr<std::recursive_mutex> Store::node(Id id)
{
    std::lock_guard l(nodesMutex_);
    auto &weak = nodes_[id];
    auto p = weak.lock();
    if (!p)
    {
        p = std::make_shared<std::recursive_mutex>();
        weak = p;
    }
    if (nodes_.size() > 4096)
    {
        for (auto i = nodes_.begin(); i != nodes_.end();)
        {
            if (i->second.expired())
                i = nodes_.erase(i);
            else
                ++i;
        }
    }
    return p;
}
std::unique_lock<std::recursive_mutex> Store::lockNode(std::recursive_mutex &mutex)
{
    Timer t(metrics, "directory_lock_wait");
    metrics.count("directory_locks");
    return std::unique_lock<std::recursive_mutex>(mutex);
}
std::unique_lock<std::recursive_mutex> Store::lockTransaction()
{
    Timer t(metrics, "journal_queue_wait");
    metrics.count("transaction_locks");
    return std::unique_lock<std::recursive_mutex>(transactionMutex_);
}
Store::Location Store::location(Id id, bool create)
{
    if (id == 0)
        return {Fd(checked(::dup(root_.get()), "dup root")), "0"};
    auto page = pageName(id, super_.pageSize);
    if (create)
    {
        if (::mkdirat(root_.get(), page.c_str(), 0700) == 0)
            syncFd(root_.get());
        else if (errno != EEXIST)
            fail(errno, "create page");
    }

    Fd fd(checked(openAt(root_.get(), page.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                  "open page"));
    return {std::move(fd), std::to_string(id)};
}
std::string Store::physicalName(Id id) const
{
    return id ? pageName(id, super_.pageSize) + "/" + std::to_string(id) + ".obj" : "0.obj";
}
Fd Store::object(Id id, int flags)
{
    auto p = location(id);

    Fd f(checked(openAt(p.directory.get(), (p.base + ".obj").c_str(), flags | O_NOFOLLOW | O_CLOEXEC),
                 "open object"));
    fileSize(f.get());
    return f;
}
Meta Store::metadata(Id id)
{
    if (poisoned_)
        fail(EIO, "failed transaction requires restart");
    auto p = location(id);
    auto m = Meta::decode(readFile(p.directory.get(), p.base + ".meta", 4096));
    check(m.id == id, "object identity mismatch");
    return m;
}
void Store::scan(Id id, const std::function<void(const Record &)> &fn, std::uint64_t *generation)
{
    Timer timer(metrics, "directory_load");
    auto fd = object(id, O_RDONLY);
    auto header = unseal("PFS2DIRH", readAt(fd.get(), directoryHeaderSize, 0, &metrics));
    Decoder d{header};
    check(d.u64() == id, "directory identity mismatch");
    auto seq = d.u64();
    d.end();
    off_t offset = directoryHeaderSize;
    auto size = fileSize(fd.get());
    while (static_cast<std::uint64_t>(offset) < size)
    {
        auto h = readAt(fd.get(), 12, offset, &metrics);
        check(h.size() == 12, "unconfirmed directory tail without journal evidence");
        Decoder hd{h};
        hd.raw(8);
        auto len = hd.u32();
        check(len <= 1024, "invalid directory record length");
        auto b = readAt(fd.get(), len + 16, offset, &metrics);
        auto r = Record::decode(b);
        check(r.sequence == seq + 1, "directory sequence discontinuity");
        seq = r.sequence;
        fn(r);
        offset += b.size();
    }
    check(static_cast<std::uint64_t>(offset) == size, "directory record boundary");
    check(seq == metadata(id).logSequence, "directory sequence disagrees with durable sidecar");
    if (generation)
        *generation = seq;
    metrics.count("directory_log_bytes_read", size);
}
std::shared_ptr<const Entries> Store::cached(Id id)
{
    {
        std::lock_guard lock(cacheMutex_);
        auto existing = cache_.find(id);
        if (existing != cache_.end())
        {
            existing->second.used = ++cacheClock_;
            metrics.count("cache_hits");
            return existing->second.entries;
        }
    }
    metrics.count("cache_misses");
    if (cacheLimit_ < 256)
        return {};
    std::size_t reserved = 0;
    auto reserve = [&](std::size_t bytes)
    {
        std::lock_guard lock(cacheMutex_);
        if (bytes > cacheLimit_)
            return false;
        while (cacheUsed_ + cacheBuilding_ > cacheLimit_ - bytes)
        {
            auto victim = cache_.end();
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
                if (it->second.entries.use_count() == 1 &&
                    (victim == cache_.end() || it->second.used < victim->second.used))
                    victim = it;
            if (victim == cache_.end())
                return false;
            cacheUsed_ -= victim->second.bytes;
            cache_.erase(victim);
            metrics.count("cache_evictions");
        }
        cacheBuilding_ += bytes;
        reserved += bytes;
        return true;
    };
    auto unreserve = [&](std::size_t bytes)
    {
        std::lock_guard lock(cacheMutex_);
        cacheBuilding_ -= bytes;
        reserved -= bytes;
    };
    if (!reserve(256))
    {
        metrics.count("cache_bypasses");
        return {};
    }
    try
    {
        auto entries = std::make_shared<Entries>();
        bool oversized = false;
        scan(id,
             [&](const Record &record)
             {
                 if (oversized)
                     return;
                 auto found = entries->find(record.name);
                 const auto cost = 256 + 2 * record.name.size();
                 if (record.add)
                 {
                     check(found == entries->end(), "duplicate ADD");
                     if (!reserve(cost))
                     {
                         entries.reset();
                         unreserve(reserved);
                         oversized = true;
                         return;
                     }
                     entries->emplace(record.name, record.entry);
                 }
                 else
                 {
                     check(found != entries->end() && found->second == record.entry,
                           "REMOVE identity mismatch");
                     entries->erase(found);
                     unreserve(cost);
                 }
             });
        if (oversized)
        {
            metrics.count("cache_bypasses");
            return {};
        }
        std::lock_guard lock(cacheMutex_);
        cache_[id] = {entries, reserved, ++cacheClock_};
        metrics.count("directory_live_entries_loaded", entries->size());
        cacheUsed_ += reserved;
        cacheBuilding_ -= reserved;
        reserved = 0;
        return entries;
    }
    catch (...)
    {
        unreserve(reserved);
        throw;
    }
}
void Store::invalidate(Id id)
{
    std::lock_guard l(cacheMutex_);
    auto i = cache_.find(id);
    if (i != cache_.end())
    {
        cacheUsed_ -= i->second.bytes;
        cache_.erase(i);
    }
}
void Store::updateCache(Id id, const Bytes &records)
{
    std::lock_guard l(cacheMutex_);
    auto i = cache_.find(id);
    if (i == cache_.end())
        return;
    auto &c = i->second;
    std::size_t pos = 0;
    while (pos < records.size())
    {
        Bytes head(records.begin() + pos, records.begin() + pos + 12);
        Decoder d{head};
        d.raw(8);
        auto len = d.u32() + 16;
        auto r = Record::decode(Bytes(records.begin() + pos, records.begin() + pos + len));
        pos += len;
        if (r.add)
        {
            auto cost = 256 + 2 * r.name.size();
            if (cost > cacheLimit_ || cacheUsed_ + cacheBuilding_ > cacheLimit_ - cost)
            {
                cacheUsed_ -= c.bytes;
                cache_.erase(i);
                metrics.count("cache_evictions");
                return;
            }
            auto [it, inserted] = c.entries->emplace(r.name, r.entry);
            (void)it;
            if (inserted)
            {
                c.bytes += 256 + 2 * r.name.size();
                cacheUsed_ += 256 + 2 * r.name.size();
            }
        }
        else if (c.entries->erase(r.name))
        {
            c.bytes -= 256 + 2 * r.name.size();
            cacheUsed_ -= 256 + 2 * r.name.size();
        }
    }
    if (cacheUsed_ > cacheLimit_)
    {
        cacheUsed_ -= c.bytes;
        cache_.erase(i);
        metrics.count("cache_evictions");
    }
}
std::optional<Entry> Store::lookup(Id id, const std::string &name)
{
    Timer timer(metrics, "lookup");
    auto n = node(id);
    auto l = lockNode(*n);
    if (metadata(id).kind != Kind::Directory)
        fail(ENOTDIR, "lookup parent");
    if (auto c = cached(id))
    {
        auto i = c->find(name);
        return i == c->end() ? std::optional<Entry>{} : i->second;
    }
    std::optional<Entry> found;
    scan(id,
         [&](const Record &r)
         {
             if (r.name != name)
                 return;
             if (r.add)
             {
                 check(!found, "duplicate ADD");
                 found = r.entry;
             }
             else
             {
                 check(found && *found == r.entry, "REMOVE identity mismatch");
                 found.reset();
             }
         });
    return found;
}
Id Store::resolve(const std::string &path)
{
    if (path.empty() || path.front() != '/')
        fail(EINVAL, "expected absolute virtual path");
    Id id = 0;
    std::size_t start = 1;
    while (start < path.size())
    {
        auto end = path.find('/', start);
        if (end == path.npos)
            end = path.size();
        auto name = path.substr(start, end - start);
        start = end + 1;
        if (name.empty() || name == ".")
            continue;
        if (name == "..")
        {
            id = metadata(id).parent;
            continue;
        }
        validName(name);
        auto e = lookup(id, name);
        if (!e)
            fail(ENOENT, "virtual object not found");
        id = e->id;
    }
    return id;
}
std::pair<Id, std::string> Store::parent(const std::string &path)
{
    if (path.empty() || path == "/")
        fail(EBUSY, "root cannot be modified");
    auto last = path.find_last_of('/');
    if (last == path.npos)
        fail(EINVAL, "absolute path required");
    auto name = path.substr(last + 1);
    validName(name);
    auto id = resolve(last ? path.substr(0, last) : "/");
    if (metadata(id).kind != Kind::Directory)
        fail(ENOTDIR, "parent");
    return {id, name};
}
Entries Store::list(Id id)
{
    Timer t(metrics, "readdir");
    auto n = node(id);
    auto l = lockNode(*n);
    if (metadata(id).kind != Kind::Directory)
        fail(ENOTDIR, "readdir");
    if (auto c = cached(id))
        return *c;
    Entries entries;
    scan(id,
         [&](const Record &r)
         {
             auto i = entries.find(r.name);
             if (r.add)
             {
                 check(i == entries.end(), "duplicate ADD");
                 entries.emplace(r.name, r.entry);
             }
             else
             {
                 check(i != entries.end() && i->second == r.entry, "REMOVE mismatch");
                 entries.erase(i);
             }
         });
    return entries;
}
Store::Transaction Store::transaction()
{
    guard();
    if (super_.nextTx == UINT64_MAX)
        fail(EOVERFLOW, "transaction IDs exhausted");
    Transaction tx{super_.nextTx++, {}};
    saveSuper();
    fault("id-reserved");
    return tx;
}
Bytes Store::encodeTransaction(const Transaction &tx, bool committed)
{
    Encoder e;
    e.u64(tx.id);
    e.u32(tx.actions.size());
    for (auto &a : tx.actions)
    {
        e.u8(a.type);
        e.u64(a.id);
        e.u64(a.before);
        e.u32(a.bytes.size());
        e.raw(a.bytes);
    }
    e.u8(committed ? 1 : 0);
    return seal("PFS2JRNL", e.bytes);
}
Store::Transaction Store::decodeTransaction(const Bytes &bytes)
{
    auto p = unseal("PFS2JRNL", bytes);
    Decoder d{p};
    Transaction tx{d.u64(), {}};
    check(tx.id > 0 && tx.id < super_.nextTx, "journal tx outside allocator");
    auto count = d.u32();
    check(count <= 64, "journal action limit");
    for (unsigned i = 0; i < count; ++i)
    {
        Action a;
        a.type = d.u8();
        a.id = d.u64();
        a.before = d.u64();
        a.bytes = d.raw(d.u32());
        check(a.type >= 1 && a.type <= 3 && a.id < super_.nextId, "invalid journal action");
        tx.actions.push_back(std::move(a));
    }
    auto marker = d.u8();
    check(marker <= 1, "invalid commit marker");
    tx.committed = marker == 1;
    d.end();
    return tx;
}
void Store::apply(const Transaction &tx)
{
    unsigned index = 0;
    for (auto &a : tx.actions)
    {
        auto p = location(a.id, true);
        if (a.type == 1)
        {
            // Create immutable initial object. Data cannot be externally opened before commit.
            int raw = openAt(p.directory.get(), (p.base + ".obj").c_str(),
                             O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (raw < 0 && errno != EEXIST)
                fail(errno, "create object");
            Fd fd(raw >= 0 ? raw
                           : checked(openAt(p.directory.get(), (p.base + ".obj").c_str(),
                                            O_RDWR | O_NOFOLLOW | O_CLOEXEC),
                                     "redo object"));
            auto size = fileSize(fd.get());
            check(size <= a.bytes.size(), "created object changed before commit");
            auto existing = readAt(fd.get(), size, 0, &metrics);
            check(std::equal(existing.begin(), existing.end(), a.bytes.begin()),
                  "created object redo mismatch");
            writeAll(fd.get(), a.bytes, 0, &metrics);
            syncFd(fd.get());
            syncFd(p.directory.get());
            fault("object-created");
        }
        else if (a.type == 2)
        {
            // Full sidecar with expected old generation, atomically published.
            auto target = Meta::decode(a.bytes);
            if (!target.nlink)
            {
                // The redo journal protects this publication until the tombstone
                // and its pending marker are both durable.
                Fd marker(checked(openAt(pending_.get(), std::to_string(a.id).c_str(),
                                         O_CREAT | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600), "register pending deletion"));
                syncFd(marker.get());
                syncFd(pending_.get());
                fault("pending-registered");
            }
            check(target.id == a.id, "redo metadata identity");
            try
            {
                auto old = metadata(a.id);
                if (old.encode() == a.bytes)
                {
                    ++index;
                    continue;
                }
                check(old.generation == a.before, "metadata redo generation mismatch");
            }
            catch (const std::system_error &e)
            {
                if (e.code().value() != ENOENT || a.before != UINT64_MAX)
                    throw;
            }
            atomicFile(p.directory.get(), p.base + ".meta", a.bytes);
        }
        else
        {
            auto fd = object(a.id, O_RDWR);
            auto size = fileSize(fd.get());
            check(size >= a.before && size <= a.before + a.bytes.size(), "directory redo extent mismatch");
            check(!tx.committed || size == a.before + a.bytes.size(), "truncated committed directory record");
            auto existing = readAt(fd.get(), size - a.before, a.before, &metrics);
            if (!std::equal(existing.begin(), existing.end(), a.bytes.begin()))
            {
                check(!tx.committed, "committed directory redo bytes mismatch");
                // BEGIN owns exactly this suffix. A torn, unconfirmed record may be replaced;
                // a different complete/checksummed record is an identity conflict, never success.
                std::size_t offset = 0;
                while (offset + 12 <= existing.size())
                {
                    Decoder header{existing, offset + 8};
                    auto length = std::uint64_t(header.u32()) + 16;
                    if (length > 1040 || length > existing.size() - offset)
                        break;
                    Bytes record(existing.begin() + offset, existing.begin() + offset + length);
                    try
                    {
                        Record::decode(record);
                    }
                    catch (const std::system_error &)
                    {
                        break;
                    }
                    check(std::equal(record.begin(), record.end(), a.bytes.begin() + offset),
                          "conflicting valid directory redo record");
                    offset += length;
                }
            }
            writeAll(fd.get(), a.bytes, a.before, &metrics);
            fault("directory-written");
            syncFd(fd.get());
            updateCache(a.id, a.bytes);
        }
        ++index;
        fault(("action-" + std::to_string(index)).c_str());
    }
}
void Store::commit(Transaction &tx)
{
    Timer timer(metrics, "transaction");
    try
    {
        auto begin = encodeTransaction(tx, false);

        atomicFile(root_.get(), "journal", begin);
        fault("begin");
        apply(tx);
        atomicFile(root_.get(), "journal", encodeTransaction(tx, true));
        fault("commit");
        atomicFile(root_.get(), "journal", {});
        fault("journal-clean");
        metrics.count("transactions");
    }
    catch (...)
    {
        poisoned_ = true;
        metrics.count("transaction_errors");
        throw;
    }
}
void Store::recover()
{
    Timer timer(metrics, "recovery");
    auto b = readFile(root_.get(), "journal", 16 * 1024 * 1024);
    if (b.empty())
        return;
    auto tx = decodeTransaction(b);
    apply(tx);
    metrics.count("recovered_transactions");
    // Validate only references introduced by this transaction, not the full
    // accumulated histories of its parent directories.
    for (const auto &action : tx.actions)
    {
        if (action.type != 3)
            continue;
        std::size_t offset = 0;
        while (offset < action.bytes.size())
        {
            Decoder header{action.bytes, offset + 8};
            auto length = std::uint64_t(header.u32()) + 16;
            check(length <= action.bytes.size() - offset, "invalid recovered record length");
            Record record = Record::decode(Bytes(action.bytes.begin() + offset,
                                                action.bytes.begin() + offset + length));
            if (record.add)
            {
                auto child = metadata(record.entry.id);
                check(child.nlink == 1 && child.kind == record.entry.kind && child.parent == action.id,
                      "recovered dangling reference");
            }
            offset += length;
        }
    }
    atomicFile(root_.get(), "journal", {});
    fault("recovery-complete");
}
void Store::append(Transaction &tx, Id id, const std::vector<Record> &records)
{
    auto fd = object(id, O_RDONLY);
    auto m = metadata(id);
    std::uint64_t seq = m.logSequence;
    Encoder e;
    for (auto r : records)
    {
        check(seq < UINT64_MAX, "directory sequence exhausted");
        r.sequence = ++seq;
        r.tx = tx.id;
        e.raw(r.encode());
        if (r.entry.kind == Kind::Directory)
        {
            if (r.add)
                ++m.childDirectories;
            else
            {
                check(m.childDirectories > 0, "directory link count underflow");
                --m.childDirectories;
            }
        }
    }
    tx.actions.push_back({3, id, fileSize(fd.get()), e.bytes});
    m.logSequence = seq;
    m.mtime = m.ctime = now();
    metaAction(tx, m);
}
void Store::metaAction(Transaction &tx, Meta m)
{
    auto before = m.generation;
    check(before < UINT64_MAX, "metadata generation exhausted");
    ++m.generation;
    tx.actions.push_back({2, m.id, before, m.encode()});
}
Id Store::create(const std::string &path, Kind kind, mode_t mode, uid_t uid, gid_t gid,
                 const std::string &target)
{
    if ((mode & S_ISUID) || ((mode & S_ISGID) && kind != Kind::Directory))
        fail(EOPNOTSUPP, "setuid and regular-file setgid unsupported");
    Timer timer(metrics, "create");
    auto txlock = lockTransaction();
    guard();
    auto [pid, name] = parent(path);
    auto n = node(pid);
    auto lock = lockNode(*n);
    if (lookup(pid, name))
        fail(EEXIST, "entry exists");
    if (super_.nextId == UINT64_MAX)
        fail(EOVERFLOW, "object IDs exhausted");
    Meta m;
    m.id = super_.nextId++;
    m.parent = pid;
    m.kind = kind;
    m.mode = mode & 07777;
    m.uid = uid;
    m.gid = gid;
    m.atime = m.mtime = m.ctime = now();
    auto pm = metadata(pid);
    if (pm.mode & S_ISGID)
    {
        m.gid = pm.gid;
        if (kind == Kind::Directory)
            m.mode |= S_ISGID;
    }
    auto tx = transaction();
    Bytes payload;
    if (kind == Kind::Directory)
        payload = directoryHeader(m.id);
    else if (kind == Kind::Symlink)
    {
        if (target.size() > 4095)
            fail(ENAMETOOLONG, "symlink target");
        payload.assign(target.begin(), target.end());
    }
    tx.actions.push_back({1, m.id, 0, payload});
    tx.actions.push_back({2, m.id, UINT64_MAX, m.encode()});
    append(tx, pid, {{true, 0, 0, name, {m.id, kind}}});
    commit(tx);
    return m.id;
}
void Store::remove(const std::string &path, bool directory)
{
    Timer timer(metrics, "unlink");
    auto txlock = lockTransaction();
    guard();
    auto [pid, name] = parent(path);
    auto e = lookup(pid, name);
    if (!e)
        fail(ENOENT, "entry missing");
    auto ids = std::set<Id>{pid, e->id};
    std::vector<std::shared_ptr<std::recursive_mutex>> ns;
    std::vector<std::unique_lock<std::recursive_mutex>> locks;
    for (auto id : ids)
    {
        ns.push_back(node(id));
        locks.push_back(lockNode(*ns.back()));
    }
    if (directory && e->kind != Kind::Directory)
        fail(ENOTDIR, "rmdir file");
    if (!directory && e->kind == Kind::Directory)
        fail(EISDIR, "unlink directory");
    if (directory && !list(e->id).empty())
        fail(ENOTEMPTY, "directory not empty");
    auto tx = transaction();
    append(tx, pid, {{false, 0, 0, name, *e}});
    auto m = metadata(e->id);
    m.nlink = 0;
    m.ctime = now();
    metaAction(tx, m);
    commit(tx);
    collect(e->id);
}
void Store::rename(const std::string &source, const std::string &destination, unsigned flags)
{
    Timer timer(metrics, "rename");
    if (flags & ~1U)
        fail(EOPNOTSUPP, "only RENAME_NOREPLACE supported");
    auto txlock = lockTransaction();
    guard();
    auto [sp, sn] = parent(source);
    auto [dp, dn] = parent(destination);
    auto se = lookup(sp, sn), de = lookup(dp, dn);
    if (!se)
        fail(ENOENT, "rename source missing");
    if (de && flags)
        fail(EEXIST, "rename target exists");
    if (sp == dp && sn == dn)
        return;
    std::set<Id> ids{sp, dp, se->id};
    if (de)
        ids.insert(de->id);
    std::vector<std::shared_ptr<std::recursive_mutex>> ns;
    std::vector<std::unique_lock<std::recursive_mutex>> locks;
    for (auto id : ids)
    {
        ns.push_back(node(id));
        locks.push_back(lockNode(*ns.back()));
    }
    if (de)
    {
        if (se->kind == Kind::Directory && de->kind != Kind::Directory)
            fail(ENOTDIR, "rename directory over file");
        if (se->kind != Kind::Directory && de->kind == Kind::Directory)
            fail(EISDIR, "rename file over directory");
        if (de->kind == Kind::Directory && !list(de->id).empty())
            fail(ENOTEMPTY, "rename target directory not empty");
    }
    if (se->kind == Kind::Directory)
    {
        std::set<Id> seen;
        for (Id p = dp;; p = metadata(p).parent)
        {
            if (p == se->id)
                fail(EINVAL, "rename would create a cycle");
            check(seen.insert(p).second, "existing parent cycle");
            if (p == 0)
                break;
        }
    }
    auto tx = transaction();
    std::vector<Record> src{{false, 0, 0, sn, *se}}, dst;
    if (de)
        dst.push_back({false, 0, 0, dn, *de});
    dst.push_back({true, 0, 0, dn, *se});
    if (sp == dp)
    {
        src.insert(src.end(), dst.begin(), dst.end());
        append(tx, sp, src);
    }
    else
    {
        append(tx, sp, src);
        append(tx, dp, dst);
    }
    auto m = metadata(se->id);
    m.parent = dp;
    m.ctime = now();
    metaAction(tx, m);
    if (de)
    {
        auto old = metadata(de->id);
        old.nlink = 0;
        old.ctime = now();
        metaAction(tx, old);
    }
    commit(tx);
    if (de)
        collect(de->id);
}
std::unique_ptr<Handle> Store::createOpen(const std::string &path, mode_t mode, uid_t uid, gid_t gid,
                                          int flags)
{
    auto lock = lockTransaction();
    auto id = create(path, Kind::File, mode, uid, gid);
    return open(id, flags);
}
std::string Store::statistics()
{
    {
        std::lock_guard c(cacheMutex_);
        std::lock_guard m(metrics.mutex);
        metrics.counters["cache_bytes"] = cacheUsed_ + cacheBuilding_;
        metrics.counters["cache_building_bytes"] = cacheBuilding_;
        metrics.counters["cache_max_bytes"] = cacheLimit_;
    }
    {
        std::lock_guard n(nodesMutex_);
        std::lock_guard m(metrics.mutex);
        std::size_t count = 0;
        for (auto &[id, n] : openCounts_)
        {
            (void)id;
            count += n;
        }
        metrics.counters["open_handles"] = count;
    }
    return metrics.json();
}
void Store::recordError(int code, const std::string &detail) noexcept
{
    try
    {
        metrics.count("operation_errors");
        if (!writable_ || (code != EIO && code != ENOSPC && code != EDQUOT && code != EUCLEAN))
            return;
        auto timestamp = now();
        std::ostringstream line;
        line << "{\"timestamp_ns\":" << std::uint64_t(timestamp.tv_sec) * 1000000000 + timestamp.tv_nsec
             << ",\"event\":\"operation_error\",\"errno\":" << code << ",\"detail\":\"";
        for (unsigned char byte : detail)
        {
            if (byte == '"' || byte == '\\')
                line << '\\' << byte;
            else if (byte >= 32 && byte < 127)
                line << byte;
            else
                line << '?';
        }
        line << "\"}\n";
        auto text = line.str();
        Fd log(::openat(root_.get(), "events.log", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                        0600));
        if (log.get() >= 0)
        {
            auto ignored = ::write(log.get(), text.data(), text.size());
            (void)ignored;
        }
    }
    catch (...)
    {
    } // Best-effort diagnostics must not hide the original filesystem error.
}
std::unique_ptr<Handle> Store::open(Id id, int flags)
{
    auto n = node(id);
    auto lock = lockNode(*n);
    auto m = metadata(id);
    if (flags & O_DIRECTORY)
    {
        if (m.kind != Kind::Directory)
            fail(ENOTDIR, "open directory");
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
            fail(EISDIR, "cannot write directory data");
    }
    else if (m.kind != Kind::File)
        fail(m.kind == Kind::Directory ? EISDIR : ELOOP, "open non-regular object");
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        guard();
    auto h = std::make_unique<Handle>();
    h->id = id;
    h->fd = object(id, flags & (O_ACCMODE | O_TRUNC | O_SYNC | O_DSYNC));
    {
        std::lock_guard l(nodesMutex_);
        ++openCounts_[id];
    }
    metrics.count("opens");
    return h;
}
std::unique_ptr<Handle> Store::openDirectory(Id id)
{
    auto n = node(id);
    auto lock = lockNode(*n);
    auto h = open(id, O_RDONLY | O_DIRECTORY);
    try
    {
        h->snapshot.emplace_back(".", Entry{id, Kind::Directory});
        h->snapshot.emplace_back("..", Entry{metadata(id).parent, Kind::Directory});
        for (auto &entry : list(id))
            h->snapshot.push_back(entry);
    }
    catch (...)
    {
        release(std::move(h));
        throw;
    }
    return h;
}
void Store::release(std::unique_ptr<Handle> h)
{
    if (!h)
        return;
    auto n = node(h->id);
    auto lock = lockNode(*n);
    auto id = h->id;
    h.reset();
    {
        std::lock_guard l(nodesMutex_);
        auto i = openCounts_.find(id);
        check(i != openCounts_.end() && i->second > 0, "open count");
        if (!--i->second)
            openCounts_.erase(i);
    }
    if (writable_ && !poisoned_)
        collect(id);
    metrics.count("releases");
}
void Store::collect(Id id)
{
    if (!id)
        return;
    {
        std::lock_guard l(nodesMutex_);
        if (openCounts_.contains(id))
            return;
    }
    Meta m;
    try
    {
        m = metadata(id);
    }
    catch (const std::system_error &e)
    {
        if (e.code().value() != ENOENT)
            throw;
        // A durable pending marker may outlive both files.
        auto p = location(id);
        struct stat st{};
        if (::fstatat(p.directory.get(), (p.base + ".obj").c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
            fail(EIO, "pending object lacks tombstone");
        if (errno != ENOENT)
            fail(errno, "stat pending object");
        syncFd(p.directory.get());
        if (::unlinkat(pending_.get(), std::to_string(id).c_str(), 0) && errno != ENOENT)
            fail(errno, "remove completed pending deletion");
        syncFd(pending_.get());
        return;
    }
    if (m.nlink)
        return;
    auto p = location(id);
    if (::unlinkat(p.directory.get(), (p.base + ".obj").c_str(), 0) && errno != ENOENT)
        fail(errno, "collect object");
    fault("gc-object");
    // The sidecar is the durable tombstone. Never remove it before data deletion is durable.
    syncFd(p.directory.get());
    fault("gc-object-synced");
    if (::unlinkat(p.directory.get(), (p.base + ".meta").c_str(), 0) && errno != ENOENT)
        fail(errno, "collect sidecar");
    fault("gc-meta");
    syncFd(p.directory.get());
    if (::unlinkat(pending_.get(), std::to_string(id).c_str(), 0) && errno != ENOENT)
        fail(errno, "remove pending deletion");
    syncFd(pending_.get());
    fault("pending-completed");
    invalidate(id);
}
ssize_t Store::read(Handle &h, void *data, std::size_t size, off_t offset)
{
    Timer t(metrics, "read");
    fault("data-read");
    metrics.count("pread_calls");
    auto n = ::pread(h.fd.get(), data, size, offset);
    if (n < 0)
        fail(errno, "pread");
    metrics.count("read_bytes", n);
    metrics.sample("fuse_read_size", size);
    metrics.sample("host_read_size", size);
    return n;
}
ssize_t Store::write(Handle &h, const void *data, std::size_t size, off_t offset)
{
    Timer t(metrics, "write");
    guard();
    fault("data-write");
    metrics.count("pwrite_calls");
    auto n = ::pwrite(h.fd.get(), data, size, offset);
    if (n < 0)
        fail(errno, "pwrite");
    metrics.count("write_bytes", n);
    metrics.sample("fuse_write_size", size);
    metrics.sample("host_write_size", size);
    return n;
}
void Store::sync(Handle &h, bool dataOnly)
{
    Timer t(metrics, "file_fsync");
    syncFd(h.fd.get(), dataOnly);
    auto p = location(h.id);
    Fd meta(checked(openAt(p.directory.get(), (p.base + ".meta").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC),
                    "open fsync sidecar"));
    syncFd(meta.get());
    syncFd(p.directory.get());
}
struct stat Store::getattr(Id id, int fd)
{
    Timer t(metrics, "getattr");
    auto n = node(id);
    auto lock = lockNode(*n);
    auto m = metadata(id);
    struct stat s{};
    auto owned = fd < 0 ? object(id, O_RDONLY) : Fd();
    checked(::fstat(fd < 0 ? owned.get() : fd, &s), "stat object");
    s.st_ino = id + 1;
    s.st_mode = typeMode(m.kind) | m.mode;
    s.st_uid = m.uid;
    s.st_gid = m.gid;
    s.st_nlink = m.nlink;
    if (m.kind == Kind::Directory)
    {
        s.st_nlink = m.nlink ? 2 + m.childDirectories : 0;
        s.st_atim = m.atime;
        s.st_mtim = m.mtime;
        s.st_ctim = m.ctime;
    }
    else if (m.ctime.tv_sec > s.st_ctim.tv_sec ||
             (m.ctime.tv_sec == s.st_ctim.tv_sec && m.ctime.tv_nsec > s.st_ctim.tv_nsec))
        s.st_ctim = m.ctime;
    return s;
}
void Store::truncate(Id id, off_t size, int fd)
{
    guard();
    if (size < 0)
        fail(EINVAL, "negative truncate");
    if (metadata(id).kind != Kind::File)
        fail(EISDIR, "truncate non-file");
    auto owned = fd < 0 ? object(id, O_WRONLY) : Fd();
    checked(::ftruncate(fd < 0 ? owned.get() : fd, size), "truncate");
}
void Store::attributes(Id id, std::optional<mode_t> mode, std::optional<uid_t> uid, std::optional<gid_t> gid)
{
    auto txlock = lockTransaction();
    guard();
    auto n = node(id);
    auto lock = lockNode(*n);
    auto m = metadata(id);
    if (mode && ((*mode & S_ISUID) || ((*mode & S_ISGID) && m.kind != Kind::Directory)))
        fail(EOPNOTSUPP, "setuid and regular-file setgid unsupported");
    if (mode)
        m.mode = *mode & 07777;
    if (uid)
        m.uid = *uid;
    if (gid)
        m.gid = *gid;
    if (uid || gid)
        m.mode &= ~(S_ISUID | S_ISGID);
    m.ctime = now();
    auto tx = transaction();
    metaAction(tx, m);
    commit(tx);
}
void Store::times(Id id, const timespec times[2])
{
    timespec defaults[2]{{0, UTIME_NOW}, {0, UTIME_NOW}};
    if (!times)
        times = defaults;
    for (unsigned i = 0; i < 2; ++i)
        if (times[i].tv_nsec != UTIME_NOW && times[i].tv_nsec != UTIME_OMIT &&
            (times[i].tv_nsec < 0 || times[i].tv_nsec >= 1000000000))
            fail(EINVAL, "invalid timestamp nanoseconds");
    if (times[0].tv_nsec == UTIME_OMIT && times[1].tv_nsec == UTIME_OMIT)
        return;
    auto txlock = lockTransaction();
    guard();
    auto n = node(id);
    auto lock = lockNode(*n);
    auto m = metadata(id);
    if (m.kind == Kind::Directory)
    {
        auto current = now();
        auto applyTime = [&](timespec old, timespec value)
        {
            return value.tv_nsec == UTIME_OMIT ? old : value.tv_nsec == UTIME_NOW ? current : value;
        };
        m.atime = applyTime(m.atime, times[0]);
        m.mtime = applyTime(m.mtime, times[1]);
        m.ctime = current;
        auto tx = transaction();
        metaAction(tx, m);
        commit(tx);
    }
    else
    {
        auto fd = object(id, O_RDONLY);
        checked(::futimens(fd.get(), times), "utimens");
    }
}
std::string Store::readlink(Id id)
{
    if (metadata(id).kind != Kind::Symlink)
        fail(EINVAL, "not a symlink");
    auto fd = object(id, O_RDONLY);
    auto b = readAt(fd.get(), 4096, 0, &metrics);
    return {b.begin(), b.end()};
}
struct statvfs Store::statfs()
{
    struct statvfs st{};
    checked(::fstatvfs(root_.get(), &st), "statvfs");
    st.f_namemax = 255;
    return st;
}
void Store::syncDirectory(Id id)
{
    auto n = node(id);
    auto lock = lockNode(*n);
    auto f = object(id, O_RDONLY);
    syncFd(f.get());
    auto p = location(id);
    syncFd(p.directory.get());
}
void Store::shutdown()
{
    auto lock = lockTransaction();
    guard();
    Timer timer(metrics, "shutdown");
    checked(::syncfs(root_.get()), "sync store on unmount");
    auto json = statistics();
    atomicFile(root_.get(), "stats.json", Bytes(json.begin(), json.end()));
}
void Store::compact(Id id)
{
    Timer timer(metrics, "compact");
    guard();
    auto n = node(id);
    Entries entries;
    std::uint64_t generation;
    {
        auto lock = lockNode(*n);
        entries = list(id);
        generation = metadata(id).logSequence;
    }
    metrics.count("compaction_live_entries", entries.size());
    auto beforeSize = fileSize(object(id, O_RDONLY).get());
    Encoder e;
    e.raw(directoryHeader(id, generation - entries.size()));
    std::uint64_t seq = generation - entries.size();
    for (auto &[name, entry] : entries)
        e.raw(Record{true, ++seq, 0, name, entry}.encode());
    // Construction does not block writers. Publication checks the captured generation.
    auto p = location(id);
    auto tmp = ".pfs-c-" + std::to_string(::getpid()) + "-" + std::to_string(++tempCounter_) + ".tmp";
    Fd f(checked(
        openAt(staging_.get(), tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600),
        "compact temporary"));
    try
    {
        writeAll(f.get(), e.bytes, 0, &metrics);
        syncFd(f.get());
        fault("compact-synced");
        auto lock = lockNode(*n);
        auto current = metadata(id).logSequence;
        if (current != generation)
            fail(EAGAIN, "directory changed during compaction; retry");
        metrics.count("renameat_calls");
        checked(::renameat(staging_.get(), tmp.c_str(), p.directory.get(), (p.base + ".obj").c_str()),
                "publish compaction");
        fault("compact-published");
        syncFd(p.directory.get());
        syncFd(staging_.get());
        invalidate(id);
        metrics.count("compaction_reclaimed_bytes",
                      beforeSize > e.bytes.size() ? beforeSize - e.bytes.size() : 0);
    }
    catch (...)
    {
        ::unlinkat(staging_.get(), tmp.c_str(), 0);
        throw;
    }
}
std::vector<Id> Store::objects()
{
    std::set<Id> ids{0};
    for (auto &page : names(root_.get(), &metrics))
    {
        if (page.size() != 16 || !std::all_of(page.begin(), page.end(), [](char c)
                                              { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            continue;
        Fd dir(checked(openAt(root_.get(), page.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                       "scan page"));
        for (auto &name : names(dir.get(), &metrics))
        {
            if (!name.ends_with(".meta") && !name.ends_with(".obj"))
                continue;
            auto dot = name.find('.');
            Id id;
            try
            {
                std::size_t used;
                id = std::stoull(name.substr(0, dot), &used);
                check(used == dot, "invalid object filename");
            }
            catch (...)
            {
                fail(EIO, "invalid object filename");
            }
            check(id > 0 && id < super_.nextId && pageName(id, super_.pageSize) == page &&
                      name.substr(0, dot) == std::to_string(id),
                  "object placement or ID mismatch");
            ids.insert(id);
        }
    }
    return {ids.begin(), ids.end()};
}
void Store::cleanupPending()
{
    Timer timer(metrics, "pending_cleanup");
    for (const auto &name : names(pending_.get(), &metrics))
    {
        std::size_t consumed = 0;
        Id id = std::stoull(name, &consumed);
        check(consumed == name.size() && std::to_string(id) == name && id > 0 && id < super_.nextId,
              "invalid pending deletion ID");
        // Recovery has completed; a linked object is never eligible for GC.
        try
        {
            check(metadata(id).nlink == 0, "pending deletion refers to linked object");
        }
        catch (const std::system_error &e)
        {
            if (e.code().value() != ENOENT)
                throw;
        }
        collect(id);
        metrics.count("pending_deletions_recovered");
    }
    bool changed = false;
    for (const auto &name : names(staging_.get(), &metrics))
    {
        check(name.starts_with(".pfs-") && name.ends_with(".tmp"), "unexpected staging entry");
        checked(::unlinkat(staging_.get(), name.c_str(), 0), "remove abandoned staging file");
        changed = true;
    }
    if (changed)
        syncFd(staging_.get());
}
void Store::cleanup()
{
    // Only nlink=0 objects are collectable; unpublished init artifacts are never guessed away.
    for (auto id : objects())
    {
        auto m = metadata(id);
        if (!m.nlink)
            collect(id);
    }
    auto clean = [&](int dir)
    {
        bool changed = false;
        for (auto &name : names(dir, &metrics))
        {
            if (name.starts_with(".pfs-") && name.ends_with(".tmp"))
            {
                checked(::unlinkat(dir, name.c_str(), 0), "remove abandoned temporary");
                changed = true;
            }
        }
        if (changed)
            syncFd(dir);
    };
    clean(root_.get());
    for (auto &page : names(root_.get(), &metrics))
    {
        if (page.size() != 16 || !std::all_of(page.begin(), page.end(), [](char c)
                                              { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            continue;
        Fd d(checked(openAt(root_.get(), page.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                     "cleanup page"));
        clean(d.get());
    }
}
std::string Store::fsck()
{
    Timer timer(metrics, "fsck");
    auto all = objects();
    std::set<Id> reachable{0};
    std::vector<Id> pending{0};
    std::uint64_t dirs = 0, files = 0;
    while (!pending.empty())
    {
        auto id = pending.back();
        pending.pop_back();
        auto m = metadata(id);
        check(m.nlink == 1, "reachable object marked unlinked");
        auto f = object(id, O_RDONLY);
        if (m.kind == Kind::Directory)
        {
            ++dirs;
            std::uint64_t subdirectories = 0;
            invalidate(id); // fsck always reads authoritative storage, including on a warm Store.
            for (auto &[name, e] : list(id))
            {
                (void)name;
                check(e.id != 0 && reachable.insert(e.id).second, "duplicate reference or namespace cycle");
                auto child = metadata(e.id);
                check(e.id < super_.nextId && child.kind == e.kind && child.parent == id,
                      "entry type or parent mismatch");
                pending.push_back(e.id);
                if (e.kind == Kind::Directory)
                    ++subdirectories;
            }
            check(subdirectories == m.childDirectories, "directory link count mismatch");
        }
        else
            ++files;
    }
    std::uint64_t orphans = 0;
    for (auto id : all)
    {
        auto m = metadata(id);
        if (!reachable.contains(id))
        {
            check(m.nlink == 0, "unreachable linked object");
            ++orphans;
        }
    }
    std::ostringstream o;
    o << "{\"ok\":true,\"objects\":" << all.size() << ",\"directories\":" << dirs << ",\"files\":" << files
      << ",\"unlinked_objects\":" << orphans << "}";
    return o.str();
}
std::string Store::inspect(std::optional<Id> id)
{
    std::ostringstream o;
    if (id)
    {
        auto m = metadata(*id);
        auto s = getattr(*id);
        o << "{\"object_id\":" << *id << ",\"parent_id\":" << m.parent << ",\"kind\":" << unsigned(m.kind)
          << ",\"generation\":" << m.generation << ",\"size\":" << s.st_size << ",\"physical\":\""
          << physicalName(*id) << "\"";
        if (m.kind == Kind::Directory)
        {
            auto entries = list(*id);
            std::uint64_t compactBytes = directoryHeaderSize;
            for (auto &[name, entry] : entries)
            {
                (void)entry;
                compactBytes += 46 + name.size();
            }
            o << ",\"live_entries\":" << entries.size() << ",\"log_bytes\":" << s.st_size
              << ",\"reclaimable_bytes\":"
              << (static_cast<std::uint64_t>(s.st_size) > compactBytes ? s.st_size - compactBytes : 0);
        }
        o << '}';
    }
    else
    {
        std::uint64_t pages = 0, maxEntries = 0, totalEntries = 0;
        for (auto &p : names(root_.get(), &metrics))
        {
            if (p.size() != 16 || !std::all_of(p.begin(), p.end(), [](char c)
                                               { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                continue;
            Fd d(checked(openAt(root_.get(), p.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                         "inspect page"));
            auto count = names(d.get(), &metrics).size();
            ++pages;
            totalEntries += count;
            maxEntries = std::max<std::uint64_t>(maxEntries, count);
        }
        o << "{\"format_version\":2,\"objects\":" << objects().size()
          << ",\"format_minor\":" << super_.minorVersion
          << ",\"objects_per_page\":" << super_.pageSize
          << ",\"page_encoding\":\"hex16\",\"next_object_id\":" << super_.nextId
          << ",\"next_txid\":" << super_.nextTx << ",\"pages\":" << pages
          << ",\"physical_page_entries\":" << totalEntries << ",\"max_page_entries\":" << maxEntries << "}";
    }
    return o.str();
}
} // namespace personalfs
