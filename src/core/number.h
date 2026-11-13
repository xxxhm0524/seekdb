#pragma once
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
namespace mvd {
inline bool parse_int64(std::string_view text, int64_t &out) {
  if (text.empty()) return false;
  const char *begin = text.data();
  const char *end = begin + text.size();
  // from_chars does not accept a leading '+', handle it ourselves.
  bool neg = false;
  if (*begin == '+') {
    ++begin;
  } else if (*begin == '-') {
    neg = true;
    ++begin;
  }
  if (begin == end) return false;
  uint64_t magnitude = 0;
  for (const char *p = begin; p != end; ++p) {
    if (*p < '0' || *p > '9') return false;
    unsigned digit = static_cast<unsigned>(*p - '0');
    if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10)
      return false;
    magnitude = magnitude * 10 + digit;
  }
  if (neg) {
    if (magnitude > (uint64_t{1} << 63)) return false;
    out = (magnitude == (uint64_t{1} << 63))
              ? std::numeric_limits<int64_t>::min()
              : -static_cast<int64_t>(magnitude);
  } else {
    if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
    out = static_cast<int64_t>(magnitude);
  }
  return true;
}

enum class Float32Parse { VALID, INVALID, OVERFLOW };

inline Float32Parse parse_float32_value(std::string_view text, float &out) {
  if (text.empty()) return Float32Parse::INVALID;
  const char *begin = text.data(), *end = begin + text.size();
  if (*begin == '+') {
    ++begin;
    if (begin == end || *begin == '+' || *begin == '-') return Float32Parse::INVALID;
  }
  float value = 0;
  auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
  if (parsed.ec == std::errc{} && parsed.ptr == end) {
    if (!std::isfinite(value)) return Float32Parse::INVALID;
    out = value;
    return Float32Parse::VALID;
  }
  // Preserve strtof semantics for underflow, finite boundary rounding, hex
  // forms, and diagnostics. Ordinary decimal inputs avoid a temporary string.
  std::string tmp(text);
  errno = 0;
  char *tail = nullptr;
  float v = std::strtof(tmp.c_str(), &tail);
  if (tail == tmp.c_str() || tail != tmp.c_str() + tmp.size()) return Float32Parse::INVALID;
  if (!std::isfinite(v))
    return errno == ERANGE ? Float32Parse::OVERFLOW : Float32Parse::INVALID;
  out = v;
  return Float32Parse::VALID;
}

inline bool parse_float32(std::string_view text, float &out) {
  return parse_float32_value(text, out) == Float32Parse::VALID;
}

}
