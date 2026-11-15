// Exercise actual mappings and fallback reads through File's public interface.
#include "storage.hpp"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/mman.h>
#include <sys/resource.h>

static size_t maps = 0, unmaps = 0;
static bool reject_map = false;
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void*, size_t);
extern "C" void* __wrap_mmap(void* p, size_t n, int prot, int flags, int fd, off_t offset) {
    ++maps;
    if (reject_map) { errno = ENOMEM; return MAP_FAILED; }
    return __real_mmap(p, n, prot, flags, fd, offset);
}
extern "C" int __wrap_munmap(void* p, size_t n) {
    ++unmaps; return __real_munmap(p, n);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    ::setenv("MVD_READ_MAP_MIB", "64", 1);
    std::string original(8192, 'a'), changed(8192, 'b'), buffer(8192, '\0');
    {
        mvd::File file(dir / "database.mvd");
        file.write(0, original.data(), original.size());
        file.prepare_read_mapping(8192); assert(maps == 1);
        file.read(0, buffer.data(), buffer.size()); assert(buffer == original);
        file.prepare_read_mapping(8192); assert(maps == 1);
        file.write(0, changed.data(), changed.size()); assert(unmaps == 1);
        file.read(0, buffer.data(), buffer.size()); assert(buffer == changed);
        file.prepare_read_mapping(8192); assert(maps == 2);
        file.truncate(4096); assert(unmaps == 2);
        bool eof = false;
        try { file.read(4096, buffer.data(), 1); }
        catch (const mvd::IoError&) { eof = true; }
        assert(eof);
        file.prepare_read_mapping(4096); assert(maps == 3);
        file.read(4096, buffer.data(), 0);
        file.write(8192, original.data(), original.size()); assert(unmaps == 3);
        file.read(8192, buffer.data(), buffer.size()); assert(buffer == original);
        file.prepare_read_mapping(16384); assert(maps == 4);
        {
            mvd::File replacement(dir / "replacement.mvd");
            replacement.write(0, original.data(), original.size()); replacement.sync();
        }
        file.replace_with(dir / "replacement.mvd"); assert(unmaps == 4);
        assert(file.size() == 8192);
        file.read(0, buffer.data(), buffer.size()); assert(buffer == original);
        reject_map = true;
        file.prepare_read_mapping(8192); assert(maps == 5);
        file.read(0, buffer.data(), buffer.size()); assert(buffer == original);
        file.prepare_read_mapping(8192); assert(maps == 5); // Failed optional work is not retried per read.
        reject_map = false; file.release_read_mapping();
        struct rlimit old_limit {}; assert(::getrlimit(RLIMIT_AS, &old_limit) == 0);
        auto small_limit = old_limit; small_limit.rlim_cur = 16384;
        assert(::setrlimit(RLIMIT_AS, &small_limit) == 0);
        file.prepare_read_mapping(8192); assert(maps == 5);
        file.read(0, buffer.data(), buffer.size()); assert(buffer == original);
        assert(::setrlimit(RLIMIT_AS, &old_limit) == 0);
        file.release_read_mapping();
        file.truncate(65ULL << 20);
        file.prepare_read_mapping(file.size()); assert(maps == 5);
        file.read(0, buffer.data(), buffer.size()); assert(buffer == original);
        file.truncate(8192); file.prepare_read_mapping(8192); assert(maps == 6);
    }
    assert(unmaps == 5);
    ::setenv("MVD_READ_MAP_MIB", "0", 1);
    {
        mvd::File file(dir / "database.mvd"); file.prepare_read_mapping(8192); assert(maps == 6);
        file.read(0, buffer.data(), buffer.size()); assert(buffer == original);
    }
    for (const char* bad : {"", "-1", "65", "1x", "18446744073709551616"}) {
        ::setenv("MVD_READ_MAP_MIB", bad, 1); bool refused = false;
        try { mvd::File file(dir / "invalid.mvd"); }
        catch (const mvd::Error&) { refused = true; }
        assert(refused);
    }
    std::cout << "{\"passed\":true,\"actual_mapping_attempts\":" << maps
              << ",\"actual_unmaps\":" << unmaps
              << ",\"write_truncate_replace_invalidation\":true,\"mmap_enomem_fallback\":true,\"rlimit_and_size_fallback\":true}\n";
}
