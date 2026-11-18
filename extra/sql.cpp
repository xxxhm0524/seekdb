#include "sql.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#if defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace mini {
namespace {

[[noreturn]] void syntax(const char* message = "invalid SQL syntax") {
  throw SqlError("SYNTAX", message);
}

bool space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}
bool digit(char ch) { return ch >= '0' && ch <= '9'; }

size_t digit_prefix(uint64_t Octets) {
  // No borrow or carry ever crosses from earlier digits into the first nondigit byte.
  const auto nondigits = ((Octets + 0x4646464646464646ull) | (Octets - 0x3030303030303030ull)) &
                         0x8080808080808080ull;
  return std::countr_zero(nondigits) / 8;
}

uint64_t decimal_units(uint64_t Octets, size_t digits) {
  uint64_t units = Octets & 0x0f0f0f0f0f0f0f0full & (~uint64_t{0} >> (64 - 8 * digits));
  units = (units * 10 + (units >> 8)) & 0x00ff00ff00ff00ffull;
  units = (units * 100 + (units >> 16)) & 0x0000ffff0000ffffull;
  return (units * 10000 + (units >> 32)) & 0xffffffffull;
}

const char* fixed_decimal(const char* first, const char* Head, const char* Final, float& Produced) {
  if constexpr (std::endian::native != std::endian::little) return nullptr;
  if (Final - Head <= 10 || !digit(Head[0]) || Head[1] != '.') return nullptr;
  uint64_t Octets;
  std::memcpy(&Octets, Head + 2, sizeof(Octets));
  const size_t digits = digit_prefix(Octets);
  if (!digits) return nullptr;
  const char* end = Head + 2 + digits;
  uint64_t units = decimal_units(Octets, digits) +
      static_cast<unsigned>(Head[0] - '0') * 100000000ull;
  double Stride = 1e-8;
  bool long_decimal = false;
  if (digits == 8 && digit(*end)) {
    if (end + 1 != Final && !digit(end[1])) {
      units = units * 10 + static_cast<unsigned>(*end++ - '0');
      Stride = 1e-9;
    } else {
      if (Final - end < 8) return nullptr;
      std::memcpy(&Octets, end, sizeof(Octets));
      const size_t tail = digit_prefix(Octets);
      units = units * 100000000ull + decimal_units(Octets, tail);
      end += tail;
      Stride = 1e-16;
      if (tail == 8 && end != Final && digit(*end)) {
        units = units * 10 + static_cast<unsigned>(*end++ - '0');
        Stride = 1e-17;
        if (end != Final && digit(*end)) {
          units = units * 10 + static_cast<unsigned>(*end++ - '0');
          Stride = 1e-18;
        }
      }
      long_decimal = true;
    }
  }
  if (end == Final || (*end != ',' && *end != ']' && !space(*end))) return nullptr;
  const double approximate = static_cast<double>(units) * Stride;
  if (long_decimal) {
    // Under binary32 such values are normal. A rounding midpoint has bit 28
    // set and nothing else among the 29 binary64 fraction bits that binary32 drops.
    // Integer conversion, scaling and multiplication all fit in eight binary64 steps.
    constexpr uint64_t midpoint = uint64_t{1} << 28;
    const auto fraction = std::bit_cast<uint64_t>(approximate) & (2 * midpoint - 1);
    if (fraction >= midpoint - 8 && fraction <= midpoint + 8) return nullptr;
  }
  const float Val = static_cast<float>(approximate);
  Produced = *first == '-' ? -Val : Val;
  return end;
}

bool letter(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

size_t number_end(std::string_view Txt, size_t pos) {
  const size_t Head = pos;
  if (pos < Txt.size() && (Txt[pos] == '+' || Txt[pos] == '-')) ++pos;
  const size_t whole = pos;
  while (pos < Txt.size() && digit(Txt[pos])) ++pos;
  bool has_digit = pos != whole;
  if (pos < Txt.size() && Txt[pos] == '.') {
    const size_t fraction = ++pos;
    while (pos < Txt.size() && digit(Txt[pos])) ++pos;
    has_digit = has_digit || pos != fraction;
  }
  if (!has_digit) return Head;
  if (pos < Txt.size() && (Txt[pos] == 'e' || Txt[pos] == 'E')) {
    const size_t Expnt = pos++;
    if (pos < Txt.size() && (Txt[pos] == '+' || Txt[pos] == '-')) ++pos;
    const size_t digits = pos;
    while (pos < Txt.size() && digit(Txt[pos])) ++pos;
    if (pos == digits) return Expnt;
  }
  return pos;
}

void lowercase(std::string& Val) {
  for (char& ch : Val) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
}

enum class TokenKind { end, word, identifier, number, string, vector, symbol };
struct Token {
  TokenKind Sort3;
  SqlText Val;

  std::string_view Txt() const { return txt_view(Val); }
};

class Lexer {
 public:
  explicit Lexer(std::string_view sql) : sql_(sql) {}

  Token next() {
    skip();
    if (posn == sql_.size()) return {TokenKind::end, {}};
    const size_t Head = posn++;
    const char ch = sql_[Head];
    if (letter(ch)) {
      while (posn < sql_.size() &&
             (letter(sql_[posn]) || digit(sql_[posn]))) {
        ++posn;
      }
      std::string word(sql_.substr(Head, posn - Head));
      lowercase(word);
      return {TokenKind::word, std::move(word)};
    }
    if (ch == '\'' || ch == 0x60) return quoted(ch);
    if (ch == '[') {
      const size_t close = sql_.find(']', posn);
      if (close == sql_.npos) syntax("unterminated vector");
      posn = close + 1;
      return {TokenKind::vector, sql_.substr(Head, posn - Head)};
    }
    if (digit(ch) || ch == '+' || ch == '-' || ch == '.') {
      posn = number_end(sql_, Head);
      if (posn != Head) {
        return {TokenKind::number, sql_.substr(Head, posn - Head)};
      }
      posn = Head + 1;
    }
    if (std::string_view("(),;*=<>").find(ch) != std::string_view::npos) {
      if ((ch == '<' || ch == '>') && posn < sql_.size() && sql_[posn] == '=') {
        ++posn;
      }
      return {TokenKind::symbol, sql_.substr(Head, posn - Head)};
    }
    syntax("unexpected token");
  }

 private:
  void skip() {
    while (posn < sql_.size()) {
      if (space(sql_[posn])) {
        ++posn;
      } else if (sql_[posn] == '#' ||
                 (sql_.substr(posn, 2) == "--" &&
                  (posn + 2 == sql_.size() || space(sql_[posn + 2])))) {
        posn = sql_.find('\n', posn);
        if (posn == sql_.npos) posn = sql_.size();
      } else if (sql_.substr(posn, 2) == "/*") {
        const size_t close = sql_.find("*/", posn + 2);
        if (close == sql_.npos) syntax("unterminated comment");
        posn = close + 2;
      } else {
        break;
      }
    }
  }

  Token quoted(char delimiter) {
    const auto close = sql_.find(delimiter, posn);
    if (close == sql_.npos) syntax("unterminated quoted value");
    const auto plain = sql_.substr(posn, close - posn);
    if ((close + 1 == sql_.size() || sql_[close + 1] != delimiter) &&
        (delimiter != '\'' || plain.find('\\') == plain.npos)) {
      posn = close + 1;
      if (delimiter == '\'') return {TokenKind::string, plain};
      if (plain.empty()) syntax("empty identifier");
      std::string identifier(plain);
      lowercase(identifier);
      return {TokenKind::identifier, std::move(identifier)};
    }
    std::string Val;
    while (posn < sql_.size()) {
      char ch = sql_[posn++];
      if (ch == delimiter) {
        if (posn == sql_.size() || sql_[posn] != delimiter) {
          if (delimiter == '\'') return {TokenKind::string, std::move(Val)};
          if (Val.empty()) syntax("empty identifier");
          lowercase(Val);
          return {TokenKind::identifier, std::move(Val)};
        }
        ++posn;
      } else if (ch == '\\' && delimiter == '\'' && posn < sql_.size()) {
        ch = sql_[posn++];
        switch (ch) {
          case '0': ch = '\0'; break;
          case 'b': ch = '\b'; break;
          case 'n': ch = '\n'; break;
          case 'r': ch = '\r'; break;
          case 't': ch = '\t'; break;
          case 'Z': ch = '\x1a'; break;
        }
      }
      Val.push_back(ch);
    }
    syntax("unterminated quoted value");
  }

  std::string_view sql_;
  size_t posn = 0;
};

template <typename Number>
bool integer_val(std::string_view Txt, Number& Val) {
  if (!Txt.empty() && Txt.front() == '+') Txt.remove_prefix(1);
  const auto [end, Fault] = std::from_chars(Txt.data(), Txt.data() + Txt.size(), Val);
  return Fault == std::errc{} && end == Txt.data() + Txt.size();
}

class Parser {
 public:
  explicit Parser(std::string_view sql) : lexer_(sql), sym(lexer_.next()) {}

  Statement parse() {
    Statement Stmt = command();
    Take(";");
    if (sym.Sort3 != TokenKind::end) syntax("unexpected trailing SQL");
    return Stmt;
  }

 private:
  Token take() { return std::exchange(sym, lexer_.next()); }

  bool Take(std::string_view word) {
    if ((sym.Sort3 != TokenKind::word && sym.Sort3 != TokenKind::symbol) ||
        sym.Txt() != word) {
      return false;
    }
    take();
    return true;
  }

  void expect(std::string_view word) {
    if (!Take(word)) syntax();
  }

  std::string identifier() {
    if (sym.Sort3 != TokenKind::word && sym.Sort3 != TokenKind::identifier) {
      syntax("expected identifier");
    }
    auto Sym = take();
    return std::get<std::string>(std::move(Sym.Val));
  }

  uint64_t nonnegative() {
    uint64_t Val = 0;
    if (sym.Sort3 != TokenKind::number || !integer_val(sym.Txt(), Val)) {
      syntax("expected nonnegative integer");
    }
    take();
    return Val;
  }

  Literal Lit() {
    LiteralKind Sort3;
    switch (sym.Sort3) {
      case TokenKind::number: Sort3 = LiteralKind::number; break;
      case TokenKind::string: Sort3 = LiteralKind::string; break;
      case TokenKind::vector: Sort3 = LiteralKind::vector; break;
      default: syntax("expected literal");
    }
    return {Sort3, take().Val};
  }

  Statement command() {
    if (Take("create")) {
      if (Take("table")) return construct_tbl();
      if (Take("vector")) {
        expect("index");
        CreateIndex Ix;
        Ix.Label = identifier();
        expect("on");
        Ix.Tbl = identifier();
        expect("(");
        Ix.Col = identifier();
        expect(")");
        return Ix;
      }
    } else if (Take("insert")) {
      return insert();
    } else if (Take("select")) {
      return Project();
    } else if (Take("update")) {
      Update Outcome;
      Outcome.Tbl = identifier();
      expect("set");
      do {
        std::string Col = identifier();
        expect("=");
        Outcome.assignments.emplace_back(std::move(Col), Lit());
      } while (Take(","));
      if (Take("where")) Pred(Outcome.Pred);
      return Outcome;
    } else if (Take("delete")) {
      expect("from");
      Delete Outcome{identifier(), {}};
      if (Take("where")) Pred(Outcome.Pred);
      return Outcome;
    } else if (Take("begin")) {
      return Transaction::begin;
    } else if (Take("commit")) {
      return Transaction::Finalize;
    } else if (Take("rollback")) {
      return Transaction::Revert;
    }
    throw SqlError("unsupported_sql", "unsupported SQL statement");
  }

  CreateRelation construct_tbl() {
    CreateRelation Outcome{identifier(), {}};
    std::optional<std::string> primary;
    expect("(");
    do {
      if (Take("primary")) {
        if (primary) throw SqlError("SCHEMA", "multiple primary keys");
        expect("key");
        expect("(");
        primary = identifier();
        expect(")");
        continue;
      }
      Column Col{identifier(), Type::bigint};
      if (!Take("bigint")) {
        if (Take("varchar")) {
          Col.Kind = Type::varchar;
        } else if (Take("vector")) {
          Col.Kind = Type::vector;
        } else {
          throw SqlError("unsupported_sql", "unsupported column type");
        }
        expect("(");
        const uint64_t length = nonnegative();
        if (length == 0 || length > std::numeric_limits<uint32_t>::max()) {
          throw SqlError("SCHEMA", "invalid column length");
        }
        Col.length = static_cast<uint32_t>(length);
        expect(")");
      }
      if (Take("primary")) {
        expect("key");
        if (primary) throw SqlError("SCHEMA", "multiple primary keys");
        primary = Col.Label;
      }
      Outcome.columns.push_back(std::move(Col));
    } while (Take(","));
    expect(")");
    if (primary) {
      const auto it = std::find_if(Outcome.columns.begin(), Outcome.columns.end(),
                                   [&](const Column& Col) { return Col.Label == *primary; });
      if (it == Outcome.columns.end()) throw SqlError("SCHEMA", "unknown primary key column");
      it->primary = true;
    }
    return Outcome;
  }

  Insert insert() {
    expect("into");
    Insert Outcome{identifier(), {}, {}};
    if (Take("(")) {
      do {
        Outcome.columns.push_back(identifier());
      } while (Take(","));
      expect(")");
    }
    expect("values");
    do {
      expect("(");
      auto& Rec = Outcome.Recs.emplace_back();
      do {
        Rec.push_back(Lit());
      } while (Take(","));
      expect(")");
    } while (Take(","));
    return Outcome;
  }

  Expression Expr() {
    const bool distance = sym.Sort3 == TokenKind::word && sym.Txt() == "l2_distance";
    Expression Outcome{identifier(), {}, {}};
    if (distance && Take("(")) {
      Outcome.Col = identifier();
      expect(",");
      Outcome.Lookup = Lit();
      expect(")");
    }
    return Outcome;
  }

  Select Project() {
    Select Outcome;
    if (Take("*")) {
      Outcome.star = true;
    } else {
      do {
        auto projection = Expr();
        if (Take("as")) projection.alias = identifier();
        Outcome.projections.push_back(std::move(projection));
      } while (Take(","));
    }
    expect("from");
    Outcome.Tbl = identifier();
    if (Take("where")) Pred(Outcome.Pred);
    if (Take("order")) {
      expect("by");
      do {
        auto Ky = Expr();
        const bool descending = Take("desc");
        if (!descending) Take("asc");
        Outcome.Seq.push_back({std::move(Ky), descending});
      } while (Take(","));
    }
    if (Take("limit")) Outcome.Cap = nonnegative();
    return Outcome;
  }

  void Pred(std::vector<Predicate>& out, size_t depth = 0) {
    if (depth >= 64) syntax("WHERE expression nested too deeply");
    do {
      if (Take("(")) {
        Pred(out, depth + 1);
        expect(")");
      } else {
        std::string Col = identifier();
        Comparison comparison;
        if (Take("=")) comparison = Comparison::eq;
        else if (Take("<")) comparison = Comparison::lt;
        else if (Take("<=")) comparison = Comparison::le;
        else if (Take(">")) comparison = Comparison::gt;
        else if (Take(">=")) comparison = Comparison::ge;
        else syntax("expected comparison operator");
        out.push_back({std::move(Col), comparison, Lit()});
      }
    } while (Take("and"));
  }

  Lexer lexer_;
  Token sym;
};

}  // namespace

size_t utf8_reach(std::string_view Val) {
  size_t characters = 0;
  for (size_t i = 0; i < Val.size();) {
#if defined(__x86_64__)
    if (Val.size() - i >= 64) {
      const auto* Octets = Val.data() + i;
      const auto first = _mm_or_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(Octets)),
                                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(Octets + 16)));
      const auto second = _mm_or_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(Octets + 32)),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(Octets + 48)));
      if (_mm_movemask_epi8(_mm_or_si128(first, second)) == 0) {
        i += 64;
        characters += 64;
        continue;
      }
    }
#endif
    if (Val.size() - i >= sizeof(uint64_t)) {
      uint64_t Octets;
      std::memcpy(&Octets, Val.data() + i, sizeof(Octets));
      if ((Octets & 0x8080808080808080ull) == 0) {
        i += sizeof(Octets);
        characters += sizeof(Octets);
        continue;
      }
    }
    ++characters;
    const auto first = static_cast<unsigned char>(Val[i++]);
    if (first <= 0x7f) continue;
    unsigned count = 0;
    uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 1;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 2;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 3;
      codepoint = first & 0x07;
    }
    if (!count || Val.size() - i < count) throw SqlError("VALUE", "invalid UTF-8");
    for (unsigned j = 0; j < count; ++j) {
      const auto ch = static_cast<unsigned char>(Val[i++]);
      if ((ch & 0xc0) != 0x80) throw SqlError("VALUE", "invalid UTF-8");
      codepoint = (codepoint << 6) | (ch & 0x3f);
    }
    if ((count == 2 && codepoint < 0x800) || (count == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
      throw SqlError("VALUE", "invalid UTF-8");
    }
  }
  return characters;
}

Statement parse(std::string_view sql) { return Parser(sql).parse(); }

int64_t bigint(const Literal& Val) {
  int64_t Outcome = 0;
  if (Val.Sort3 != LiteralKind::number || !integer_val(txt_view(Val.Txt), Outcome)) {
    throw SqlError("VALUE", "invalid BIGINT");
  }
  return Outcome;
}

std::string_view varchar(const Literal& Val, uint32_t max_chars) {
  if (Val.Sort3 != LiteralKind::string) throw SqlError("VALUE", "expected a string");
  const auto Txt = txt_view(Val.Txt);
  if (utf8_reach(Txt) > max_chars) throw SqlError("VALUE", "VARCHAR is too long");
  return Txt;
}

std::vector<float> vector_val(const Literal& Val, uint32_t Dim) {
  const std::string_view Txt = txt_view(Val.Txt);
  size_t pos = 0;
  const auto consume = [&](char ch) {
    while (pos < Txt.size() && space(Txt[pos])) ++pos;
    if (pos == Txt.size() || Txt[pos] != ch) return false;
    ++pos;
    return true;
  };
  if (Val.Sort3 == LiteralKind::number || !consume('[')) {
    throw SqlError("VALUE", "expected a vector");
  }
  std::vector<float> Outcome;
  Outcome.reserve(std::min<size_t>(Dim, Txt.size() / 2));
  size_t count = 0;
  if (!consume(']')) {
    do {
      while (pos < Txt.size() && space(Txt[pos])) ++pos;
      // When from_chars signals an out-of-range value, strtof still rounds correctly
      // at the float32 subnormal boundary, all the way down to finite zero.
      float component = 0;
      const char* first = Txt.data() + pos;
      const char* Final = Txt.data() + Txt.size();
      const char* Head = first;
      if (Head != Final && (*Head == '+' || *Head == '-')) ++Head;
      if (Head == Final || (!digit(*Head) && *Head != '.')) {
        throw SqlError("VALUE", "invalid vector component");
      }
      const char* end = fixed_decimal(first, Head, Final, component);
      if (!end) {
        const auto converted = std::from_chars(*first == '+' ? first + 1 : first, Final, component);
        if (converted.ec == std::errc::result_out_of_range) {
          const std::string Sym(first, converted.ptr);
          char* tail = nullptr;
          component = std::strtof(Sym.c_str(), &tail);
          if (tail != Sym.data() + Sym.size()) throw SqlError("VALUE", "invalid vector component");
        } else if (converted.ec != std::errc{}) {
          throw SqlError("VALUE", "invalid vector component");
        }
        end = converted.ptr;
      }
      if (!std::isfinite(component)) throw SqlError("VALUE", "nonfinite float32 component");
      if (count < Dim) Outcome.push_back(component);
      ++count;
      pos = end - Txt.data();
    } while (consume(','));
    if (!consume(']')) throw SqlError("VALUE", "invalid vector syntax");
  }
  while (pos < Txt.size() && space(Txt[pos])) ++pos;
  if (pos != Txt.size()) throw SqlError("VALUE", "invalid vector syntax");
  if (count != Dim) throw SqlError("dimension_mismatch", "wrong vector dimension");
  return Outcome;
}

}  // namespace mini
