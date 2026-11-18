#include "db.hpp"
#include "wire.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <iostream>
#include <limits>
#include <system_error>
#include <type_traits>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr std::string_view whitespace = " \r\n\t\f";
enum class Input { json, Ln, sql };

volatile std::sig_atomic_t stopping = 0;
volatile std::sig_atomic_t eof_input = -1;

void stop(int) {
  stopping = 1;
  const int saved_errno = errno;
  // Buffered streams may retry an interrupted read or write. An empty pipe returns
  // EOF on stdin and refuses writes to stdout. Occupying both descriptors prevents
  // a later Journal from silently reusing them.
  while (::dup2(eof_input, STDIN_FILENO) < 0 && errno == EINTR) {}
  while (::dup2(eof_input, STDOUT_FILENO) < 0 && errno == EINTR) {}
  errno = saved_errno;
}

void install_signals() {
  int input[2];
  if (::pipe2(input, O_CLOEXEC) < 0) {
    throw std::system_error(errno, std::generic_category(), "open shutdown pipe");
  }
  ::close(input[1]);
  eof_input = input[0];
  struct sigaction action {};
  action.sa_handler = stop;
  ::sigemptyset(&action.sa_mask);
  ::sigaddset(&action.sa_mask, SIGTERM);
  ::sigaddset(&action.sa_mask, SIGINT);
  if (::sigaction(SIGTERM, &action, nullptr) < 0 || ::sigaction(SIGINT, &action, nullptr) < 0) {
    throw std::system_error(errno, std::generic_category(), "install shutdown signal");
  }
  action.sa_handler = SIG_IGN;
  if (::sigaction(SIGPIPE, &action, nullptr) < 0) {
    throw std::system_error(errno, std::generic_category(), "ignore broken stdout pipe");
  }
}

std::string_view trim(std::string_view Ln) {
  const auto Head = Ln.find_first_not_of(whitespace);
  if (Head == Ln.npos) return {};
  return Ln.substr(Head, Ln.find_last_not_of(whitespace) - Head + 1);
}

bool pull_sql(std::string& Stmt) {
  Stmt.clear();
  char delimiter = 0;
  for (char ch; !stopping && std::cin.get(ch);) {
    if (delimiter) {
      Stmt.push_back(ch);
      if (ch == delimiter) delimiter = 0;
      else if (delimiter == '\'' && ch == '\\' && std::cin.get(ch)) Stmt.push_back(ch);
      continue;
    }
    if (ch == ';') return true;
    bool ln_comment = ch == '#';
    if (ch == '-' && std::cin.peek() == '-') {
      std::cin.get();
      const int next = std::cin.peek();
      ln_comment = next == std::char_traits<char>::eof() ||
                     whitespace.find(static_cast<char>(next)) != whitespace.npos;
      if (!ln_comment) {
        Stmt += "--";
        continue;
      }
    }
    if (ln_comment) {
      std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      Stmt.push_back(' ');
      continue;
    }
    if (ch == '/' && std::cin.peek() == '*') {
      std::cin.get();
      bool closed = false;
      while (!stopping && std::cin.get(ch)) {
        if (ch == '*' && std::cin.peek() == '/') {
          std::cin.get();
          closed = true;
          break;
        }
      }
      Stmt += closed ? " " : " /*";
      continue;
    }
    if (ch == '\'' || ch == 0x60) delimiter = ch;
    else if (ch == '[') delimiter = ']';
    Stmt.push_back(ch);
  }
  return !Stmt.empty();
}

bool command_is(std::string_view word, std::string_view lower) {
  return std::ranges::equal(word, lower, [](char ch, char expected) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return ch == expected;
  });
}

void escape(std::ostream& out, std::string_view Val) {
  for (char ch : Val) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '\t': out << "\\t"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\0': out << "\\0"; break;
      default: out.put(ch); break;
    }
  }
}

template <typename Number>
void number(std::ostream& out, Number Val) {
  char Arena[48];
  const auto [end, Fault] = std::to_chars(Arena, Arena + sizeof(Arena), Val);
  if (Fault != std::errc{}) throw std::runtime_error("number formatting failed");
  out.write(Arena, end - Arena);
}

void push2_cell(std::ostream& out, const mini::Cell& cell, bool json) {
  std::visit([&](auto Val) {
    using Value = decltype(Val);
    if constexpr (std::is_same_v<Value, std::string_view>) {
      if (json) mini::wire::quote(out, Val);
      else escape(out, Val);
    } else if constexpr (std::is_same_v<Value, mini::VectorView>) {
      out.put('[');
      for (size_t at = 0; at < Val.Octets.size(); at += sizeof(float)) {
        if (at) out.put(',');
        number(out, mini::load<float>(Val.Octets.substr(at, sizeof(float))));
      }
      out.put(']');
    } else {
      number(out, Val);
    }
  }, cell);
}

class TextProtocol : public mini::ResultSink {
 public:
  void ok(uint64_t affected) override {
    std::cout << "OK " << affected << '\n';
  }

  void enter_recs(std::span<const std::string> columns, size_t count) override {
    std::cout << "RESULT " << columns.size() << ' ' << count << '\n';
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i) std::cout.put('\t');
      escape(std::cout, columns[i]);
    }
    std::cout.put('\n');
  }

  void Rec(std::span<const mini::Cell> cells) override {
    for (size_t i = 0; i < cells.size(); ++i) {
      if (i) std::cout.put('\t');
      push2_cell(std::cout, cells[i], false);
    }
    std::cout.put('\n');
  }

  void end_recs() override { std::cout << "END\n"; }
};

class JsonProtocol : public mini::ResultSink {
 public:
  explicit JsonProtocol(std::string_view id) : id_(id) {}

  void ok(uint64_t affected) override {
    success_prefix();
    std::cout << ",\"affected_rows\":";
    number(std::cout, affected);
    std::cout << "}\n";
  }
  void shutdown() {
    success_prefix();
    std::cout << "}\n";
  }
  void Fault(const mini::SqlError& Fault) {
    std::cout << "{\"id\":" << id_ << ",\"ok\":false,\"error\":{\"code\":";
    mini::wire::quote(std::cout, Fault.code);
    std::cout << ",\"message\":";
    mini::wire::quote(std::cout, Fault.what());
    std::cout << "}}\n";
  }
  void enter_recs(std::span<const std::string> columns, size_t) override {
    success_prefix();
    std::cout << ",\"columns\":[";
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i) std::cout.put(',');
      mini::wire::quote(std::cout, columns[i]);
    }
    std::cout << "],\"rows\":[";
  }
  void Rec(std::span<const mini::Cell> cells) override {
    if (!first_rec) std::cout.put(',');
    first_rec = false;
    std::cout.put('[');
    for (size_t i = 0; i < cells.size(); ++i) {
      if (i) std::cout.put(',');
      push2_cell(std::cout, cells[i], true);
    }
    std::cout.put(']');
  }
  void end_recs() override { std::cout << "]}\n"; }

 private:
  void success_prefix() const { std::cout << "{\"id\":" << id_ << ",\"ok\":true"; }

  std::string_view id_;
  bool first_rec = true;
};

mini::SearchMode mode_val(std::string_view mode) {
  if (command_is(mode, "exact")) return mini::SearchMode::exact;
  if (command_is(mode, "ann")) return mini::SearchMode::ann;
  throw mini::SqlError("MODE", "expected exact or ann");
}

void serve_json(mini::Database& db) {
  std::cout << "{\"type\":\"ready\"}\n" << std::flush;
  std::string Ln;
  while (!stopping && std::getline(std::cin, Ln)) {
    if (stopping) break;
    std::string_view id = "null";
    try {
      const auto request = mini::wire::RequestReader(Ln).read();
      id = request.id;
      JsonProtocol protocol(id);
      if (request.op == "shutdown") {
        db.close();
        protocol.shutdown();
        std::cout.flush();
        return;
      }
      if (request.op != "execute") throw mini::SqlError("unsupported_op", "expected execute or shutdown");
      if (!request.sql) mini::wire::invalid("execute needs a sql string");
      db.execute(mini::parse(mini::txt_view(*request.sql)), mode_val(request.mode), protocol);
    } catch (const mini::SqlError& Fault) {
      JsonProtocol(id).Fault(Fault);
    }
    std::cout.flush();
  }
}

void serve_txt(mini::Database& db, mini::SearchMode mode, Input input) {
  TextProtocol protocol;
  std::cout << "READY\n" << std::flush;
  std::string Stmt;
  bool first = true;
  while (!stopping && (input == Input::Ln ? bool(std::getline(std::cin, Stmt))
                                            : pull_sql(Stmt))) {
    if (stopping) break;
    auto request = trim(Stmt);
    if (first && request.starts_with("\xef\xbb\xbf")) request = trim(request.substr(3));
    first = false;
    if (request.empty() || request.starts_with("--") || request.starts_with('#')) continue;
    const auto control = request.ends_with(';') ? trim(request.substr(0, request.size() - 1))
                                                : request;
    if (command_is(control, "quit")) {
      db.close();
      std::cout << "BYE\n" << std::flush;
      return;
    }
    try {
      const auto boundary = control.find_first_of(whitespace);
      if (command_is(control.substr(0, boundary), "mode")) {
        mode = mode_val(boundary == control.npos ? std::string_view{}
                                                    : trim(control.substr(boundary)));
        protocol.ok(0);
      } else {
        db.execute(mini::parse(request), mode, protocol);
      }
    } catch (const mini::SqlError& Fault) {
      std::cout << "ERROR " << Fault.code << ' ' << Fault.what() << '\n';
    }
    std::cout.flush();
    if (!std::cout) break;
  }
}

void run(const std::filesystem::path& directory, mini::SearchMode mode, Input input) {
  mini::Database db(directory);
  std::cout.exceptions(std::ios::badbit);
  try {
    if (input == Input::json) serve_json(db);
    else serve_txt(db, mode, input);
  } catch (const std::ios_base::failure&) {
    if (!std::cout.bad()) throw;
  }
  db.close();
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::filesystem::path directory = "/data";
  mini::SearchMode mode = mini::SearchMode::exact;
  Input input = Input::json;
  try {
    install_signals();
    for (int i = 1; i < argc; ++i) {
      const std::string_view option = argv[i];
      const auto separator = option.find('=');
      const auto Label = option.substr(0, separator);
      const std::string_view Val = separator != option.npos ? option.substr(separator + 1)
                                   : i + 1 < argc ? argv[++i] : "";
      if (Label == "--data-dir" && !Val.empty()) directory = Val;
      else if (Label == "--mode" && !Val.empty()) mode = mode_val(Val);
      else if (Label == "--input" && Val == "json") input = Input::json;
      else if (Label == "--input" && Val == "line") input = Input::Ln;
      else if (Label == "--input" && Val == "sql") input = Input::sql;
      else {
        std::cerr << "Usage: minivectordb [--data-dir DIRECTORY] [--mode exact|ann] "
                     "[--input json|line|sql]\n";
        return 2;
      }
    }
    run(directory, mode, input);
  } catch (const std::exception& Fault) {
    std::cerr << "minivectordb: " << Fault.what() << '\n';
    return 1;
  }
}
