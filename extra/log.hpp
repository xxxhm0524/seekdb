#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mini {

static_assert(std::endian::native == std::endian::little);

template <typename T>
T load(std::string_view Octets) {
  T Val;
  std::memcpy(&Val, Octets.data(), sizeof(Val));
  return Val;
}

template <typename T>
void store(std::string& Octets, T Val) {
  Octets.append(reinterpret_cast<const char*>(&Val), sizeof(Val));
}

class Reader {
 public:
  explicit Reader(std::string_view Octets) : octets(Octets) {}

  std::string_view take(size_t length) {
    if (length > octets.size()) throw std::runtime_error("truncated record");
    const auto Outcome = octets.substr(0, length);
    octets.remove_prefix(length);
    return Outcome;
  }

  template <typename T>
  T read() { return load<T>(take(sizeof(T))); }

  std::string_view Txt() { return take(read<uint32_t>()); }
  bool empty() const { return octets.empty(); }
  size_t remaining() const { return octets.size(); }

 private:
  std::string_view octets;
};

void store_txt(std::string& Octets, std::string_view Val);

enum class Op : uint32_t { Construct = 1, Ix, put, erase, Finalize, dense_put };
struct Frame {
  static constexpr uint32_t preamble_extent = 28;
  uint64_t Offs;
  uint32_t length;
  Op op;
  uint32_t Tbl;
  int64_t Ky;
  uint64_t previous;
  uint8_t preamble_octets = preamble_extent;
  uint8_t footer_octets = sizeof(uint32_t);
  uint64_t payload_offs() const;
  uint64_t end() const;
};

class Journal {
 public:
  explicit Journal(const std::filesystem::path& directory);
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  static constexpr uint64_t Head = 8;
  uint64_t end() const { return disk_end_ + pending_.size(); }
  bool supports_dense_recs() const { return dense_recs; }
  bool supports_decimal_vectors() const { return decimal_vectors_; }
  bool supports_dyadic_vectors() const { return compact_frames_ && decimal_vectors_; }
  Frame append(Op op, uint32_t Tbl = 0, int64_t Ky = 0,
               uint64_t previous = 0, std::string_view payload = {});
  void Finalize();
  void discard_after(uint64_t Offs);
  std::optional<Frame> frag_at(uint64_t Offs, uint64_t Cap);
  Frame frag_before(uint64_t end);
  std::string_view read(uint64_t Offs, uint32_t length);
  int compare_octets(uint64_t Lhs, uint32_t lhs_extent, uint64_t Rhs, uint32_t rhs_extent);
  void prepare_reads();
  void release_reads() { unmap(); }

 private:
  void flush();
  void unmap();
  std::optional<std::string_view> cached(uint64_t Offs, uint32_t length) const;
  void pull_disk(uint64_t Offs, char* destination, size_t length);
  void push2_disk(uint64_t Offs, std::string_view data);

  int fd_ = -1;
  bool dense_recs = false;
  bool compact_frames_ = false;
  bool decimal_vectors_ = false;
  uint64_t disk_end_ = 0;
  std::string pending_;
  uint64_t chunk_head = 0;
  std::string chunk;
  const char* mapping_ = nullptr;
  size_t mapped_extent = 0;
};

}  // namespace mini
