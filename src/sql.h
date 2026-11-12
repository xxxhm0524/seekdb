// MiniVectorDB - SQL lexer, parser and AST
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common.h"

namespace mvd {

enum class ColumnType : uint8_t {
  BIGINT = 1,
  VARCHAR = 2,
  VECTOR = 3,
};

struct ColumnDef {
  std::string name;
  ColumnType type = ColumnType::BIGINT;
  int32_t max_chars = 0;  // VARCHAR(n)
  int32_t dim = 0;        // VECTOR(n)
};

// A value used for INSERT / UPDATE literals and WHERE comparisons.
struct Value {
  enum class Kind : uint8_t { INT, RAW, STRING, VECTOR, NULLV } kind = Kind::NULLV;
  int64_t i = 0;
  std::string s;              // string, raw numeric text or raw vector text
  std::vector<float> vec;     // parsed vector

  static Value make_int(int64_t v) {
    Value x;
    x.kind = Kind::INT;
    x.i = v;
    return x;
  }
  static Value make_raw(std::string v) {
    Value x;
    x.kind = Kind::RAW;
    x.s = std::move(v);
    return x;
  }
  static Value make_string(std::string v) {
    Value x;
    x.kind = Kind::STRING;
    x.s = std::move(v);
    return x;
  }
  static Value make_vector(std::vector<float> v) {
    Value x;
    x.kind = Kind::VECTOR;
    x.vec = std::move(v);
    return x;
  }
};

struct Expr {
  enum class Kind : uint8_t {
    COLUMN,
    L2_DISTANCE,
    VECTOR_LITERAL,
  } kind = Kind::COLUMN;
  std::string column;       // COLUMN or L2_DISTANCE vector column
  std::vector<float> vec;   // L2_DISTANCE query vector
  std::string vec_text;     // original text (documentation)
};

enum class CompareOp : uint8_t { EQ, NE, LT, LE, GT, GE };

struct Condition {
  std::string column;
  CompareOp op = CompareOp::EQ;
  Value value;
  // Filled in by the engine before scanning.
  int resolved_column = -1;
  Value typed_value;
};

struct SelectItem {
  bool star = false;
  Expr expr;
  std::string alias;  // explicit alias, may be empty
};

struct OrderKey {
  std::string name;  // output alias or table column
  bool desc = false;
  bool is_distance = false;
  int ordinal = 0;  // 1-based ORDER BY ordinal, 0 if a name/expression
  Expr expr;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------
struct Statement {
  enum class Kind : uint8_t {
    CREATE_TABLE,
    DROP_TABLE,
    CREATE_VECTOR_INDEX,
    INSERT,
    SELECT,
    UPDATE,
    DELETE,
    BEGIN,
    COMMIT,
    ROLLBACK,
    SET,
  } kind;
  virtual ~Statement() = default;
};

struct CreateTableStmt : Statement {
  CreateTableStmt() { kind = Kind::CREATE_TABLE; }
  std::string table;
  std::vector<ColumnDef> columns;
  std::string primary_key;  // column name
  bool if_not_exists = false;
};

struct DropTableStmt : Statement {
  DropTableStmt() { kind = Kind::DROP_TABLE; }
  std::string table;
  bool if_exists = false;
};

struct CreateVectorIndexStmt : Statement {
  CreateVectorIndexStmt() { kind = Kind::CREATE_VECTOR_INDEX; }
  std::string index_name;
  std::string table;
  std::string column;
  bool if_not_exists = false;
};

struct InsertStmt : Statement {
  InsertStmt() { kind = Kind::INSERT; }
  std::string table;
  std::vector<std::string> columns;  // empty = all columns in schema order
  std::vector<std::vector<Value>> rows;
};

struct SelectStmt : Statement {
  SelectStmt() { kind = Kind::SELECT; }
  std::vector<SelectItem> items;
  std::string table;
  std::vector<Condition> where;
  std::vector<OrderKey> order;
  bool has_limit = false;
  int64_t limit = 0;
  int64_t offset = 0;
};

struct UpdateStmt : Statement {
  UpdateStmt() { kind = Kind::UPDATE; }
  std::string table;
  std::vector<std::pair<std::string, Value>> sets;
  std::vector<Condition> where;
};

struct DeleteStmt : Statement {
  DeleteStmt() { kind = Kind::DELETE; }
  std::string table;
  std::vector<Condition> where;
};

struct BeginStmt : Statement {
  BeginStmt() { kind = Kind::BEGIN; }
};
struct CommitStmt : Statement {
  CommitStmt() { kind = Kind::COMMIT; }
};
struct RollbackStmt : Statement {
  RollbackStmt() { kind = Kind::ROLLBACK; }
};
struct SetStmt : Statement {
  SetStmt() { kind = Kind::SET; }
  std::string name;
  std::string value;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class SqlParser {
 public:
  explicit SqlParser(std::string sql);
  std::unique_ptr<Statement> parse();

 private:
  struct Token {
    enum class Type { IDENT, NUMBER, STRING, SYMBOL, END } type;
    std::string text;
    size_t pos;
  };

  void tokenize();
  const Token &peek(size_t ahead = 0) const;
  bool consume_symbol(char c);
  bool consume_keyword(const char *kw);
  bool accept_keyword(const char *kw);
  void expect_symbol(char c);
  std::string expect_ident();
  std::string expect_string_or_ident_for_value();
  int64_t expect_int();
  Value parse_value_for_column(const ColumnDef &col);
  std::string collect_vector_literal();  // Opening '[' was already consumed.
  Expr parse_expr();
  std::vector<Condition> parse_where();
  Condition parse_condition();
  std::unique_ptr<Statement> parse_create_table();
  std::unique_ptr<Statement> parse_insert();
  std::unique_ptr<Statement> parse_select();
  std::unique_ptr<Statement> parse_update();
  std::unique_ptr<Statement> parse_delete();
  std::unique_ptr<Statement> parse_set();

  static std::string lower(const std::string &s);
  [[noreturn]] void syntax_error(const std::string &message);

  std::string sql_;
  std::vector<Token> tokens_;
  size_t index_ = 0;
};

}  // namespace mvd
