#pragma once
#include <cstdint>
#include <string_view>
namespace mvd {
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

}
