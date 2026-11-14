#include "engine.hpp"
#include <iostream>
#include <set>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
mvd::Json strict_json(const std::string& line) {
    std::vector<std::set<std::string>> objects;
    auto callback = [&](int, mvd::Json::parse_event_t event, mvd::Json& value) {
        using Event = mvd::Json::parse_event_t;
        if (event == Event::object_start) objects.emplace_back();
        else if (event == Event::object_end) objects.pop_back();
        else if (event == Event::key && !objects.back().insert(value.get<std::string>()).second)
            mvd::fail("invalid_request", "duplicate JSON object key");
        return true;
    };
    return mvd::Json::parse(line, callback);
}
void error_response(const mvd::Json& id, const std::string& code, const std::string& message) {
    std::cout << mvd::Json({{"id", id}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}}).dump()
              << '\n' << std::flush;
}
}
int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false); std::cin.tie(nullptr);
    try {
        std::filesystem::path directory = "/data";
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--data-dir" && i + 1 < argc) directory = argv[++i];
            else throw std::runtime_error("usage: minivectordb [--data-dir DIRECTORY]");
        }
        std::filesystem::create_directories(directory);
        mvd::Engine engine(directory);
        std::cout << "{\"type\":\"ready\"}\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line)) {
            mvd::Json id = nullptr;
            try {
                mvd::Json request;
                try { request = strict_json(line); }
                catch (const mvd::Error&) { throw; }
                catch (const mvd::Json::exception&) { mvd::fail("invalid_request", "malformed JSON"); }
                if (!request.is_object() || !request.contains("id")) mvd::fail("invalid_request", "request must contain an id");
                id = request["id"];
                if (!request.contains("op") || !request["op"].is_string()) mvd::fail("invalid_request", "request requires string op");
                std::string op = request["op"].get<std::string>();
                if (op == "shutdown") { engine.shutdown(); mvd::Result{}.write(id, std::cout); return 0; }
                if (op != "execute" || !request.contains("sql") || !request["sql"].is_string()) mvd::fail("invalid_request", "expected execute with SQL string");
                if (request.contains("search_mode") && !request["search_mode"].is_string()) mvd::fail("invalid_request", "search_mode must be a string");
                auto result = engine.execute(request["sql"].get<std::string>(), request.value("search_mode", "exact"));
                result.write(id, std::cout);
            } catch (const mvd::Error& error) { error_response(id, error.code, error.what()); }
            catch (const mvd::IoError& error) { error_response(id, "io_error", error.what()); return 1; }
            catch (const std::bad_alloc&) { error_response(id, "resource_limit", "memory allocation failed"); return 1; }
            catch (const std::exception& error) { error_response(id, "internal_error", error.what()); return 1; }
            if (!std::cout) return 1;
        }
        engine.shutdown(); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
