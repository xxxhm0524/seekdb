#pragma once
#include "ast.h"
#include <cstddef>
namespace mvd {
class SqlParser {
 public:
  explicit SqlParser(std::string sql);
  std::unique_ptr<Statement> parse();

 private:
  struct Token {
    enum class Type { IDENT, NUMBER, STRING, SYMBOL, END } type;
    std::string text;
    size_t pos;
    bool quoted = false;
  };

  void tokenize();
  const Token &peek(size_t ahead = 0) const;
  bool consume_symbol(char c);
  bool consume_keyword(const char *kw);
  bool is_distance_call() const;
  bool accept_keyword(const char *kw);
  void expect_symbol(char c);
  std::string expect_ident();
  int64_t expect_int();
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

}
