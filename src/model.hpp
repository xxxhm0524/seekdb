#pragma once
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace mvd {
using Json = nlohmann::json;
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, const std::string& message) : std::runtime_error(message), code(std::move(c)) {}
};
struct IoError : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] inline void fail(const std::string& code, const std::string& message) { throw Error(code, message); }
inline std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
enum class Type { Integer, Text, Vector };
struct Column { std::string name; Type type; uint32_t width = 0; };
using Value = std::variant<int64_t, std::string, std::vector<float>>;
using Row = std::vector<Value>;
struct Ref { uint64_t offset = 0; uint32_t length = 0; };
struct Table {
    std::string name;
    std::vector<Column> columns;
    size_t primary = 0;
    uint64_t root = 0, count = 0;
    std::vector<std::string> indexes;
    size_t column(const std::string& name) const {
        for (size_t i = 0; i < columns.size(); ++i) if (lower(columns[i].name) == lower(name)) return i;
        fail("unknown_column", "unknown column: " + name);
    }
};
using Catalog = std::vector<Table>;
size_t utf8_length(const std::string& s);
Value parse_value(const Column& column, const std::string& text, bool quoted);
std::string encode_row(const Table& table, const Row& row);
Row decode_row(const Table& table, const std::string& bytes);
Json value_json(const Value& value);
Json catalog_json(const Catalog& catalog);
Catalog catalog_from_json(const Json& value);
}
