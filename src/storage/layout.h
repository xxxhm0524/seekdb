#pragma once
#include "../model/layout.h"
#include <cstdint>
#include <memory>
namespace mvd {
struct alignas(4096) Page {
  uint32_t magic = kPageMagic;
  uint32_t nrows = 0;
  uint32_t page_no = 0;
  uint32_t crc = 0;
  uint8_t payload[kPageSize - kPageHeaderSize] = {0};

  uint8_t *row_ptr(uint32_t slot, uint32_t row_size) {
    return payload + static_cast<size_t>(slot) * row_size;
  }
  const uint8_t *row_ptr(uint32_t slot, uint32_t row_size) const {
    return payload + static_cast<size_t>(slot) * row_size;
  }
  uint32_t capacity(uint32_t row_size) const {
    return (kPageSize - kPageHeaderSize) / row_size;
  }
};

// Reuse aligned page buffers across cache eviction, WAL replay and commits.
// Returning them to the general allocator on every scan can fragment its heap
// when small transaction nodes are allocated between aligned 64 KiB pages.
std::shared_ptr<Page> make_page(const Page *source = nullptr);

constexpr uint32_t kIoBatchPages = 64;
struct alignas(4096) PageIoBuffer {
  uint8_t bytes[kPageSize * kIoBatchPages];
};

// A full page image produced while committing a transaction.
struct PageImage {
  uint32_t table_id = 0;
  uint32_t page_no = 0;
  std::shared_ptr<Page> page;
  uint64_t wal_offset = 0;
  uint32_t wal_crc = 0;
  uint32_t compact_offset = 0;
  uint32_t compact_size = 0;
};

static_assert(sizeof(Page)==kPageSize);
}
