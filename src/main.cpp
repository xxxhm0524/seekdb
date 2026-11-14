#include "engine.hpp"
#include "request.hpp"
#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void error_response(std::string_view id, const std::string& code, const std::string& message) {
    const std::string body = mvd::Json({{"ok", false}, {"error", {{"code", code}, {"message", message}}}}).dump();
    std::cout << "{\"id\":" << id << ',';
    std::cout.write(body.data() + 1, static_cast<std::streamsize>(body.size() - 1));
    std::cout << '\n' << std::flush;
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
            std::string_view id = "null";
            bool output_started = false;
            try {
                auto request = mvd::parse_request_fields(line);
                if (!request.contains("id")) mvd::fail("invalid_request", "request must contain an id");
                id = request.at("id").raw;
                if (!request.contains("op") || request.at("op").kind != '"') mvd::fail("invalid_request", "request requires string op");
                const std::string& op = request.at("op").text;
                if (op == "shutdown") { engine.shutdown(); mvd::Result{}.write(id, std::cout, output_started); return std::cout ? 0 : 1; }
                if (op != "execute" || !request.contains("sql") || request.at("sql").kind != '"') mvd::fail("invalid_request", "expected execute with SQL string");
                if (request.contains("search_mode") && request.at("search_mode").kind != '"') mvd::fail("invalid_request", "search_mode must be a string");
                auto result = engine.execute(request.at("sql").text, request.contains("search_mode") ? request.at("search_mode").text : "exact");
                result.write(id, std::cout, output_started);
            } catch (const mvd::Error& error) {
                if (output_started) { std::cerr << error.what() << '\n'; return 1; }
                error_response(id, error.code, error.what());
            } catch (const mvd::IoError& error) {
                if (output_started) std::cerr << error.what() << '\n';
                else error_response(id, "io_error", error.what());
                return 1;
            } catch (const std::bad_alloc&) {
                if (output_started) std::cerr << "memory allocation failed while publishing response\n";
                else error_response(id, "resource_limit", "memory allocation failed");
                return 1;
            } catch (const std::exception& error) {
                if (output_started) std::cerr << error.what() << '\n';
                else error_response(id, "internal_error", error.what());
                return 1;
            }
            if (!std::cout) return 1;
        }
        engine.shutdown(); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
