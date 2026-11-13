#include "../runtime/database.h"
#include "../runtime/shared.h"
#include <algorithm>
#include "../query/vector_index.h"
namespace mvd::detail {
Response Database::exec_create_table(CreateTableStmt &stmt) {
  std::string lower = lower_copy(stmt.table);
  if (table_ids_.count(lower)) {
    if (stmt.if_not_exists) return Response{};
    throw_error("TABLE_EXISTS", "table already exists: " + stmt.table);
  }
  TableSchema schema;
  schema.id = next_table_id_++;
  schema.name = stmt.table;
  schema.columns = stmt.columns;
  // Resolve primary key.
  schema.pk_index = -1;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (lower_copy(schema.columns[i].name) == lower_copy(stmt.primary_key))
      schema.pk_index = static_cast<int>(i);
  }
  if (schema.pk_index < 0)
    throw_error("INVALID_SCHEMA", "PRIMARY KEY column not found: " + stmt.primary_key);
  build_row_layout(schema);
  std::string path = data_dir_ + "/t_" + std::to_string(schema.id) + ".dat";
  auto table = std::make_unique<Table>(schema, path, &cache_);
  // This ID is not referenced by the catalog. An interrupted DROP can leave
  // an orphan with the same path after restart; CREATE must start empty.
  table->open(false, true);
  // Make sure the table file is durable before the catalog references it.
  if (!table->fsync_file()) {
    table.reset();
    std::remove(path.c_str());
    throw_error("IO_ERROR", "failed to sync new table file");
  }
  uint32_t id = schema.id;
  std::string name = schema.name;
  try {
    tables_[id] = std::move(table);
    table_ids_[lower] = id;
    save_catalog();
  } catch (...) {
    tables_.erase(id);
    table_ids_.erase(lower);
    table.reset();
    std::remove(path.c_str());
    throw;
  }
  (void)name;
  Response resp;
  return resp;
}

Response Database::exec_drop_table(DropTableStmt &stmt) {
  Table *table = find_table(stmt.table);
  if (!table) {
    if (stmt.if_exists) return Response{};
    throw_error("TABLE_NOT_FOUND", "table not found: " + stmt.table);
  }
  uint32_t id = table->id();
  std::string path = table->data_path();
  std::string lower = lower_copy(table->name());
  // Publish the prospective directory before discarding live rows or dirty
  // pages. A failed catalog write must leave the current table recoverable.
  save_catalog(id);
  drop_vector_index(*table);
  indexes_.erase(id);
  cache_.drop_table(id);
  tables_.erase(id);
  table_ids_.erase(lower);
  std::remove(path.c_str());
  Response resp;
  return resp;
}

Response Database::exec_create_vector_index(CreateVectorIndexStmt &stmt) {
  Table &table = require_table(stmt.table);
  int col = table.column_index(stmt.column);
  if (col < 0)
    throw_error("COLUMN_NOT_FOUND", "column not found: " + stmt.column);
  if (table.schema().columns[col].type != ColumnType::VECTOR)
    throw_error("INVALID_ARGUMENT", "vector index requires a VECTOR column");
  const bool previous = table.schema().vector_index_created;
  const bool unavailable = index_unavailable(table.id());
  try {
    table.schema().vector_index_created = true;
    rebuild_vector_index(table);
    save_catalog();
  } catch (...) {
    drop_vector_index(table);
    table.schema().vector_index_created = previous;
    if(auto entry=indexes_.find(table.id());entry!=indexes_.end())entry->second.unavailable=unavailable;
    throw;
  }
  return Response{};
}


} // namespace mvd::detail
