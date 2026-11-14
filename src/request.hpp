#pragma once
#include "model.hpp"
namespace mvd {
Json parse_request_json(const std::string& line, bool* fast = nullptr);
}
