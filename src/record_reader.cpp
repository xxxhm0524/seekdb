#include "storage.hpp"
#include <bit>

namespace mvd {
namespace { struct MaterializationLimit {}; }
Row Storage::read_selected(const Table& table, Ref ref, const std::vector<bool>& wanted) const {
    return read_selected_impl(table, ref, wanted, SIZE_MAX);
}
std::optional<Row> Storage::read_selected_bounded(const Table& table, Ref ref, const std::vector<bool>& wanted, size_t limit) const {
    try { return read_selected_impl(table, ref, wanted, limit); }
    catch (const MaterializationLimit&) { return std::nullopt; }
}
Row Storage::read_selected_impl(const Table& table, Ref ref, const std::vector<bool>& wanted, size_t limit) const {
    if (wanted.size() != table.columns.size()) throw IoError("invalid projected record shape");
    if (ref.offset < 8192 || ref.offset > end_ || ref.length > end_ - ref.offset)
        throw IoError("invalid projected record reference");
    if (table.columns.size() > limit / sizeof(Value)) throw MaterializationLimit{};
    limit -= table.columns.size() * sizeof(Value);
    Row row(table.columns.size());
    uint64_t position = 0;
    auto range = [&](uint64_t size, void* data) {
        if (position > ref.length || size > ref.length - position) throw IoError("truncated projected record");
        if (data && size) file_.read(ref.offset + position, data, static_cast<size_t>(size));
        position += size;
    };
    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& column = table.columns[i];
        if (column.type == Type::Integer) {
            std::array<unsigned char, 8> bytes{};
            range(8, wanted[i] ? bytes.data() : nullptr);
            if (wanted[i]) row[i] = std::bit_cast<int64_t>(read_le(bytes.data(), 8));
        } else if (column.type == Type::Text) {
            std::array<unsigned char, 4> length{};
            range(4, length.data());
            uint64_t size = read_le(length.data(), 4);
            if (position > ref.length || size > ref.length - position) throw IoError("truncated projected string");
            if (wanted[i]) {
                if (size > limit) throw MaterializationLimit{};
                limit -= static_cast<size_t>(size);
                std::string text(static_cast<size_t>(size), '\0');
                range(size, text.data()); row[i] = std::move(text);
            } else range(size, nullptr);
        } else {
            uint64_t size = uint64_t(column.width) * sizeof(float);
            if (position > ref.length || size > ref.length - position) throw IoError("truncated projected vector");
            if (wanted[i]) {
                if (size > limit) throw MaterializationLimit{};
                limit -= static_cast<size_t>(size);
                std::string bytes(static_cast<size_t>(size), '\0');
                range(size, bytes.data());
                std::vector<float> vector; vector.reserve(column.width);
                for (uint32_t j = 0; j < column.width; ++j)
                    vector.push_back(std::bit_cast<float>(static_cast<uint32_t>(read_le(
                        reinterpret_cast<const unsigned char*>(bytes.data()) + uint64_t(j) * 4, 4))));
                row[i] = std::move(vector);
            } else range(size, nullptr);
        }
    }
    if (position != ref.length) throw IoError("unexpected projected record tail");
    return row;
}
}
