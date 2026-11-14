#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
namespace mvd {
std::vector<float> parse_vector_literal(std::string_view text, uint32_t dimensions);
}
