#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif

#include "common.h"
#include "engine.h"

namespace {

volatile std::sig_atomic_t g_shutdown = 0;

void handle_signal(int) { g_shutdown = 1; }

std::string format_json_value(const mvd::OutValue &v) {
  switch (v.kind) {
    case mvd::OutValue::Kind::INT:
      return std::to_string(v.i);
    case mvd::OutValue::Kind::DOUBLE:
      return mvd::format_double(v.d);
    case mvd::OutValue::Kind::STRING: {
      std::string s = "\"";
      mvd::json_escape_append(s, v.s);
      s.push_back('"');
      return s;
    }
  }
  return "null";
}

std::string format_text_value(const mvd::OutValue &v) {
  switch (v.kind) {
    case mvd::OutValue::Kind::INT:
      return std::to_string(v.i);
    case mvd::OutValue::Kind::DOUBLE:
      return mvd::format_double(v.d);
    case mvd::OutValue::Kind::STRING:
      return v.s;
  }
  return "";
}

void emit_response(const mvd::Response &resp, const std::string &mode,
                   const std::string &marker) {
  if (!resp.ok) {
    if (mode == "json") {
      std::string out = "{\"error\":\"";
      mvd::json_escape_append(out, resp.error_code);
      out += "\",\"message\":\"";
      mvd::json_escape_append(out, resp.error_message);
      out += "\"}";
      std::cout << out;
    } else if (mode == "miniob") {
      std::cout << "FAILURE";
    } else {
      std::cout << "ERROR " << resp.error_code << ": " << resp.error_message;
    }
    std::cout << "\n";
    std::cout.flush();
    if (!marker.empty()) {
      std::cout << marker << "\n";
      std::cout.flush();
    }
    return;
  }
  if (mode == "miniob") {
    if (resp.is_query) {
      for (size_t c = 0; c < resp.columns.size(); ++c) {
        if (c) std::cout << " | ";
        std::cout << resp.columns[c];
      }
      std::cout << '\n';
      for (const auto &row : resp.rows) {
        for (size_t c = 0; c < row.size(); ++c) {
          if (c) std::cout << " | ";
          std::cout << format_text_value(row[c]);
        }
        std::cout << '\n';
      }
    } else {
      std::cout << "SUCCESS\n";
    }
    std::cout.flush();
    if (!marker.empty()) {
      std::cout << marker << "\n";
      std::cout.flush();
    }
    return;
  }
  if (mode == "json") {
    std::string out;
    if (resp.is_query) {
      out = "{\"columns\":[";
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
      out += "]}";
    } else if (resp.has_affected) {
      out = "{\"affected_rows\":" + std::to_string(resp.affected_rows) + "}";
    } else {
      out = "{}";
    }
    std::cout << out;
  } else if (mode == "mysql") {
    if (resp.is_query) {
      for (size_t c = 0; c < resp.columns.size(); ++c) {
        if (c) std::cout << '\t';
        std::cout << resp.columns[c];
      }
      std::cout << '\n';
      for (const auto &row : resp.rows) {
        for (size_t c = 0; c < row.size(); ++c) {
          if (c) std::cout << '\t';
          std::cout << format_text_value(row[c]);
        }
        std::cout << '\n';
      }
    } else if (resp.has_affected) {
      std::cout << "OK " << resp.affected_rows;
    } else {
      std::cout << "OK";
    }
  } else {  // tsv (mysqltest-like)
    if (resp.is_query) {
      for (const auto &row : resp.rows) {
        for (size_t c = 0; c < row.size(); ++c) {
          if (c) std::cout << '\t';
          std::cout << format_text_value(row[c]);
        }
        std::cout << '\n';
      }
    } else if (resp.has_affected) {
      std::cout << "OK " << resp.affected_rows << "\n";
    } else {
      std::cout << "OK\n";
    }
  }
  if (mode != "tsv") std::cout << "\n";
  std::cout.flush();
  if (!marker.empty()) {
    std::cout << marker << "\n";
    std::cout.flush();
  }
}

// Split complete statements terminated by ';' outside quotes/comments.
void extract_statements(const std::string &buffer, std::vector<std::string> &out,
                        std::string &rest) {
  size_t start = 0;
  size_t i = 0;
  enum State { NORMAL, SQUOTE, DQUOTE, BACKTICK, LINE_COMMENT, BLOCK_COMMENT } state = NORMAL;
  while (i < buffer.size()) {
    char c = buffer[i];
    char n = (i + 1 < buffer.size()) ? buffer[i + 1] : '\0';
    switch (state) {
      case NORMAL:
        if (c == '\'') state = SQUOTE;
        else if (c == '"') state = DQUOTE;
        else if (c == '`') state = BACKTICK;
        else if (c == '-' && n == '-') { state = LINE_COMMENT; ++i; }
        else if (c == '#') state = LINE_COMMENT;
        else if (c == '/' && n == '*') { state = BLOCK_COMMENT; ++i; }
        else if (c == ';') {
          out.push_back(buffer.substr(start, i - start + 1));
          start = i + 1;
        }
        break;
      case SQUOTE:
        if (c == '\\' && i + 1 < buffer.size()) ++i;
        else if (c == '\'') {
          if (n == '\'') ++i;
          else state = NORMAL;
        }
        break;
      case DQUOTE:
        if (c == '\\' && i + 1 < buffer.size()) ++i;
        else if (c == '"') {
          if (n == '"') ++i;
          else state = NORMAL;
        }
        break;
      case BACKTICK:
        if (c == '`') state = NORMAL;
        break;
      case LINE_COMMENT:
        if (c == '\n') state = NORMAL;
        break;
      case BLOCK_COMMENT:
        if (c == '*' && n == '/') { state = NORMAL; ++i; }
        break;
    }
    ++i;
  }
  rest = buffer.substr(start);
}

std::string trim(const std::string &s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool only_comments_or_space(const std::string &s) {
  // A cheap check used to skip empty/comment-only statements.
  bool in_line_comment = false;
  bool in_block = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    char n = (i + 1 < s.size()) ? s[i + 1] : '\0';
    if (in_line_comment) {
      if (c == '\n') in_line_comment = false;
      continue;
    }
    if (in_block) {
      if (c == '*' && n == '/') { in_block = false; ++i; }
      continue;
    }
    if (c == '-' && n == '-') { in_line_comment = true; ++i; continue; }
    if (c == '#') { in_line_comment = true; continue; }
    if (c == '/' && n == '*') { in_block = true; ++i; continue; }
    if (!std::isspace(static_cast<unsigned char>(c)) && c != ';') return false;
  }
  return true;
}

struct Options {
  std::string data_dir = "/data";
  std::string ready_marker;
  std::string output = "json";
  int query_threads = 2;
  int cache_mb = 64;
  bool ann_default = false;
};

Options parse_options(int argc, char **argv) {
  Options opt;
  if (const char *env = std::getenv("MVD_OUTPUT")) opt.output = env;
  if (const char *env = std::getenv("MVD_READY_MARKER")) opt.ready_marker = env;
  if (const char *env = std::getenv("MVD_QUERY_THREADS")) opt.query_threads = std::atoi(env);
  if (const char *env = std::getenv("MVD_CACHE_MB")) opt.cache_mb = std::atoi(env);
  if (const char *env = std::getenv("MVD_DATA_DIR")) opt.data_dir = env;
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
    else if (take("--ready-marker=", v)) opt.ready_marker = v;
    else if (arg == "--ready-marker" && i + 1 < argc) opt.ready_marker = argv[++i];
    else if (take("--output=", v)) opt.output = v;
    else if (arg == "--output" && i + 1 < argc) opt.output = argv[++i];
    else if (take("--query-threads=", v)) opt.query_threads = std::atoi(v.c_str());
    else if (arg == "--query-threads" && i + 1 < argc) opt.query_threads = std::atoi(argv[++i]);
    else if (take("--cache-mb=", v)) opt.cache_mb = std::atoi(v.c_str());
    else if (arg == "--cache-mb" && i + 1 < argc) opt.cache_mb = std::atoi(argv[++i]);
    else if (take("--vector-mode=", v)) opt.ann_default = (v == "ann" || v == "approximate");
    else if (arg == "--vector-mode" && i + 1 < argc)
      opt.ann_default = (std::string(argv[++i]) == "ann" || std::string(argv[i]) == "approximate");
    else if (take("--mode=", v)) opt.ann_default = (v == "ann" || v == "approximate");
    else if (arg == "--mode" && i + 1 < argc)
      opt.ann_default = (std::string(argv[++i]) == "ann" || std::string(argv[i]) == "approximate");
    else if (arg == "--ann") opt.ann_default = true;
    else if (arg == "--exact") opt.ann_default = false;
    else if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "minivectordb [--data-dir=/data] [--output=json|tsv|mysql] "
                   "[--ready-marker=TOKEN] [--query-threads=N] [--cache-mb=N] "
                   "[--vector-mode=exact|ann]\n");
      std::exit(0);
    }
  }
  if (opt.query_threads < 1) opt.query_threads = 1;
  if (opt.cache_mb < 1) opt.cache_mb = 1;
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
    size_t cache_pages = static_cast<size_t>(opt.cache_mb) * 1024 * 1024 / mvd::kPageSize;
    if (cache_pages < 8) cache_pages = 8;
    mvd::Engine engine(opt.data_dir, cache_pages);
    engine.open();
    if (opt.ann_default) engine.set_ann_mode(true);
    std::fprintf(stderr, "minivectordb: ready\n");
    std::fflush(stderr);
    if (!opt.ready_marker.empty()) {
      std::cout << opt.ready_marker << "\n";
      std::cout.flush();
    }

    std::string buffer;
    std::vector<std::string> statements;
    std::string rest;
    while (!g_shutdown) {
      std::string line;
      bool got = false;
#ifndef _WIN32
      struct pollfd pfd;
      pfd.fd = STDIN_FILENO;
      pfd.events = POLLIN;
      int pr = ::poll(&pfd, 1, 200);
      if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
        if (!std::getline(std::cin, line)) break;
        got = true;
      } else if (pr < 0 && errno != EINTR) {
        break;
      }
#else
      if (!std::getline(std::cin, line)) break;
      got = true;
#endif
      if (got) {
        buffer += line;
        buffer.push_back('\n');
      }
      statements.clear();
      extract_statements(buffer, statements, rest);
      buffer = rest;
      for (const auto &raw : statements) {
        std::string stmt = trim(raw);
        if (stmt.empty() || only_comments_or_space(stmt)) continue;
        mvd::Response resp = engine.execute(stmt);
        emit_response(resp, opt.output, opt.ready_marker);
      }
      if (!got) {
        // EOF: execute any trailing statement without a semicolon.
        std::string stmt = trim(buffer);
        if (!stmt.empty() && !only_comments_or_space(stmt)) {
          mvd::Response resp = engine.execute(stmt);
          emit_response(resp, opt.output, opt.ready_marker);
        }
        break;
      }
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
