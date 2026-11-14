#include "../runtime/database.h"
#include "../runtime/shared.h"
#include <algorithm>
namespace mvd::detail {
void Database::for_each_visible(
    Table &table, const std::vector<BoundCondition> &where,
    const std::function<bool(const VisibleRow &)> &visit) {
  const TableSchema &schema = table.schema();
  const TxTable *tx_table = tx_.find_table(table.id());
  std::vector<uint8_t> materialized;
  for (const auto &condition : where) {
    if (condition.resolved_column != schema.pk_index || condition.op != CompareOp::EQ) continue;
    int64_t pk = condition.typed_value.i;
    uint64_t pos = 0;
    bool overlay = false;
    if (tx_table) {
      auto entry = tx_table->rows.find(pk);
      if (entry != tx_table->rows.end()) {
        if (entry->second.deleted) return;
        table.materialize_state(entry->second, nullptr, materialized);
        pos = entry->second.pos;
        overlay = true;
      }
    }
    if (!overlay && (!table.find_pk(pk, pos) || !table.read_row(pos, materialized))) return;
    if (row_matches(table, materialized.data(), where)) {
      VisibleRow row;
      row.row = materialized.data(); row.pk = pk; row.pos = pos; row.from_overlay = overlay;
      visit(row);
    }
    return;
  }
  std::vector<uint8_t> wide_row;
  const bool wide=schema.pages_per_row>1;
  for (uint32_t p = 0; p < table.page_count(); p+=schema.pages_per_row) {
    std::shared_ptr<Page> page;
    uint32_t n=1;
    if(!wide){
      page=table.get_page(p);
      if(page->magic!=kPageMagic)continue;
      n=std::min(page->nrows,page->capacity(schema.row_size));
    }
    for (uint32_t slot = 0; slot < n; ++slot) {
      const uint64_t pos=wide?p/schema.pages_per_row:static_cast<uint64_t>(p)*schema.rows_per_page+slot;
      const uint8_t *row;
      if(wide){if(!table.read_row(pos,wide_row))continue;row=wide_row.data();}
      else row=table.row_data(*page,slot);
      if (!table.row_is_valid(row)) continue;
      int64_t pk = table.row_id(row);
      const uint8_t *use = row;
      bool overlay = false;
      if (tx_table) {
        auto it = tx_table->rows.find(pk);
        if (it != tx_table->rows.end()) {
          if (it->second.deleted) continue;
          table.materialize_state(it->second, row, materialized);
          use = materialized.data();
          overlay = true;
        }
      }
      bool match = true;
      for (const auto &cond : where) {
        if (!table.compare(use, cond)) {
          match = false;
          break;
        }
      }
      if (!match) continue;
      VisibleRow vr;
      vr.row = use;
      vr.page = overlay ? nullptr : page;
      vr.pk = pk;
      vr.pos = pos;
      vr.from_overlay = overlay;
      if (!visit(vr)) return;
    }
  }
  if (tx_table) {
    std::vector<std::pair<int64_t, const RowState *>> inserted;
    for (const auto &entry : tx_table->rows)
      if (!entry.second.deleted && !entry.second.existed) inserted.emplace_back(entry.first, &entry.second);
    if (tx_.spilled()) std::sort(inserted.begin(), inserted.end(), [](const auto &a, const auto &b) {
      return a.second->pos < b.second->pos;
    });
    for (const auto &entry : inserted) {
      const RowState &state = *entry.second;
      if (state.deleted || state.existed) continue;
      table.materialize_state(state, nullptr, materialized);
      bool match = true;
      for (const auto &cond : where) {
        if (!table.compare(materialized.data(), cond)) {
          match = false;
          break;
        }
      }
      if (!match) continue;
      VisibleRow vr;
      vr.row = materialized.data();
      vr.page = nullptr;
      vr.pk = entry.first;
      vr.pos = state.pos;
      vr.from_overlay = true;
      if (!visit(vr)) return;
    }
  }
}

void Database::for_each_visible_key(
    Table &table, const std::vector<BoundCondition> &where,
    const std::function<void(int64_t, uint64_t)> &visit) {
  if (!where.empty()) {
    for_each_visible(table, where, [&](const VisibleRow &row) {
      visit(row.pk, row.pos);
      return true;
    });
    return;
  }
  // An unfiltered identity walk needs no row payload. The committed PK map and
  // transaction overlay already contain that information; don't scan wide
  // records merely to rediscover it. Snapshot keys before callbacks mutate
  // the overlay, and keep any required spool reads in physical order.
  const TxTable *changes = tx_.find_table(table.id());
  std::vector<std::pair<int64_t, uint64_t>> keys;
  keys.reserve(table.row_count());
  for (const auto &entry : table.pk_pos_) {
    uint64_t pos = entry.second;
    if (changes) {
      auto changed = changes->rows.find(entry.first);
      if (changed != changes->rows.end()) {
        if (changed->second.deleted) continue;
        pos = changed->second.pos;
      }
    }
    keys.emplace_back(entry.first, pos);
  }
  if (changes) for (const auto &entry : changes->rows)
    if (!entry.second.deleted && !entry.second.existed)
      keys.emplace_back(entry.first, entry.second.pos);
  std::sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) {
    return a.second < b.second;
  });
  for (const auto &entry : keys) visit(entry.first, entry.second);
}

bool Database::row_matches(const Table &table, const uint8_t *row,
                         const std::vector<BoundCondition> &where) const {
  for (const auto &cond : where) {
    if (!table.compare(row, cond)) return false;
  }
  return true;
}


} // namespace mvd::detail
