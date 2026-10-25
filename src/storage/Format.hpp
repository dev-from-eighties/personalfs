#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <vector>
namespace personalfs
{
using Bytes = std::vector<std::uint8_t>;
using Id = std::uint64_t;
[[noreturn]] void fail(int code, const std::string &what);
void check(bool condition, const std::string &what);
struct Encoder
{
    Bytes bytes;
    void u8(std::uint8_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void raw(const Bytes &v);
    void str(const std::string &v);
};
struct Decoder
{
    const Bytes &bytes;
    std::size_t pos = 0;
    std::uint8_t u8();
    std::uint32_t u32();
    std::uint64_t u64();
    Bytes raw(std::size_t n);
    std::string str();
    void end();
};
std::uint32_t crc32c(const Bytes &bytes);
Bytes seal(const std::string &magic, const Bytes &payload);
Bytes unseal(const std::string &magic, const Bytes &bytes);
struct Super
{
    std::uint32_t minorVersion = 1;
    std::uint32_t pageSize = 4096;
    Id nextId = 1;
    std::uint64_t nextTx = 1;
    Bytes encode() const;
    static Super decode(const Bytes &bytes);
};
enum class Kind : std::uint8_t
{
    File = 1,
    Directory = 2,
    Symlink = 3
};
struct Meta
{
    Id id = 0, parent = 0;
    Kind kind = Kind::Directory;
    std::uint32_t mode = 0755, uid = 0, gid = 0, nlink = 1;
    std::uint64_t generation = 0, logSequence = 0, childDirectories = 0;
    timespec atime{}, mtime{}, ctime{};
    Bytes encode() const;
    static Meta decode(const Bytes &bytes);
};
struct Entry
{
    Id id;
    Kind kind;
    bool operator==(const Entry &) const = default;
};
struct Record
{
    bool add = true;
    std::uint64_t sequence = 0, tx = 0;
    std::string name;
    Entry entry{};
    Bytes encode() const;
    static Record decode(const Bytes &bytes);
};
Bytes directoryHeader(Id id, std::uint64_t generation = 0);
constexpr std::size_t directoryHeaderSize = 32;
mode_t typeMode(Kind kind);
timespec now();
} // namespace personalfs
