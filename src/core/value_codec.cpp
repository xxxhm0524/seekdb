#include "../model/codec.h"
#include "error.h"
#include "number.h"
namespace mvd {
void parse_vector_for_column(const std::string &text, const ColumnDef &col,
                             std::vector<float> &out) {
  out.clear();
  out.reserve(static_cast<size_t>(col.dim));
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= text.size() || text[i] != '[')
    throw_error("INVALID_VECTOR", "expected numeric vector component");
  ++i;
  std::vector<std::string_view> parts;
  parts.reserve(static_cast<size_t>(col.dim));
  while (true) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < text.size() && text[i] == ']') { ++i; break; }
    size_t start = i;
    while (i < text.size() && text[i] != ',' && text[i] != ']') ++i;
    size_t end = i;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    if (end == start) throw_error("INVALID_VECTOR", "expected numeric vector component");
    parts.emplace_back(text.data()+start, end-start);
    if (i < text.size() && text[i] == ',') {
      ++i;
      size_t next = i;
      while (next < text.size() && std::isspace(static_cast<unsigned char>(text[next]))) ++next;
      if (next >= text.size() || text[next] == ']')
        throw_error("INVALID_VECTOR", "expected numeric vector component after comma");
      continue;
    }
    if (i < text.size() && text[i] == ']') { ++i; break; }
    throw_error("INVALID_VECTOR", "expected numeric vector component");
  }
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i != text.size()) throw_error("INVALID_VECTOR", "expected numeric vector component");
  for (const auto &part : parts) {
    float v = 0;
    Float32Parse status = parse_float32_value(part, v);
    if (status == Float32Parse::INVALID)
      throw_error("INVALID_VECTOR", "expected numeric vector component");
    if (status == Float32Parse::OVERFLOW)
      throw_error("VECTOR_OVERFLOW", "vector component overflows float32");
    out.push_back(v);
  }
  if (static_cast<int>(out.size()) != col.dim)
    throw_error("VECTOR_DIM_MISMATCH",
                col.name + " must have " + std::to_string(col.dim) + " dimensions");
}

Value coerce_value(const TableSchema &schema, int col_index,
                           const Value &v) {
  const ColumnDef &col = schema.columns[col_index];
  if (col.type == ColumnType::BIGINT) {
    int64_t out;
    if (v.kind == Value::Kind::INT) return v;
    if ((v.kind == Value::Kind::RAW || v.kind == Value::Kind::STRING) &&
        parse_int64(v.s, out))
      return Value::make_int(out);
    throw_error("INVALID_VALUE", "invalid BIGINT literal");
  }
  if (col.type == ColumnType::VARCHAR) {
    if (v.kind == Value::Kind::STRING || v.kind == Value::Kind::RAW)
      return v;
    throw_error("INVALID_VALUE", "invalid VARCHAR literal");
  }
  if (col.type == ColumnType::VECTOR) {
    std::vector<float> vec;
    if (!v.s.empty()) parse_vector_for_column(v.s, col, vec);
    else if (!v.vec.empty()) vec = v.vec;
    else throw_error("INVALID_VECTOR", "expected numeric vector component");
    if (static_cast<int>(vec.size()) != col.dim)
      throw_error("VECTOR_DIM_MISMATCH",
                  col.name + " must have " + std::to_string(col.dim) + " dimensions");
    return Value::make_vector(std::move(vec));
  }
  return v;
}

}
