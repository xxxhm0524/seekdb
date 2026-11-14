#include "../src/request.hpp"
#include <bit>
#include <chrono>
#include <iostream>
#include <random>
#include <set>

using mvd::Json;
static Json original(const std::string& line) {
    if (line.find('\0') != std::string::npos) throw std::runtime_error("raw NUL");
    std::vector<std::set<std::string>> objects;
    auto callback = [&](int, Json::parse_event_t event, Json& value) {
        using Event = Json::parse_event_t;
        if (event == Event::object_start) objects.emplace_back();
        else if (event == Event::object_end) objects.pop_back();
        else if (event == Event::key && !objects.back().insert(value.get<std::string>()).second) throw std::runtime_error("duplicate");
        return true;
    };
    return Json::parse(line, callback);
}
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static Json tree(std::mt19937& rng, unsigned depth = 0) {
    switch (rng() % (depth < 4 ? 7 : 5)) {
    case 0: return nullptr;
    case 1: return bool(rng() % 2);
    case 2: return std::bit_cast<int64_t>((uint64_t(rng()) << 32) | rng());
    case 3: return uint64_t(rng()) << 32 | rng();
    case 4: {
        static const std::array<std::string, 8> fragments{"abc", "\"\\'", "\r\n\t", std::string("x\0y", 3), "\xe4\xb8\xad", "\xf0\x9f\x99\x82", "", "/*[]{}*/"};
        std::string value; for (unsigned i = 0, n = rng() % 8; i < n; ++i) value += fragments[rng() % fragments.size()];
        return value;
    }
    case 5: {
        Json value = Json::array(); for (unsigned i = 0, n = rng() % 4; i < n; ++i) value.push_back(tree(rng, depth + 1)); return value;
    }
    default: {
        Json value = Json::object(); for (unsigned i = 0, n = rng() % 4; i < n; ++i) value[std::to_string(i)] = tree(rng, depth + 1); return value;
    }
    }
}
int main() try {
    std::mt19937 rng(434291); size_t checked = 0, raw_checked = 0;
    auto check = [&](const std::string& input) {
        bool old_ok = false, new_ok = false; Json old_value; mvd::RequestFields fields;
        try { old_value = original(input); old_ok = old_value.is_object(); } catch (const std::exception&) {}
        try { fields = mvd::parse_request_fields(input); new_ok = true; } catch (const std::exception&) {}
        require(old_ok == new_ok, "request grammar acceptance differs");
        if (old_ok) {
            require(fields.size() == old_value.size(), "request field count changed");
            for (const auto& [key, field] : fields) {
                require(original(std::string(field.raw)).dump() == old_value.at(key).dump(), "representable request value changed");
                if (old_value.at(key).is_string()) require(field.kind == '"' && field.text == old_value.at(key).get<std::string>(), "request string decoding changed");
            }
        }
        ++checked;
    };
    for (unsigned i = 0; i < 12000; ++i) {
        const auto input = Json({{"id", tree(rng)}, {"op", "execute"}, {"sql", "SELECT id FROM t;"}}).dump(-1, ' ', i % 2);
        check(input); check(" \t" + input + "\r");
        if (!input.empty()) {
            auto broken = input; broken.erase(rng() % broken.size(), 1); check(broken);
            broken = input; broken.insert(rng() % (broken.size() + 1), 1, char(rng() % 256)); check(broken);
        }
    }
    for (const std::string number : {"0.0", "-0.0", "1e-9999", "0e99999999999999999", "1.234567890123456789", "18446744073709551615", "18446744073709551616", "-9223372036854775808", "-9223372036854775809", "1.7976931348623157e308", "1.7976931348623159e308"}) {
        check("{\"id\":" + number + ",\"op\":\"execute\",\"sql\":\"SELECT 1\"}");
    }
    for (const auto input : {"{\"a\":1,\"a\":2}", "{\"a\":{\"b\":1,\"b\":2}}", "{\"a\":1,\"\\u0061\":2}", "[0,{\"x\":1,\"x\":2}]", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\udc00\"", "{}garbage", "[1,]", "{\"id\":NaN}", "\xef\xbb\xbf{\"id\":1}"}) check(input);
    for (unsigned byte = 0; byte < 256; ++byte) check("{\"x\":\"" + std::string(1, char(byte)) + "\"}");
    check(std::string("{\"id\":1}\0ignored", 17));
    // The old DOM is intentionally NOT an oracle for arbitrary-precision IDs.
    // Check their validated raw spans directly, and use Python/real processes
    // in protocol_ids.py for independent value-and-JSON-type round trips.
    for (const std::string& number : std::vector<std::string>{"18446744073709551616", "18446744073709551617", "-9223372036854775809", "1" + std::string(400, '0'), "1" + std::string(3000, '0')}) {
        const std::string input = "{\"id\":" + number + ",\"op\":\"shutdown\"}";
        auto fields = mvd::parse_request_fields(input);
        require(fields.at("id").raw == number && fields.at("id").kind == '0', "arbitrary-precision integer ID changed"); ++raw_checked;
    }
    {
        const std::string id = "{\"number\":1" + std::string(400, '0') + ",\"array\":[true,null,\"x\"]}";
        const std::string input = "{\"id\":" + id + ",\"op\":\"shutdown\"}";
        require(mvd::parse_request_fields(input).at("id").raw == id, "composite arbitrary-precision ID changed"); ++raw_checked;
    }
    Json request{{"id", 7}, {"op", "execute"}, {"sql", "INSERT INTO t VALUES(1,'" + std::string(250000, 'x') + "');"}, {"search_mode", "exact"}};
    std::string input = request.dump();
    require(mvd::parse_request_fields(input).at("sql").text == request.at("sql").get<std::string>(), "large SQL changed");
    size_t checksum = 0; auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 200; ++i) checksum += original(input)["sql"].get_ref<const std::string&>().size();
    double old_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 200; ++i) checksum -= mvd::parse_request_fields(input).at("sql").text.size();
    double new_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    require(checksum == 0, "request timing checksum");
    std::cout << "{\"passed\":true,\"representable_tree_parity_cases\":" << checked << ",\"raw_large_id_cases\":" << raw_checked
              << ",\"large_string_bytes\":250000,\"timed_frames\":200,\"original_seconds\":" << old_seconds << ",\"fast_seconds\":" << new_seconds << "}\n";
} catch (const std::exception& error) {
    std::cout << Json({{"passed", false}, {"error", error.what()}}).dump() << '\n'; return 1;
}
