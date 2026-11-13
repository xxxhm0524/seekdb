#include "../runtime/database.h"
#include "../runtime/shared.h"
#include "../query/plan.h"
#include <algorithm>
namespace mvd::detail {
Response Database::exec_insert(InsertStmt &stmt) {
  Table &table = require_table(stmt.table);
  const TableSchema &schema = table.schema();
  if (stmt.rows.empty()) throw_error("INVALID_VALUE", "INSERT requires values");
  // Determine source column order.
  std::vector<int> target_cols;
  if (stmt.columns.empty()) {
    if (stmt.rows[0].size() != schema.columns.size())
      throw_error("INVALID_VALUE", "column count does not match table");
    for (size_t i = 0; i < schema.columns.size(); ++i)
      target_cols.push_back(static_cast<int>(i));
  } else {
    std::vector<bool> columns_seen(schema.columns.size(), false);
    for (const auto &name : stmt.columns) {
      int idx = table.column_index(name);
      if (idx < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + name);
      if (columns_seen[idx])
        throw_error("INVALID_VALUE", "duplicate INSERT column: " + name);
      columns_seen[idx] = true;
      target_cols.push_back(idx);
    }
    if (target_cols.size() != stmt.rows[0].size())
      throw_error("INVALID_VALUE", "column count does not match values");
  }
  int64_t affected = 0;
  for (const auto &values : stmt.rows) {
    if (values.size() != target_cols.size())
      throw_error("INVALID_VALUE", "column count does not match values");
    auto row=tx_.row_buffer();
    row.assign(schema.row_size,0);
    table.set_row_valid(row.data(), true);
    // Validate/encode every provided value.  Missing columns stay zero; we
    // require every column to be provided except when a column list is used
    // and the omitted column is nullable -- this dialect does not support NULL,
    // so require completeness.
    std::vector<bool> present(schema.columns.size(), false);
    for (size_t vi = 0; vi < target_cols.size(); ++vi) {
      int ci = target_cols[vi];
      table.encode_value(row.data(), ci, values[vi]);
      present[ci] = true;
    }
    for (size_t ci = 0; ci < schema.columns.size(); ++ci) {
      if (!present[ci]) {
        if (static_cast<int>(ci) == schema.pk_index)
          throw_error("INVALID_VALUE", "primary key value is required");
        // Defaults: numeric zero / empty string / zero vector; the problem
        // does not define NULL, so an explicit value is required.
        throw_error("INVALID_VALUE",
                    "all columns must be provided in INSERT: " +
                        schema.columns[ci].name);
      }
    }
    int64_t pk;
    std::memcpy(&pk, row.data() + schema.col_offsets[schema.pk_index], sizeof(pk));
    table.set_row_id(row.data(), pk);
    const RowState* previous=tx_.find_row(table.id(),pk);
    if (previous) {
      if (!previous->deleted)
        throw_error("DUPLICATE_KEY", "duplicate primary key: " + std::to_string(pk));
      // Insert after delete: reuse the existing slot.
      RowState state = *previous;
      state.deleted = false;
      state.row = std::move(row);
      state.spool.reset(); state.spool_size = 0;
      state.patches.clear();
      tx_.stage(table, pk, std::move(state));
      ++affected;
      continue;
    }
    uint64_t base_pos;
    if (table.find_pk(pk, base_pos))
      throw_error("DUPLICATE_KEY", "duplicate primary key: " + std::to_string(pk));
    RowState state;
    state.existed = false;
    state.deleted = false;
    state.pos = tx_.allocate_position(table);
    state.row = std::move(row);
    tx_.stage(table, pk, std::move(state));
    ++affected;
  }
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = affected;
  return resp;
}

Response Database::exec_update(UpdateStmt &stmt) {
  Table &table = require_table(stmt.table);
  const TableSchema &schema = table.schema();
  auto where=bind_conditions(table.schema(),stmt.where);
  // Resolve SET columns and typed values.
  auto patch = std::make_shared<RowPatch>();
  std::vector<uint8_t> validation_row(schema.row_size, 0);
  for (auto &kv : stmt.sets) {
    int ci = table.column_index(kv.first);
    if (ci < 0)
      throw_error("COLUMN_NOT_FOUND", "column not found: " + kv.first);
    if (ci == schema.pk_index)
      throw_error("UNSUPPORTED", "updating the primary key is not supported");
    Value v = coerce_value(schema, ci, kv.second);
    // Deferred patches must be valid when UPDATE returns, including VARCHAR
    // lengths, rather than failing at COMMIT or a later SELECT.
    table.encode_value(validation_row.data(), ci, v);
    patch->emplace_back(ci, std::move(v));
  }
  auto tracked_patch=tx_.track_patch(std::move(*patch));
  int64_t matched = 0;
  for_each_visible_key(table, where, [&](int64_t pk, uint64_t pos) {
    RowState state;
    const RowState* previous=tx_.find_row(table.id(),pk);
    if (previous) {
      state = *previous;
    } else {
      state.existed = true;
      state.pos = pos;
    }
    state.deleted = false;
    state.patches.push_back(tracked_patch);
    if (state.patches.size()>1) {
      std::vector<int> covered;
      std::vector<std::shared_ptr<const RowPatch>> kept;
      for (auto candidate=state.patches.rbegin();candidate!=state.patches.rend();++candidate) {
        bool needed=false;
        for (const auto &change:**candidate)
          needed=needed||std::find(covered.begin(),covered.end(),change.first)==covered.end();
        if (!needed) continue;
        kept.push_back(*candidate);
        for (const auto &change:**candidate)
          if (std::find(covered.begin(),covered.end(),change.first)==covered.end()) covered.push_back(change.first);
      }
      std::reverse(kept.begin(),kept.end());state.patches=std::move(kept);
    }
    tx_.stage(table, pk, std::move(state));
    ++matched;
  });
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = matched;
  return resp;
}

Response Database::exec_delete(DeleteStmt &stmt) {
  Table &table = require_table(stmt.table);
  auto where=bind_conditions(table.schema(),stmt.where);
  int64_t matched = 0;
  for_each_visible_key(table, where, [&](int64_t pk, uint64_t pos) {
    RowState state;
    const RowState* previous=tx_.find_row(table.id(),pk);
    if (previous) {
      state = *previous;
    } else {
      state.existed = true;
      state.pos = pos;
    }
    state.deleted = true;
    std::vector<uint8_t>().swap(state.row);  // Tombstones retain no raw capacity.
    state.spool.reset(); state.spool_size = 0;
    state.patches.clear();
    tx_.stage(table, pk, std::move(state));
    ++matched;
  });
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = matched;
  return resp;
}


} // namespace mvd::detail
