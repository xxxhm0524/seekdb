#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace mvd {
struct OutValue {
  enum class Kind { INT, DOUBLE, STRING, VECTOR } kind = Kind::INT;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<float> vec;
};

struct Response {
  bool ok = true;
  bool is_query = false;
  bool has_affected = false;
  int64_t affected_rows = 0;
  std::vector<std::string> columns;
  std::vector<std::vector<OutValue>> rows;
  std::string error_code;
  std::string error_message;
};

}
