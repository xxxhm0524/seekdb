// Lossless, process-local page-image cache. This is never a disk format.
#pragma once
#include "common.h"

namespace mvd {
inline constexpr size_t kCompactPageBytes = 65536;
inline constexpr size_t kCompactPageWords = kCompactPageBytes / sizeof(uint64_t);
inline constexpr size_t kCompactPageMaxBytes = 4096;
static_assert(kCompactPageWords <= 0x7fff);

inline std::string pack_page_image(const void *page, size_t limit = kCompactPageMaxBytes) {
  const auto *bytes = static_cast<const uint8_t *>(page);
  auto word = [&](size_t i) {
    uint64_t value;
    std::memcpy(&value, bytes+i*sizeof(value), sizeof(value));
    return value;
  };
  auto repeated = [&](size_t i) {
    return i+3 < kCompactPageWords && word(i) == word(i+1) &&
        word(i) == word(i+2) && word(i) == word(i+3);
  };
  std::string packed;
  size_t i = 0;
  while (i < kCompactPageWords) {
    if (repeated(i)) {
      uint64_t value = word(i);
      size_t end = i+4;
      while (end < kCompactPageWords && word(end) == value) ++end;
      if (packed.size()+sizeof(uint16_t)+sizeof(value) > limit) return {};
      uint16_t tag = static_cast<uint16_t>(0x8000U | (end-i));
      packed.append(reinterpret_cast<const char *>(&tag), sizeof(tag));
      packed.append(reinterpret_cast<const char *>(&value), sizeof(value));
      i = end;
    } else {
      size_t begin = i;
      if (packed.size()+sizeof(uint16_t) > limit) return {};
      size_t maximum = (limit-packed.size()-sizeof(uint16_t))/sizeof(uint64_t);
      do {
        ++i;
        if (i-begin > maximum) return {};
      } while (i < kCompactPageWords && !repeated(i));
      uint16_t tag = static_cast<uint16_t>(i-begin);
      packed.append(reinterpret_cast<const char *>(&tag), sizeof(tag));
      packed.append(reinterpret_cast<const char *>(bytes+begin*sizeof(uint64_t)),
                    (i-begin)*sizeof(uint64_t));
    }
  }
  return packed;
}

inline bool unpack_page_image(std::string_view packed, void *page) {
  auto *output = static_cast<uint8_t *>(page);
  size_t cursor = 0, written = 0;
  while (written < kCompactPageWords) {
    if (packed.size()-cursor < sizeof(uint16_t)) return false;
    uint16_t tag;
    std::memcpy(&tag, packed.data()+cursor, sizeof(tag));
    cursor += sizeof(tag);
    size_t count = tag & 0x7fffU;
    if (!count || count > kCompactPageWords-written) return false;
    size_t bytes = count*sizeof(uint64_t);
    if (tag & 0x8000U) {
      if (packed.size()-cursor < sizeof(uint64_t)) return false;
      uint64_t value;
      std::memcpy(&value, packed.data()+cursor, sizeof(value));
      cursor += sizeof(value);
      if (!value) std::memset(output+written*sizeof(value), 0, bytes);
      else for (size_t j = 0; j < count; ++j)
        std::memcpy(output+(written+j)*sizeof(value), &value, sizeof(value));
    } else {
      if (packed.size()-cursor < bytes) return false;
      std::memcpy(output+written*sizeof(uint64_t), packed.data()+cursor, bytes);
      cursor += bytes;
    }
    written += count;
  }
  return cursor == packed.size();
}
}  // namespace mvd
