#pragma once
#include "model.hpp"
#include <map>
#include <string_view>
namespace mvd {
// Raw value spans borrow the complete validated frame until its response ends.
// IDs never enter a bounded integer/double DOM representation.
struct RequestValue { char kind = 0; std::string text; std::string_view raw; };
using RequestFields = std::map<std::string, RequestValue>;
RequestFields parse_request_fields(const std::string& line);
}
