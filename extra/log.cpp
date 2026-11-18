#include "log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mini {
namespace {

constexpr std::string_view legacy_magic = "MINIVDB1";
constexpr std::string_view dense_magic = "MINIVDB2";
constexpr std::string_view decimal_magic = "MINIVDB4";
constexpr std::string_view dyadic_magic = "MINIVDB5";
constexpr std::string_view magic = "MINIVDB3";
static_assert(magic.size() == Journal::Head && dense_magic.size() == Journal::Head && legacy_magic.size() == Journal::Head);
static_assert(decimal_magic.size() == Journal::Head && dyadic_magic.size() == Journal::Head);
constexpr uint32_t preamble_extent = Frame::preamble_extent;
constexpr uint32_t frag_overhead = preamble_extent + sizeof(uint32_t);
constexpr uint8_t compact_inject = 0x80;
constexpr uint32_t compact_preamble = 1 + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(int64_t);
constexpr uint32_t compact_overhead = compact_preamble + sizeof(uint16_t);
constexpr uint8_t compact_op = 0x20;
constexpr uint8_t wide_frag = 0x40;
constexpr uint8_t wide_footer = 0x80;
constexpr uint32_t max_compact_frag = 0x7fffu;
constexpr uint64_t pull_pg = 4 * 1024;
constexpr size_t chunk_extent = 256 * 1024;
constexpr size_t map_cap = 256 * 1024 * 1024;

[[noreturn]] void io_fault(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

void persist_fdesc(int fd) {
  while (::fdatasync(fd) < 0) {
    if (errno != EINTR) io_fault("sync log");
  }
}

}  // namespace

void store_txt(std::string& Octets, std::string_view Val) {
  if (Val.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("encoded string is too large");
  }
  store(Octets, static_cast<uint32_t>(Val.size()));
  if (!Val.empty()) Octets.append(Val);
}

uint64_t Frame::payload_offs() const { return Offs + preamble_octets; }
uint64_t Frame::end() const { return payload_offs() + length + footer_octets; }

Journal::Journal(const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  const auto path = directory / "minivectordb.log";
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0) io_fault("open log");
  try {
    if (::flock(fd_, LOCK_EX | LOCK_NB) < 0) io_fault("lock log");
    struct stat info {};
    if (::fstat(fd_, &info) < 0) io_fault("stat log");
    if (!S_ISREG(info.st_mode)) throw std::runtime_error("log must be a regular file");
    disk_end_ = static_cast<uint64_t>(info.st_size);
    if (disk_end_ < Head) {
      // An interrupted initialization leaves no Frag behind at this point.
      push2_disk(0, dyadic_magic);
      dense_recs = true;
      decimal_vectors_ = true;
      compact_frames_ = true;
      disk_end_ = Head;
      persist_fdesc(fd_);
      const int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (dir < 0) io_fault("open data directory");
      int Outcome;
      do {
        Outcome = ::fsync(dir);
      } while (Outcome < 0 && errno == EINTR);
      const int Fault = errno;
      ::close(dir);
      if (Outcome < 0) throw std::system_error(Fault, std::generic_category(), "sync data directory");
    } else {
      const auto version = read(0, Head);
      compact_frames_ = version == magic || version == dyadic_magic;
      decimal_vectors_ = version == decimal_magic || version == dyadic_magic;
      dense_recs = compact_frames_ || decimal_vectors_ || version == dense_magic;
      if (!dense_recs && version != legacy_magic) throw std::runtime_error("unknown log format");
    }
  } catch (...) {
    ::close(fd_);
    throw;
  }
}

Journal::~Journal() {
  unmap();
  ::close(fd_);
}

void Journal::pull_disk(uint64_t Offs, char* destination, size_t length) {
  while (length) {
    const ssize_t count = ::pread(fd_, destination, length, static_cast<off_t>(Offs));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) io_fault("read log");
    if (!count) throw std::runtime_error("unexpected end of log");
    Offs += count;
    destination += count;
    length -= count;
  }
}

void Journal::push2_disk(uint64_t Offs, std::string_view data) {
  while (!data.empty()) {
    const ssize_t count = ::pwrite(fd_, data.data(), data.size(), static_cast<off_t>(Offs));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) io_fault("write log");
    if (!count) throw std::runtime_error("zero-length log write");
    Offs += count;
    data.remove_prefix(count);
  }
}

void Journal::flush() {
  push2_disk(disk_end_, pending_);
  disk_end_ += pending_.size();
  pending_.clear();
}

void Journal::prepare_reads() {
  if (mapping_ || !pending_.empty()) return;
  uint64_t Allowance = map_cap;
  struct rlimit address_cap {};
  if (::getrlimit(RLIMIT_AS, &address_cap) == 0 && address_cap.rlim_cur != RLIM_INFINITY) {
    Allowance = std::min<uint64_t>(Allowance, address_cap.rlim_cur / 4);
  }
  if (disk_end_ > Allowance) return;
  void* mapped = ::mmap(nullptr, disk_end_, PROT_READ, MAP_SHARED, fd_, 0);
  if (mapped != MAP_FAILED) {
    mapping_ = static_cast<const char*>(mapped);
    mapped_extent = disk_end_;
  }
}

void Journal::unmap() {
  if (!mapping_) return;
  ::munmap(const_cast<char*>(mapping_), mapped_extent);
  mapping_ = nullptr;
  mapped_extent = 0;
}

std::optional<std::string_view> Journal::cached(uint64_t Offs, uint32_t length) const {
  if (Offs >= disk_end_) {
    return std::string_view(pending_).substr(Offs - disk_end_, length);
  }
  if (mapping_ && Offs + length <= mapped_extent) {
    return std::string_view(mapping_ + Offs, length);
  }
  if (Offs >= chunk_head && Offs + length <= chunk_head + chunk.size()) {
    return std::string_view(chunk.data() + (Offs - chunk_head), length);
  }
  return std::nullopt;
}

std::string_view Journal::read(uint64_t Offs, uint32_t length) {
  if (Offs > end() || length > end() - Offs) throw std::runtime_error("log read out of bounds");
  if (Offs < disk_end_ && length > disk_end_ - Offs) flush();
  if (const auto existing = cached(Offs, length)) return *existing;
  const uint64_t aligned = Offs & ~(pull_pg - 1);
  // The window advances during a forward Sweep. When a Hop lands elsewhere in
  // the Journal, only the pages this record or field requires are fetched.
  const bool adjacent = !chunk.empty() && aligned >= chunk_head &&
                        aligned - chunk_head <= chunk.size();
  const uint64_t window = adjacent ? chunk_extent : pull_pg;
  chunk_head = aligned;
  const uint64_t needed = Offs - aligned + length;
  chunk.resize(std::min(disk_end_ - aligned, std::max(window, needed)));
  pull_disk(aligned, chunk.data(), chunk.size());
  return {chunk.data() + (Offs - chunk_head), length};
}

int Journal::compare_octets(uint64_t Lhs, uint32_t lhs_extent, uint64_t Rhs, uint32_t rhs_extent) {
  if (Lhs > end() || lhs_extent > end() - Lhs || Rhs > end() || rhs_extent > end() - Rhs) {
    throw std::runtime_error("log comparison out of bounds");
  }
  const auto lhs_cached = cached(Lhs, lhs_extent);
  const auto rhs_cached = cached(Rhs, rhs_extent);
  if (lhs_cached && rhs_cached) return lhs_cached->compare(*rhs_cached);

  // Every range lies within a single Rec payload. Misses go into separate bounded
  // buffers, so pairwise value comparisons never disturb the sequential read Stash.
  constexpr uint32_t chunk_extent = 16 * 1024;
  std::array<char, chunk_extent> lhs_arena;
  std::array<char, chunk_extent> rhs_arena;
  const auto piece = [&](uint64_t Offs, uint32_t size, char* Arena) -> std::string_view {
    if (const auto existing = cached(Offs, size)) return *existing;
    pull_disk(Offs, Arena, size);
    return {Arena, size};
  };
  const uint32_t common_extent = std::min(lhs_extent, rhs_extent);
  for (uint32_t at = 0; at < common_extent;) {
    const uint32_t size = std::min(chunk_extent, common_extent - at);
    const auto a = piece(Lhs + at, size, lhs_arena.data());
    const auto b = piece(Rhs + at, size, rhs_arena.data());
    const int Seq = a.compare(b);
    if (Seq != 0) return Seq;
    at += size;
  }
  return (lhs_extent > rhs_extent) - (lhs_extent < rhs_extent);
}

Frame Journal::append(Op op, uint32_t Tbl, int64_t Ky, uint64_t previous, std::string_view payload) {
  const uint32_t overhead = frag_overhead + 2 * static_cast<uint32_t>(compact_frames_);
  if (payload.size() > std::numeric_limits<uint32_t>::max() - overhead) {
    throw std::length_error("row is too large");
  }
  Frame Frag{end(), static_cast<uint32_t>(payload.size()), op, Tbl, Ky, previous};
  if (compact_frames_ && (op == Op::put || op == Op::dense_put) && !previous &&
      Tbl <= std::numeric_limits<uint16_t>::max() && Frag.length <= max_compact_frag - compact_overhead) {
    Frag.preamble_octets = compact_preamble;
    Frag.footer_octets = sizeof(uint16_t);
    store(pending_, static_cast<uint8_t>(compact_inject | (op == Op::dense_put ? compact_op : 0)));
    store(pending_, static_cast<uint16_t>(Frag.length));
    store(pending_, static_cast<uint16_t>(Tbl));
    store(pending_, Ky);
    pending_.append(payload);
    store(pending_, static_cast<uint16_t>(Frag.length + compact_overhead));
    if (pending_.size() >= chunk_extent) flush();
    return Frag;
  }
  if (compact_frames_) {
    // A one-byte tag sits at both ends, letting either scan direction recognize a wide Frag.
    store(pending_, wide_frag);
    ++Frag.preamble_octets;
    ++Frag.footer_octets;
  }
  store(pending_, Frag.length);
  store(pending_, static_cast<uint32_t>(op));
  store(pending_, Tbl);
  store(pending_, Ky);
  store(pending_, previous);
  if (!payload.empty()) pending_.append(payload);
  store(pending_, Frag.length + overhead);
  if (compact_frames_) store(pending_, wide_footer);
  if (pending_.size() >= chunk_extent) flush();
  return Frag;
}

void Journal::Finalize() {
  append(Op::Finalize);
  flush();
  persist_fdesc(fd_);
}

void Journal::discard_after(uint64_t Offs) {
  if (Offs > end() || Offs < Head) throw std::runtime_error("invalid rollback point");
  if (Offs >= disk_end_) {
    pending_.resize(Offs - disk_end_);
    return;
  }
  unmap();
  while (::ftruncate(fd_, static_cast<off_t>(Offs)) < 0) {
    if (errno != EINTR) io_fault("truncate log");
  }
  pending_.clear();
  chunk.clear();
  disk_end_ = Offs;
}

std::optional<Frame> Journal::frag_at(uint64_t Offs, uint64_t Cap) {
  if (Offs >= Cap) return std::nullopt;
  if (compact_frames_) {
    const auto marker = load<uint8_t>(read(Offs, 1));
    if (marker == compact_inject || marker == (compact_inject | compact_op)) {
      if (Cap - Offs < compact_overhead) return std::nullopt;
      Reader input(read(Offs + 1, compact_preamble - 1));
      const auto length = input.read<uint16_t>();
      const auto Tbl = input.read<uint16_t>();
      const auto Ky = input.read<int64_t>();
      const auto op = marker == compact_inject ? Op::put : Op::dense_put;
      Frame Frag{Offs, length, op, Tbl, Ky, 0, compact_preamble, sizeof(uint16_t)};
      const uint32_t size = length + compact_overhead;
      if (size > max_compact_frag || size > Cap - Offs ||
          load<uint16_t>(read(Frag.end() - sizeof(uint16_t), sizeof(uint16_t))) != size) return std::nullopt;
      return Frag;
    }
    if (marker != wide_frag) return std::nullopt;
  }
  const uint32_t extra = static_cast<uint32_t>(compact_frames_);
  const uint32_t overhead = frag_overhead + 2 * extra;
  if (Cap - Offs < overhead) return std::nullopt;
  Reader input(read(Offs + extra, preamble_extent));
  const uint32_t length = input.read<uint32_t>();
  const auto op = static_cast<Op>(input.read<uint32_t>());
  Frame Frag{Offs, length, op, input.read<uint32_t>(), input.read<int64_t>(),
              input.read<uint64_t>()};
  Frag.preamble_octets += extra;
  Frag.footer_octets += extra;
  if (length > Cap - Offs - overhead ||
      length > std::numeric_limits<uint32_t>::max() - overhead ||
      op < Op::Construct || op > (dense_recs ? Op::dense_put : Op::Finalize) ||
      (op == Op::Finalize && (length || Frag.Tbl || Frag.Ky || Frag.previous)) ||
      load<uint32_t>(read(Frag.end() - Frag.footer_octets, sizeof(uint32_t))) != length + overhead ||
      (compact_frames_ && load<uint8_t>(read(Frag.end() - 1, 1)) != wide_footer)) {
    return std::nullopt;
  }
  return Frag;
}

Frame Journal::frag_before(uint64_t Walker) {
  const uint32_t minimum = compact_frames_ ? compact_overhead : frag_overhead;
  if (Walker < Head + minimum) throw std::runtime_error("invalid rollback frame");
  uint32_t size;
  if (compact_frames_) {
    const auto marker = load<uint8_t>(read(Walker - 1, 1));
    if (marker < wide_footer) {
      size = load<uint16_t>(read(Walker - sizeof(uint16_t), sizeof(uint16_t)));
    } else {
      if (marker != wide_footer) throw std::runtime_error("invalid frame footer");
      size = load<uint32_t>(read(Walker - 1 - sizeof(uint32_t), sizeof(uint32_t)));
    }
  } else {
    size = load<uint32_t>(read(Walker - sizeof(uint32_t), sizeof(uint32_t)));
  }
  if (size < minimum || size > Walker - Head) throw std::runtime_error("invalid frame size");
  const auto Frag = frag_at(Walker - size, Walker);
  if (!Frag || Frag->end() != Walker) throw std::runtime_error("invalid rollback frame");
  return *Frag;
}

}  // namespace mini
