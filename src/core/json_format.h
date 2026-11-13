#pragma once
#include <cstdio>
#include <string>
#include <string_view>
namespace mvd {
inline void json_escape_append(std::string &out, std::string_view s) {
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
}

inline std::string format_double(double v) {
  char buf[64];
  // 17 significant digits round-trip for double, but output a compact form.
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  std::string s(buf);
  // Prefer shortest representation that round-trips is complex; %.17g is fine
  // for JSON parsers and avoids precision loss.
  return s;
}

inline std::string format_float_g(float v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

}
