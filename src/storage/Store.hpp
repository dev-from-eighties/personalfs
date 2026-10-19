#pragma once
#include "storage/Format.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sys/statvfs.h>
namespace personalfs
{
class Fd
{
    int fd_ = -1;

  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd();
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept;
    Fd &operator=(Fd &&other) noexcept;
    int get() const
    {
        return fd_;
    }
};
struct Metrics
{
    struct Histogram
    {
        std::uint64_t count = 0, totalNs = 0, maxNs = 0;
        std::array<std::uint64_t, 64> bins{};
    };
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    mutable std::mutex mutex;
    std::map<std::string, std::uint64_t> counters;
    std::map<std::string, Histogram> histograms;
    void count(const std::string &name, std::uint64_t amount = 1) noexcept;
    void sample(const std::string &name, std::uint64_t ns) noexcept;
    std::string json() const;
};
class Timer
{
    Metrics &metrics_;
    std::string name_;
    std::chrono::steady_clock::time_point start_;

  public:
    Timer(Metrics &m, std::string n)
        : metrics_(m), name_(std::move(n)), start_(std::chrono::steady_clock::now())
    {
    }
    ~Timer();
};
using Entries = std::map<std::string, Entry, std::less<>>;
struct Handle
{
    Id id;
    Fd fd;
    std::vector<std::pair<std::string, Entry>> snapshot;
    virtual ~Handle() = default;
};
class Store
{
  public:
    enum class Mode
    {
        ReadWrite,
        Check
    };
    static void init(const std::string &path, std::uint32_t pageSize = 4096);
    explicit Store(const std::string &path, Mode mode = Mode::ReadWrite,
                   std::size_t cacheBytes = 64 * 1024 * 1024);
    ~Store();
    Store(const Store &) = delete;
    Metrics metrics;
    Id resolve(const std::string &path);
    Meta metadata(Id id);
    struct stat getattr(Id id, int fd = -1);
    Entries list(Id id);
    Id create(const std::string &path, Kind kind, mode_t mode, uid_t uid, gid_t gid,
              const std::string &target = {});
    void remove(const std::string &path, bool directory);
    void rename(const std::string &source, const std::string &destination, unsigned flags = 0);
    std::unique_ptr<Handle> open(Id id, int flags);
    std::unique_ptr<Handle> openDirectory(Id id);
    std::unique_ptr<Handle> createOpen(const std::string &path, mode_t mode, uid_t uid, gid_t gid, int flags);
    std::string statistics();
    void recordError(int code, const std::string &detail) noexcept;
    void release(std::unique_ptr<Handle> handle);
    ssize_t read(Handle &h, void *data, std::size_t size, off_t offset);
    ssize_t write(Handle &h, const void *data, std::size_t size, off_t offset);
    void sync(Handle &h, bool dataOnly);
    void truncate(Id id, off_t size, int fd = -1);
    void attributes(Id id, std::optional<mode_t> mode, std::optional<uid_t> uid, std::optional<gid_t> gid);
    void times(Id id, const timespec times[2]);
    std::string readlink(Id id);
    struct statvfs statfs();
    void compact(Id id);
    std::string fsck();
    std::string inspect(std::optional<Id> id = {});
    void syncDirectory(Id id);
    void shutdown();
    std::string physicalName(Id id) const;

  private:
    struct Location
    {
        Fd directory;
        std::string base;
    };
    struct Action
    {
        std::uint8_t type;
        Id id;
        std::uint64_t before;
        Bytes bytes;
    };
    struct Transaction
    {
        std::uint64_t id;
        std::vector<Action> actions;
        bool committed = false;
    };
    struct Cache
    {
        std::shared_ptr<Entries> entries;
        std::size_t bytes;
        std::uint64_t used;
    };
    Fd root_, lock_;
    Super super_;
    bool writable_;
    std::atomic<bool> poisoned_{false};
    std::recursive_mutex transactionMutex_;
    std::mutex nodesMutex_, cacheMutex_;
    std::map<Id, std::weak_ptr<std::recursive_mutex>> nodes_;
    std::map<Id, std::size_t> openCounts_;
    std::map<Id, Cache> cache_;
    std::size_t cacheLimit_, cacheUsed_ = 0, cacheBuilding_ = 0;
    std::uint64_t cacheClock_ = 0;
    std::atomic<std::uint64_t> tempCounter_{0};
    std::shared_ptr<std::recursive_mutex> node(Id id);
    std::unique_lock<std::recursive_mutex> lockNode(std::recursive_mutex &mutex);
    std::unique_lock<std::recursive_mutex> lockTransaction();
    Location location(Id id, bool create = false);
    Fd object(Id id, int flags);
    int openAt(int directory, const char *name, int flags, mode_t mode = 0);
    Bytes readFile(int dir, const std::string &name, std::size_t limit);
    void atomicFile(int dir, const std::string &name, const Bytes &bytes);
    void syncFd(int fd, bool dataOnly = false);
    void guard();
    void saveSuper();
    Transaction transaction();
    void commit(Transaction &tx);
    void apply(const Transaction &tx);
    Bytes encodeTransaction(const Transaction &tx, bool committed);
    Transaction decodeTransaction(const Bytes &bytes);
    void recover();
    void scan(Id id, const std::function<void(const Record &)> &fn, std::uint64_t *generation = nullptr);
    std::optional<Entry> lookup(Id id, const std::string &name);
    std::shared_ptr<const Entries> cached(Id id);
    void invalidate(Id id);
    void updateCache(Id id, const Bytes &records);
    std::pair<Id, std::string> parent(const std::string &path);
    void append(Transaction &tx, Id parent, const std::vector<Record> &records);
    void metaAction(Transaction &tx, Meta m);
    void collect(Id id);
    std::vector<Id> objects();
    void cleanup();
};
void fault(const char *point);
} // namespace personalfs
