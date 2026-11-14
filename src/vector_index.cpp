#include "vector_index.hpp"
#include <cstdlib>
#include <numeric>
#include <queue>
namespace mvd {
static size_t vector_budget() {
    size_t mib = 16;
    if (const char* value = std::getenv("MVD_VECTOR_CACHE_MIB")) {
        try { mib = std::stoull(value); } catch (...) { fail("invalid_config", "invalid vector cache size"); }
        if (mib > 64) fail("invalid_config", "vector cache maximum is 64 MiB");
    }
    return mib * 1024 * 1024;
}
std::unique_ptr<VectorIndex> VectorIndex::create(Storage& store, const Table& table) {
    size_t budget = vector_budget(), column = table.columns.size();
    for (size_t i = 0; i < table.columns.size(); ++i) if (table.columns[i].type == Type::Vector) column = i;
    if (!budget || column == table.columns.size()) return nullptr;
    auto index = std::make_unique<VectorIndex>(table.root, table.name); index->vector_column_ = column;
    // Reserve by the known row count, but never reserve beyond the entire budget.
    if (table.count > budget / (sizeof(Entry) + sizeof(size_t) + table.columns.size() * sizeof(Value))) return nullptr;
    index->entries_.reserve(static_cast<size_t>(table.count));
    size_t used = index->entries_.capacity() * sizeof(Entry);
    bool complete = true;
    BTree tree(store);
    tree.scan(table.root, [&](int64_t key, Ref ref) {
        if (ref.length > budget) { complete = false; return false; }
        Row row = decode_row(table, store.read(ref));
        used += row.capacity() * sizeof(Value);
        for (const auto& v : row) {
            if (auto s = std::get_if<std::string>(&v)) used += s->capacity() + 32;
            if (auto a = std::get_if<std::vector<float>>(&v)) used += a->capacity() * sizeof(float) + 32;
        }
        if (used > budget) { complete = false; return false; }
        index->entries_.push_back({key, ref, std::move(row)}); return true;
    });
    if (!complete) return nullptr;
    if (!table.indexes.empty() && !index->entries_.empty()) {
        size_t n = index->entries_.size(), dimensions = table.columns[column].width;
        // A full binary tree splitting down to 32-point leaves has fewer than
        // 4*ceil(n/32) nodes. Account for vector capacities, not just payload.
        size_t max_nodes = 4 * ((n + 31) / 32);
        uint64_t extra = uint64_t(n) * sizeof(size_t) + uint64_t(max_nodes) * (sizeof(Node) + 2 * uint64_t(dimensions) * sizeof(float) + 64);
        if (extra <= budget && used <= budget - extra) {
            index->order_.resize(n); std::iota(index->order_.begin(), index->order_.end(), 0);
            index->nodes_.reserve(max_nodes); index->build(0, n);
        }
    }
    return index;
}
int VectorIndex::build(size_t begin, size_t end) {
    size_t dimensions = std::get<std::vector<float>>(entries_[order_[begin]].row[vector_column_]).size();
    Node node; node.begin = begin; node.end = end;
    node.low.assign(dimensions, std::numeric_limits<float>::infinity());
    node.high.assign(dimensions, -std::numeric_limits<float>::infinity());
    for (size_t at = begin; at < end; ++at) {
        const auto& v = std::get<std::vector<float>>(entries_[order_[at]].row[vector_column_]);
        for (size_t d = 0; d < dimensions; ++d) { node.low[d] = std::min(node.low[d], v[d]); node.high[d] = std::max(node.high[d], v[d]); }
    }
    int id = static_cast<int>(nodes_.size()); nodes_.push_back(std::move(node));
    if (end - begin > 32) {
        const auto& box = nodes_[id]; size_t split = 0; double spread = -1;
        for (size_t d = 0; d < dimensions; ++d) {
            double width = double(box.high[d]) - double(box.low[d]);
            if (width > spread) { spread = width; split = d; }
        }
        size_t middle = begin + (end - begin) / 2;
        std::nth_element(order_.begin() + begin, order_.begin() + middle, order_.begin() + end, [&](size_t a, size_t b) {
            float x = std::get<std::vector<float>>(entries_[a].row[vector_column_])[split];
            float y = std::get<std::vector<float>>(entries_[b].row[vector_column_])[split];
            return x == y ? entries_[a].key < entries_[b].key : x < y;
        });
        int left = build(begin, middle), right = build(middle, end);
        nodes_[id].left = left; nodes_[id].right = right;
    }
    return id;
}
std::vector<VectorIndex::Hit> VectorIndex::search(const std::vector<float>& query, size_t k,
                    const std::function<bool(const Row&)>& predicate) const {
    auto better = [&](const Hit& a, const Hit& b) {
        return a.distance == b.distance ? entries_[a.entry].key < entries_[b.entry].key : a.distance < b.distance;
    };
    std::priority_queue<Hit, std::vector<Hit>, decltype(better)> heap(better);
    if (!k) return {};
    auto visit = [&](size_t i) {
        const auto& e = entries_[i];
        if (!predicate(e.row)) return;
        const auto& vector = std::get<std::vector<float>>(e.row[vector_column_]);
        double sum = 0;
        for (size_t d = 0; d < query.size(); ++d) { double diff = double(vector[d]) - double(query[d]); sum += diff * diff; }
        Hit hit{i, std::sqrt(sum)};
        if (heap.size() < k) heap.push(hit);
        else if (better(hit, heap.top())) { heap.pop(); heap.push(hit); }
    };
    auto lower_bound = [&](int node) {
        double sum = 0; const auto& box = nodes_[node];
        for (size_t d = 0; d < query.size(); ++d) {
            double diff = query[d] < box.low[d] ? double(box.low[d]) - double(query[d]) :
                          query[d] > box.high[d] ? double(query[d]) - double(box.high[d]) : 0;
            sum += diff * diff;
        }
        return std::sqrt(sum);
    };
    std::function<void(int, double)> walk = [&](int id, double bound) {
        if (heap.size() == k && bound > heap.top().distance) return;
        const auto& node = nodes_[id];
        if (node.left < 0) { for (size_t i = node.begin; i < node.end; ++i) visit(order_[i]); return; }
        double left = lower_bound(node.left), right = lower_bound(node.right);
        if (left <= right) { walk(node.left, left); walk(node.right, right); }
        else { walk(node.right, right); walk(node.left, left); }
    };
    if (nodes_.empty()) for (size_t i = 0; i < entries_.size(); ++i) visit(i);
    else walk(0, lower_bound(0));
    std::vector<Hit> hits; hits.reserve(heap.size());
    while (!heap.empty()) { hits.push_back(heap.top()); heap.pop(); }
    std::reverse(hits.begin(), hits.end()); return hits;
}
}
