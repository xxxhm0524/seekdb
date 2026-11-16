#include "vector_literal.hpp"
#include "model.hpp"
#include "fast_float/fast_float.h"
#include <cfenv>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#include <charconv>
#include <cstdlib>

namespace mvd {
namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
void whitespace(std::string_view text, size_t& position, bool sql) {
    while (position < text.size()) {
        const char c = text[position];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || (sql && c == '\f')) { ++position; continue; }
        if (sql && c == '-' && position + 1 < text.size() && text[position + 1] == '-') {
            while (position < text.size() && text[position] != '\n') ++position;
            continue;
        }
        if (sql && c == '/' && position + 1 < text.size() && text[position + 1] == '*') {
            const size_t end = text.find("*/", position + 2);
            if (end == std::string_view::npos) fail("invalid_vector", "unterminated vector comment");
            position = end + 2; continue;
        }
        break;
    }
}
std::from_chars_result decimal_number(const char* first, const char* last, double& value, bool fast) {
    if (fast) {
        const auto result = fast_float::from_chars(first, last, value);
        return {result.ptr, result.ec};
    }
    return std::from_chars(first, last, value);
}
float checked_float(double value) {
    const double overflow = std::ldexp(2.0 - std::ldexp(1.0, -24), 127);
    const double maximum = std::numeric_limits<float>::max();
    if (!std::isfinite(value) || std::abs(value) >= overflow) fail("invalid_vector", "vector value overflows finite float32");
    if (value > maximum) return std::numeric_limits<float>::max();
    if (value < -maximum) return -std::numeric_limits<float>::max();
    return static_cast<float>(value);
}
float component_checked(std::string_view text, size_t& position, bool sql, bool fast) {
    size_t begin = position;
    if (position < text.size() && (text[position] == '-' || (sql && text[position] == '+'))) ++position;
    if (position == text.size()) fail("invalid_vector", "incomplete vector number");
    if (sql) {
        size_t digits = 0;
        while (position < text.size() && digit(text[position])) { ++position; ++digits; }
        if (position < text.size() && text[position] == '.') {
            ++position;
            while (position < text.size() && digit(text[position])) { ++position; ++digits; }
        }
        if (!digits) fail("invalid_vector", "vector number requires digits");
    } else {
        if (text[position] == '0') ++position;
        else {
            if (text[position] < '1' || text[position] > '9') fail("invalid_vector", "invalid vector number");
            do { ++position; } while (position < text.size() && digit(text[position]));
        }
        if (position < text.size() && text[position] == '.') {
            ++position;
            if (position == text.size() || !digit(text[position])) fail("invalid_vector", "missing vector decimal digits");
            do { ++position; } while (position < text.size() && digit(text[position]));
        }
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) ++position;
        if (position == text.size() || !digit(text[position])) fail("invalid_vector", "missing vector exponent digits");
        do { ++position; } while (position < text.size() && digit(text[position]));
    }
    double value = 0;
    const size_t numeric_begin = begin + (sql && text[begin] == '+');
    auto parsed = decimal_number(text.data() + numeric_begin, text.data() + position, value, fast);
    if (parsed.ec == std::errc::result_out_of_range) {
        // A double underflow still rounds to finite float32 zero. Rare extreme
        // exponents use the C conversion; ordinary values allocate nothing.
        std::string number(text.substr(begin, position - begin));
        char* end = nullptr; value = std::strtod(number.c_str(), &end);
        if (end != number.c_str() + number.size()) fail("invalid_vector", "invalid vector number");
    } else if (parsed.ec != std::errc{} || parsed.ptr != text.data() + position) fail("invalid_vector", "invalid vector number");
    return checked_float(value);
}
float component(std::string_view text, size_t& position, bool sql, bool fast) {
    if (fast && position < text.size()) {
        const char initial = text[position];
        const bool valid_start = digit(initial) || initial == '-' || (sql && (initial == '+' || initial == '.'));
        const char* first = text.data() + position;
        const char* last = text.data() + text.size();
        bool prefix_ok = valid_start;
        if (sql && initial == '+') {
            ++first;
            prefix_ok = first < last && (digit(*first) || *first == '.');
        }
        if (prefix_ok) {
            double value = 0;
            const auto format = sql ? fast_float::chars_format::general : fast_float::chars_format::json;
            const auto parsed = fast_float::from_chars(first, last, value, format);
            if (parsed.ec == std::errc{} && parsed.ptr > first && std::isfinite(value)) {
                const size_t end = static_cast<size_t>(parsed.ptr - text.data());
                const char next = end < text.size() ? text[end] : char(0);
                const bool whitespace_end = next == ' ' || next == '\t' || next == '\r' || next == '\n' || (sql && next == '\f');
                const bool comment_end = sql && end + 1 < text.size() &&
                    ((next == '/' && text[end + 1] == '*') || (next == '-' && text[end + 1] == '-'));
                if (end == text.size() || next == ',' || next == ']' || whitespace_end || comment_end) {
                    const float result = checked_float(value);
                    position = end;
                    return result;
                }
            }
        }
    }
    return component_checked(text, position, sql, fast);
}
}
std::vector<float> parse_vector_literal(std::string_view text, uint32_t dimensions, bool sql_literals) {
    bool fast = std::fegetround() == FE_TONEAREST;
#if defined(__x86_64__) || defined(_M_X64)
    fast = fast && (_mm_getcsr() & 0xffc0U) == 0x1f80U;
#endif
    size_t at = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf) at = 3;
    whitespace(text, at, sql_literals);
    if (at == text.size() || text[at++] != '[') fail("invalid_vector", "vector must be an array");
    std::vector<float> values; values.reserve(std::min<size_t>(dimensions, 128));
    size_t count = 0; whitespace(text, at, sql_literals);
    if (at < text.size() && text[at] == ']') ++at;
    else for (;;) {
        float value = component(text, at, sql_literals, fast); ++count;
        if (values.size() < dimensions) values.push_back(value);
        whitespace(text, at, sql_literals);
        if (at < text.size() && text[at] == ']') { ++at; break; }
        if (at == text.size() || text[at++] != ',') fail("invalid_vector", "missing vector separator");
        whitespace(text, at, sql_literals);
    }
    whitespace(text, at, sql_literals);
    if (at != text.size() || count != dimensions) fail("invalid_vector", "incorrect vector dimension or trailing input");
    return values;
}
}
