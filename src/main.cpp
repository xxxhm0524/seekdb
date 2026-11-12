#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "common.h"
#include "engine.h"

namespace {

volatile std::sig_atomic_t g_shutdown = 0;

void handle_signal(int) { g_shutdown = 1; }

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
    skip_ws();
    if (pos >= text.size()) return false;
    char c = text[pos];
    if (c == '"') {
      std::string ignored;
      return parse_string(ignored);
    }
    if (c == '{' || c == '[') {
      char open = c;
      char close = (c == '{') ? '}' : ']';
      ++pos;
      skip_ws();
      if (pos < text.size() && text[pos] == close) {
        ++pos;
        return true;
      }
      while (true) {
        if (open == '{') {
          std::string key;
          if (!parse_string(key)) return false;
          if (!take(':')) return false;
        }
        if (!skip_value()) return false;
        skip_ws();
        if (pos < text.size() && text[pos] == ',') {
          ++pos;
          continue;
        }
        if (pos < text.size() && text[pos] == close) {
          ++pos;
          return true;
        }
        return false;
      }
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

struct JsonRequest {
  std::string id_raw = "null";
  std::string op;
  std::string sql;
  std::string search_mode;
};

bool parse_request(const std::string &line, JsonRequest &req, std::string &error) {
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

void emit_json_response(const mvd::Response &resp, const std::string &id_raw) {
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
  std::cout << out << '\n';
  std::cout.flush();
}

void emit_error(const std::string &id_raw, const std::string &code,
                const std::string &message) {
  mvd::Response resp;
  resp.ok = false;
  resp.error_code = code;
  resp.error_message = message;
  emit_json_response(resp, id_raw);
}

struct Options {
  std::string data_dir = "/data";
  int query_threads = 2;
  int cache_mb = 64;
  int tx_memory_mb = 8;
  int ann_min_rows = 40000;
  int sort_memory_mb = 16;
  bool ann_default = false;
};

Options parse_options(int argc, char **argv) {
  Options opt;
  if (const char *env = std::getenv("VECTORDB_DATA_DIR")) opt.data_dir = env;
  else if (const char *env = std::getenv("MVD_DATA_DIR")) opt.data_dir = env;
  else if (const char *env = std::getenv("MVDB_DATA_DIR")) opt.data_dir = env;
  if (const char *env = std::getenv("MVD_QUERY_THREADS")) opt.query_threads = std::atoi(env);
  if (const char *env = std::getenv("MVD_CACHE_MB")) opt.cache_mb = std::atoi(env);
  if (const char *env = std::getenv("MVD_TX_MEMORY_MB")) opt.tx_memory_mb = std::atoi(env);
  if (const char *env = std::getenv("MVD_ANN_MIN_ROWS")) opt.ann_min_rows = std::atoi(env);
  if (const char *env = std::getenv("MVD_SORT_MEMORY_MB")) opt.sort_memory_mb = std::atoi(env);
  if (const char *env = std::getenv("MVD_VECTOR_MODE")) {
    std::string m = env;
    opt.ann_default = (m == "ann" || m == "approximate");
  }
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto take = [&](const std::string &prefix, std::string &value) -> bool {
      if (arg.rfind(prefix, 0) == 0) {
        value = arg.substr(prefix.size());
        return true;
      }
      return false;
    };
    std::string v;
    if (take("--data-dir=", v)) opt.data_dir = v;
    else if (arg == "--data-dir" && i + 1 < argc) opt.data_dir = argv[++i];
    else if (take("--query-threads=", v)) opt.query_threads = std::atoi(v.c_str());
    else if (arg == "--query-threads" && i + 1 < argc) opt.query_threads = std::atoi(argv[++i]);
    else if (take("--cache-mb=", v)) opt.cache_mb = std::atoi(v.c_str());
    else if (arg == "--cache-mb" && i + 1 < argc) opt.cache_mb = std::atoi(argv[++i]);
    else if (take("--tx-memory-mb=", v)) opt.tx_memory_mb = std::atoi(v.c_str());
    else if (arg == "--tx-memory-mb" && i + 1 < argc) opt.tx_memory_mb = std::atoi(argv[++i]);
    else if (take("--sort-memory-mb=", v)) opt.sort_memory_mb = std::atoi(v.c_str());
    else if (arg == "--sort-memory-mb" && i + 1 < argc) opt.sort_memory_mb = std::atoi(argv[++i]);
    else if (take("--ann-min-rows=", v)) opt.ann_min_rows = std::atoi(v.c_str());
    else if (arg == "--ann-min-rows" && i + 1 < argc) opt.ann_min_rows = std::atoi(argv[++i]);
    else if (take("--vector-mode=", v)) opt.ann_default = (v == "ann" || v == "approximate");
    else if (arg == "--vector-mode" && i + 1 < argc) {
      opt.ann_default = (std::string(argv[i + 1]) == "ann" ||
                         std::string(argv[i + 1]) == "approximate");
      ++i;
    } else if (take("--mode=", v)) {
      opt.ann_default = (v == "ann" || v == "approximate");
    } else if (arg == "--mode" && i + 1 < argc) {
      opt.ann_default = (std::string(argv[i + 1]) == "ann" ||
                         std::string(argv[i + 1]) == "approximate");
      ++i;
    } else if (arg == "--ann") {
      opt.ann_default = true;
    } else if (arg == "--exact") {
      opt.ann_default = false;
    } else if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "minivectordb [--data-dir=/data] [--query-threads=N] "
                   "[--cache-mb=N] [--tx-memory-mb=N] [--sort-memory-mb=N] [--ann-min-rows=N] [--vector-mode=exact|ann]\n");
      std::exit(0);
    }
  }
  // Older output-format flags are accepted but ignored: stdout is always the
  // JSONL judge protocol.
  if (opt.query_threads < 1) opt.query_threads = 1;
  if (opt.cache_mb < 1) opt.cache_mb = 1;
  if (opt.tx_memory_mb < 1) opt.tx_memory_mb = 1;
  if (opt.ann_min_rows < 1) opt.ann_min_rows = 1;
  if (opt.sort_memory_mb < 1) opt.sort_memory_mb = 1;
  return opt;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt = parse_options(argc, argv);
#ifdef _OPENMP
  omp_set_num_threads(opt.query_threads);
#endif
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    size_t cache_pages =
        static_cast<size_t>(opt.cache_mb) * 1024 * 1024 / mvd::kPageSize;
    if (cache_pages < 8) cache_pages = 8;
    mvd::Engine engine(opt.data_dir, cache_pages, static_cast<size_t>(opt.tx_memory_mb)*1024*1024,
        static_cast<size_t>(opt.ann_min_rows), static_cast<size_t>(opt.sort_memory_mb)*1024*1024);
    engine.open();
    if (opt.ann_default) engine.set_ann_mode(true);

    // The judge waits for this exact object before sending any request.
    std::cout << "{\"type\":\"ready\"}\n";
    std::cout.flush();

    std::string line;
    while (!g_shutdown && std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;  // no response is defined for blank lines

      JsonRequest req;
      std::string parse_error;
      if (!parse_request(line, req, parse_error)) {
        emit_error("null", "invalid_request", parse_error);
        continue;
      }
      if (req.op == "shutdown") {
        engine.shutdown();
        std::string out = "{\"id\":" + req.id_raw + ",\"ok\":true}\n";
        std::cout << out;
        std::cout.flush();
        return 0;
      }
      if (req.op != "execute") {
        emit_error(req.id_raw, "unsupported_op", "unsupported operation");
        continue;
      }
      // Each execute starts from its configured default (exact for the
      // standard entrypoint), rather than inheriting a previous request.
      engine.set_ann_mode(req.search_mode.empty() ? opt.ann_default :
                          req.search_mode == "ann" || req.search_mode == "approximate");

      mvd::Response resp = engine.execute(req.sql);
      emit_json_response(resp, req.id_raw);
    }

    engine.shutdown();
    return 0;
  } catch (const mvd::DbError &e) {
    std::fprintf(stderr, "fatal: %s: %s\n", e.code.c_str(), e.message.c_str());
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
}
