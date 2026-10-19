#include "fuse/FuseMain.hpp"
#include "storage/Store.hpp"
#include <iostream>
#include <sys/xattr.h>
namespace
{
std::uint64_t number(const std::string &text)
{
    std::size_t used = 0;
    if (text.empty() || text[0] == '-')
        throw std::invalid_argument("expected unsigned integer");
    auto n = std::stoull(text, &used);
    if (used != text.size())
        throw std::invalid_argument("expected unsigned integer");
    return n;
}
void usage()
{
    std::cout << "PersonalFS v2 (independent object store)\n"
                 "  personalfs init STORE [--objects-per-page N]\n"
                 "  personalfs mount STORE MOUNTPOINT [--foreground] [--allow-other] [--cache-bytes N] [-o "
                 "OPTIONS]\n"
                 "  personalfs inspect STORE [--object ID]\n"
                 "  personalfs fsck STORE --check|--repair\n"
                 "  personalfs compact-dir STORE ID\n"
                 "  personalfs stats MOUNTPOINT\n";
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (!args.empty() && args[0] == "v2")
            args.erase(args.begin());
        if (args.empty() || args[0] == "--help")
        {
            usage();
            return 0;
        }
        if (args.size() < 2)
            throw std::invalid_argument("missing store or mountpoint");
        auto command = args[0];
        if (command == "init")
        {
            std::uint64_t pages = 4096;
            if (args.size() != 2)
            {
                if (args.size() != 4 || args[2] != "--objects-per-page")
                    throw std::invalid_argument("invalid init arguments");
                pages = number(args[3]);
            }
            if (pages < 1 || pages > 16384)
                throw std::invalid_argument("invalid objects-per-page");
            personalfs::Store::init(args[1], pages);
            std::cout << "Initialized PersonalFS v2 store\n";
            return 0;
        }
        if (command == "stats")
        {
            if (args.size() != 2)
                throw std::invalid_argument("stats accepts one mountpoint");
            std::vector<char> bytes(1024 * 1024);
            auto n = ::getxattr(args[1].c_str(), "user.personalfs.stats", bytes.data(), bytes.size());
            if (n < 0)
                personalfs::fail(errno, "read mounted statistics");
            std::cout.write(bytes.data(), n) << '\n';
            return 0;
        }
        if (command == "mount")
        {
            if (args.size() < 3)
                throw std::invalid_argument("missing mountpoint");
            std::size_t cache = 64 * 1024 * 1024;
            std::vector<std::string> options;
            for (std::size_t i = 3; i < args.size(); ++i)
            {
                if (args[i] == "--foreground")
                    options.push_back("-f");
                else if (args[i] == "--allow-other")
                {
                    options.push_back("-o");
                    options.push_back("allow_other");
                }
                else if (args[i] == "--cache-bytes" && i + 1 < args.size())
                    cache = number(args[++i]);
                else if (args[i] == "-o" && i + 1 < args.size())
                {
                    options.push_back("-o");
                    options.push_back(args[++i]);
                }
                else
                    throw std::invalid_argument("unknown mount option");
            }
            personalfs::Store store(args[1], personalfs::Store::Mode::ReadWrite, cache);
            return personalfs::mount(store, args[2], options);
        }
        if (command == "inspect")
        {
            if (args.size() != 2 && (args.size() != 4 || args[2] != "--object"))
                throw std::invalid_argument("invalid inspect arguments");
            personalfs::Store store(args[1], personalfs::Store::Mode::Check);
            std::cout << store.inspect(args.size() == 4 ? std::optional<personalfs::Id>{number(args[3])}
                                                        : std::nullopt)
                      << '\n';
            return 0;
        }
        if (command == "fsck")
        {
            if (args.size() != 3 || (args[2] != "--check" && args[2] != "--repair"))
                throw std::invalid_argument("choose --check or --repair");
            personalfs::Store store(args[1],
                                    args[2] == "--check" ? personalfs::Store::Mode::Check
                                                         : personalfs::Store::Mode::ReadWrite,
                                    0);
            std::cout << store.fsck() << '\n';
            return 0;
        }
        if (command == "compact-dir")
        {
            if (args.size() != 3)
                throw std::invalid_argument("missing directory ObjectID");
            personalfs::Store store(args[1]);
            store.compact(number(args[2]));
            std::cout << store.fsck() << '\n';
            return 0;
        }
        throw std::invalid_argument("unknown command (use --help)");
    }
    catch (const std::exception &e)
    {
        std::cerr << "personalfs: " << e.what() << '\n';
        return 1;
    }
}
