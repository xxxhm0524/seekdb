#pragma once
#include <cstdint>
namespace mvd {
constexpr uint32_t kPageSize = 65536;
constexpr uint32_t kPageHeaderSize = 16;
constexpr uint32_t kPageMagic = 0x5044564D;  // "MVDP"

}
