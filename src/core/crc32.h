#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstddef>
namespace mvd {
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

}
