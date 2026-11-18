#pragma once

#include "sql.hpp"

#include <ostream>
#include <set>

#if defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace mini::wire {

struct Request {
  std::string_view id;
  std::string op;
  std::optional<SqlText> sql;
  std::string mode = "exact";
};

[[noreturn]] inline void invalid(const char* message) {
  throw SqlError("invalid_request", message);
}

// Fields the engine actually consumes are decoded; every other JSON value is merely validated.
// The identifier keeps its original JSON span so even huge integers round-trip bit-exactly.
class RequestReader {
 public:
  explicit RequestReader(std::string_view Ln) : ln(Ln) {}

  Request read() {
    try {
      utf8_reach(ln);
    } catch (const SqlError&) {
      invalid("request is not valid UTF-8");
    }
    Request request;
    object([&](std::string_view Ky) {
      if (Ky == "id") {
        skip_space();
        const size_t Head = at_;
        Val(0);
        request.id = ln.substr(Head, at_ - Head);
      } else if (Ky == "op") {
        request.op = txt_view(string());
      } else if (Ky == "sql") {
        request.sql = string();
      } else if (Ky == "search_mode") {
        request.mode = txt_view(string());
      } else {
        Val(0);
      }
    });
    skip_space();
    if (at_ != ln.size()) invalid("unexpected data after request");
    if (request.id.empty()) invalid("request needs an id");
    return request;
  }

 private:
  static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
  char peek() const { return at_ < ln.size() ? ln[at_] : '\0'; }
  char next() {
    if (at_ == ln.size()) invalid("incomplete JSON");
    return ln[at_++];
  }
  bool take(char ch) {
    if (peek() != ch) return false;
    ++at_;
    return true;
  }
  void skip_space() {
    while (at_ < ln.size() && std::string_view(" \r\n\t").find(peek()) != ln.npos) ++at_;
  }
  bool Take(char ch) {
    skip_space();
    return take(ch);
  }
  void expect(char ch) {
    if (!Take(ch)) invalid("unexpected JSON token");
  }
  void Lit(std::string_view Txt) {
    if (!ln.substr(at_).starts_with(Txt)) invalid("invalid JSON literal");
    at_ += Txt.size();
  }

  uint32_t hex_word() {
    uint32_t word = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = next();
      word <<= 4;
      if (digit(ch)) word |= ch - '0';
      else if (ch >= 'a' && ch <= 'f') word |= ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') word |= ch - 'A' + 10;
      else invalid("invalid JSON Unicode escape");
    }
    return word;
  }

  static void attach_utf8(std::string& out, uint32_t Pt) {
    const auto continuation = [&](unsigned shift) {
      out.push_back(static_cast<char>(0x80 | ((Pt >> shift) & 0x3f)));
    };
    if (Pt < 0x80) {
      out.push_back(static_cast<char>(Pt));
    } else if (Pt < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (Pt >> 6)));
      continuation(0);
    } else if (Pt < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (Pt >> 12)));
      continuation(6);
      continuation(0);
    } else {
      out.push_back(static_cast<char>(0xf0 | (Pt >> 18)));
      continuation(12);
      continuation(6);
      continuation(0);
    }
  }

  static bool plain_string(std::string_view Txt) {
#if defined(__x86_64__)
    const auto escape = _mm_set1_epi8('\\');
    const auto max_control = _mm_set1_epi8(0x1f);
    while (Txt.size() >= 16) {
      const auto Octets = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Txt.data()));
      const auto control = _mm_cmpeq_epi8(_mm_min_epu8(Octets, max_control), Octets);
      if (_mm_movemask_epi8(_mm_or_si128(control, _mm_cmpeq_epi8(Octets, escape)))) return false;
      Txt.remove_prefix(16);
    }
#endif
    for (unsigned char ch : Txt) {
      if (ch < 0x20 || ch == '\\') return false;
    }
    return true;
  }

  SqlText string() {
    expect('"');
    const auto close = ln.find('"', at_);
    if (close == ln.npos) invalid("incomplete JSON");
    const auto plain = ln.substr(at_, close - at_);
    if (plain_string(plain)) {
      at_ = close + 1;
      return plain;
    }
    std::string Outcome;
    for (char ch = next(); ch != '"'; ch = next()) {
      if (static_cast<unsigned char>(ch) < 0x20) invalid("unescaped JSON control character");
      if (ch != '\\') {
        Outcome.push_back(ch);
        continue;
      }
      switch (next()) {
        case '"': Outcome.push_back('"'); break;
        case '\\': Outcome.push_back('\\'); break;
        case '/': Outcome.push_back('/'); break;
        case 'b': Outcome.push_back('\b'); break;
        case 'f': Outcome.push_back('\f'); break;
        case 'n': Outcome.push_back('\n'); break;
        case 'r': Outcome.push_back('\r'); break;
        case 't': Outcome.push_back('\t'); break;
        case 'u': {
          uint32_t Pt = hex_word();
          if (Pt >= 0xd800 && Pt <= 0xdbff) {
            Lit("\\u");
            const uint32_t Lo = hex_word();
            if (Lo < 0xdc00 || Lo > 0xdfff) invalid("invalid JSON surrogate pair");
            Pt = 0x10000 + ((Pt - 0xd800) << 10) + Lo - 0xdc00;
          } else if (Pt >= 0xdc00 && Pt <= 0xdfff) {
            invalid("unpaired JSON surrogate");
          }
          attach_utf8(Outcome, Pt);
          break;
        }
        default: invalid("invalid JSON escape");
      }
    }
    return Outcome;
  }

  void digits() {
    if (!digit(peek())) invalid("invalid JSON number");
    while (digit(peek())) ++at_;
  }
  void number() {
    take('-');
    if (!take('0')) digits();
    if (take('.')) digits();
    if (take('e') || take('E')) {
      if (!take('+')) take('-');
      digits();
    }
  }

  template <typename Field>
  void object(Field field) {
    expect('{');
    if (Take('}')) return;
    std::set<std::string> keys;
    do {
      auto Ky = string();
      const auto view = txt_view(Ky);
      if (!keys.emplace(view).second) invalid("duplicate JSON key");
      expect(':');
      field(view);
    } while (Take(','));
    expect('}');
  }

  void Val(unsigned depth) {
    if (depth >= 64) invalid("JSON nesting is too deep");
    skip_space();
    switch (peek()) {
      case '{': object([&](std::string_view) { Val(depth + 1); }); break;
      case '[':
        ++at_;
        if (!Take(']')) {
          do { Val(depth + 1); } while (Take(','));
          expect(']');
        }
        break;
      case '"': string(); break;
      case 't': Lit("true"); break;
      case 'f': Lit("false"); break;
      case 'n': Lit("null"); break;
      default: number(); break;
    }
  }

  std::string_view ln;
  size_t at_ = 0;
};

inline void quote(std::ostream& out, std::string_view Txt) {
  constexpr std::string_view hex = "0123456789abcdef";
  out.put('"');
  for (unsigned char ch : Txt) {
    if (ch == '"' || ch == '\\') {
      out.put('\\');
      out.put(static_cast<char>(ch));
    } else if (ch < 0x20) {
      out << "\\u00" << hex[ch >> 4] << hex[ch & 0xf];
    } else {
      out.put(static_cast<char>(ch));
    }
  }
  out.put('"');
}

}  // namespace mini::wire
