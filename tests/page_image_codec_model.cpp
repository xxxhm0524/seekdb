#include "page_image_codec.h"
#include <iostream>
#include <random>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
int main() {
  try {
    std::array<uint64_t, mvd::kCompactPageWords> input{}, output{};
    std::mt19937_64 random(20261112);
    size_t checked = 0, compact = 0, rejected = 0;
    for (size_t trial = 0; trial < 1200; ++trial) {
      size_t i = 0;
      if (trial == 0) input.fill(0);
      else if (trial == 1) input.fill(UINT64_MAX);
      else if (trial == 2) input.fill(0x80989ff080989ff0ULL);
      else if (trial == 3 || trial == 4) {
        input.fill(0);
        for (size_t j = 0; j < (trial == 3 ? 511U : 512U); ++j) input[j] = random();
      } else if (trial == 5) {
        for (size_t j = 0; j < input.size(); ++j) input[j] = j%2 ? 0 : random();
      } else if (trial == 6) {
        for (size_t j = 0; j < input.size(); j += 2) input[j] = input[j+1] = random();
      } else while (i < input.size()) {
        uint64_t value = random();
        size_t run = trial % 3 == 0 ? 1 : 1+static_cast<size_t>(random()%81);
        run = std::min(run, input.size()-i);
        std::fill_n(input.begin()+static_cast<ptrdiff_t>(i), run, value);
        i += run;
      }
      auto packed = mvd::pack_page_image(input.data());
      if (!packed.empty()) {
        ++compact;
        require(packed.size() <= mvd::kCompactPageMaxBytes, "cache packet too large");
        require(mvd::unpack_page_image(packed, output.data()) && input == output,
                "compact round trip changed bytes");
      } else ++rejected;
      if (trial < 4) require(!packed.empty(), "compressible fixture was rejected");
      if (trial == 4) require(packed.empty(), "size boundary was not enforced");
      auto full = mvd::pack_page_image(input.data(), mvd::kCompactPageBytes+1024);
      require(!full.empty() && mvd::unpack_page_image(full, output.data()) && input == output,
              "literal/run round trip changed bytes");
      require(!mvd::unpack_page_image(full+'x', output.data()), "trailing data accepted");
      ++checked;
    }
    input.fill(0);
    auto packed = mvd::pack_page_image(input.data());
    require(packed.size() == sizeof(uint16_t), "zero run retained a redundant payload word");
    for (size_t size = 0; size < packed.size(); ++size)
      require(!mvd::unpack_page_image(std::string_view(packed).substr(0, size), output.data()),
              "truncated packet accepted");
    for (uint16_t bad : {uint16_t(0), uint16_t(0x8000), uint16_t(0xc000),
                        uint16_t(0x4001), uint16_t(0xffff), uint16_t(0x2000)}) {
      std::string corrupt(reinterpret_cast<const char *>(&bad), sizeof(bad));
      require(!mvd::unpack_page_image(corrupt, output.data()), "invalid count/payload accepted");
    }
    std::cout << "{\"passed\":true,\"pages_checked\":" << checked
              << ",\"compact\":" << compact << ",\"raw_fallback\":" << rejected
              << ",\"malformed_packets_rejected\":true}" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
