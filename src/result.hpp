#pragma once
#include "model.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>

namespace mvd {
// Finish the response before writing its first byte to stdout. Large result
// sets spill to /data instead of growing the process heap with the table.
class RowOutput {
    std::filesystem::path path_;
    std::ofstream file_;
    std::string memory_;
    bool first_ = true;
    void append(const std::string& bytes);
public:
    explicit RowOutput(const std::filesystem::path& directory) : path_(directory / "response.tmp") {}
    ~RowOutput();
    void add(const Json& row);
    void write(std::ostream& output);
};
struct Result {
    Json fields = Json::object();
    std::unique_ptr<RowOutput> rows;
    void write(const Json& id, std::ostream& out);
};
}
