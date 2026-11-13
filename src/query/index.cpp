#include "../runtime/database.h"
#include "../runtime/shared.h"
#include "../query/vector_index.h"
#include "../query/distance.h"
#include <algorithm>
namespace mvd::detail {
VectorIndex* Database::index_handle(uint32_t id)const{auto entry=indexes_.find(id);return entry==indexes_.end()?nullptr:entry->second.index.get();}
bool Database::index_unavailable(uint32_t id)const{auto entry=indexes_.find(id);return entry!=indexes_.end()&&entry->second.unavailable;}
void Database::drop_vector_index(Table& table){auto entry=indexes_.find(table.id());if(entry==indexes_.end())return;vector_search_bytes_-=entry->second.bytes;entry->second.bytes=0;entry->second.index.reset();}

void Database::rebuild_vector_index(Table &table) {
  drop_vector_index(table);
  const auto &schema = table.schema();
  if (!schema.vector_index_created || schema.vector_index < 0 || tx_.active()) return;
  auto& derived=indexes_.try_emplace(table.id()).first->second;derived.unavailable=false;
  try {
    // Graph edges use the same double distances as exact scans. In-range
    // vectors can also use a conservative rejection test during construction.
    auto index = std::make_unique<VectorIndex>(schema.columns[schema.vector_index].dim,
        schema.col_offsets[schema.vector_index], schema.columns, schema.col_offsets,
        squared_l2_checked, table.row_count(), ann_min_rows_, true, graph_build_screen());
    size_t charge = index->estimate(table.row_count());
    if (charge > kVectorIndexBudget - vector_search_bytes_) {
      index = std::make_unique<VectorIndex>(schema.columns[schema.vector_index].dim,
          schema.col_offsets[schema.vector_index], schema.columns, schema.col_offsets,
          squared_l2_checked, table.row_count(), ann_min_rows_, false);
      charge=index->estimate(table.row_count());
    }
    if (charge > kVectorIndexBudget - vector_search_bytes_) {
      derived.unavailable = true;
      return;
    }
    index->reserve(table.row_count());
    for_each_visible(table, {}, [&](const VisibleRow &row) {
      index->upsert(row.pk, row.pos, row.row);
      return true;
    });
    derived.bytes = charge;
    vector_search_bytes_ += charge;
    derived.index = std::move(index);
  } catch (const std::bad_alloc &) {
    derived.unavailable = true;
  }
}

void Database::update_vector_index(Table &table, const TxTable &changes) {
  auto entry=indexes_.find(table.id());if(entry==indexes_.end())return;
  auto& derived=entry->second;auto* index=derived.index.get();
  if(!index)return;
  if (changes.rows.size()>4096) {
    bool rebuild=false;
    for (const auto &entry:changes.rows) {
      const auto &state=entry.second;
      rebuild=rebuild||state.deleted||state.has_raw_row();
      for (const auto &patch:state.patches) for (const auto &change:*patch)
        rebuild=rebuild||change.first==table.schema().vector_index||index->relevant_column(change.first);
      if (rebuild) break;
    }
    if (rebuild) {
      drop_vector_index(table);
      derived.unavailable=false;
      return;
    }
  }
  try {
    for (const auto &entry : changes.rows) {
      const RowState &state = entry.second;
      if (state.deleted) { index->erase(entry.first); continue; }
      bool relevant = state.has_raw_row();
      for (const auto &patch : state.patches)
        for (const auto &change : *patch)
          relevant = relevant || change.first == table.schema().vector_index || index->relevant_column(change.first);
      if (!relevant) continue;
      size_t charge = index->estimate(index->count() + 1);
      if (charge > kVectorIndexBudget - vector_search_bytes_ + derived.bytes) {
        drop_vector_index(table);
        derived.unavailable = true;
        return;
      }
      std::vector<uint8_t> row;
      if (!table.read_row(state.pos, row)) throw_error("CORRUPTION", "committed index row missing");
      index->upsert(entry.first, state.pos, row.data());
      vector_search_bytes_ = vector_search_bytes_ - derived.bytes + charge;
      derived.bytes = charge;
    }
    if (index->needs_compaction()) {
      drop_vector_index(table);
      derived.unavailable = false;  // Rebuild after the transaction has cleared.
    }
  } catch (const std::bad_alloc &) {
    drop_vector_index(table);
    derived.unavailable = true;
  }
}

} // namespace mvd::detail
