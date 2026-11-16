#include "vector_index.hpp"
#include "exact_batch.hpp"
#include <bit>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <queue>
#include <map>
namespace mvd {
struct VectorIndex::ScalarColumn {
        Type type;
        std::vector<int64_t> integers;
        std::vector<uint32_t> text_ids;
        // Map keys have stable addresses, including short strings. The pointer
        // vector never borrows from a reallocating string vector.
        std::map<std::string, uint32_t> text_lookup;
        std::vector<const std::string*> text_values;
        explicit ScalarColumn(Type value) : type(value) {}
    };

VectorIndex::VectorIndex(uint64_t root, std::string table) : root_(root), table_(std::move(table)) {}
VectorIndex::~VectorIndex() = default;

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
    if (QuantizedScreen::supported(index->dimensions_)) index->mapped_source_ = store.read_mapping();
    if (!wanted.empty() && wanted.size() != table.columns.size()) throw IoError("invalid vector snapshot projection");
    index->columns_ = wanted.empty() ? std::vector<bool>(table.columns.size(), false) : wanted;
    index->columns_[column] = index->columns_[table.primary] = true;
    index->primary_column_ = table.primary;
    size_t scalar_count = 0, scalar_row_bytes = 0;
    for (size_t i = 0; i < index->columns_.size(); ++i) if (i != column && i != table.primary && index->columns_[i]) {
        ++scalar_count; scalar_row_bytes += table.columns[i].type == Type::Integer ? sizeof(int64_t) : sizeof(uint32_t);
    }
    if (table.columns.size() > budget / sizeof(std::unique_ptr<ScalarColumn>)) return nullptr;
    if (table.count > UINT32_MAX || table.count > budget / (sizeof(Entry) + sizeof(size_t) + scalar_row_bytes)) return nullptr;
    const size_t count = static_cast<size_t>(table.count), blocks = (count + batch::width - 1) / batch::width;
    if (!index->mapped_source_ && blocks && index->dimensions_ > budget / (blocks * batch::width * sizeof(float))) return nullptr;
    const size_t coordinate_bytes = blocks * index->dimensions_ * batch::width * sizeof(float);
    if (index->dimensions_ > budget / (2 * sizeof(float))) return nullptr;
    const size_t fixed = sizeof(VectorIndex) + index->table_.capacity() + index->columns_.capacity() / 8
        + index->dimensions_ * 2 * sizeof(float) + 128 + scalar_count * (sizeof(ScalarColumn) + 64);
    const size_t source_bytes = index->mapped_source_ ? count * sizeof(uint64_t) : coordinate_bytes;
    if (source_bytes + count * (sizeof(Entry) + scalar_row_bytes) + table.columns.size() * sizeof(std::unique_ptr<ScalarColumn>) + fixed > budget) return nullptr;
    index->scalar_columns_.resize(table.columns.size());
    if (index->mapped_source_) index->source_offsets_.reserve(count);
    else index->coordinates_.resize(coordinate_bytes / sizeof(float));
    index->entries_.reserve(count);
    size_t used = index->entries_.capacity() * sizeof(Entry) + index->coordinates_.capacity() * sizeof(float)
        + index->source_offsets_.capacity() * sizeof(uint64_t) + index->scalar_columns_.capacity() * sizeof(std::unique_ptr<ScalarColumn>) + fixed;
    for (size_t i = 0; i < table.columns.size(); ++i) if (i != column && i != table.primary && index->columns_[i]) {
        auto cached = std::make_unique<ScalarColumn>(table.columns[i].type);
        if (cached->type == Type::Integer) { cached->integers.reserve(count); used += cached->integers.capacity() * sizeof(int64_t); }
        else { cached->text_ids.reserve(count); used += cached->text_ids.capacity() * sizeof(uint32_t); }
        index->scalar_columns_[i] = std::move(cached);
    }
    if (used > budget) return nullptr;
    auto read_columns = index->columns_; read_columns[table.primary] = false;
    bool complete = true;
    const size_t scalar_columns_size = index->scalar_columns_.size();
    BTree tree(store);
    tree.scan(table.root, [&](int64_t key, Ref ref) {
        Ref source;
        auto materialized = index->mapped_source_
            ? store.read_selected_source_bounded(table, ref, read_columns, budget - std::min(budget, used), source)
            : store.read_selected_bounded(table, ref, read_columns, budget - std::min(budget, used));
        if (!materialized) { complete = false; return false; }
        Row row = std::move(*materialized);
        size_t position = index->entries_.size();
        if (position >= count) throw IoError("inconsistent vector snapshot row count");
        if (index->mapped_source_) {
            if (source.length != uint64_t(index->dimensions_) * sizeof(float) || source.offset > index->mapped_source_->size() || source.length > index->mapped_source_->size() - source.offset) {
                complete = false; return false;
            }
            index->source_offsets_.push_back(source.offset);
        } else {
            const auto& vector = std::get<std::vector<float>>(row[column]);
            for (size_t d = 0; d < index->dimensions_; ++d)
                index->coordinates_[(position / batch::width) * index->dimensions_ * batch::width + d * batch::width + position % batch::width] = vector[d];
        }
        if (index->uniform_ && position) for (size_t d = 0; d < index->dimensions_; ++d)
            if (index->coordinate(position, d) != index->coordinate(0, d)) { index->uniform_ = false; break; }
        for (size_t i = 0; i < scalar_columns_size; ++i) if (index->scalar_columns_[i]) {
            auto& cached = *index->scalar_columns_[i];
            if (cached.type == Type::Integer) cached.integers.push_back(std::get<int64_t>(row[i]));
            else {
                auto& value = std::get<std::string>(row[i]);
                auto found = cached.text_lookup.find(value);
                if (found == cached.text_lookup.end()) {
                    const size_t old_capacity = cached.text_values.capacity();
                    const size_t new_capacity = old_capacity == cached.text_values.size() ? std::min(count, std::max(size_t(1), old_capacity * 2)) : old_capacity;
                    const size_t node_bytes = sizeof(std::pair<const std::string, uint32_t>) + 4 * sizeof(void*) + 64 + value.capacity();
                    const size_t pointer_bytes = (new_capacity - old_capacity) * sizeof(const std::string*);
                    if (node_bytes > budget - used || pointer_bytes > budget - used - node_bytes) { complete = false; return false; }
                    if (new_capacity != old_capacity) {
                        cached.text_values.reserve(new_capacity);
                        used += (cached.text_values.capacity() - old_capacity) * sizeof(const std::string*);
                    }
                    if (used > budget) { complete = false; return false; }
                    if (node_bytes > budget - used) { complete = false; return false; }
                    const uint32_t id = static_cast<uint32_t>(cached.text_values.size());
                    found = cached.text_lookup.emplace(std::move(value), id).first;
                    cached.text_values.push_back(&found->first); used += node_bytes;
                }
                cached.text_ids.push_back(found->second);
            }
        }
        index->entries_.push_back({key, ref}); return true;
    });
    if (!complete) return nullptr;
    if (index->entries_.size() != count) throw IoError("inconsistent vector snapshot row count");
    if (cluster_column && *cluster_column < table.columns.size() && index->columns_[*cluster_column] &&
        table.columns[*cluster_column].type != Type::Vector &&
        uint64_t(count) * sizeof(size_t) + scalar_count * (sizeof(uint64_t) + sizeof(size_t)) + 64 <= budget - used) index->cluster(*cluster_column);
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
    // Follow the successful reference's TypedScanCache::finish layout choice:
    // certificate reranking consumes individual source vectors, so retain
    // contiguous rows when the auxiliary index and preparation scratch fit.
    if (index->screen_ && !index->mapped_source_ && index->screen_->bytes() <= budget - used) {
        const size_t spare = budget - used - index->screen_->bytes();
        if (index->dimensions_ <= spare / (batch::width * sizeof(float))) try {
            std::vector<float> scratch(index->dimensions_ * batch::width);
            if (scratch.capacity() * sizeof(float) <= spare) {
                for (size_t first = 0; first < blocks; ++first) {
                    auto* values = index->coordinates_.data() + first * index->dimensions_ * batch::width;
                    for (size_t lane = 0; lane < batch::width; ++lane)
                        for (size_t d = 0; d < index->dimensions_; ++d) scratch[lane * index->dimensions_ + d] = values[d * batch::width + lane];
                    std::copy(scratch.begin(), scratch.end(), values);
                }
                index->rowwise_ = true;
            }
        } catch (const std::bad_alloc&) {} // Allocation precedes any layout edit.
    }
    index->source_copy_budget_ = budget - used;
    if (index->screen_) index->source_copy_budget_ -= index->screen_->bytes();
    return index;
}
bool VectorIndex::materialize_source() {
    if (!mapped_source_) return true;
    const size_t blocks = (entries_.size() + batch::width - 1) / batch::width;
    if (blocks && dimensions_ > source_copy_budget_ / (blocks * batch::width * sizeof(float))) return false;
    try {
        std::vector<float> prepared(blocks * batch::width * dimensions_);
        if (prepared.capacity() * sizeof(float) > source_copy_budget_) return false;
        for (size_t entry = 0; entry < entries_.size(); ++entry) for (size_t d = 0; d < dimensions_; ++d)
            prepared[(entry / batch::width) * dimensions_ * batch::width + d * batch::width + entry % batch::width] = coordinate(entry, d);
        coordinates_.swap(prepared); rowwise_ = false;
        mapped_source_.reset(); std::vector<uint64_t>().swap(source_offsets_);
        return true;
    } catch (const std::bad_alloc&) { return false; } // Keep the bounded mapped scalar fallback.
}
float VectorIndex::coordinate(size_t entry, size_t dimension) const {
    if (mapped_source_) {
        float value;
        std::memcpy(&value, mapped_source_->data() + source_offsets_[entry] + dimension * sizeof(float), sizeof(value));
        return value;
    }
    if (rowwise_) return coordinates_[entry * dimensions_ + dimension];
    return coordinates_[(entry / batch::width) * dimensions_ * batch::width + dimension * batch::width + entry % batch::width];
}
int VectorIndex::compare(size_t entry, size_t column, const Value& rhs) const {
    if (column >= columns_.size() || !columns_[column] || column == vector_column_) throw IoError("uncached scalar column");
    if (column == primary_column_) {
        const auto* value = std::get_if<int64_t>(&rhs);
        if (!value) fail("type_error", "incompatible comparison operands");
        return (entries_[entry].key > *value) - (entries_[entry].key < *value);
    }
    const auto& cached = *scalar_columns_[column];
    if (cached.type == Type::Integer) {
        const auto* value = std::get_if<int64_t>(&rhs); if (!value) fail("type_error", "incompatible comparison operands");
        return (cached.integers[entry] > *value) - (cached.integers[entry] < *value);
    }
    const auto* value = std::get_if<std::string>(&rhs); if (!value) fail("type_error", "incompatible comparison operands");
    const auto& left = *cached.text_values[cached.text_ids[entry]];
    return (left > *value) - (left < *value);
}
void VectorIndex::append_scalar_json(std::string& output, size_t entry, size_t column) const {
    if (column == primary_column_) append_value_json(output, Value(entries_[entry].key));
    else {
        const auto& cached = *scalar_columns_[column];
        if (cached.type == Type::Integer) append_value_json(output, Value(cached.integers[entry]));
        else output += Json(*cached.text_values[cached.text_ids[entry]]).dump();
    }
}
void VectorIndex::cluster(size_t column) {
    std::vector<size_t> permutation(entries_.size()), scalar_slots;
    scalar_slots.reserve(std::count_if(scalar_columns_.begin(), scalar_columns_.end(), [](const auto& value) { return bool(value); }));
    for (size_t i = 0; i < scalar_columns_.size(); ++i) if (scalar_columns_[i]) scalar_slots.push_back(i);
    std::vector<uint64_t> saved_values(scalar_slots.size());
    std::vector<float> scratch; if (!mapped_source_) scratch.resize(dimensions_);
    std::iota(permutation.begin(), permutation.end(), 0);
    std::sort(permutation.begin(), permutation.end(), [&](size_t a, size_t b) {
        if (column == primary_column_) return entries_[a].key < entries_[b].key;
        const auto& cached = *scalar_columns_[column];
        if (cached.type == Type::Integer) {
            const auto x = cached.integers[a], y = cached.integers[b]; return x == y ? entries_[a].key < entries_[b].key : x < y;
        }
        const auto& x = *cached.text_values[cached.text_ids[a]]; const auto& y = *cached.text_values[cached.text_ids[b]];
        return x == y ? entries_[a].key < entries_[b].key : x < y;
    });
    auto set = [&](size_t entry, size_t d, float value) {
        coordinates_[(entry / batch::width) * dimensions_ * batch::width + d * batch::width + entry % batch::width] = value;
    };
    for (size_t i = 0; i < permutation.size(); ++i) if (permutation[i] != i) {
        Entry saved = entries_[i];
        const uint64_t saved_offset = mapped_source_ ? source_offsets_[i] : 0;
        for (size_t j = 0; j < scalar_slots.size(); ++j) {
            const auto& cached = *scalar_columns_[scalar_slots[j]];
            saved_values[j] = cached.type == Type::Integer ? std::bit_cast<uint64_t>(cached.integers[i]) : cached.text_ids[i];
        }
        if (!mapped_source_) for (size_t d = 0; d < dimensions_; ++d) scratch[d] = coordinate(i, d);
        size_t to = i;
        while (permutation[to] != i) {
            const size_t from = permutation[to]; entries_[to] = entries_[from];
            for (size_t col : scalar_slots) {
                auto& cached = *scalar_columns_[col];
                if (cached.type == Type::Integer) cached.integers[to] = cached.integers[from]; else cached.text_ids[to] = cached.text_ids[from];
            }
            if (mapped_source_) source_offsets_[to] = source_offsets_[from];
            else for (size_t d = 0; d < dimensions_; ++d) set(to, d, coordinate(from, d));
            permutation[to] = to; to = from;
        }
        entries_[to] = saved;
        for (size_t j = 0; j < scalar_slots.size(); ++j) {
            auto& cached = *scalar_columns_[scalar_slots[j]];
            if (cached.type == Type::Integer) cached.integers[to] = std::bit_cast<int64_t>(saved_values[j]); else cached.text_ids[to] = static_cast<uint32_t>(saved_values[j]);
        }
        if (mapped_source_) source_offsets_[to] = saved_offset;
        else for (size_t d = 0; d < dimensions_; ++d) set(to, d, scratch[d]);
        permutation[to] = to;
    }
    clustered_column_ = column;
}
std::optional<std::pair<size_t, size_t>> VectorIndex::equal_bounds(size_t column, const Value& value) const {
    if (clustered_column_ != column) return {};
    size_t lo = 0, hi = entries_.size();
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (compare(mid, column, value) < 0) lo = mid + 1; else hi = mid; }
    const size_t first = lo; hi = entries_.size();
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (compare(mid, column, value) <= 0) lo = mid + 1; else hi = mid; }
    return std::pair<size_t, size_t>{first, lo};
}
Row VectorIndex::materialize(size_t entry) const {
    Row row(columns_.size()); row[primary_column_] = entries_.at(entry).key;
    for (size_t i = 0; i < scalar_columns_.size(); ++i) if (scalar_columns_[i]) {
        const auto& cached = *scalar_columns_[i];
        if (cached.type == Type::Integer) row[i] = cached.integers[entry]; else row[i] = *cached.text_values[cached.text_ids[entry]];
    }
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
                    const std::function<bool(size_t)>& predicate, size_t begin, size_t end,
                    SearchStats* stats, bool screening) {
    if (stats) { *stats = {}; stats->rowwise_source = rowwise_ || bool(mapped_source_); }
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
        if (i < begin || i >= end || (predicate && !predicate(i))) return;
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
        bool mapped_fallback = bool(mapped_source_);
        if (mapped_fallback && materialize_source()) {
            if (stats) stats->source_reblocked = true;
            mapped_fallback = false;
        }
        if (mapped_fallback) {
            for (size_t i = begin; i < end; ++i) visit(i);
        } else {
        const auto kernel = batch::select();
        if (rowwise_) {
            // A complete scan has different locality needs. Reblock once per
            // immutable snapshot, preserving the original full-scan kernel
            // instead of paying per-query packing on an uncertified workload.
            std::vector<float> packed(batch::width * dimensions_);
            for (size_t first = 0; first < coordinates_.size(); first += packed.size()) {
                auto* values = coordinates_.data() + first;
                for (size_t lane = 0; lane < batch::width; ++lane)
                    for (size_t d = 0; d < dimensions_; ++d) packed[d * batch::width + lane] = values[lane * dimensions_ + d];
                std::copy(packed.begin(), packed.end(), values);
            }
            rowwise_ = false; if (stats) stats->source_reblocked = true;
        }
        for (size_t first = begin / batch::width * batch::width; first < end; first += batch::width) {
            unsigned mask = 0;
            if (!predicate && first >= begin && end - first >= batch::width) mask = UINT32_MAX;
            else for (size_t lane = 0; lane < batch::width && first + lane < end; ++lane)
                if (first + lane >= begin && (!predicate || predicate(first + lane))) mask |= 1U << lane;
            if (!mask) continue;
            double distances[batch::width];
            kernel.evaluate(coordinates_.data() + first * dimensions_, query.data(), dimensions_, distances);
            for (size_t lane = 0; lane < batch::width; ++lane) if (mask & (1U << lane)) consider(first + lane, distances[lane]);
        }
        }
    }
    else walk(0, lower_bound(0));
    std::vector<Hit> hits; hits.reserve(heap.size());
    while (!heap.empty()) { hits.push_back(heap.top()); heap.pop(); }
    std::reverse(hits.begin(), hits.end()); return hits;
}
std::optional<std::vector<VectorIndex::Hit>> VectorIndex::screen_search(const std::vector<float>& query, size_t k,
        const std::function<bool(size_t)>& predicate, size_t begin, size_t end, SearchStats* stats) const {
    constexpr size_t capacity = 32;
    struct Estimate { int32_t score; size_t entry; };
    auto less = [](const Estimate& a, const Estimate& b) { return a.score < b.score; };
    std::array<Estimate, capacity> candidates{}; size_t count = 0, selected = 0;
    const auto encoded = screen_->query(query);
    for (size_t first = begin / QuantizedScreen::width * QuantizedScreen::width; first < end; first += QuantizedScreen::width) {
        unsigned mask = 0;
        if (!predicate && first >= begin && end - first >= QuantizedScreen::width) mask = UINT32_MAX;
        else for (size_t lane = 0; lane < QuantizedScreen::width && first + lane < end; ++lane)
            if (first + lane >= begin && (!predicate || predicate(first + lane))) mask |= 1U << lane;
        if (!mask) continue;
        selected += std::popcount(mask);
        if (stats) stats->screened_rows += std::popcount(mask);
        std::array<int32_t, QuantizedScreen::width> estimates{};
        const int32_t block_minimum = screen_->scores(encoded, first, estimates);
        if (count == capacity && block_minimum >= candidates[0].score) continue;
        for (size_t lane = 0; lane < QuantizedScreen::width; ++lane) if (mask & (1U << lane)) {
            const int32_t score = estimates[lane];
            Estimate candidate{score, first + lane};
            if (count < capacity) {
                candidates[count++] = candidate; std::push_heap(candidates.begin(), candidates.begin() + count, less);
            } else if (less(candidate, candidates[0])) {
                // Replace the maximum with one downward sift. Scores and the
                // strict rejection certificate remain unchanged.
                size_t hole = 0;
                for (size_t child = 1; child < capacity; child = hole * 2 + 1) {
                    if (child + 1 < capacity && less(candidates[child], candidates[child + 1])) ++child;
                    if (!less(candidate, candidates[child])) break;
                    candidates[hole] = candidates[child]; hole = child;
                }
                candidates[hole] = candidate;
            }
        }
    }
    auto better = [&](const Hit& a, const Hit& b) {
        return a.distance == b.distance ? entries_[a.entry].key < entries_[b.entry].key : a.distance < b.distance;
    };
    std::sort(candidates.begin(), candidates.begin() + count, less);
    std::vector<Hit> hits; hits.reserve(std::min(count, k));
    auto finish = [&]() -> std::optional<std::vector<Hit>> {
        std::sort(hits.begin(), hits.end(), better); return std::move(hits);
    };
    for (size_t i = 0; i < count; ++i) {
        const size_t entry = candidates[i].entry; double sum = 0;
        if (mapped_source_) {
            const auto* values = mapped_source_->data() + source_offsets_[entry];
            for (size_t d = 0; d < dimensions_; ++d) {
                float value; std::memcpy(&value, values + d * sizeof(float), sizeof(value));
                double delta = double(value) - double(query[d]); sum += delta * delta;
            }
        } else if (rowwise_) {
            const float* values = coordinates_.data() + entry * dimensions_;
            for (size_t d = 0; d < dimensions_; ++d) { double delta = double(values[d]) - double(query[d]); sum += delta * delta; }
        } else for (size_t d = 0; d < dimensions_; ++d) { double delta = double(coordinate(entry, d)) - double(query[d]); sum += delta * delta; }
        Hit hit{entry, std::sqrt(sum)}; if (stats) ++stats->exact_rows;
        if (hits.size() < k) { hits.push_back(hit); std::push_heap(hits.begin(), hits.end(), better); }
        else if (better(hit, hits.front())) {
            std::pop_heap(hits.begin(), hits.end(), better); hits.back() = hit;
            std::push_heap(hits.begin(), hits.end(), better);
        }
        // Sorted selected scores precede every excluded score. Therefore the
        // next selected score bounds BOTH the remaining shortlist and all
        // excluded rows. Stop only when that entire suffix is certified worse.
        const int32_t pending = i + 1 < count ? candidates[i + 1].score : candidates[count - 1].score;
        if (hits.size() == k && screen_->lower(encoded, pending) > screen_->reference_upper(hits.front().distance)) return finish();
    }
    // Strict > protects both exact-distance and rounded-sqrt ties. Any unknown
    // bound, close boundary or unsupported environment retains the full path.
    if (selected <= capacity) return finish();
    return {};
}
}
