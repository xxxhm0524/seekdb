#include "jsonl.h"
#include "../common.h"
namespace mvd::protocol {
std::string semantic_error_name(const std::string &code) {
  std::string c;
  c.reserve(code.size());
  for (char ch : code)
    c.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  if (c == "duplicate_key") return "duplicate_key";
  if (c == "unsupported") return "unsupported_sql";
  if (c == "vector_dim_mismatch") return "dimension_mismatch";
  if (c == "invalid_vector") return "invalid_vector";
  if (c == "vector_overflow") return "vector_overflow";
  if (c == "varchar_too_long") return "varchar_too_long";
  if (c == "invalid_schema") return "invalid_schema";
  if (c == "table_exists") return "table_exists";
  if (c == "table_not_found") return "table_not_found";
  if (c == "column_not_found") return "column_not_found";
  if (c == "invalid_value") return "invalid_value";
  if (c == "constraint") return "constraint";
  if (c == "syntax_error") return "syntax_error";
  return c;
}

// ---------------------------------------------------------------------------
// Minimal JSON reader for one request object per line.
// ---------------------------------------------------------------------------

void append_utf8(std::string &out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

struct JsonCursor {
  const std::string &text;
  size_t pos = 0;

  void skip_ws() {
    while (pos < text.size()) {
      char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        ++pos;
      else
        break;
    }
  }

  bool take(char expected) {
    skip_ws();
    if (pos < text.size() && text[pos] == expected) {
      ++pos;
      return true;
    }
    return false;
  }

  bool parse_hex4(uint32_t &value) {
    if (pos + 4 > text.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = text[pos++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool parse_string(std::string &out) {
    skip_ws();
    if (pos >= text.size() || text[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < text.size()) {
      unsigned char c = static_cast<unsigned char>(text[pos++]);
      if (c == '"') return true;
      if (c == '\\') {
        if (pos >= text.size()) return false;
        char e = text[pos++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!parse_hex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (pos + 1 >= text.size() || text[pos] != '\\' || text[pos + 1] != 'u')
                return false;
              pos += 2;
              uint32_t low = 0;
              if (!parse_hex4(low)) return false;
              if (low < 0xDC00 || low > 0xDFFF) return false;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return false;
            }
            append_utf8(out, cp);
            break;
          }
          default:
            return false;
        }
      } else {
        if (c < 0x20) return false;
        out.push_back(static_cast<char>(c));
      }
    }
    return false;
  }

  bool skip_value() {
    // Request IDs and ignored extension fields may themselves be nested JSON.
    // Keep nesting on the heap so a valid input cannot exhaust the call stack.
    std::vector<char> containers;
    bool need_value = true;
    while (true) {
      skip_ws();
      if (need_value) {
        if (pos >= text.size()) return false;
        char open = text[pos];
        if (open == '{' || open == '[') {
          ++pos;
          char close = open == '{' ? '}' : ']';
          skip_ws();
          if (pos < text.size() && text[pos] == close) {
            ++pos;
          } else {
            containers.push_back(close);
            if (open == '{') {
              std::string key;
              if (!parse_string(key) || !take(':')) return false;
            }
            continue;
          }
        } else if (!skip_scalar()) {
          return false;
        }
        need_value = false;
      }
      if (containers.empty()) return true;
      if (take(',')) {
        if (containers.back() == '}') {
          std::string key;
          if (!parse_string(key) || !take(':')) return false;
        }
        need_value = true;
      } else if (take(containers.back())) {
        containers.pop_back();
      } else {
        return false;
      }
    }
  }

  bool skip_scalar() {
    skip_ws();
    if (pos >= text.size()) return false;
    char c = text[pos];
    if (c == '"') {
      std::string ignored;
      return parse_string(ignored);
    }
    // Number, true, false, null.
    size_t start = pos;
    while (pos < text.size()) {
      char ch = text[pos];
      if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
          ch == '\r' || ch == '\n')
        break;
      ++pos;
    }
    if (pos == start) return false;
    std::string token = text.substr(start, pos - start);
    if (token == "true" || token == "false" || token == "null") return true;
    // Validate JSON number syntax without converting IDs through double.
    // JSON integers can be larger than DBL_MAX and must still echo unchanged.
    size_t n = 0;
    if (token[n] == '-') ++n;
    if (n == token.size()) return false;
    if (token[n] == '0') ++n;
    else {
      if (token[n] < '1' || token[n] > '9') return false;
      while (n < token.size() && token[n] >= '0' && token[n] <= '9') ++n;
    }
    if (n < token.size() && token[n] == '.') {
      size_t begin = ++n;
      while (n < token.size() && token[n] >= '0' && token[n] <= '9') ++n;
      if (n == begin) return false;
    }
    if (n < token.size() && (token[n] == 'e' || token[n] == 'E')) {
      ++n;
      if (n < token.size() && (token[n] == '+' || token[n] == '-')) ++n;
      size_t begin = n;
      while (n < token.size() && token[n] >= '0' && token[n] <= '9') ++n;
      if (n == begin) return false;
    }
    return n == token.size();
  }
};

bool parse_request(const std::string &line, Request &req, std::string &error) {
  JsonCursor c{line};
  c.skip_ws();
  if (!c.take('{')) {
    error = "request must be a JSON object";
    return false;
  }
  c.skip_ws();
  if (c.pos < line.size() && line[c.pos] == '}') {
    ++c.pos;
    c.skip_ws();
    if (c.pos == line.size()) {
      error = "empty request object";
      return false;
    }
  }
  while (true) {
    std::string key;
    if (!c.parse_string(key)) {
      error = "invalid object key";
      return false;
    }
    if (!c.take(':')) {
      error = "missing ':' after key";
      return false;
    }
    c.skip_ws();
    if (key == "id") {
      size_t start = c.pos;
      if (!c.skip_value()) {
        error = "invalid id value";
        return false;
      }
      req.id_raw = line.substr(start, c.pos - start);
    } else if (key == "op") {
      if (!c.parse_string(req.op)) {
        error = "invalid op value";
        return false;
      }
    } else if (key == "sql") {
      if (!c.parse_string(req.sql)) {
        error = "invalid sql value";
        return false;
      }
    } else if (key == "search_mode") {
      if (!c.parse_string(req.search_mode)) {
        error = "invalid search_mode value";
        return false;
      }
    } else {
      if (!c.skip_value()) {
        error = "invalid field value";
        return false;
      }
    }
    c.skip_ws();
    if (c.pos < line.size() && line[c.pos] == ',') {
      ++c.pos;
      continue;
    }
    if (c.pos < line.size() && line[c.pos] == '}') {
      ++c.pos;
      break;
    }
    error = "malformed request object";
    return false;
  }
  c.skip_ws();
  if (c.pos != line.size()) {
    error = "trailing characters after request object";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// JSON response emission
// ---------------------------------------------------------------------------

std::string format_json_value(const mvd::OutValue &v) {
  switch (v.kind) {
    case mvd::OutValue::Kind::INT:
      return std::to_string(v.i);
    case mvd::OutValue::Kind::DOUBLE: {
      if (!std::isfinite(v.d)) return "0";
      char buf[64];
      // Preserve the float64 SQL scalar used by ORDER BY, including adjacent
      // representable distances. VECTOR components below remain float32/9g.
      std::snprintf(buf, sizeof(buf), "%.17g", v.d);
      return std::string(buf);
    }
    case mvd::OutValue::Kind::VECTOR: {
      std::string out = "[";
      char buf[64];
      for (size_t i = 0; i < v.vec.size(); ++i) {
        if (i) out.push_back(',');
        float f = v.vec[i];
        if (!std::isfinite(f)) f = 0.0f;
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
        out += buf;
      }
      out.push_back(']');
      return out;
    }
    case mvd::OutValue::Kind::STRING: {
      std::string s = "\"";
      mvd::json_escape_append(s, v.s);
      s.push_back('"');
      return s;
    }
  }
  return "null";
}

std::string encode_response(const mvd::Response &resp, const std::string &id_raw) {
  std::string out = "{\"id\":";
  out += id_raw.empty() ? "null" : id_raw;
  if (!resp.ok) {
    out += ",\"ok\":false,\"error\":{\"code\":\"";
    mvd::json_escape_append(out, semantic_error_name(resp.error_code));
    out += "\",\"message\":\"";
    mvd::json_escape_append(out, resp.error_message);
    out += "\"}}";
  } else {
    out += ",\"ok\":true";
    if (resp.is_query) {
      out += ",\"columns\":[";
      for (size_t i = 0; i < resp.columns.size(); ++i) {
        if (i) out.push_back(',');
        out.push_back('"');
        mvd::json_escape_append(out, resp.columns[i]);
        out.push_back('"');
      }
      out += "],\"rows\":[";
      for (size_t r = 0; r < resp.rows.size(); ++r) {
        if (r) out.push_back(',');
        out.push_back('[');
        for (size_t c = 0; c < resp.rows[r].size(); ++c) {
          if (c) out.push_back(',');
          out += format_json_value(resp.rows[r][c]);
        }
        out.push_back(']');
      }
      out += "]";
    } else if (resp.has_affected) {
      out += ",\"affected_rows\":" + std::to_string(resp.affected_rows);
    }
    out += "}";
  }
  return out+"\n";
}

std::string encode_error(const std::string &id_raw, const std::string &code,
                const std::string &message) {
  mvd::Response resp;
  resp.ok = false;
  resp.error_code = code;
  resp.error_message = message;
  return encode_response(resp, id_raw);
}

}
