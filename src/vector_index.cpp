#include "vector_index.hpp"
#include "exact_batch.hpp"
#include <bit>
#include <cstdlib>
#include <numeric>
#include <queue>
namespace mvd {
static size_t vector_budget() {
    size_t mib = 16;
    if (const char* value = std::getenv("MVD_VECTOR_CACHE_MIB")) {
        try { mib = std::stoull(value); }
        catch (const std::invalid_argument&) { fail("invalid_config", "invalid vector cache size"); }
        catch (const std::out_of_range&) { fail("invalid_config", "invalid vector cache size"); }
        if (mib > 64) fail("invalid_config", "vector cache maximum is 64 MiB");
    }
    return mib * 1024 * 1024;
}
bool VectorIndex::covers(const std::vector<bool>& wanted) const {
    if (wanted.size() != columns_.size()) return false;
    for (size_t i = 0; i < wanted.size(); ++i) if (wanted[i] && !columns_[i]) return false;
    return true;
}
std::unique_ptr<VectorIndex> VectorIndex::create(Storage& store, const Table& table, const std::vector<bool>& wanted,
                                               std::optional<size_t> cluster_column) {
    size_t budget = vector_budget(), column = table.columns.size();
    for (size_t i = 0; i < table.columns.size(); ++i) if (table.columns[i].type == Type::Vector) column = i;
    if (!budget || column == table.columns.size()) return nullptr;
    auto index = std::make_unique<VectorIndex>(table.root, table.name); index->vector_column_ = column;
    index->dimensions_ = table.columns[column].width;
    if (!wanted.empty() && wanted.size() != table.columns.size()) throw IoError("invalid vector snapshot projection");
    index->columns_ = wanted.empty() ? std::vector<bool>(table.columns.size(), false) : wanted;
    index->columns_[column] = index->columns_[table.primary] = true;
    // Reserve by the known row count, but never reserve beyond the entire budget.
    if (table.count > budget / (sizeof(Entry) + sizeof(size_t) + table.columns.size() * sizeof(Value))) return nullptr;
    size_t count = static_cast<size_t>(table.count), blocks = (count + batch::width - 1) / batch::width;
    if (blocks && index->dimensions_ > budget / (blocks * batch::width * sizeof(float))) return nullptr;
    size_t coordinate_bytes = blocks * index->dimensions_ * batch::width * sizeof(float);
    if (index->dimensions_ > budget / (2 * sizeof(float))) return nullptr;
    size_t scratch = index->dimensions_ * 2 * sizeof(float);
    size_t initial_bytes = coordinate_bytes + count * sizeof(Entry) + index->columns_.capacity() / 8 + scratch + 64;
    if (initial_bytes > budget) return nullptr;
    index->coordinates_.resize(coordinate_bytes / sizeof(float));
    index->entries_.reserve(static_cast<size_t>(table.count));
    size_t used = index->entries_.capacity() * sizeof(Entry) + index->coordinates_.capacity() * sizeof(float)
                  + index->columns_.capacity() / 8 + scratch + 64;
    if (used > budget) return nullptr;
    bool complete = true;
    BTree tree(store);
    tree.scan(table.root, [&](int64_t key, Ref ref) {
        auto materialized = store.read_selected_bounded(table, ref, index->columns_, budget - std::min(budget, used));
        if (!materialized) { complete = false; return false; }
        Row row = std::move(*materialized); row[table.primary] = key;
        size_t position = index->entries_.size();
        if (position >= count) throw IoError("inconsistent vector snapshot row count");
        const auto& vector = std::get<std::vector<float>>(row[column]);
        if (index->uniform_ && position) for (size_t d = 0; d < index->dimensions_; ++d)
            if (vector[d] != index->coordinate(0, d)) { index->uniform_ = false; break; }
        for (size_t d = 0; d < index->dimensions_; ++d)
            index->coordinates_[(position / batch::width) * index->dimensions_ * batch::width + d * batch::width + position % batch::width] = vector[d];
        row[column] = int64_t(0); // The float32 payload is owned once by coordinates_.
        used += row.capacity() * sizeof(Value);
        for (const auto& v : row) {
            if (auto s = std::get_if<std::string>(&v)) used += s->capacity() + 32;
            if (auto a = std::get_if<std::vector<float>>(&v)) used += a->capacity() * sizeof(float) + 32;
        }
        if (used > budget) { complete = false; return false; }
        index->entries_.push_back({key, ref, std::move(row)}); return true;
    });
    if (!complete) return nullptr;
    if (index->entries_.size() != count) throw IoError("inconsistent vector snapshot row count");
    if (cluster_column && *cluster_column < table.columns.size() && index->columns_[*cluster_column] &&
        table.columns[*cluster_column].type != Type::Vector &&
        count <= (budget - used) / sizeof(size_t)) index->cluster(*cluster_column);
    if (!table.indexes.empty() && !index->entries_.empty() && index->dimensions_ < 32) {
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
    if (!index->uniform_ && index->dimensions_ >= 32)
        index->screen_ = QuantizedScreen::create(count, index->dimensions_, budget - used,
            [&](size_t row, size_t d) { return index->coordinate(row, d); });
    return index;
}
float VectorIndex::coordinate(size_t entry, size_t dimension) const {
    return coordinates_[(entry / batch::width) * dimensions_ * batch::width + dimension * batch::width + entry % batch::width];
}
void VectorIndex::cluster(size_t column) {
    // One permutation plus one vector of scratch; all allocations precede edits.
    std::vector<size_t> permutation(entries_.size());
    std::vector<float> scratch(dimensions_);
    std::iota(permutation.begin(), permutation.end(), 0);
    std::sort(permutation.begin(), permutation.end(), [&](size_t a, size_t b) {
        const auto& x = entries_[a].row[column]; const auto& y = entries_[b].row[column];
        return x == y ? entries_[a].key < entries_[b].key : x < y;
    });
    auto set = [&](size_t entry, size_t d, float value) {
        coordinates_[(entry / batch::width) * dimensions_ * batch::width + d * batch::width + entry % batch::width] = value;
    };
    for (size_t i = 0; i < permutation.size(); ++i) if (permutation[i] != i) {
        Entry saved = std::move(entries_[i]);
        for (size_t d = 0; d < dimensions_; ++d) scratch[d] = coordinate(i, d);
        size_t to = i;
        while (permutation[to] != i) {
            size_t from = permutation[to]; entries_[to] = std::move(entries_[from]);
            for (size_t d = 0; d < dimensions_; ++d) set(to, d, coordinate(from, d));
            permutation[to] = to; to = from;
        }
        entries_[to] = std::move(saved);
        for (size_t d = 0; d < dimensions_; ++d) set(to, d, scratch[d]);
        permutation[to] = to;
    }
    clustered_column_ = column;
}
std::optional<std::pair<size_t, size_t>> VectorIndex::equal_bounds(size_t column, const Value& value) const {
    if (clustered_column_ != column) return {};
    auto first = std::lower_bound(entries_.begin(), entries_.end(), value,
        [&](const Entry& entry, const Value& v) { return entry.row[column] < v; });
    auto last = std::upper_bound(first, entries_.end(), value,
        [&](const Value& v, const Entry& entry) { return v < entry.row[column]; });
    return std::pair<size_t, size_t>{first - entries_.begin(), last - entries_.begin()};
}
Row VectorIndex::materialize(size_t entry) const {
    Row row = entries_.at(entry).row;
    std::vector<float> vector; vector.reserve(dimensions_);
    for (size_t d = 0; d < dimensions_; ++d) vector.push_back(coordinate(entry, d));
    row[vector_column_] = std::move(vector); return row;
}
int VectorIndex::build(size_t begin, size_t end) {
    size_t dimensions = dimensions_;
    Node node; node.begin = begin; node.end = end;
    node.low.assign(dimensions, std::numeric_limits<float>::infinity());
    node.high.assign(dimensions, -std::numeric_limits<float>::infinity());
    for (size_t at = begin; at < end; ++at) {
        for (size_t d = 0; d < dimensions; ++d) { float value = coordinate(order_[at], d); node.low[d] = std::min(node.low[d], value); node.high[d] = std::max(node.high[d], value); }
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
            float x = coordinate(a, split), y = coordinate(b, split);
            return x == y ? entries_[a].key < entries_[b].key : x < y;
        });
        int left = build(begin, middle), right = build(middle, end);
        nodes_[id].left = left; nodes_[id].right = right;
    }
    return id;
}
std::vector<VectorIndex::Hit> VectorIndex::search(const std::vector<float>& query, size_t k,
                    const std::function<bool(const Row&)>& predicate, size_t begin, size_t end,
                    SearchStats* stats, bool screening) const {
    if (stats) *stats = {};
    auto better = [&](const Hit& a, const Hit& b) {
        return a.distance == b.distance ? entries_[a.entry].key < entries_[b.entry].key : a.distance < b.distance;
    };
    std::priority_queue<Hit, std::vector<Hit>, decltype(better)> heap(better);
    end = std::min(end, entries_.size());
    if (!k || begin >= end) return {};
    if (query.size() != dimensions_) fail("invalid_vector", "incorrect indexed query dimension");
    if (screening && screen_ && k <= 16 && end - begin >= 256 && QuantizedScreen::supported(dimensions_)) {
        if (stats) stats->screen_attempted = true;
        if (auto result = screen_search(query, k, predicate, begin, end, stats)) {
            if (stats) stats->screen_certified = true;
            return *result;
        }
    }
    auto consider = [&](size_t i, double distance) {
        if (stats) ++stats->exact_rows;
        Hit hit{i, distance};
        if (heap.size() < k) heap.push(hit);
        else if (better(hit, heap.top())) { heap.pop(); heap.push(hit); }
    };
    auto visit = [&](size_t i) {
        const auto& e = entries_[i];
        if (i < begin || i >= end || (predicate && !predicate(e.row))) return;
        double sum = 0;
        for (size_t d = 0; d < query.size(); ++d) { double diff = double(coordinate(i, d)) - double(query[d]); sum += diff * diff; }
        consider(i, std::sqrt(sum));
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
    if (nodes_.empty()) {
        const auto kernel = batch::select();
        for (size_t first = begin / batch::width * batch::width; first < end; first += batch::width) {
            unsigned mask = 0;
            if (!predicate && first >= begin && end - first >= batch::width) mask = UINT32_MAX;
            else for (size_t lane = 0; lane < batch::width && first + lane < end; ++lane)
                if (first + lane >= begin && (!predicate || predicate(entries_[first + lane].row))) mask |= 1U << lane;
            if (!mask) continue;
            double distances[batch::width];
            kernel.evaluate(coordinates_.data() + (first / batch::width) * dimensions_ * batch::width, query.data(), dimensions_, distances);
            for (size_t lane = 0; lane < batch::width; ++lane) if (mask & (1U << lane)) consider(first + lane, distances[lane]);
        }
    }
    else walk(0, lower_bound(0));
    std::vector<Hit> hits; hits.reserve(heap.size());
    while (!heap.empty()) { hits.push_back(heap.top()); heap.pop(); }
    std::reverse(hits.begin(), hits.end()); return hits;
}
std::optional<std::vector<VectorIndex::Hit>> VectorIndex::screen_search(const std::vector<float>& query, size_t k,
        const std::function<bool(const Row&)>& predicate, size_t begin, size_t end, SearchStats* stats) const {
    constexpr size_t capacity = 64;
    struct Estimate { int32_t score; size_t entry; };
    auto less = [](const Estimate& a, const Estimate& b) { return a.score < b.score; };
    std::array<Estimate, capacity> candidates{}; size_t count = 0, selected = 0;
    const auto encoded = screen_->query(query);
    for (size_t first = begin / QuantizedScreen::width * QuantizedScreen::width; first < end; first += QuantizedScreen::width) {
        unsigned mask = 0;
        if (!predicate && first >= begin && end - first >= QuantizedScreen::width) mask = (1U << QuantizedScreen::width) - 1;
        else for (size_t lane = 0; lane < QuantizedScreen::width && first + lane < end; ++lane)
            if (first + lane >= begin && (!predicate || predicate(entries_[first + lane].row))) mask |= 1U << lane;
        if (!mask) continue;
        selected += std::popcount(mask);
        if (stats) stats->screened_rows += std::popcount(mask);
        std::array<int32_t, QuantizedScreen::width> estimates{};
        screen_->scores(encoded, first, estimates);
        if (count == capacity && *std::min_element(estimates.begin(), estimates.end()) >= candidates[0].score) continue;
        for (size_t lane = 0; lane < QuantizedScreen::width; ++lane) if (mask & (1U << lane)) {
            const int32_t score = estimates[lane];
            Estimate candidate{score, first + lane};
            if (count < capacity) {
                candidates[count++] = candidate; std::push_heap(candidates.begin(), candidates.begin() + count, less);
            } else if (less(candidate, candidates[0])) {
                std::pop_heap(candidates.begin(), candidates.end(), less); candidates.back() = candidate;
                std::push_heap(candidates.begin(), candidates.end(), less);
            }
        }
    }
    auto better = [&](const Hit& a, const Hit& b) {
        return a.distance == b.distance ? entries_[a.entry].key < entries_[b.entry].key : a.distance < b.distance;
    };
    std::vector<Hit> hits; hits.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t entry = candidates[i].entry; double sum = 0;
        for (size_t d = 0; d < dimensions_; ++d) { double delta = double(coordinate(entry, d)) - double(query[d]); sum += delta * delta; }
        hits.push_back({entry, std::sqrt(sum)}); if (stats) ++stats->exact_rows;
    }
    std::sort(hits.begin(), hits.end(), better);
    if (hits.size() > k) hits.resize(k);
    // Strict > protects both exact-distance and rounded-sqrt ties. Any unknown
    // bound, close boundary or unsupported environment retains the full path.
    // The capacity-th estimate is no greater than every excluded estimate,
    // since the heap threshold only decreases while scanning.
    if (selected <= capacity || (hits.size() == k && screen_->lower(encoded, candidates[0].score) > screen_->reference_upper(hits.back().distance))) return hits;
    return {};
}
}
