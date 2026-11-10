#include "sql.h"

#include <cctype>
#include <cstring>
#include <sstream>

namespace mvd {

std::string SqlParser::lower(const std::string &s) {
  std::string out(s);
  for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

void SqlParser::syntax_error(const std::string &message) {
  throw_error("SYNTAX_ERROR", message);
}

SqlParser::SqlParser(std::string sql) : sql_(std::move(sql)) { tokenize(); }

const SqlParser::Token &SqlParser::peek(size_t ahead) const {
  size_t at = index_ + ahead;
  if (at >= tokens_.size()) return tokens_.back();
  return tokens_[at];
}

bool SqlParser::consume_symbol(char c) {
  if (peek().type == Token::Type::SYMBOL && peek().text.size() == 1 &&
      peek().text[0] == c) {
    ++index_;
    return true;
  }
  return false;
}

bool SqlParser::consume_keyword(const char *kw) {
  if (peek().type == Token::Type::IDENT && lower(peek().text) == kw) {
    ++index_;
    return true;
  }
  return false;
}

bool SqlParser::accept_keyword(const char *kw) { return consume_keyword(kw); }

void SqlParser::expect_symbol(char c) {
  if (!consume_symbol(c)) {
    std::string msg = "expected '";
    msg += c;
    msg += "'";
    syntax_error(msg);
  }
}

std::string SqlParser::expect_ident() {
  if (peek().type != Token::Type::IDENT) syntax_error("expected identifier");
  return tokens_[index_++].text;
}

int64_t SqlParser::expect_int() {
  bool neg = false;
  if (consume_symbol('-')) neg = true;
  else if (consume_symbol('+')) {}
  if (peek().type != Token::Type::NUMBER) syntax_error("expected integer");
  int64_t v;
  if (!parse_int64(peek().text, v))
    throw_error("VALUE_OUT_OF_RANGE", "integer literal out of range");
  ++index_;
  return neg ? -v : v;
}

void SqlParser::tokenize() {
  tokens_.clear();
  size_t i = 0;
  const std::string &s = sql_;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c)) {
      ++i;
      continue;
    }
    // -- line comment (only if followed by whitespace or end)
    if (c == '-' && i + 1 < s.size() && s[i + 1] == '-' &&
        (i + 2 == s.size() || std::isspace(static_cast<unsigned char>(s[i + 2])))) {
      i += 2;
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '#') {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
      i = (i + 1 < s.size()) ? i + 2 : s.size();
      continue;
    }
    if (std::isalpha(c) || c == '_' || c == '$') {
      size_t start = i;
      while (i < s.size()) {
        unsigned char d = static_cast<unsigned char>(s[i]);
        if (std::isalnum(d) || d == '_' || d == '$') ++i;
        else break;
      }
      tokens_.push_back({Token::Type::IDENT, s.substr(start, i - start), start});
      continue;
    }
    if (std::isdigit(c) ||
        (c == '.' && i + 1 < s.size() &&
         std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      size_t start = i;
      bool seen_dot = false;
      bool seen_exp = false;
      while (i < s.size()) {
        unsigned char d = static_cast<unsigned char>(s[i]);
        if (std::isdigit(d)) {
          ++i;
        } else if (d == '.' && !seen_dot && !seen_exp) {
          seen_dot = true;
          ++i;
        } else if ((d == 'e' || d == 'E') && !seen_exp) {
          seen_exp = true;
          ++i;
          if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        } else {
          break;
        }
      }
      tokens_.push_back({Token::Type::NUMBER, s.substr(start, i - start), start});
      continue;
    }
    if (c == '\'' || c == '"') {
      char quote = static_cast<char>(c);
      size_t start = i;
      ++i;
      std::string value;
      bool closed = false;
      while (i < s.size()) {
        char d = s[i];
        if (d == quote) {
          if (i + 1 < s.size() && s[i + 1] == quote) {
            value.push_back(quote);
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        if (d == '\\' && i + 1 < s.size()) {
          char e = s[i + 1];
          switch (e) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '0': value.push_back('\0'); break;
            case '\\': value.push_back('\\'); break;
            case '\'': value.push_back('\''); break;
            case '"': value.push_back('"'); break;
            default: value.push_back(e); break;
          }
          i += 2;
          continue;
        }
        value.push_back(d);
        ++i;
      }
      if (!closed) syntax_error("unterminated string literal");
      tokens_.push_back({Token::Type::STRING, value, start});
      continue;
    }
    if (c == '`') {
      size_t start = ++i;
      while (i < s.size() && s[i] != '`') ++i;
      if (i >= s.size()) syntax_error("unterminated quoted identifier");
      tokens_.push_back({Token::Type::IDENT, s.substr(start, i - start), start});
      ++i;
      continue;
    }
    // symbols
    if (std::strchr("(),.;*=[]+-<>!", c)) {
      tokens_.push_back({Token::Type::SYMBOL, std::string(1, s[i]), i});
      ++i;
      continue;
    }
    syntax_error(std::string("unexpected character: ") + s[i]);
  }
  tokens_.push_back({Token::Type::END, "", s.size()});
}

std::unique_ptr<Statement> SqlParser::parse() {
  if (peek().type == Token::Type::END) syntax_error("empty statement");
  std::unique_ptr<Statement> stmt;
  if (accept_keyword("create")) {
    --index_;  // rewind to CREATE
    std::string next = lower(peek(1).text);
    if (next == "table") {
      stmt = parse_create_table();
    } else if (next == "vector") {
      consume_keyword("create");
      if (!consume_keyword("vector")) syntax_error("expected VECTOR");
      if (!consume_keyword("index")) syntax_error("expected INDEX");
      auto s = std::make_unique<CreateVectorIndexStmt>();
      if (accept_keyword("if")) {
        if (!consume_keyword("not")) syntax_error("expected NOT");
        if (!consume_keyword("exists")) syntax_error("expected EXISTS");
        s->if_not_exists = true;
      }
      s->index_name = expect_ident();
      if (!consume_keyword("on")) syntax_error("expected ON");
      s->table = expect_ident();
      expect_symbol('(');
      s->column = expect_ident();
      expect_symbol(')');
      stmt = std::move(s);
    } else {
      syntax_error("expected TABLE or VECTOR INDEX after CREATE");
    }
  } else if (accept_keyword("drop")) {
    --index_;
    consume_keyword("drop");
    if (!consume_keyword("table")) syntax_error("expected TABLE after DROP");
    auto s = std::make_unique<DropTableStmt>();
    if (accept_keyword("if")) {
      if (!consume_keyword("exists")) syntax_error("expected EXISTS");
      s->if_exists = true;
    }
    s->table = expect_ident();
    stmt = std::move(s);
  } else if (accept_keyword("insert")) {
    --index_;
    stmt = parse_insert();
  } else if (accept_keyword("select")) {
    --index_;
    stmt = parse_select();
  } else if (accept_keyword("update")) {
    --index_;
    stmt = parse_update();
  } else if (accept_keyword("delete")) {
    --index_;
    stmt = parse_delete();
  } else if (accept_keyword("begin")) {
    stmt = std::make_unique<BeginStmt>();
  } else if (accept_keyword("start")) {
    if (!consume_keyword("transaction")) syntax_error("expected TRANSACTION");
    stmt = std::make_unique<BeginStmt>();
  } else if (accept_keyword("commit")) {
    accept_keyword("work");
    stmt = std::make_unique<CommitStmt>();
  } else if (accept_keyword("rollback")) {
    accept_keyword("work");
    stmt = std::make_unique<RollbackStmt>();
  } else if (accept_keyword("end")) {
    accept_keyword("transaction");
    stmt = std::make_unique<CommitStmt>();
  } else if (accept_keyword("set")) {
    --index_;
    stmt = parse_set();
  } else {
    // Give a clear unsupported error for known but unimplemented statements.
    std::string kw = lower(peek().text);
    if (kw == "alter" || kw == "truncate" || kw == "replace" || kw == "show" ||
        kw == "describe" || kw == "desc" || kw == "explain" || kw == "use" ||
        kw == "with" || kw == "create" || kw == "grant" || kw == "revoke") {
      throw_error("UNSUPPORTED", "unsupported statement: " + kw);
    }
    syntax_error("unrecognized statement");
  }
  // Optional trailing semicolons.
  while (consume_symbol(';')) {
  }
  if (peek().type != Token::Type::END)
    syntax_error("unexpected token after statement: " + peek().text);
  return stmt;
}

std::unique_ptr<Statement> SqlParser::parse_create_table() {
  consume_keyword("create");
  if (!consume_keyword("table")) syntax_error("expected TABLE");
  auto stmt = std::make_unique<CreateTableStmt>();
  if (accept_keyword("if")) {
    if (!consume_keyword("not")) syntax_error("expected NOT");
    if (!consume_keyword("exists")) syntax_error("expected EXISTS");
    stmt->if_not_exists = true;
  }
  stmt->table = expect_ident();
  expect_symbol('(');
  size_t start_index = index_;
  bool table_pk_seen = false;
  std::vector<ColumnDef> cols;
  std::string table_pk;
  while (true) {
    if (peek().type == Token::Type::END) syntax_error("unterminated CREATE TABLE");
    // table-level PRIMARY KEY (col)
    if (peek().type == Token::Type::IDENT && lower(peek().text) == "primary") {
      ++index_;
      if (!consume_keyword("key")) syntax_error("expected KEY");
      expect_symbol('(');
      table_pk = expect_ident();
      expect_symbol(')');
      if (table_pk_seen) syntax_error("duplicate PRIMARY KEY");
      table_pk_seen = true;
    } else {
      ColumnDef col;
      col.name = expect_ident();
      std::string type = lower(expect_ident());
      int32_t size = 0;
      if (type == "varchar" || type == "char" || type == "text") {
        col.type = ColumnType::VARCHAR;
        if (consume_symbol('(')) {
          size = static_cast<int32_t>(expect_int());
          expect_symbol(')');
        } else if (type == "text") {
          size = 65535;
        } else {
          syntax_error("VARCHAR requires a length");
        }
        if (size <= 0 || size > 65535)
          throw_error("INVALID_SCHEMA", "VARCHAR length out of range");
        col.max_chars = size;
      } else if (type == "vector") {
        col.type = ColumnType::VECTOR;
        if (!consume_symbol('('))
          syntax_error("VECTOR requires a dimension");
        col.dim = static_cast<int32_t>(expect_int());
        expect_symbol(')');
        if (col.dim <= 0 || col.dim > 16384)
          throw_error("INVALID_SCHEMA", "VECTOR dimension out of range");
      } else if (type == "bigint" || type == "int" || type == "integer") {
        col.type = ColumnType::BIGINT;
      } else {
        throw_error("UNSUPPORTED", "unsupported column type: " + type);
      }
      // Inline column constraints.
      while (true) {
        if (accept_keyword("primary")) {
          if (!consume_keyword("key")) syntax_error("expected KEY");
          if (table_pk_seen) syntax_error("duplicate PRIMARY KEY");
          table_pk = col.name;
          table_pk_seen = true;
        } else if (accept_keyword("not")) {
          if (!consume_keyword("null")) syntax_error("expected NULL");
        } else if (accept_keyword("unique") || accept_keyword("default") ||
                   accept_keyword("check")) {
          throw_error("UNSUPPORTED", "column constraint is not supported");
        } else {
          break;
        }
      }
      cols.push_back(std::move(col));
    }
    if (consume_symbol(',')) continue;
    expect_symbol(')');
    break;
  }
  (void)start_index;
  stmt->columns = std::move(cols);
  if (stmt->columns.empty()) syntax_error("CREATE TABLE requires at least one column");
  // If table-level PK given and an inline PK exists elsewhere, that's an error.
  if (!table_pk_seen) {
    // allow no primary key? Problem requires one BIGINT primary key.
    throw_error("INVALID_SCHEMA", "a BIGINT PRIMARY KEY is required");
  }
  stmt->primary_key = table_pk;
  return stmt;
}

std::unique_ptr<Statement> SqlParser::parse_insert() {
  auto stmt = std::make_unique<InsertStmt>();
  consume_keyword("insert");
  if (!consume_keyword("into")) syntax_error("expected INTO");
  stmt->table = expect_ident();
  if (consume_symbol('(')) {
    while (true) {
      stmt->columns.push_back(expect_ident());
      if (consume_symbol(',')) continue;
      expect_symbol(')');
      break;
    }
  }
  if (!consume_keyword("values")) syntax_error("expected VALUES");
  while (true) {
    expect_symbol('(');
    std::vector<Value> row;
    while (true) {
      Value v;
      if (peek().type == Token::Type::STRING) {
        v = Value::make_string(peek().text);
        ++index_;
      } else if (peek().type == Token::Type::NUMBER) {
        v = Value::make_raw(peek().text);
        ++index_;
      } else if (consume_symbol('-')) {
        if (peek().type != Token::Type::NUMBER) syntax_error("expected number");
        v = Value::make_raw("-" + peek().text);
        ++index_;
      } else if (consume_symbol('+')) {
        if (peek().type != Token::Type::NUMBER) syntax_error("expected number");
        v = Value::make_raw(peek().text);
        ++index_;
      } else if (consume_symbol('[')) {
        std::string raw = "[";
        int depth = 1;
        while (depth > 0) {
          if (peek().type == Token::Type::END) syntax_error("unterminated vector literal");
          if (peek().type == Token::Type::SYMBOL && peek().text == "[") ++depth;
          if (peek().type == Token::Type::SYMBOL && peek().text == "]") --depth;
          if (depth > 0) {
            if (!raw.empty() && raw.back() != '[') raw.push_back(' ');
            raw += peek().text;
          }
          ++index_;
        }
        raw += "]";
        v = Value::make_string(raw);
        v.kind = Value::Kind::VECTOR;
      } else if (peek().type == Token::Type::IDENT &&
                 lower(peek().text) == "null") {
        throw_error("UNSUPPORTED", "NULL is not supported");
      } else {
        syntax_error("expected a literal value");
      }
      row.push_back(std::move(v));
      if (consume_symbol(',')) continue;
      expect_symbol(')');
      break;
    }
    stmt->rows.push_back(std::move(row));
    if (consume_symbol(',')) continue;
    break;
  }
  return stmt;
}

Expr SqlParser::parse_expr() {
  Expr expr;
  if (peek().type != Token::Type::IDENT) syntax_error("expected expression");
  std::string name = lower(peek().text);
  if (name == "l2_distance" || name == "vector_distance") {
    ++index_;
    expr.kind = Expr::Kind::L2_DISTANCE;
    expect_symbol('(');
    expr.column = expect_ident();
    expect_symbol(',');
    if (peek().type == Token::Type::STRING) {
      expr.vec_text = peek().text;
      ++index_;
    } else if (consume_symbol('[')) {
      std::string raw = "[";
      int depth = 1;
      while (depth > 0) {
        if (peek().type == Token::Type::END)
          syntax_error("unterminated vector literal");
        if (peek().type == Token::Type::SYMBOL && peek().text == "[") ++depth;
        if (peek().type == Token::Type::SYMBOL && peek().text == "]") --depth;
        if (depth > 0) {
          if (!raw.empty() && raw.back() != '[') raw.push_back(' ');
          raw += peek().text;
        }
        ++index_;
      }
      raw += "]";
      expr.vec_text = raw;
    } else {
      syntax_error("expected vector literal");
    }
    expect_symbol(')');
    // Parse vector later when the column dimension is known.
    return expr;
  }
  expr.kind = Expr::Kind::COLUMN;
  expr.column = expect_ident();
  return expr;
}

std::vector<Condition> SqlParser::parse_where() {
  std::vector<Condition> out;
  if (!consume_keyword("where")) return out;
  out.push_back(parse_condition());
  while (accept_keyword("and")) out.push_back(parse_condition());
  return out;
}

Condition SqlParser::parse_condition() {
  Condition c;
  c.column = expect_ident();
  if (consume_symbol('=')) {
    c.op = CompareOp::EQ;
  } else if (consume_symbol('!')) {
    if (!consume_symbol('=')) syntax_error("expected '=' after '!'");
    c.op = CompareOp::NE;
  } else if (consume_symbol('<')) {
    if (consume_symbol('=')) c.op = CompareOp::LE;
    else if (consume_symbol('>')) c.op = CompareOp::NE;
    else c.op = CompareOp::LT;
  } else if (consume_symbol('>')) {
    if (consume_symbol('=')) c.op = CompareOp::GE;
    else c.op = CompareOp::GT;
  } else {
    syntax_error("expected comparison operator");
  }
  if (peek().type == Token::Type::STRING) {
    c.value = Value::make_string(peek().text);
    ++index_;
  } else if (peek().type == Token::Type::NUMBER) {
    c.value = Value::make_raw(peek().text);
    ++index_;
  } else if (consume_symbol('-')) {
    if (peek().type != Token::Type::NUMBER) syntax_error("expected number");
    c.value = Value::make_raw("-" + peek().text);
    ++index_;
  } else if (consume_symbol('+')) {
    if (peek().type != Token::Type::NUMBER) syntax_error("expected number");
    c.value = Value::make_raw(peek().text);
    ++index_;
  } else {
    syntax_error("expected literal in WHERE");
  }
  return c;
}

std::unique_ptr<Statement> SqlParser::parse_select() {
  auto stmt = std::make_unique<SelectStmt>();
  consume_keyword("select");
  while (true) {
    SelectItem item;
    if (consume_symbol('*')) {
      item.star = true;
    } else {
      item.expr = parse_expr();
      if (accept_keyword("as")) {
        item.alias = expect_ident();
      } else if (peek().type == Token::Type::IDENT &&
                 lower(peek().text) != "from" && lower(peek().text) != "where" &&
                 lower(peek().text) != "order" && lower(peek().text) != "limit") {
        item.alias = peek().text;
        ++index_;
      }
    }
    stmt->items.push_back(std::move(item));
    if (consume_symbol(',')) continue;
    break;
  }
  if (!consume_keyword("from")) syntax_error("expected FROM");
  stmt->table = expect_ident();
  stmt->where = parse_where();
  if (accept_keyword("order")) {
    if (!consume_keyword("by")) syntax_error("expected BY");
    while (true) {
      OrderKey key;
      if (peek().type == Token::Type::IDENT &&
          (lower(peek().text) == "l2_distance" ||
           lower(peek().text) == "vector_distance")) {
        key.is_distance = true;
        key.expr = parse_expr();
      } else if (peek().type == Token::Type::NUMBER) {
        key.ordinal = static_cast<int>(expect_int());
        if (key.ordinal <= 0) syntax_error("ORDER BY ordinal must be positive");
      } else {
        key.name = expect_ident();
      }
      if (accept_keyword("desc")) key.desc = true;
      else accept_keyword("asc");
      stmt->order.push_back(std::move(key));
      if (consume_symbol(',')) continue;
      break;
    }
  }
  if (accept_keyword("limit")) {
    stmt->has_limit = true;
    int64_t first = expect_int();
    if (first < 0) throw_error("INVALID_VALUE", "LIMIT must be non-negative");
    // MySQL-compatible "LIMIT offset, count"
    if (consume_symbol(',')) {
      stmt->offset = first;
      int64_t count = expect_int();
      if (count < 0) throw_error("INVALID_VALUE", "LIMIT must be non-negative");
      stmt->limit = count;
    } else {
      stmt->limit = first;
      if (accept_keyword("offset")) {
        int64_t off = expect_int();
        if (off < 0) throw_error("INVALID_VALUE", "OFFSET must be non-negative");
        stmt->offset = off;
      }
    }
  }
  return stmt;
}

std::unique_ptr<Statement> SqlParser::parse_update() {
  auto stmt = std::make_unique<UpdateStmt>();
  consume_keyword("update");
  stmt->table = expect_ident();
  if (!consume_keyword("set")) syntax_error("expected SET");
  while (true) {
    std::string col = expect_ident();
    expect_symbol('=');
    Value v;
    if (peek().type == Token::Type::STRING) {
      v = Value::make_string(peek().text);
      ++index_;
    } else if (peek().type == Token::Type::NUMBER) {
      v = Value::make_raw(peek().text);
      ++index_;
    } else if (consume_symbol('-')) {
      if (peek().type != Token::Type::NUMBER) syntax_error("expected number");
      v = Value::make_raw("-" + peek().text);
      ++index_;
    } else if (consume_symbol('+')) {
      if (peek().type != Token::Type::NUMBER) syntax_error("expected number");
      v = Value::make_raw(peek().text);
      ++index_;
    } else if (consume_symbol('[')) {
      std::string raw = "[";
      int depth = 1;
      while (depth > 0) {
        if (peek().type == Token::Type::END) syntax_error("unterminated vector literal");
        if (peek().type == Token::Type::SYMBOL && peek().text == "[") ++depth;
        if (peek().type == Token::Type::SYMBOL && peek().text == "]") --depth;
        if (depth > 0) {
          if (!raw.empty() && raw.back() != '[') raw.push_back(' ');
          raw += peek().text;
        }
        ++index_;
      }
      raw += "]";
      v = Value::make_string(raw);
      v.kind = Value::Kind::VECTOR;
    } else {
      syntax_error("expected literal value");
    }
    stmt->sets.emplace_back(std::move(col), std::move(v));
    if (consume_symbol(',')) continue;
    break;
  }
  stmt->where = parse_where();
  return stmt;
}

std::unique_ptr<Statement> SqlParser::parse_delete() {
  auto stmt = std::make_unique<DeleteStmt>();
  consume_keyword("delete");
  if (!consume_keyword("from")) syntax_error("expected FROM");
  stmt->table = expect_ident();
  stmt->where = parse_where();
  return stmt;
}

std::unique_ptr<Statement> SqlParser::parse_set() {
  auto stmt = std::make_unique<SetStmt>();
  consume_keyword("set");
  stmt->name = lower(expect_ident());
  if (!consume_symbol('=')) {
    if (accept_keyword("to")) {
    } else {
      syntax_error("expected '='");
    }
  }
  if (peek().type == Token::Type::IDENT || peek().type == Token::Type::STRING) {
    stmt->value = lower(peek().text);
    ++index_;
  } else {
    syntax_error("expected setting value");
  }
  return stmt;
}

}  // namespace mvd
