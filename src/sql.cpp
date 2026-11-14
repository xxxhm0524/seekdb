#include "sql.hpp"
#include <charconv>
#include <cctype>
#include <set>
namespace mvd {
static std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (std::isspace(c)) { ++i; continue; }
        if (c == '-' && i + 1 < s.size() && s[i+1] == '-') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i+1] == '*') {
            auto end = s.find("*/", i + 2);
            if (end == std::string::npos) fail("syntax_error", "unterminated comment");
            i = end + 2; continue;
        }
        if (c == '\'' || c == '`' || c == '"') {
            char delimiter = s[i++]; std::string text; bool ended = false;
            while (i < s.size()) {
                char x = s[i++];
                if (x == delimiter) {
                    if (i < s.size() && s[i] == delimiter) { text.push_back(x); ++i; }
                    else { ended = true; break; }
                } else if (x == '\\' && delimiter == '\'') {
                    if (i == s.size()) fail("syntax_error", "unterminated string escape");
                    x = s[i++];
                    switch (x) {
                    case 'n': text.push_back('\n'); break;
                    case 'r': text.push_back('\r'); break;
                    case 't': text.push_back('\t'); break;
                    case '0': text.push_back('\0'); break;
                    case 'b': text.push_back('\b'); break;
                    default: text.push_back(x); break;
                    }
                } else text.push_back(x);
            }
            if (!ended) fail("syntax_error", "unterminated quoted token");
            out.push_back({text, delimiter == '\'', delimiter != '\''}); continue;
        }
        if (std::isalpha(c) || c == '_' || c >= 128) {
            size_t begin = i++;
            while (i < s.size()) {
                unsigned char x = s[i];
                if (!(std::isalnum(x) || x == '_' || x == '$' || x >= 128)) break;
                ++i;
            }
            out.push_back({s.substr(begin, i - begin), false, true}); continue;
        }
        if (std::isdigit(c) || c == '.' || ((c == '+' || c == '-') && i+1 < s.size() &&
                                            (std::isdigit(static_cast<unsigned char>(s[i+1])) || s[i+1] == '.'))) {
            size_t begin = i++;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            out.push_back({s.substr(begin, i - begin), false, false}); continue;
        }
        std::string symbol(1, s[i++]);
        if ((c == '<' || c == '>' || c == '!') && i < s.size() && (s[i] == '=' || (c == '<' && s[i] == '>')))
            symbol.push_back(s[i++]);
        out.push_back({symbol, false, false});
    }
    return out;
}
class Parser {
    std::vector<Token> tokens;
    size_t at = 0;
    bool end() const { return at == tokens.size(); }
    bool is(const std::string& text) const { return !end() && !tokens[at].quoted && lower(tokens[at].text) == text; }
    bool take(const std::string& text) { if (is(text)) { ++at; return true; } return false; }
    void need(const std::string& text) { if (!take(text)) fail("syntax_error", "expected " + text); }
    std::string identifier() {
        if (end() || !tokens[at].identifier) fail("syntax_error", "expected identifier");
        return tokens[at++].text;
    }
    uint64_t unsigned_integer() {
        if (end() || tokens[at].quoted) fail("syntax_error", "expected unsigned integer");
        auto s = tokens[at++].text;
        uint64_t v = 0; auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) fail("invalid_value", "invalid unsigned integer");
        return v;
    }
    Literal literal() {
        if (end()) fail("syntax_error", "expected literal");
        if (take("[")) {
            std::string s = "[";
            while (!end() && !is("]")) s += tokens[at++].text;
            need("]"); return {s + "]", false};
        }
        auto t = tokens[at++];
        if (!t.quoted && (t.identifier || t.text == ")" || t.text == "," || t.text == ";"))
            fail("invalid_value", "expected a supported literal");
        return {t.text, t.quoted};
    }
    Expr expr(bool alias) {
        Expr e;
        if (take("l2_distance")) {
            need("("); e.column = identifier(); need(","); e.vector = literal(); need(")");
        } else e.column = identifier();
        if (alias && take("as")) e.alias = identifier();
        return e;
    }
    void where(Statement& s) {
        if (!take("where")) return;
        do {
            Condition c; c.column = identifier();
            if (end()) fail("syntax_error", "missing comparison operator");
            c.op = tokens[at++].text;
            if (c.op != "=" && c.op != "<" && c.op != ">" && c.op != "<=" && c.op != ">=" && c.op != "!=" && c.op != "<>")
                fail("unsupported", "unsupported comparison operator");
            c.value = literal(); s.conditions.push_back(std::move(c));
        } while (take("and"));
    }
public:
    explicit Parser(const std::string& s) : tokens(lex(s)) {}
    Statement parse() {
        Statement s{};
        if (take("create")) {
            if (take("vector")) {
                need("index"); s.kind = Statement::Index; s.index = identifier(); need("on");
                s.table = identifier(); need("("); s.insert_columns.push_back(identifier()); need(")");
            } else {
                need("table"); s.kind = Statement::Create; s.table = identifier(); s.definition.name = lower(s.table);
                need("("); std::optional<std::string> pk; std::set<std::string> names; size_t vectors = 0;
                do {
                    if (take("primary")) {
                        need("key"); need("("); auto name = identifier(); need(")");
                        if (pk) fail("invalid_schema", "multiple primary keys");
                        pk = name; continue;
                    }
                    Column c; c.name = identifier();
                    if (!names.insert(lower(c.name)).second) fail("invalid_schema", "duplicate column name");
                    if (take("bigint")) c.type = Type::Integer;
                    else if (take("varchar")) {
                        c.type = Type::Text; need("("); auto n = unsigned_integer(); need(")");
                        if (!n || n > UINT32_MAX) fail("invalid_schema", "invalid VARCHAR length");
                        c.width = static_cast<uint32_t>(n);
                    } else if (take("vector")) {
                        c.type = Type::Vector; need("("); auto n = unsigned_integer(); need(")");
                        if (!n || n > UINT32_MAX || ++vectors > 1) fail("invalid_schema", "invalid VECTOR definition");
                        c.width = static_cast<uint32_t>(n);
                    } else fail("unsupported", "unsupported column type");
                    if (take("primary")) { need("key"); if (pk) fail("invalid_schema", "multiple primary keys"); pk = c.name; }
                    s.definition.columns.push_back(std::move(c));
                } while (take(","));
                need(")");
                if (!pk || s.definition.columns.empty()) fail("invalid_schema", "one BIGINT primary key is required");
                s.definition.primary = s.definition.column(*pk);
                if (s.definition.columns[s.definition.primary].type != Type::Integer) fail("invalid_schema", "primary key must be BIGINT");
            }
        } else if (take("insert")) {
            s.kind = Statement::Insert; need("into"); s.table = identifier();
            if (take("(")) { do { s.insert_columns.push_back(identifier()); } while (take(",")); need(")"); }
            need("values");
            do {
                need("("); std::vector<Literal> row;
                do { row.push_back(literal()); } while (take(","));
                need(")"); s.values.push_back(std::move(row));
            } while (take(","));
        } else if (take("select")) {
            s.kind = Statement::Select;
            if (take("*")) s.star = true;
            else { do { s.projections.push_back(expr(true)); } while (take(",")); }
            need("from"); s.table = identifier(); where(s);
            if (take("order")) {
                need("by");
                do { Ordering o; o.expr = expr(false); o.descending = take("desc"); if (!o.descending) take("asc"); s.order.push_back(std::move(o)); } while (take(","));
            }
            if (take("limit")) s.limit = unsigned_integer();
        } else if (take("update")) {
            s.kind = Statement::Update; s.table = identifier(); need("set");
            do { auto name = identifier(); need("="); s.assignments.emplace_back(name, literal()); } while (take(","));
            where(s);
        } else if (take("delete")) {
            s.kind = Statement::Delete; need("from"); s.table = identifier(); where(s);
        } else if (take("begin")) { s.kind = Statement::Begin; take("transaction"); }
        else if (take("start")) { need("transaction"); s.kind = Statement::Begin; }
        else if (take("commit")) s.kind = Statement::Commit;
        else if (take("rollback")) s.kind = Statement::Rollback;
        else fail("unsupported", "unsupported SQL statement");
        take(";");
        if (!end()) fail("unsupported", "unsupported syntax or multiple statements");
        return s;
    }
};
Statement parse_sql(const std::string& sql) { return Parser(sql).parse(); }
}
