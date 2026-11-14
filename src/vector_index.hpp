#pragma once
#include "storage.hpp"
#include <memory>
namespace mvd {
// A derived, bounded snapshot, never the source of truth. Nodes contain exact
// float32 bounding boxes; their L2 lower bounds allow exact branch-and-bound.
class VectorIndex {
public:
    struct Entry { int64_t key; Ref ref; Row row; };
    struct Hit { size_t entry; double distance; };
private:
    struct Node {
        size_t begin = 0, end = 0;
        int left = -1, right = -1;
        std::vector<float> low, high;
    };
    uint64_t root_;
    std::string table_;
    size_t vector_column_ = 0;
    std::vector<Entry> entries_;
    std::vector<size_t> order_;
    std::vector<Node> nodes_;
    int build(size_t begin, size_t end);
public:
    VectorIndex(uint64_t root, std::string table) : root_(root), table_(std::move(table)) {}
    static std::unique_ptr<VectorIndex> create(Storage& store, const Table& table);
    bool valid(const Table& table) const { return root_ == table.root && table_ == table.name; }
    const std::vector<Entry>& entries() const { return entries_; }
    std::vector<Hit> search(const std::vector<float>& query, size_t k,
                           const std::function<bool(const Row&)>& matches) const;
};
}
