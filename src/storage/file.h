#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
// Keep the Windows ABI declaration narrow: Windows.h defines DELETE and other
// macros that collide with the SQL AST. This rename stays on the same volume.
extern "C" __declspec(dllimport) int __stdcall MoveFileExA(const char *, const char *, unsigned long);
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/uio.h>
#endif
#endif

namespace mvd {
// Visit the byte ranges that must be written into a new file extent. Only
// complete, file-aligned zero blocks may become holes; partial blocks stay in
// a neighbouring write. A final zero suffix may wait for the next append.
inline constexpr size_t kSparseIoMinRun = 16 * 1024;
template <class Write>
inline bool write_nonzero_extents(const void *data, size_t length,
                                  uint64_t offset, const Write &write) {
  static constexpr std::array<uint8_t, 4096> zeros{};
  const auto *bytes = static_cast<const uint8_t *>(data);
  size_t begin = 0;
  size_t position = (zeros.size() - offset % zeros.size()) % zeros.size();
  while (position + zeros.size() <= length) {
    if (std::memcmp(bytes + position, zeros.data(), zeros.size()) != 0) {
      position += zeros.size();
      continue;
    }
    size_t zero_begin = position;
    do {
      position += zeros.size();
    } while (position + zeros.size() <= length &&
             std::memcmp(bytes + position, zeros.data(), zeros.size()) == 0);
    bool suffix = length - position < zeros.size() &&
        std::memcmp(bytes + position, zeros.data(), length - position) == 0;
    // Keep small interior gaps in one large write. The established sparse
    // suffix needs no additional I/O boundary and remains worthwhile.
    if (position - zero_begin >= kSparseIoMinRun || suffix) {
      if (zero_begin > begin && !write(begin, zero_begin - begin)) return false;
      begin = position;
      if (suffix) return true;
    }
  }
  return begin == length || write(begin, length - begin);
}

inline bool path_exists(const std::string &path) {
#ifdef _WIN32
  struct _stat64 st;
  return _stat64(path.c_str(), &st) == 0;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
#endif
}

inline void make_dir(const std::string &path) {
#ifdef _WIN32
  _mkdir(path.c_str());
#else
  ::mkdir(path.c_str(), 0755);
#endif
}

class FileHandle {
 public:
  FileHandle() = default;
  explicit FileHandle(int fd) noexcept : fd_(fd) {}
  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;
  FileHandle(FileHandle &&other) noexcept
      : fd_(other.fd_) {
#ifdef __linux__
    sparse_holes_ = other.sparse_holes_;
#endif
    other.fd_ = -1;
  }
  FileHandle &operator=(FileHandle &&other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
#ifdef __linux__
      sparse_holes_ = other.sparse_holes_;
#endif
      other.fd_ = -1;
    }
    return *this;
  }
  ~FileHandle() { close(); }

  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  void close() {
    if (fd_ >= 0) {
#ifdef _WIN32
      _close(fd_);
#else
      ::close(fd_);
#endif
      fd_ = -1;
    }
  }

  static FileHandle open(const std::string &path, int flags) {
#ifdef _WIN32
    int fd = _open(path.c_str(), flags | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = ::open(path.c_str(), flags, 0644);
#endif
    return FileHandle(fd);
  }

  int64_t size() const {
    if (fd_ < 0) return -1;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(io_mutex_);
    return _lseeki64(fd_, 0, SEEK_END);
#else
    return ::lseek(fd_, 0, SEEK_END);
#endif
  }

  bool pread_full(void *buf, size_t len, uint64_t offset) const {
    uint8_t *p = static_cast<uint8_t *>(buf);
    size_t done = 0;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(io_mutex_);
    while (done < len) {
      if (_lseeki64(fd_, static_cast<__int64>(offset + done), SEEK_SET) < 0)
        return false;
      int n = _read(fd_, p + done, static_cast<unsigned>(len - done));
      if (n < 0) return false;
      if (n == 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
#else
    while (done < len) {
      ssize_t n = ::pread(fd_, p + done, len - done, offset + done);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
#endif
  }

  bool pwrite_full(const void *buf, size_t len, uint64_t offset) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t done = 0;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(io_mutex_);
    while (done < len) {
      if (_lseeki64(fd_, static_cast<__int64>(offset + done), SEEK_SET) < 0)
        return false;
      int n = _write(fd_, p + done, static_cast<unsigned>(len - done));
      if (n < 0) return false;
      if (n == 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
#else
    while (done < len) {
      ssize_t n = ::pwrite(fd_, p + done, len - done, offset + done);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
#endif
  }

#ifdef __linux__
  bool pwritev_full(const iovec *vectors, int count, uint64_t offset) {
    std::vector<iovec> pending(vectors, vectors+count);
    size_t first = 0;
    while (first < pending.size()) {
      ssize_t written = ::pwritev(fd_, pending.data()+first,
          static_cast<int>(pending.size()-first), static_cast<off_t>(offset));
      if (written < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (written == 0) return false;
      offset += static_cast<uint64_t>(written);
      size_t used = static_cast<size_t>(written);
      while (first < pending.size() && used >= pending[first].iov_len) {
        used -= pending[first].iov_len;
        ++first;
      }
      if (first < pending.size() && used) {
        pending[first].iov_base = static_cast<uint8_t *>(pending[first].iov_base)+used;
        pending[first].iov_len -= used;
      }
    }
    return true;
  }

  // Full aligned table pages, with the same logical bytes as pwritev_full.
  // New zero blocks need no write. Existing bytes must be cleared before
  // they can be skipped; hole punching is an optional filesystem facility.
  bool pwritev_sparse(const iovec *vectors, int count, uint64_t offset) {
    if (count <= 0) return count == 0;
    constexpr size_t block = 4096;
    uint64_t length = 0;
    for (int i = 0; i < count; ++i) {
      if (vectors[i].iov_len % block || offset % block)
        return pwritev_full(vectors, count, offset);
      length += vectors[i].iov_len;
    }
    if (!length) return true;
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - offset)
      return false;
    int64_t old_size = size();
    if (old_size < 0) return false;
    if (offset + length > static_cast<uint64_t>(old_size) && !ftruncate(offset + length))
      return false;
    alignas(block) static constexpr std::array<uint8_t, block> zeros{};
    std::vector<iovec> pending;
    pending.reserve(static_cast<size_t>(count));
    uint64_t position = offset, data_offset = offset, zero_offset = offset;
    uint64_t zero_length = 0;
    auto flush_data = [&]() {
      if (pending.empty()) return true;
      bool ok = pending.size() == 1
          ? pwrite_full(pending[0].iov_base, pending[0].iov_len, data_offset)
          : pwritev_full(pending.data(), static_cast<int>(pending.size()), data_offset);
      pending.clear();
      return ok;
    };
    auto flush_zero = [&]() {
      if (!zero_length) return true;
      uint64_t begin = zero_offset, bytes = zero_length;
      zero_length = 0;
      if (begin >= static_cast<uint64_t>(old_size)) return true;
      uint64_t existing = std::min(bytes, static_cast<uint64_t>(old_size) - begin);
      if (sparse_holes_) {
        int result;
        do {
          result = ::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                               static_cast<off_t>(begin), static_cast<off_t>(existing));
        } while (result != 0 && errno == EINTR);
        if (result == 0) return true;
        sparse_holes_ = false;
      }
      // A failed/unsupported punch may have done partial work. Writing all
      // zeros is idempotent and also supports filesystems or sandboxes that
      // permit normal writes but deny fallocate.
      std::array<iovec, 64> zero_vectors;
      for (auto &v : zero_vectors) v = {const_cast<uint8_t *>(zeros.data()), block};
      while (bytes) {
        size_t blocks = static_cast<size_t>(std::min<uint64_t>(bytes / block, zero_vectors.size()));
        if (!pwritev_full(zero_vectors.data(), static_cast<int>(blocks), begin)) return false;
        begin += blocks * block;
        bytes -= blocks * block;
      }
      return true;
    };
    for (int i = 0; i < count; ++i) {
      auto *bytes = static_cast<uint8_t *>(vectors[i].iov_base);
      size_t begin = 0;
      while (begin < vectors[i].iov_len) {
        bool zero = std::memcmp(bytes + begin, zeros.data(), block) == 0;
        size_t end = begin + block;
        while (end < vectors[i].iov_len &&
               (std::memcmp(bytes + end, zeros.data(), block) == 0) == zero)
          end += block;
        if (zero && end - begin >= kSparseIoMinRun) {
          if (!flush_data()) return false;
          if (!zero_length) zero_offset = position;
          zero_length += end - begin;
        } else {
          if (!flush_zero()) return false;
          if (pending.empty()) data_offset = position;
          // Coalesce only within this input buffer, preserving the page
          // boundaries between independently allocated scatter vectors.
          if (begin != 0 && !pending.empty() &&
              static_cast<uint8_t *>(pending.back().iov_base) + pending.back().iov_len == bytes + begin)
            pending.back().iov_len += end - begin;
          else
            pending.push_back({bytes + begin, end - begin});
        }
        position += end - begin;
        begin = end;
      }
    }
    return flush_data() && flush_zero();
  }
#endif

  bool append(const void *buf, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t done = 0;
    while (done < len) {
#ifdef _WIN32
      int n = _write(fd_, p + done, static_cast<unsigned>(len - done));
#else
      ssize_t n = ::write(fd_, p + done, len - done);
#endif
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
  }

  bool fsync() {
    if (fd_ < 0) return false;
#ifdef _WIN32
    return _commit(fd_) == 0;
#else
    return ::fsync(fd_) == 0;
#endif
  }

  // Only call after syncing the corresponding writes. Clean WAL cache pages
  // are redundant with our bounded buffers and count toward cgroup memory.
  void discard_clean_cache(uint64_t offset, uint64_t length) const {
#ifdef __linux__
    long page_bytes = ::sysconf(_SC_PAGESIZE);
    if (page_bytes <= 0) return;
    uint64_t alignment = static_cast<uint64_t>(page_bytes);
    uint64_t begin = offset / alignment * alignment;
    uint64_t end = (offset + length) / alignment * alignment;
    if (end > begin)
      (void)::posix_fadvise(fd_, static_cast<off_t>(begin),
                            static_cast<off_t>(end - begin), POSIX_FADV_DONTNEED);
#else
    (void)offset;
    (void)length;
#endif
  }

  bool ftruncate(uint64_t size) {
    if (fd_ < 0) return false;
#ifdef _WIN32
    return _chsize_s(fd_, static_cast<__int64>(size)) == 0;
#else
    return ::ftruncate(fd_, static_cast<off_t>(size)) == 0;
#endif
  }

 private:
  int fd_ = -1;
#ifdef __linux__
  bool sparse_holes_ = true;
#endif
  mutable std::mutex io_mutex_;
};

inline void fsync_dir(const std::string &dir) {
#ifdef _WIN32
  (void)dir;
#else
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#endif
}

inline bool read_file(const std::string &path, std::string &out) {
  FileHandle f = FileHandle::open(path, O_RDONLY);
  if (!f.valid()) return false;
  int64_t sz = f.size();
  if (sz < 0) return false;
  out.resize(static_cast<size_t>(sz));
  if (sz == 0) return true;
  return f.pread_full(out.data(), out.size(), 0);
}

inline bool write_file_atomic(const std::string &path, const std::string &data) {
  std::string tmp = path + ".tmp";
#ifndef _WIN32
  // Prepare allocations before rename: no bad_alloc may turn a published
  // directory into a reported failure and cause its table to be rolled back.
  size_t slash = path.find_last_of('/');
  const std::string parent = slash == std::string::npos ? "." : path.substr(0, slash);
#endif
  {
    FileHandle f = FileHandle::open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f.valid()) return false;
    if (!data.empty() && !f.append(data.data(), data.size())) return false;
    if (!f.fsync()) return false;
  }
  bool renamed = false;
#ifdef _WIN32
  // MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH. Do not unlink the old
  // directory before attempting replacement: a failed rename must preserve it.
  renamed = MoveFileExA(tmp.c_str(), path.c_str(), 0x1ul | 0x8ul) != 0;
#else
  renamed = ::rename(tmp.c_str(), path.c_str()) == 0;
#endif
  if (!renamed) return false;
#ifndef _WIN32
  // Persist the rename itself.
  fsync_dir(parent);
#endif
  return true;
}

}
