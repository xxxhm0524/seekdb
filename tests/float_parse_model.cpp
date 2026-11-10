// Compare the optimized conversion with the previous strtof behavior, bit for bit.
#include "common.h"
#include <bit>
#include <iostream>
#include <random>

int main() {
  size_t checked = 0;
  auto check = [&](const std::string &text) {
    errno = 0;
    char *end = nullptr;
    float expected = std::strtof(text.c_str(), &end);
    auto status = mvd::Float32Parse::VALID;
    if (text.empty() || end == text.c_str() || end != text.c_str()+text.size())
      status = mvd::Float32Parse::INVALID;
    else if (!std::isfinite(expected))
      status = errno == ERANGE ? mvd::Float32Parse::OVERFLOW : mvd::Float32Parse::INVALID;
    float actual = 0;
    auto actual_status = mvd::parse_float32_value(text, actual);
    if (actual_status != status || (status == mvd::Float32Parse::VALID &&
        std::bit_cast<uint32_t>(actual) != std::bit_cast<uint32_t>(expected))) {
      std::cerr << "float conversion differs for: " << text << '\n';
      std::exit(1);
    }
    ++checked;
  };
  for (const char *text : {"", " ", "+", "--1", "+-1", "+-0", "+-1e30", "++1", "1x", "1e", "NaN", "nan(foo)",
      "Infinity", "-inf", "1e9999", "-1e9999", "1e-9999", "+0x1p+2", "0x1.2p-2",
      "3.40282347e38", "3.4028236e38", "1.17549435e-38", "1.40129846e-45",
      "7.00649232e-46", "7.00649233e-46", "-0", "+0", "1 ", " 1"}) check(text);
  std::mt19937_64 random(20261110);
  char buffer[128];
  for (int i = 0; i < 200000; ++i) {
    float value = std::bit_cast<float>(static_cast<uint32_t>(random()));
    if (std::isfinite(value)) {
      std::snprintf(buffer, sizeof(buffer), "%.9g", value); check(buffer);
      std::snprintf(buffer, sizeof(buffer), "%.17g", static_cast<double>(value)); check(buffer);
    }
    std::string decimal = random()%2 ? "+" : "-";
    decimal += std::to_string(random()%10)+".";
    for (unsigned j = 0, digits = 1+random()%24; j < digits; ++j)
      decimal.push_back(static_cast<char>('0'+random()%10));
    decimal += "e"+std::to_string(static_cast<int>(random()%201)-100);
    check(decimal);
  }
  std::cout << "float parse model: PASS (" << checked << " conversions)\n";
}
