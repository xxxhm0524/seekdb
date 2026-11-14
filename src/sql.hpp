#pragma once
#include "model.hpp"
namespace mvd {
struct Token { std::string text; bool quoted = false, identifier = false, delimited = false; };
struct Literal { std::string text; bool quoted = false; };
struct Expr { std::string column; std::optional<Literal> vector; std::string alias; };
struct Condition { std::string column, op; Literal value; };
struct Ordering { Expr expr; bool descending = false; };
struct Statement {
    enum Kind { Create, Insert, Select, Update, Delete, Index, Begin, Commit, Rollback } kind;
    std::string table, index;
    Table definition;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<Literal>> values;
    std::vector<Expr> projections;
    std::vector<Condition> conditions;
    std::vector<Ordering> order;
    std::vector<std::pair<std::string, Literal>> assignments;
    std::optional<uint64_t> limit;
    bool star = false;
};
Statement parse_sql(const std::string& sql);
}
