// Self-contained HNSW graph and bounded compact attribute/vector snapshot.
// Algorithm reference: Malkov & Yashunin, https://arxiv.org/abs/1603.09320.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>
#include "sql.h"

namespace mvd {
class VectorIndex {
 public:
  using Distance = double (*)(const float *, const float *, int);
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
      Distance distance, size_t expected, size_t minimum_rows, bool allow_graph = true)
      : dim_(dimension), vector_offset_(vector_offset), distance_(distance),
        graph_(allow_graph && dimension >= 32 && expected >= minimum_rows),
        minimum_rows_(minimum_rows), allow_graph_(allow_graph) {
    columns_.resize(definitions.size());
    bytes_per_node_ = static_cast<size_t>(dim_) * 4 + (graph_ ? 384 : 96);
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
  size_t estimate(size_t nodes) const { return nodes * bytes_per_node_ * 2 + 1024; }
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
  bool supports(const std::vector<Condition> &where) const {
    for (const auto &condition : where) if (!relevant_column(condition.resolved_column)) return false;
    return true;
  }
  bool matches(uint32_t id, const std::vector<Condition> &where) const {
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
  void erase(int64_t pk) {
    auto found = ids_.find(pk);
    if (found != ids_.end()) { nodes_[found->second].active = false; ids_.erase(found); }
  }
  void upsert(int64_t pk, uint64_t pos, const uint8_t *row) {
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
      auto candidates = layer_search(input, entry, 256, layer);
      auto selected = select(input, candidates, 32, id);
      nodes_[id].links[layer] = selected;
      for (uint32_t neighbor : selected) {
        auto &links = nodes_[neighbor].links[layer];
        links.push_back(id);
        size_t maximum = layer == 0 ? 64 : 32;
        if (links.size() > maximum) {
          std::vector<Hit> choices;
          for (uint32_t other : links) choices.push_back({distance_(vector(neighbor), vector(other), dim_), other});
          links = select(vector(neighbor), choices, maximum, neighbor);
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
    return layer_search(query, entry, std::max<size_t>(384, k * 8), 0);
  }
 private:
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
  std::vector<Hit> layer_search(const float *query, uint32_t entry, size_t ef, int layer) {
    auto &visited = visited_;
    auto &stamp = stamp_;
    if (++stamp == 0) { std::fill(visited.begin(), visited.end(), 0); ++stamp; }
    auto minimum = [this](const Hit &a, const Hit &b) { return less(b, a); };
    auto maximum = [this](const Hit &a, const Hit &b) { return less(a, b); };
    std::priority_queue<Hit, std::vector<Hit>, decltype(minimum)> candidates(minimum);
    std::priority_queue<Hit, std::vector<Hit>, decltype(maximum)> best(maximum);
    Hit start{distance_(query, vector(entry), dim_), entry};
    candidates.push(start); best.push(start); visited[entry] = stamp;
    while (!candidates.empty()) {
      Hit current = candidates.top(); candidates.pop();
      if (best.size() >= ef && current.distance > best.top().distance) break;
      if (static_cast<size_t>(layer) >= nodes_[current.node].links.size()) continue;
      for (uint32_t next : nodes_[current.node].links[layer]) {
        if (visited[next] == stamp) continue;
        visited[next] = stamp;
        Hit candidate{distance_(query, vector(next), dim_), next};
        if (best.size() < ef || less(candidate, best.top())) {
          candidates.push(candidate); best.push(candidate);
          if (best.size() > ef) best.pop();
        }
      }
    }
    std::vector<Hit> result;
    while (!best.empty()) { result.push_back(best.top()); best.pop(); }
    std::sort(result.begin(), result.end(), maximum);
    return result;
  }
  std::vector<uint32_t> select(const float *query, std::vector<Hit> candidates,
                               size_t maximum, uint32_t self) const {
    std::sort(candidates.begin(), candidates.end(), [this](const Hit &a,const Hit &b){return less(a,b);});
    std::vector<uint32_t> result;
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
    (void)query;
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
  size_t bytes_per_node_ = 0;
  std::vector<Node> nodes_;
  std::vector<float> vectors_;
  std::vector<Column> columns_;
  std::unordered_map<int64_t,uint32_t> ids_;
  std::vector<uint32_t> visited_;
  uint32_t stamp_ = 0, entry_ = 0;
  int max_level_ = -1;
  uint64_t random_ = 0x9E3779B97F4A7C15ULL;
};
}  // namespace mvd
