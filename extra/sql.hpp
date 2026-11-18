#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mini {

struct SqlError : std::runtime_error {
  std::string_view code;
  SqlError(std::string_view code, const char* message) : std::runtime_error(message), code(code) {}
};

enum class Type : uint8_t { bigint, varchar, vector };
struct Column {
  std::string Label;
  Type Kind;
  uint32_t length = 0;
  bool primary = false;
};

enum class LiteralKind { number, string, vector };
using SqlText = std::variant<std::string_view, std::string>;

inline std::string_view txt_view(const SqlText& Txt) {
  if (const auto* view = std::get_if<std::string_view>(&Txt)) return *view;
  return std::get<std::string>(Txt);
}

struct Literal {
  LiteralKind Sort3;
  SqlText Txt;
};

enum class Comparison { eq, lt, le, gt, ge };
struct Predicate {
  std::string Col;
  Comparison comparison;
  Literal Val;
};

struct Expression {
  std::string Col;
  std::optional<Literal> Lookup;
  std::string alias;
};
struct Ordering {
  Expression Ky;
  bool descending;
};
struct CreateRelation { std::string Label; std::vector<Column> columns; };
struct CreateIndex { std::string Label, Tbl, Col; };
struct Insert {
  std::string Tbl;
  std::vector<std::string> columns;
  std::vector<std::vector<Literal>> Recs;
};
struct Select {
  std::string Tbl;
  bool star = false;
  std::vector<Expression> projections;
  std::vector<Predicate> Pred;
  std::vector<Ordering> Seq;
  std::optional<uint64_t> Cap;
};
struct Update {
  std::string Tbl;
  std::vector<std::pair<std::string, Literal>> assignments;
  std::vector<Predicate> Pred;
};
struct Delete { std::string Tbl; std::vector<Predicate> Pred; };
enum class Transaction { begin, Finalize, Revert };
using Statement = std::variant<CreateRelation, CreateIndex, Insert, Select, Update, Delete, Transaction>;

// Unescaped literals point into sql, so that buffer has to outlive Stmt execution.
Statement parse(std::string_view sql);
size_t utf8_reach(std::string_view Val);
int64_t bigint(const Literal& Val);
std::string_view varchar(const Literal& Val, uint32_t max_chars);
std::vector<float> vector_val(const Literal& Val, uint32_t Dim);

}  // namespace mini
