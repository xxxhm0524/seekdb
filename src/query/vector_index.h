// Self-contained HNSW graph and bounded compact attribute/vector snapshot.
// Algorithm reference: Malkov & Yashunin, https://arxiv.org/abs/1603.09320.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>
#include "../model/value.h"

namespace mvd {
class VectorIndex {
 public:
  using Distance = double (*)(const float *, const float *, int);
  using DistanceScreen = bool (*)(const float *, const float *, int, float);
  struct Node {
    int64_t pk;
    uint64_t pos;
    bool active = true;
    std::vector<std::vector<uint32_t>> links;
  };
  struct Hit { double distance; uint32_t node; };
  struct Column {
    bool enabled = false;
    ColumnType type = ColumnType::BIGINT;
    uint32_t offset = 0;
    std::vector<int64_t> integers;
    std::vector<std::string> strings;
  };
  VectorIndex(int dimension, uint32_t vector_offset,
      const std::vector<ColumnDef> &definitions, const std::vector<uint32_t> &offsets,
      Distance distance, size_t expected, size_t minimum_rows, bool allow_graph = true,
      DistanceScreen build_screen = nullptr)
      : dim_(dimension), vector_offset_(vector_offset), distance_(distance),
        graph_(allow_graph && dimension >= 32 && expected >= minimum_rows),
        minimum_rows_(minimum_rows), allow_graph_(allow_graph) {
    if (graph_ && dim_ >= 64 && dim_ <= 16384) build_screen_ = build_screen;
    columns_.resize(definitions.size());
    bytes_per_node_ = static_cast<size_t>(dim_) * 4 + (graph_ ? 384 : 96) + 8;
    for (size_t i = 0; i < definitions.size(); ++i) {
      auto &column = columns_[i];
      column.type = definitions[i].type;
      column.offset = offsets[i];
      column.enabled = column.type == ColumnType::BIGINT ||
          (column.type == ColumnType::VARCHAR && definitions[i].max_chars <= 64);
      if (column.enabled) {
        if (column.type == ColumnType::BIGINT) {
          bytes_per_node_ += 8;
        } else {
          bytes_per_node_ += 40 + static_cast<size_t>(definitions[i].max_chars) * 4;
        }
      }
    }
  }
  void reserve(size_t expected) {
    nodes_.reserve(expected); vectors_.reserve(expected*static_cast<size_t>(dim_));
    visited_.reserve(expected); ids_.reserve(expected);
    for (auto &column:columns_) if (column.enabled) {
      if (column.type==ColumnType::BIGINT) column.integers.reserve(expected);
      else column.strings.reserve(expected);
    }
  }
  size_t estimate(size_t nodes) const {
    return nodes * bytes_per_node_ * 2 + columns_.capacity()*sizeof(Column) + 8192;
  }
  size_t count() const { return nodes_.size(); }
  size_t live() const { return ids_.size(); }
  bool needs_compaction() const { return count() > live() + std::max<size_t>(256, live()/3) ||
      (!graph_ && allow_graph_ && dim_>=32 && live()>=minimum_rows_); }
  const Node &node(uint32_t id) const { return nodes_[id]; }
  const float *vector(uint32_t id) const { return vectors_.data() + static_cast<size_t>(id) * dim_; }
  bool find(int64_t pk,uint32_t &id) const {
    auto found=ids_.find(pk);
    if (found==ids_.end()) return false;
    id=found->second;return true;
  }
  int64_t integer(uint32_t id,int column) const {return columns_[column].integers[id];}
  const std::string &string(uint32_t id,int column) const {return columns_[column].strings[id];}
  bool relevant_column(int column) const {
    return column >= 0 && static_cast<size_t>(column) < columns_.size() && columns_[column].enabled;
  }
  bool supports(const std::vector<BoundCondition> &where) const {
    for (const auto &condition : where) if (!relevant_column(condition.resolved_column)) return false;
    return true;
  }
  bool matches(uint32_t id, const std::vector<BoundCondition> &where) const {
    if (!nodes_[id].active) return false;
    for (const auto &condition : where) {
      const auto &column = columns_[condition.resolved_column];
      int comparison;
      if (column.type == ColumnType::BIGINT) {
        int64_t a = column.integers[id], b = condition.typed_value.i;
        comparison = a < b ? -1 : a > b ? 1 : 0;
      } else comparison = column.strings[id].compare(condition.typed_value.s);
      bool pass = false;
      switch (condition.op) {
        case CompareOp::EQ: pass = comparison == 0; break;
        case CompareOp::NE: pass = comparison != 0; break;
        case CompareOp::LT: pass = comparison < 0; break;
        case CompareOp::LE: pass = comparison <= 0; break;
        case CompareOp::GT: pass = comparison > 0; break;
        case CompareOp::GE: pass = comparison >= 0; break;
      }
      if (!pass) return false;
    }
    return true;
  }
  const std::vector<uint32_t> *matching_nodes(const std::vector<BoundCondition> &where) {
    if (where.empty() || nodes_.empty()) return nullptr;
    // Charge a bounded predicate cache to the index budget. Only row IDs are
    // cached: distances and TopK are still evaluated for every query.
    size_t budget = std::min<size_t>(4*1024*1024, nodes_.size()*8);
    try {
      std::string key;
      for (const auto &condition : where) {
        const auto &column = columns_[condition.resolved_column];
        size_t extra = column.type == ColumnType::BIGINT ? 8 : 4+condition.typed_value.s.size();
        if (extra > 4096 || key.size()+5+extra > 4096) return nullptr;
        int32_t resolved = condition.resolved_column;
        key.append(reinterpret_cast<const char *>(&resolved), sizeof(resolved));
        key.push_back(static_cast<char>(condition.op));
        if (column.type == ColumnType::BIGINT) {
          key.append(reinterpret_cast<const char *>(&condition.typed_value.i), 8);
        } else {
          uint32_t length = static_cast<uint32_t>(condition.typed_value.s.size());
          key.append(reinterpret_cast<const char *>(&length), sizeof(length));
          key.append(condition.typed_value.s);
        }
      }
      for (auto &entry : filter_cache_) if (entry.key == key) {
        entry.age = ++filter_age_;
        return &entry.ids;
      }
      FilterEntry entry;
      entry.key = std::move(key);
      entry.ids.reserve(std::min<size_t>(64, nodes_.size()));
      auto charge = [&] { return sizeof(FilterEntry)+entry.key.capacity()+1+entry.ids.capacity()*4; };
      if (charge() > budget) return nullptr;
      for (uint32_t id = 0; id < nodes_.size(); ++id) if (matches(id, where)) {
        entry.ids.push_back(id);
        if (charge() > budget) return nullptr;
      }
      entry.charge = charge();
      while (!filter_cache_.empty() && (filter_cache_.size() >= 64 || filter_bytes_+entry.charge > budget)) {
        auto oldest = std::min_element(filter_cache_.begin(), filter_cache_.end(),
            [](const auto &a, const auto &b) { return a.age < b.age; });
        filter_bytes_ -= oldest->charge;
        filter_cache_.erase(oldest);
      }
      if (filter_cache_.capacity() == 0) filter_cache_.reserve(64);
      entry.age = ++filter_age_;
      filter_bytes_ += entry.charge;
      filter_cache_.push_back(std::move(entry));
      return &filter_cache_.back().ids;
    } catch (const std::bad_alloc &) {
      clear_filter_cache();
      return nullptr;
    }
  }
  void erase(int64_t pk) {
    auto found = ids_.find(pk);
    if (found != ids_.end()) {
      clear_filter_cache();
      has_tombstones_ = true;
      nodes_[found->second].active = false; ids_.erase(found);
    }
  }
  void upsert(int64_t pk, uint64_t pos, const uint8_t *row) {
    clear_filter_cache();
    const float *input = reinterpret_cast<const float *>(row + vector_offset_);
    auto found = ids_.find(pk);
    if (found != ids_.end() && std::equal(input, input + dim_, vector(found->second))) {
      nodes_[found->second].pos = pos;
      attributes(found->second, row, false);
      return;
    }
    erase(pk);
    uint32_t id = static_cast<uint32_t>(nodes_.size());
    int level = graph_ ? random_level() : 0;
    nodes_.push_back(Node{pk, pos, true, {}});
    if (graph_) nodes_.back().links.resize(static_cast<size_t>(level + 1));
    if (build_screen_) {
      for (int i = 0; i < dim_; ++i) {
        float magnitude = std::fabs(input[i]);
        if (magnitude != 0 && !(magnitude >= 0x1p-32f && magnitude <= 0x1p48f)) {
          build_screen_ = nullptr;
          break;
        }
      }
    }
    vectors_.insert(vectors_.end(), input, input + dim_);
    visited_.push_back(0);
    attributes(id, row, true);
    bool first = ids_.empty();
    ids_[pk] = id;
    if (!graph_) return;
    if (first) { entry_ = id; max_level_ = level; return; }
    uint32_t entry = entry_;
    for (int layer = max_level_; layer > level; --layer) entry = greedy(input, entry, layer);
    for (int layer = std::min(level, max_level_); layer >= 0; --layer) {
      auto candidates = layer_search<true>(input, entry, 256, layer);
      auto selected = select(candidates, 32, id);
      nodes_[id].links[layer] = selected;
      for (uint32_t neighbor : selected) {
        auto &links = nodes_[neighbor].links[layer];
        links.push_back(id);
        size_t maximum = layer == 0 ? 64 : 32;
        // Re-pruning on every incoming edge repeats the expensive diversity
        // calculation. A bounded slack window amortizes that work.
        if (links.size() > maximum + (layer == 0 ? 16 : 8)) {
          std::vector<Hit> choices;
          choices.reserve(links.size());
          for (uint32_t other : links) choices.push_back({distance_(vector(neighbor), vector(other), dim_), other});
          std::sort(choices.begin(), choices.end(),
                    [this](const Hit &a, const Hit &b) { return less(a, b); });
          links = select(choices, maximum, neighbor);
        }
      }
      if (!candidates.empty()) entry = candidates.front().node;
    }
    if (level > max_level_) { entry_ = id; max_level_ = level; }
  }
  bool approximate_available(size_t k) const {
    return graph_ && live() >= minimum_rows_ && k <= 64;
  }
  std::vector<Hit> search(const float *query, size_t k) {
    if (ids_.empty()) return {};
    uint32_t entry = entry_;
    for (int layer = max_level_; layer > 0; --layer) entry = greedy(query, entry, layer);
    const size_t dimension_width = std::min<size_t>(1024, static_cast<size_t>(dim_) * 2);
    const size_t width = std::max({size_t(384), k * 8, dimension_width});
    // The SQL layer needs one extra live hit to check the TopK boundary.
    return layer_search(query, entry, width, 0, k + 1);
  }
 private:
  struct FilterEntry {
    std::string key;
    std::vector<uint32_t> ids;
    uint64_t age = 0;
    size_t charge = 0;
  };
  void clear_filter_cache() {
    filter_cache_.clear();
    filter_bytes_ = 0;
  }
  bool less(const Hit &a, const Hit &b) const {
    if (a.distance != b.distance) return a.distance < b.distance;
    return nodes_[a.node].pk < nodes_[b.node].pk;
  }
  int random_level() {
    random_ ^= random_ << 13; random_ ^= random_ >> 7; random_ ^= random_ << 17;
    double uniform = (static_cast<double>(random_ >> 11) + 1.0) / 9007199254740993.0;
    return std::min(12, static_cast<int>(-std::log(uniform) / std::log(32.0)));
  }
  uint32_t greedy(const float *query, uint32_t entry, int layer) const {
    Hit best{distance_(query, vector(entry), dim_), entry};
    bool changed;
    do {
      changed = false;
      if (static_cast<size_t>(layer) >= nodes_[best.node].links.size()) break;
      for (uint32_t id : nodes_[best.node].links[layer]) {
        Hit candidate{distance_(query, vector(id), dim_), id};
        if (less(candidate, best)) { best = candidate; changed = true; }
      }
    } while (changed);
    return best.node;
  }
  template <bool Build = false>
  std::vector<Hit> layer_search(const float *query, uint32_t entry, size_t ef, int layer, size_t result_limit = 0) {
    auto &visited = visited_;
    auto &stamp = stamp_;
    if (++stamp == 0) { std::fill(visited.begin(), visited.end(), 0); ++stamp; }
    auto minimum = [this](const Hit &a, const Hit &b) { return less(b, a); };
    auto maximum = [this](const Hit &a, const Hit &b) { return less(a, b); };
    std::vector<Hit> pending;
    pending.reserve(std::min(ef * 2, nodes_.size()));
    std::priority_queue<Hit, std::vector<Hit>, decltype(minimum)> candidates(minimum, std::move(pending));
    std::vector<Hit> best;
    best.reserve(std::min(ef + 1, nodes_.size()));
    Hit start{distance_(query, vector(entry), dim_), entry};
    candidates.push(start); best.push_back(start); visited[entry] = stamp;
    const DistanceScreen screen = Build ? build_screen_ : nullptr;
    float rejection_limit = std::numeric_limits<float>::infinity();
    // In-range nonzero coordinates are multiples of 2^-55. Nonzero
    // differences and squares stay normal in float32, and the sum fits for
    // dim <= 16384. Each sum path has at most dim+8 roundings including
    // subtraction and squaring. Four float32 roundoff units per operation
    // cover both evaluations and rounding the relaxed cutoff back to float.
    const double margin = 1 + (dim_ + 8) * 0x1p-22;
    while (!candidates.empty()) {
      Hit current = candidates.top(); candidates.pop();
      if (best.size() >= ef && current.distance > best.front().distance) break;
      if (static_cast<size_t>(layer) >= nodes_[current.node].links.size()) continue;
      const auto &links = nodes_[current.node].links[layer];
      for (size_t edge = 0; edge < links.size(); ++edge) {
#if defined(__GNUC__) || defined(__clang__)
        // These unvisited neighbors will be scored later in this same loop.
        if (edge + 4 < links.size()) {
          uint32_t ahead = links[edge + 4];
          if (visited[ahead] != stamp) {
            __builtin_prefetch(vector(ahead), 0, 1);
            __builtin_prefetch(vector(ahead) + dim_ / 2, 0, 1);
          }
        }
#endif
        uint32_t next = links[edge];
        if (visited[next] == stamp) continue;
        visited[next] = stamp;
        if (screen && best.size() == ef && screen(query, vector(next), dim_, rejection_limit))
          continue;
        Hit candidate{distance_(query, vector(next), dim_), next};
        if (best.size() < ef || less(candidate, best.front())) {
          candidates.push(candidate);
          // A deleted node can tie its replacement on both distance and PK.
          // Keep the original push/pop sequence whenever such ties are possible.
          if (best.size() < ef || has_tombstones_) {
            best.push_back(candidate);
            std::push_heap(best.begin(), best.end(), maximum);
            if (best.size() > ef) {
              std::pop_heap(best.begin(), best.end(), maximum);
              best.pop_back();
            }
          } else {
            size_t hole = 0;
            for (size_t child = 1; child < best.size(); child = hole * 2 + 1) {
              if (child + 1 < best.size() && less(best[child], best[child + 1])) ++child;
              if (!less(candidate, best[child])) break;
              best[hole] = best[child];
              hole = child;
            }
            best[hole] = candidate;
          }
          if (screen && best.size() == ef)
            rejection_limit = static_cast<float>(best.front().distance * margin);
        }
      }
    }
    if (result_limit != 0) {
      if (has_tombstones_) {
        best.erase(std::remove_if(best.begin(), best.end(),
            [&](const Hit &hit) { return !nodes_[hit.node].active; }), best.end());
      }
      const auto end = best.begin() + std::min(result_limit, best.size());
      std::partial_sort(best.begin(), end, best.end(), maximum);
      best.erase(end, best.end());
      return best;
    }
    if (!has_tombstones_) {
      std::sort_heap(best.begin(), best.end(), maximum);
      return best;
    }
    std::vector<Hit> result;
    result.reserve(best.size());
    while (!best.empty()) {
      result.push_back(best.front());
      std::pop_heap(best.begin(), best.end(), maximum);
      best.pop_back();
    }
    std::sort(result.begin(), result.end(), maximum);
    return result;
  }
  std::vector<uint32_t> select(const std::vector<Hit> &candidates,
                               size_t maximum, uint32_t self) const {
    std::vector<uint32_t> result;
    result.reserve(maximum);
    for (const auto &candidate : candidates) {
      if (candidate.node == self || !nodes_[candidate.node].active) continue;
      bool diverse = true;
      for (uint32_t selected : result)
        if (distance_(vector(candidate.node), vector(selected), dim_) < candidate.distance) { diverse = false; break; }
      if (diverse) result.push_back(candidate.node);
      if (result.size() == maximum) break;
    }
    for (const auto &candidate : candidates) {
      if (result.size() == maximum) break;
      if (candidate.node != self && nodes_[candidate.node].active &&
          std::find(result.begin(), result.end(), candidate.node) == result.end()) result.push_back(candidate.node);
    }
    return result;
  }
  void attributes(uint32_t id, const uint8_t *row, bool append) {
    for (auto &column : columns_) {
      if (!column.enabled) continue;
      if (column.type == ColumnType::BIGINT) {
        int64_t value; std::memcpy(&value, row + column.offset, 8);
        if (append) column.integers.push_back(value); else column.integers[id] = value;
      } else {
        uint32_t size; std::memcpy(&size, row + column.offset, 4);
        std::string value(reinterpret_cast<const char *>(row + column.offset + 4), size);
        if (append) column.strings.push_back(std::move(value)); else column.strings[id] = std::move(value);
      }
    }
  }
  int dim_;
  uint32_t vector_offset_;
  Distance distance_;
  bool graph_;
  size_t minimum_rows_;
  bool allow_graph_;
  bool has_tombstones_ = false;
  DistanceScreen build_screen_ = nullptr;
  size_t bytes_per_node_ = 0;
  std::vector<Node> nodes_;
  std::vector<float> vectors_;
  std::vector<Column> columns_;
  std::unordered_map<int64_t,uint32_t> ids_;
  std::vector<uint32_t> visited_;
  uint32_t stamp_ = 0, entry_ = 0;
  int max_level_ = -1;
  uint64_t random_ = 0x9E3779B97F4A7C15ULL;
  std::vector<FilterEntry> filter_cache_;
  size_t filter_bytes_ = 0;
  uint64_t filter_age_ = 0;
};
}  // namespace mvd
