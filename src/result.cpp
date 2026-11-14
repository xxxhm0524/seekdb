#include "result.hpp"
#include <array>
namespace mvd {
RowOutput::~RowOutput() {
    file_.close(); std::error_code error; std::filesystem::remove(path_, error);
}
void RowOutput::append(const std::string& s) {
    if (!file_.is_open() && memory_.size() + s.size() > 262144) {
        file_.open(path_, std::ios::binary | std::ios::trunc);
        if (!file_) throw IoError("open response temporary file");
        file_.write(memory_.data(), static_cast<std::streamsize>(memory_.size()));
        std::string().swap(memory_);
    }
    if (file_.is_open()) {
        file_.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!file_) throw IoError("write response temporary file");
    } else memory_ += s;
}
void RowOutput::add(const Json& row) {
    std::string bytes = row.dump();
    if (!first_) append(",");
    append(bytes); first_ = false;
}
void RowOutput::write(std::ostream& out) {
    if (!file_.is_open()) { out << memory_; return; }
    file_.close();
    if (file_.fail()) throw IoError("flush response temporary file");
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw IoError("read response temporary file");
    std::array<char, 65536> bytes{};
    while (input) { input.read(bytes.data(), bytes.size()); out.write(bytes.data(), input.gcount()); }
    if (!input.eof()) throw IoError("read response temporary file");
}
void Result::write(const Json& id, std::ostream& out) {
    fields["id"] = id; fields["ok"] = true;
    if (!rows) out << fields.dump();
    else {
        std::string head = fields.dump(); head.pop_back();
        out << head << ",\"rows\":["; rows->write(out); out << "]}";
    }
    out << '\n' << std::flush;
}
}
