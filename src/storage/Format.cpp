#include "storage/Format.hpp"
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <limits>
namespace personalfs
{
void fail(int code, const std::string &what)
{
    throw std::system_error(code, std::generic_category(), what);
}
void check(bool condition, const std::string &what)
{
    if (!condition)
        fail(EIO, what);
}
void Encoder::u8(std::uint8_t v)
{
    bytes.push_back(v);
}
void Encoder::u32(std::uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        u8(v >> (i * 8));
}
void Encoder::u64(std::uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i)
        u8(v >> (i * 8));
}
void Encoder::raw(const Bytes &v)
{
    bytes.insert(bytes.end(), v.begin(), v.end());
}
void Encoder::str(const std::string &v)
{
    check(v.size() <= UINT32_MAX, "string too long");
    u32(v.size());
    bytes.insert(bytes.end(), v.begin(), v.end());
}
std::uint8_t Decoder::u8()
{
    check(pos < bytes.size(), "truncated binary structure");
    return bytes[pos++];
}
std::uint32_t Decoder::u32()
{
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= std::uint32_t(u8()) << (i * 8);
    return v;
}
std::uint64_t Decoder::u64()
{
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= std::uint64_t(u8()) << (i * 8);
    return v;
}
Bytes Decoder::raw(std::size_t n)
{
    check(n <= bytes.size() - pos, "invalid binary length");
    Bytes r(bytes.begin() + pos, bytes.begin() + pos + n);
    pos += n;
    return r;
}
std::string Decoder::str()
{
    auto r = raw(u32());
    return {r.begin(), r.end()};
}
void Decoder::end()
{
    check(pos == bytes.size(), "unexpected trailing bytes");
}
std::uint32_t crc32c(const Bytes &bytes)
{
    std::uint32_t crc = ~0U;
    for (auto b : bytes)
    {
        crc ^= b;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
    }
    return ~crc;
}
Bytes seal(const std::string &magic, const Bytes &payload)
{
    check(magic.size() == 8, "internal magic length");
    Encoder e;
    for (auto c : magic)
        e.u8(c);
    e.u32(payload.size());
    e.raw(payload);
    e.u32(crc32c(e.bytes));
    return e.bytes;
}
Bytes unseal(const std::string &magic, const Bytes &bytes)
{
    check(bytes.size() >= 16, "truncated envelope");
    Decoder d{bytes};
    auto m = d.raw(8);
    check(std::string(m.begin(), m.end()) == magic, "unsupported magic or format");
    auto p = d.raw(d.u32());
    auto crc = d.u32();
    d.end();
    check(crc32c(Bytes(bytes.begin(), bytes.end() - 4)) == crc, "CRC32C integrity failure");
    return p;
}
Bytes Super::encode() const
{
    Encoder e;
    e.u32(2);
    e.u32(minorVersion);
    e.u32(pageSize);
    e.u32(16);
    e.u64(0);
    e.u64(nextId);
    e.u64(nextTx);
    e.u64(0);
    return seal("PFS2SUPR", e.bytes);
}
Super Super::decode(const Bytes &bytes)
{
    auto p = unseal("PFS2SUPR", bytes);
    Decoder d{p};
    Super s;
    check(d.u32() == 2, "unsupported format version");
    s.minorVersion = d.u32();
    check(s.minorVersion <= 1, "unsupported format version");
    s.pageSize = d.u32();
    check(s.pageSize >= 1 && s.pageSize <= 16384, "invalid objects_per_page");
    check(d.u32() == 16, "unsupported page encoding");
    check(d.u64() == 0, "invalid root ID");
    s.nextId = d.u64();
    s.nextTx = d.u64();
    check(d.u64() == 0, "unsupported flags");
    d.end();
    check(s.nextId > 0 && s.nextTx > 0, "invalid allocator");
    return s;
}
timespec now()
{
    timespec t{};
    if (clock_gettime(CLOCK_REALTIME, &t))
        fail(errno, "clock_gettime");
    return t;
}
mode_t typeMode(Kind k)
{
    switch (k)
    {
    case Kind::File:
        return S_IFREG;
    case Kind::Directory:
        return S_IFDIR;
    case Kind::Symlink:
        return S_IFLNK;
    }
    fail(EIO, "invalid object type");
}
Bytes Meta::encode() const
{
    Encoder e;
    e.u32(2);
    e.u64(id);
    e.u8(static_cast<std::uint8_t>(kind));
    e.u32(mode);
    e.u32(uid);
    e.u32(gid);
    e.u32(nlink);
    e.u64(parent);
    e.u64(generation);
    e.u64(logSequence);
    e.u64(childDirectories);
    for (auto t : {atime, mtime, ctime})
    {
        e.u64(static_cast<std::uint64_t>(t.tv_sec));
        e.u32(t.tv_nsec);
    }
    return seal("PFS2META", e.bytes);
}
Meta Meta::decode(const Bytes &bytes)
{
    auto p = unseal("PFS2META", bytes);
    Decoder d{p};
    Meta m;
    check(d.u32() == 2, "metadata version");
    m.id = d.u64();
    m.kind = static_cast<Kind>(d.u8());
    typeMode(m.kind);
    m.mode = d.u32();
    m.uid = d.u32();
    m.gid = d.u32();
    m.nlink = d.u32();
    m.parent = d.u64();
    m.generation = d.u64();
    m.logSequence = d.u64();
    m.childDirectories = d.u64();
    for (auto t : {&m.atime, &m.mtime, &m.ctime})
    {
        t->tv_sec = static_cast<std::int64_t>(d.u64());
        t->tv_nsec = d.u32();
        check(t->tv_nsec < 1000000000, "invalid timestamp");
    }
    check((m.mode & ~07777U) == 0 && m.nlink <= 1, "invalid attributes");
    d.end();
    return m;
}
Bytes Record::encode() const
{
    Encoder e;
    e.u8(add ? 1 : 2);
    e.u64(sequence);
    e.u64(tx);
    e.str(name);
    e.u64(entry.id);
    e.u8(static_cast<std::uint8_t>(entry.kind));
    return seal("PFS2DREC", e.bytes);
}
Record Record::decode(const Bytes &bytes)
{
    auto p = unseal("PFS2DREC", bytes);
    Decoder d{p};
    Record r;
    auto op = d.u8();
    check(op == 1 || op == 2, "invalid directory opcode");
    r.add = op == 1;
    r.sequence = d.u64();
    r.tx = d.u64();
    r.name = d.str();
    r.entry.id = d.u64();
    r.entry.kind = static_cast<Kind>(d.u8());
    typeMode(r.entry.kind);
    d.end();
    check(!r.name.empty() && r.name.size() <= 255 && r.name != "." && r.name != ".." &&
              r.name.find('/') == std::string::npos && r.name.find('\0') == std::string::npos,
          "invalid entry name");
    return r;
}
Bytes directoryHeader(Id id, std::uint64_t generation)
{
    Encoder e;
    e.u64(id);
    e.u64(generation);
    return seal("PFS2DIRH", e.bytes);
}
} // namespace personalfs
