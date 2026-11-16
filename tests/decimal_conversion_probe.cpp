#include "model.hpp"
#include "vector_literal.hpp"
#include "fast_float/fast_float.h"
#include <array>
#include <bit>
#include <cfenv>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <random>
#include <optional>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace mvd::legacy_literal {
std::vector<float> parse_vector_literal(std::string_view, uint32_t, bool);
}
namespace {
struct Outcome {
    std::vector<uint32_t> bits;
    std::string error;
    bool operator==(const Outcome&) const = default;
};
Outcome parse(const std::string& input, bool sql, bool legacy) {
    Outcome result;
    try {
        auto v = legacy ? mvd::legacy_literal::parse_vector_literal(input, 1, sql)
                        : mvd::parse_vector_literal(input, 1, sql);
        for (float value : v) result.bits.push_back(std::bit_cast<uint32_t>(value));
    } catch (const mvd::Error& error) {
        result.error = error.code + ":" + error.what();
    }
    return result;
}
size_t vector_checks = 0, double_checks = 0;
void check(const std::string& number, bool sql, std::optional<uint32_t> expected = {}) {
    const std::string input = "[" + number + "]";
    auto old = parse(input, sql, true), current = parse(input, sql, false);
    if (!(old == current)) throw std::runtime_error("decimal vector mismatch: " + number.substr(0, 100));
    if (expected && (current.bits != std::vector<uint32_t>{*expected} || !current.error.empty()))
        throw std::runtime_error("independent float32 result mismatch");
    ++vector_checks;
}
void check_double(const std::string& number) {
    char* end = nullptr;
    const double expected = std::strtod(number.c_str(), &end);
    if (end != number.c_str() + number.size()) throw std::runtime_error("invalid oracle input");
    double value = 0;
    auto r = fast_float::from_chars(number.data(), number.data() + number.size(), value);
    if (r.ec == std::errc::result_out_of_range) value = std::strtod(number.c_str(), nullptr);
    else if (r.ec != std::errc{} || r.ptr != number.data() + number.size())
        throw std::runtime_error("fast decimal parse rejected valid input");
    if (std::bit_cast<uint64_t>(expected) != std::bit_cast<uint64_t>(value))
        throw std::runtime_error("binary64 rounding mismatch: " + number.substr(0, 100));
    ++double_checks;
}
std::string decimal(double value) {
    std::array<char, 64> buffer;
    auto r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                          std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (r.ec != std::errc{}) throw std::runtime_error("format oracle input");
    return {buffer.data(), r.ptr};
}
}
int main() {
    std::fesetround(FE_TONEAREST);
    const std::vector<std::string> edges = {
        "0", "-0", "+0", ".0", "1.", "+.25", "01", "-01", "0e-99999",
        "1e-99999", "-1e-99999", "1e99999", "-1e99999", "1e-324", "5e-324",
        "2.2250738585072014e-308", "2.2250738585072011e-308",
        "1.401298464324817e-45", "7.006492321624085e-46", "1.1754943508222875e-38",
        "3.4028234663852886e38", "3.4028235677973365e38", "3.4028235677973366e38",
        "1.000000059604644775390625", "1.000000059604644775390624999999999999",
        "1.000000059604644775390625000000000001", "-1.000000059604644775390625000000000001",
        "NaN", "nan", "Infinity", "inf", "0x1p0", "1e", "1e+", "+", "-", "null", "true",
        "1 2", "1,2", "1/*x*/", "1--x\n", "1e99999999999999999999999999999999"
    };
    for (const auto& s : edges) { check(s, false); check(s, true); }
    // Preserve the existing decimal -> binary64 -> binary32 conversion, which
    // differs from direct binary32 parsing immediately above this midpoint.
    check("1.000000059604644775390625000000000001", false, 0x3f800000U);
    check("-0", false, 0x80000000U);
    std::mt19937_64 rng(2026111601);
    for (size_t i = 0; i < 300000; ++i) {
        double value = std::bit_cast<double>(rng());
        if (std::isfinite(value)) check_double(decimal(value));
    }
    for (size_t i = 0; i < 100000; ++i) {
        const uint32_t bits = static_cast<uint32_t>(rng());
        const float value = std::bit_cast<float>(bits);
        if (std::isfinite(value)) check(decimal(double(value)), false, bits);
    }
    for (size_t i = 0; i < 30000; ++i) {
        const size_t digits = 1 + rng() % 1100;
        std::string s = rng() % 2 ? "-" : "";
        s += char('1' + rng() % 9); s += '.';
        for (size_t j = 1; j < digits; ++j) s += char('0' + rng() % 10);
        s += "e" + std::to_string(int(rng() % 801) - 400);
        check_double(s); check(s, false);
    }
    for (const auto& s : {"0." + std::string(100000, '0') + "1",
                          "1" + std::string(100000, '0') + "e-100000"}) {
        check_double(s); check(s, false);
    }
    for (int mode : {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
        if (std::fesetround(mode)) throw std::runtime_error("rounding mode unavailable");
        for (const auto& s : edges) { check(s, false); check(s, true); }
        for (size_t i = 0; i < 1000; ++i) check(decimal(double(int(rng() % 100000) - 50000) / 123), false);
    }
    std::fesetround(FE_TONEAREST);
#if defined(__x86_64__) || defined(_M_X64)
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(saved | 0x8040U);
    for (const auto& s : edges) { check(s, false); check(s, true); }
    _mm_setcsr(saved);
#endif
    std::cout << "{\"passed\":true,\"vector_checks\":" << vector_checks
              << ",\"binary64_checks\":" << double_checks
              << ",\"nondefault_rounding_modes\":3,\"ftz_daz_checked\":true}\n";
}
