#pragma once
#include <cstdint>
#include <cstring>
#include <string>
namespace mvd {
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

}
