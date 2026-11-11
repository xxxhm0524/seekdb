#include "common.h"
#include <iostream>
#include <stdexcept>

#ifdef __linux__
static bool deny_punch = false;
extern "C" int __real_fallocate(int, int, off_t, off_t);
extern "C" int __wrap_fallocate(int fd, int mode, off_t offset, off_t length) {
  if (deny_punch) { errno = EOPNOTSUPP; return -1; }
  return __real_fallocate(fd, mode, offset, length);
}
#endif

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
  try {
    size_t checks = 0;
    std::array<uint8_t, 65536> source{};
    std::array<uint8_t, 65536> result{};
    for (unsigned pattern = 0; pattern < 5; ++pattern) {
      source.fill(0);
      if (pattern == 1) source.fill(0x5a);
      if (pattern == 2)
        for (size_t i = 0; i < source.size(); i += 8211) source[i] = 1;
      if (pattern == 3)
        for (size_t i = 0; i < 8193; ++i) source[i] = static_cast<uint8_t>(i | 1);
      if (pattern == 4) source[source.size()-1] = 1;
      for (uint64_t offset = 0; offset < 4096; ++offset) {
        result.fill(0);
        size_t previous_end = 0, written = 0;
        bool ok = mvd::write_nonzero_extents(source.data(), source.size(), offset,
            [&](size_t begin, size_t length) {
              require(begin >= previous_end && length > 0 && begin + length <= source.size(),
                      "invalid or overlapping extent");
              std::memcpy(result.data()+begin, source.data()+begin, length);
              previous_end = begin + length;
              written += length;
              return true;
            });
        require(ok && source == result, "a nonzero byte was omitted");
        if (pattern == 1) require(written == source.size(), "dense bytes were skipped");
        if (pattern == 0) require(written < 4096, "zero-only page was not sparse");
        ++checks;
      }
    }
    source.fill(1);
    require(!mvd::write_nonzero_extents(source.data(), source.size(), 0,
        [](size_t, size_t) { return false; }), "failed write was ignored");
    ++checks;
#ifdef __linux__
    require(argc == 2 || argc == 3, "supply a temporary file path");
    deny_punch = argc == 3;
    struct alignas(4096) Buffer { std::array<uint8_t, 65536> bytes; };
    auto buffers = std::make_unique<Buffer[]>(4);
    auto verify = std::make_unique<Buffer[]>(4);
    std::array<iovec, 4> vectors;
    for (size_t i = 0; i < 4; ++i) vectors[i] = {buffers[i].bytes.data(), 65536};
    for (bool direct : {false, true}) {
      auto file = mvd::FileHandle::open(argv[1], O_RDWR | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0));
      require(file.valid(), "cannot open model file");
      for (unsigned pass = 0; pass < 5; ++pass) {
        for (size_t i = 0; i < 4; ++i) {
          auto &bytes = buffers[i].bytes;
          bytes.fill(pass == 0 || pass == 3 ? 0xa5 : 0);
          if (pass == 1)
            for (size_t j = 0; j < bytes.size(); j += 16384) bytes[j] = static_cast<uint8_t>(i+1);
          if (pass == 4 && i == 3) bytes.back() = 0x6b;
        }
        require(file.pwritev_sparse(vectors.data(), 4, 65536), "sparse write failed");
        require(file.fsync() && file.size() == 5*65536, "sparse file size/sync is wrong");
        auto reader = mvd::FileHandle::open(argv[1], O_RDONLY);
        require(reader.pread_full(verify[0].bytes.data(), 65536, 0), "prefix read failed");
        require(std::all_of(verify[0].bytes.begin(), verify[0].bytes.end(), [](uint8_t b) { return b == 0; }),
                "write changed the prefix hole");
        for (size_t i = 0; i < 4; ++i) {
          require(reader.pread_full(verify[i].bytes.data(), 65536, (i+1)*65536), "readback failed");
          require(verify[i].bytes == buffers[i].bytes, "overwrite left stale nonzero bytes");
        }
        if (!deny_punch && pass == 2) {
          struct stat stats{};
          require(::fstat(file.fd(), &stats) == 0 && stats.st_blocks*512 < stats.st_size/2,
                  "zero-only overwrite did not release blocks");
        }
        ++checks;
      }
    }
    std::remove(argv[1]);
#else
    (void)argc; (void)argv;
#endif
    std::cout << "{\"passed\":true,\"checks\":" << checks
              << ",\"all_file_offsets_mod_4096\":true}" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
