#include "../runtime/database.h"
#include "../runtime/shared.h"
#include <algorithm>
namespace mvd::detail {
void Database::load_catalog() {
  tables_.clear();
  table_ids_.clear();
  next_table_id_ = 1;
  std::string data;
  FileHandle catalog = FileHandle::open(catalog_path_, O_RDONLY);
  if (!catalog.valid()) {
    if (errno == ENOENT) return;
    throw_error("IO_ERROR", "cannot open catalog");
  }
  int64_t bytes = catalog.size();
  if (bytes < 0) throw_error("IO_ERROR", "cannot stat catalog");
  if (bytes == 0) throw_error("CORRUPTION", "empty catalog");
  data.resize(static_cast<size_t>(bytes));
  if (!catalog.pread_full(data.data(), data.size(), 0))
    throw_error("IO_ERROR", "cannot read catalog");
  Reader r(data);
  uint32_t magic, version, count;
  if (!r.u32(magic) || !r.u32(version) || !r.u32(count))
    throw_error("CORRUPTION", "invalid catalog header");
  if (magic != 0x4D564443 || version != 1)
    throw_error("CORRUPTION", "unsupported catalog version");
  // Last 4 bytes are a CRC over everything before them.
  if (data.size() < 12) throw_error("CORRUPTION", "catalog truncated");
  uint32_t stored_crc;
  std::memcpy(&stored_crc, data.data() + data.size() - 4, 4);
  if (crc32(data.data(), data.size() - 4) != stored_crc)
    throw_error("CORRUPTION", "catalog checksum mismatch");
  for (uint32_t i = 0; i < count; ++i) {
    TableSchema schema;
    uint32_t id;
    std::string name;
    uint32_t ncols;
    if (!r.u32(id) || !r.str(name) || !r.u32(ncols))
      throw_error("CORRUPTION", "invalid catalog entry");
    schema.id = id;
    schema.name = name;
    for (uint32_t c = 0; c < ncols; ++c) {
      ColumnDef col;
      uint8_t type;
      std::string cname;
      int32_t max_chars, dim;
      if (!r.u8(type) || !r.str(cname) || !r.i32(max_chars) || !r.i32(dim))
        throw_error("CORRUPTION", "invalid catalog column");
      col.type = static_cast<ColumnType>(type);
      col.name = std::move(cname);
      col.max_chars = max_chars;
      col.dim = dim;
      schema.columns.push_back(std::move(col));
    }
    int32_t pk_idx, vec_idx;
    uint8_t has_index;
    if (!r.i32(pk_idx) || !r.i32(vec_idx) || !r.u8(has_index))
      throw_error("CORRUPTION", "invalid catalog table");
    schema.pk_index = pk_idx;
    schema.vector_index = vec_idx;
    schema.vector_index_created = has_index != 0;
    build_row_layout(schema);
    std::string path = data_dir_ + "/t_" + std::to_string(id) + ".dat";
    auto table = std::make_unique<Table>(std::move(schema), path, &cache_);
    table_ids_[lower_copy(table->name())] = id;
    if (id >= next_table_id_) next_table_id_ = id + 1;
    tables_[id] = std::move(table);
  }
}

void Database::save_catalog(uint32_t excluded_table) {
  if (excluded_table && !tables_.count(excluded_table))
    throw_error("INTERNAL_ERROR", "catalog exclusion references missing table");
  std::string data;
  write_u32(data, 0x4D564443);
  write_u32(data, 1);
  write_u32(data, static_cast<uint32_t>(tables_.size() - (excluded_table ? 1 : 0)));
  for (auto &kv : tables_) {
    if (excluded_table && kv.first == excluded_table) continue;
    TableSchema &schema = kv.second->schema();
    write_u32(data, schema.id);
    write_u32(data, static_cast<uint32_t>(schema.name.size()));
    data += schema.name;
    write_u32(data, static_cast<uint32_t>(schema.columns.size()));
    for (auto &col : schema.columns) {
      write_u8(data, static_cast<uint8_t>(col.type));
      write_u32(data, static_cast<uint32_t>(col.name.size()));
      data += col.name;
      write_i32(data, col.max_chars);
      write_i32(data, col.dim);
    }
    write_i32(data, schema.pk_index);
    write_i32(data, schema.vector_index);
    write_u8(data, schema.vector_index_created ? 1 : 0);
  }
  uint32_t crc = crc32(data.data(), data.size());
  write_u32(data, crc);
  if (!write_file_atomic(catalog_path_, data))
    throw_error("IO_ERROR", "failed to write catalog");
}


} // namespace mvd::detail
