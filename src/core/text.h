#pragma once
#include <cctype>
#include <string>
namespace mvd {inline std::string lower_copy(std::string value){for(char& c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return value;}}
