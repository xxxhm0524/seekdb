#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
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

enum class CompareOp : uint8_t { EQ, NE, LT, LE, GT, GE };

struct BoundCondition {int resolved_column=-1;CompareOp op=CompareOp::EQ;Value typed_value;};
}
