// MiniVectorDB - common utilities
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

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------
struct DbError {
  std::string code;
  std::string message;
};

[[noreturn]] inline void throw_error(const std::string &code,
                                     const std::string &message) {
  throw DbError{code, message};
}

// ---------------------------------------------------------------------------
// CRC32 (IEEE)
// ---------------------------------------------------------------------------
inline uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
  static constexpr auto tables = [] {
    std::array<std::array<uint32_t, 256>, 8> result{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit)
        c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      result[0][i] = c;
    }
    for (size_t slice = 1; slice < result.size(); ++slice)
      for (size_t i = 0; i < result[slice].size(); ++i) {
        uint32_t previous = result[slice - 1][i];
        result[slice][i] = result[0][previous & 255U] ^ (previous >> 8);
      }
    return result;
  }();
  const auto *bytes = static_cast<const uint8_t *>(data);
  crc = ~crc;
  // Each table advances one byte's contribution by the remaining bytes.
  if constexpr (std::endian::native == std::endian::little) {
    while (len >= 8) {
      uint64_t word;
      std::memcpy(&word, bytes, sizeof(word));
      word ^= crc;
      crc = tables[7][word & 255U] ^ tables[6][(word >> 8) & 255U] ^
            tables[5][(word >> 16) & 255U] ^ tables[4][(word >> 24) & 255U] ^
            tables[3][(word >> 32) & 255U] ^ tables[2][(word >> 40) & 255U] ^
            tables[1][(word >> 48) & 255U] ^ tables[0][word >> 56];
      bytes += 8;
      len -= 8;
    }
  }
  for (size_t i = 0; i < len; ++i)
    crc = tables[0][(crc ^ bytes[i]) & 255U] ^ (crc >> 8);
  return ~crc;
}

inline uint32_t crc32(const void *data, size_t len) {
  return crc32_update(0, data, len);
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------
// Returns the number of Unicode code points, or -1 if the string is invalid
// UTF-8 (overlong, surrogate, > U+10FFFF, truncated).
inline int utf8_codepoint_count(std::string_view s) {
  int count = 0;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    size_t n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 4;
    } else {
      return -1;
    }
    if (i + n > s.size()) return -1;
    for (size_t k = 1; k < n; ++k) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return -1;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
        (n == 4 && cp < 0x10000))
      return -1;
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
    if (cp > 0x10FFFF) return -1;
    ++count;
    i += n;
  }
  return count;
}

// Lexicographic UTF-8 byte order equals Unicode code point order for valid
// UTF-8.  We store strings as UTF-8 and compare bytewise.

// ---------------------------------------------------------------------------
// Number parsing
// ---------------------------------------------------------------------------
inline bool parse_int64(std::string_view text, int64_t &out) {
  if (text.empty()) return false;
  const char *begin = text.data();
  const char *end = begin + text.size();
  // from_chars does not accept a leading '+', handle it ourselves.
  bool neg = false;
  if (*begin == '+') {
    ++begin;
  } else if (*begin == '-') {
    neg = true;
    ++begin;
  }
  if (begin == end) return false;
  uint64_t magnitude = 0;
  for (const char *p = begin; p != end; ++p) {
    if (*p < '0' || *p > '9') return false;
    unsigned digit = static_cast<unsigned>(*p - '0');
    if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10)
      return false;
    magnitude = magnitude * 10 + digit;
  }
  if (neg) {
    if (magnitude > (uint64_t{1} << 63)) return false;
    out = (magnitude == (uint64_t{1} << 63))
              ? std::numeric_limits<int64_t>::min()
              : -static_cast<int64_t>(magnitude);
  } else {
    if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
    out = static_cast<int64_t>(magnitude);
  }
  return true;
}

enum class Float32Parse { VALID, INVALID, OVERFLOW };

inline Float32Parse parse_float32_value(std::string_view text, float &out) {
  if (text.empty()) return Float32Parse::INVALID;
  const char *begin = text.data(), *end = begin + text.size();
  if (*begin == '+') {
    ++begin;
    if (begin == end || *begin == '+' || *begin == '-') return Float32Parse::INVALID;
  }
  float value = 0;
  auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
  if (parsed.ec == std::errc{} && parsed.ptr == end) {
    if (!std::isfinite(value)) return Float32Parse::INVALID;
    out = value;
    return Float32Parse::VALID;
  }
  // Preserve strtof semantics for underflow, finite boundary rounding, hex
  // forms, and diagnostics. Ordinary decimal inputs avoid a temporary string.
  std::string tmp(text);
  errno = 0;
  char *tail = nullptr;
  float v = std::strtof(tmp.c_str(), &tail);
  if (tail == tmp.c_str() || tail != tmp.c_str() + tmp.size()) return Float32Parse::INVALID;
  if (!std::isfinite(v))
    return errno == ERANGE ? Float32Parse::OVERFLOW : Float32Parse::INVALID;
  out = v;
  return Float32Parse::VALID;
}

inline bool parse_float32(std::string_view text, float &out) {
  return parse_float32_value(text, out) == Float32Parse::VALID;
}

// Parses a vector literal of the form "[1, 2.5, -3e-2]" (brackets required).
inline bool parse_vector_literal(std::string_view text, int dim,
                                 std::vector<float> &out) {
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
    ++i;
  if (i >= text.size() || text[i] != '[') return false;
  ++i;
  out.clear();
  out.reserve(static_cast<size_t>(dim));
  while (true) {
    while (i < text.size() &&
           std::isspace(static_cast<unsigned char>(text[i])))
      ++i;
    if (i < text.size() && text[i] == ']') {
      ++i;
      break;
    }
    size_t start = i;
    while (i < text.size() && text[i] != ',' && text[i] != ']') ++i;
    size_t end = i;
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
      --end;
    if (end == start) return false;
    float v;
    if (!parse_float32(text.substr(start, end - start), v)) return false;
    out.push_back(v);
    if (out.size() > static_cast<size_t>(dim)) return false;
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    if (i < text.size() && text[i] == ']') {
      ++i;
      break;
    }
    return false;
  }
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
    ++i;
  return i == text.size();  // caller checks dimension
}

// ---------------------------------------------------------------------------
// JSON escaping / formatting
// ---------------------------------------------------------------------------
inline void json_escape_append(std::string &out, std::string_view s) {
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
}

inline std::string format_double(double v) {
  char buf[64];
  // 17 significant digits round-trip for double, but output a compact form.
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  std::string s(buf);
  // Prefer shortest representation that round-trips is complex; %.17g is fine
  // for JSON parsers and avoids precision loss.
  return s;
}

inline std::string format_float_g(float v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
// Visit the byte ranges that must be written into a new file extent. Only
// complete, file-aligned zero blocks may become holes; partial blocks stay in
// a neighbouring write. A final zero suffix may wait for the next append.
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
    if (position > begin && !write(begin, position - begin)) return false;
    do {
      position += zeros.size();
    } while (position + zeros.size() <= length &&
             std::memcmp(bytes + position, zeros.data(), zeros.size()) == 0);
    begin = position;
    if (length - begin < zeros.size() &&
        std::memcmp(bytes + begin, zeros.data(), length - begin) == 0)
      return true;
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
  explicit FileHandle(int fd) : fd_(fd) {}
  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;
  FileHandle(FileHandle &&other) noexcept
      : fd_(other.fd_), io_mutex_(std::move(other.io_mutex_)) {
#ifdef __linux__
    sparse_holes_ = other.sparse_holes_;
#endif
    other.fd_ = -1;
  }
  FileHandle &operator=(FileHandle &&other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      io_mutex_ = std::move(other.io_mutex_);
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
    return _lseeki64(fd_, 0, SEEK_END);
#else
    return ::lseek(fd_, 0, SEEK_END);
#endif
  }

  bool pread_full(void *buf, size_t len, uint64_t offset) const {
    uint8_t *p = static_cast<uint8_t *>(buf);
    size_t done = 0;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(*io_mutex_);
    while (done < len) {
      _lseeki64(fd_, static_cast<__int64>(offset + done), SEEK_SET);
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
    std::lock_guard<std::mutex> lock(*io_mutex_);
    while (done < len) {
      _lseeki64(fd_, static_cast<__int64>(offset + done), SEEK_SET);
      int n = _write(fd_, p + done, static_cast<unsigned>(len - done));
      if (n < 0) return false;
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
        if (zero) {
          if (!flush_data()) return false;
          if (!zero_length) zero_offset = position;
          zero_length += end - begin;
        } else {
          if (!flush_zero()) return false;
          if (pending.empty()) data_offset = position;
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
  std::unique_ptr<std::mutex> io_mutex_ = std::make_unique<std::mutex>();
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
  {
    FileHandle f = FileHandle::open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f.valid()) return false;
    if (!data.empty() && !f.append(data.data(), data.size())) return false;
    if (!f.fsync()) return false;
  }
  bool renamed = false;
#ifdef _WIN32
  std::remove(path.c_str());
  renamed = std::rename(tmp.c_str(), path.c_str()) == 0;
#else
  renamed = ::rename(tmp.c_str(), path.c_str()) == 0;
#endif
  if (!renamed) return false;
#ifndef _WIN32
  // Persist the rename itself.
  size_t slash = path.find_last_of('/');
  fsync_dir(slash == std::string::npos ? "." : path.substr(0, slash));
#endif
  return true;
}

inline void write_u32(std::string &out, uint32_t v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}
inline void write_i32(std::string &out, int32_t v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}
inline void write_u64(std::string &out, uint64_t v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}
inline void write_u8(std::string &out, uint8_t v) { out.push_back(static_cast<char>(v)); }

struct Reader {
  const std::string &data;
  size_t pos = 0;
  explicit Reader(const std::string &d) : data(d) {}
  bool u8(uint8_t &v) {
    if (pos + 1 > data.size()) return false;
    v = static_cast<uint8_t>(data[pos++]);
    return true;
  }
  bool u32(uint32_t &v) {
    if (pos + 4 > data.size()) return false;
    std::memcpy(&v, data.data() + pos, 4);
    pos += 4;
    return true;
  }
  bool i32(int32_t &v) {
    if (pos + 4 > data.size()) return false;
    std::memcpy(&v, data.data() + pos, 4);
    pos += 4;
    return true;
  }
  bool u64(uint64_t &v) {
    if (pos + 8 > data.size()) return false;
    std::memcpy(&v, data.data() + pos, 8);
    pos += 8;
    return true;
  }
  bool str(std::string &s) {
    uint32_t n;
    if (!u32(n)) return false;
    if (pos + n > data.size()) return false;
    s.assign(data.data() + pos, n);
    pos += n;
    return true;
  }
};

}  // namespace mvd
