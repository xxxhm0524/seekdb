#include "model.hpp"
#include <bit>
#include <charconv>
#include <cstring>

namespace mvd {
size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n) {
        uint8_t first = static_cast<uint8_t>(s[i++]);
        if (first < 128) continue;
        unsigned extra; uint32_t cp, minimum;
        if (first >= 0xc2 && first <= 0xdf) { extra = 1; cp = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { extra = 2; cp = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { extra = 3; cp = first & 7; minimum = 0x10000; }
        else fail("invalid_value", "invalid UTF-8");
        if (i + extra > s.size()) fail("invalid_value", "truncated UTF-8");
        while (extra--) {
            uint8_t c = static_cast<uint8_t>(s[i++]);
            if ((c & 0xc0) != 0x80) fail("invalid_value", "invalid UTF-8 continuation");
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            fail("invalid_value", "invalid Unicode codepoint");
    }
    return n;
}
Value parse_value(const Column& c, const std::string& text, bool quoted) {
    if (c.type == Type::Integer) {
        if (quoted) fail("type_error", "BIGINT requires an integer literal");
        int64_t v = 0;
        const char* start = text.data();
        if (!text.empty() && text[0] == '+') ++start;
        auto r = std::from_chars(start, text.data() + text.size(), v);
        if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || start == r.ptr)
            fail("invalid_value", "BIGINT literal is invalid or out of range");
        return v;
    }
    if (c.type == Type::Text) {
        if (!quoted) fail("type_error", "VARCHAR requires a string literal");
        if (utf8_length(text) > c.width) fail("value_too_long", "VARCHAR exceeds its Unicode length");
        return text;
    }
    Json a;
    try { a = Json::parse(text); } catch (...) { fail("invalid_vector", "invalid vector literal"); }
    if (!a.is_array() || a.size() != c.width) fail("invalid_vector", "incorrect vector dimension");
    std::vector<float> v; v.reserve(a.size());
    for (const auto& x : a) {
        if (!x.is_number()) fail("invalid_vector", "vector elements must be numeric");
        double d = x.get<double>();
        if (!std::isfinite(d) || std::abs(d) > static_cast<double>(std::numeric_limits<float>::max()))
            fail("invalid_vector", "vector value overflows finite float32");
        float f = static_cast<float>(d);
        if (!std::isfinite(f)) fail("invalid_vector", "vector value is not finite float32");
        v.push_back(f);
    }
    return v;
}
static void put(std::string& s, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
}
static uint64_t get(const std::string& s, size_t& at, unsigned n) {
    if (at > s.size() || n > s.size() - at) throw IoError("truncated row");
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= uint64_t(static_cast<uint8_t>(s[at++])) << (8 * i);
    return v;
}
std::string encode_row(const Table& t, const Row& row) {
    std::string s;
    for (size_t i = 0; i < t.columns.size(); ++i) {
        switch (t.columns[i].type) {
        case Type::Integer: put(s, std::bit_cast<uint64_t>(std::get<int64_t>(row[i])), 8); break;
        case Type::Text: {
            const auto& v = std::get<std::string>(row[i]);
            if (v.size() > UINT32_MAX) fail("resource_limit", "string is too large");
            put(s, v.size(), 4); s.append(v); break;
        }
        case Type::Vector:
            for (float f : std::get<std::vector<float>>(row[i])) put(s, std::bit_cast<uint32_t>(f), 4);
            break;
        }
    }
    return s;
}
Row decode_row(const Table& t, const std::string& s) {
    Row row; row.reserve(t.columns.size()); size_t at = 0;
    for (const auto& c : t.columns) {
        switch (c.type) {
        case Type::Integer: row.emplace_back(std::bit_cast<int64_t>(get(s, at, 8))); break;
        case Type::Text: {
            uint64_t n = get(s, at, 4);
            if (n > s.size() - at) throw IoError("truncated string");
            row.emplace_back(s.substr(at, n)); at += n; break;
        }
        case Type::Vector: {
            if (c.width > (s.size() - at) / 4) throw IoError("truncated vector");
            std::vector<float> v; v.reserve(c.width);
            for (uint32_t i = 0; i < c.width; ++i) v.push_back(std::bit_cast<float>(static_cast<uint32_t>(get(s, at, 4))));
            row.emplace_back(std::move(v)); break;
        }
        }
    }
    if (at != s.size()) throw IoError("unexpected trailing row bytes");
    return row;
}
Json value_json(const Value& v) { return std::visit([](const auto& x) { return Json(x); }, v); }
Json catalog_json(const Catalog& tables) {
    Json a = Json::array();
    for (const auto& t : tables) {
        Json cs = Json::array();
        for (const auto& c : t.columns) cs.push_back({{"name", c.name}, {"type", static_cast<int>(c.type)}, {"width", c.width}});
        a.push_back({{"name", t.name}, {"columns", cs}, {"primary", t.primary}, {"root", t.root},
                     {"count", t.count}, {"indexes", t.indexes}});
    }
    return a;
}
Catalog catalog_from_json(const Json& value) {
    Catalog out;
    for (const auto& v : value) {
        Table t;
        t.name = v.at("name").get<std::string>(); t.primary = v.at("primary").get<size_t>();
        t.root = v.at("root").get<uint64_t>(); t.count = v.at("count").get<uint64_t>();
        t.indexes = v.at("indexes").get<std::vector<std::string>>();
        for (const auto& c : v.at("columns")) {
            int type = c.at("type").get<int>();
            if (type < 0 || type > 2) throw IoError("invalid column type");
            t.columns.push_back({c.at("name").get<std::string>(), static_cast<Type>(type), c.at("width").get<uint32_t>()});
        }
        if (t.primary >= t.columns.size() || t.columns[t.primary].type != Type::Integer) throw IoError("invalid primary key");
        out.push_back(std::move(t));
    }
    return out;
}
}
