// Bounded, lossless process-local 64 KiB cache. Disk bytes are unchanged.
#pragma once
#include "page_image_codec.h"
#include <new>
#include <unordered_map>

namespace mvd {
class CompactBlockCache {
 public:
  explicit CompactBlockCache(size_t budget = 16*1024*1024) : budget_(budget) {}
  CompactBlockCache(const CompactBlockCache &) = delete;
  CompactBlockCache &operator=(const CompactBlockCache &) = delete;
  bool empty() const { return entries_.empty(); }
  bool contains(uint64_t key) const { return entries_.find(key) != entries_.end(); }
  size_t memory_bytes() const { return used_; }
  size_t entries() const { return entries_.size(); }
  bool get(uint64_t key, void *output) {
    auto found = entries_.find(key);
    if (found == entries_.end()) return false;
    const auto &entry = found->second;
    if (!unpack_page_image(entry.packed, output) ||
        crc32(output, kCompactPageBytes) != entry.crc) {
      erase(key);
      return false;
    }
    return true;
  }
  void erase(uint64_t key) {
    auto found = entries_.find(key);
    if (found == entries_.end()) return;
    used_ -= found->second.charge;
    entries_.erase(found);
  }
  template<class Predicate> void erase_if(Predicate predicate) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      auto current = it++;
      if (predicate(current->first)) erase(current->first);
    }
  }
  void put(uint64_t key, const void *input) noexcept {
    // Invalidation precedes allocation: an uncached replacement must never
    // leave the previous contents available under the same key.
    erase(key);
    if (!budget_) return;
    try {
      auto packed = pack_page_image(input);
      if (packed.empty()) return;
      // Retain admitted blocks when full: replacing a compressed prefix on
      // every raw-cache eviction would thrash during a sequential second pass.
      // Charge string capacity, map nodes, buckets and allocator overhead.
      size_t charge = packed.capacity()+128;
      if (charge > budget_ || used_ > budget_-charge) return;
      uint32_t checksum = crc32(input, kCompactPageBytes);
      entries_.emplace(key, Entry{std::move(packed), checksum, charge});
      used_ += charge;
    } catch (const std::bad_alloc &) {
      // Optional cache allocation must not change a successful disk write.
    }
  }
 private:
  struct Entry {
    std::string packed;
    uint32_t crc;
    size_t charge;
  };
  size_t budget_, used_ = 0;
  std::unordered_map<uint64_t, Entry> entries_;
};
}  // namespace mvd
