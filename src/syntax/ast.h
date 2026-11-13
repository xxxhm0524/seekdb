#pragma once
#include "../model/value.h"
#include <memory>
namespace mvd {
struct Expr {
  enum class Kind : uint8_t {
    COLUMN,
    L2_DISTANCE,
  } kind = Kind::COLUMN;
  std::string column;       // COLUMN or L2_DISTANCE vector column
  std::string vec_text;     // original text (documentation)
};

struct Condition {
  std::string column;
  CompareOp op = CompareOp::EQ;
  Value value;
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
}
