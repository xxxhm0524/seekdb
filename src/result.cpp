#include "result.hpp"
#include <array>
namespace mvd {
RowOutput::~RowOutput() {
    input_.close(); file_.close();
    if (spill_created_) { std::error_code error; std::filesystem::remove(path_, error); }
}
void RowOutput::append(const std::string& s) {
    if (!file_.is_open() && memory_.size() + s.size() > 262144) {
        file_.open(path_, std::ios::binary | std::ios::trunc);
        spill_created_ = true;
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
void RowOutput::prepare() {
    if (prepared_) return;
    if (file_.fail()) throw IoError("write response temporary file");
    if (file_.is_open()) {
        file_.close();
        if (file_.fail()) throw IoError("flush response temporary file");
        input_.open(path_, std::ios::binary);
        if (!input_) throw IoError("open finalized response temporary file");
    }
    prepared_ = true;
}
void RowOutput::write(std::ostream& out) {
    if (!prepared_) throw IoError("response was not finalized");
    if (!input_.is_open()) { out << memory_; return; }
    std::array<char, 65536> bytes{};
    while (input_) { input_.read(bytes.data(), bytes.size()); out.write(bytes.data(), input_.gcount()); }
    if (!input_.eof()) throw IoError("read response temporary file");
}
void Result::write(const Json& id, std::ostream& out, bool& output_started) {
    fields["id"] = id; fields["ok"] = true;
    if (!rows) {
        std::string body = fields.dump(); output_started = true; out << body;
    }
    else {
        std::string head = fields.dump(); head.pop_back();
        rows->prepare();
        output_started = true;
        out << head << ",\"rows\":["; rows->write(out); out << "]}";
    }
    out << '\n' << std::flush;
}
}
