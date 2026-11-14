#include "../model/schema.h"
#include "text.h"
#include "error.h"
#include <unordered_set>
namespace mvd {
namespace {uint32_t align_up(uint32_t value,uint32_t alignment){return (value+alignment-1)/alignment*alignment;}}
void build_row_layout(TableSchema &schema) {
  int vec_count = 0;
  std::unordered_set<std::string> names;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const auto &col = schema.columns[i];
    if ((col.type==ColumnType::VARCHAR&&(col.max_chars<=0||col.max_chars>65535)) || (col.type==ColumnType::VECTOR&&(col.dim<=0||col.dim>16384)) || (col.type!=ColumnType::BIGINT&&col.type!=ColumnType::VARCHAR&&col.type!=ColumnType::VECTOR))
      throw_error("INVALID_SCHEMA","invalid column type or size");
    if (col.name.empty()) throw_error("INVALID_SCHEMA", "empty column name");
    std::string lower = lower_copy(col.name);
    if (!names.insert(lower).second)
      throw_error("INVALID_SCHEMA", "duplicate column name: " + col.name);
    if (col.type == ColumnType::VECTOR) {
      ++vec_count;
      schema.vector_index = static_cast<int>(i);
    }
  }
  if (vec_count > 1)
    throw_error("INVALID_SCHEMA", "at most one VECTOR column is allowed");
  schema.vector_index = -1;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (schema.columns[i].type == ColumnType::VECTOR)
      schema.vector_index = static_cast<int>(i);
  }
  // pk_index is set by the caller (parse_create_table leaves the primary_key
  // name).  If it is still -1, use the first BIGINT column.
  if (schema.pk_index < 0) {
    for (size_t i = 0; i < schema.columns.size(); ++i) {
      if (schema.columns[i].type == ColumnType::BIGINT) {
        schema.pk_index = static_cast<int>(i);
        break;
      }
    }
  }
  if (schema.pk_index < 0 || static_cast<size_t>(schema.pk_index)>=schema.columns.size())
    throw_error("INVALID_SCHEMA", "a BIGINT PRIMARY KEY is required");
  if (schema.columns[schema.pk_index].type != ColumnType::BIGINT)
    throw_error("INVALID_SCHEMA", "primary key must be BIGINT");

  uint32_t offset = 16;  // valid byte + padding + int64 id
  schema.col_offsets.assign(schema.columns.size(), 0);
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const auto &col = schema.columns[i];
    switch (col.type) {
      case ColumnType::BIGINT:
        offset = align_up(offset, 8);
        schema.col_offsets[i] = offset;
        offset += 8;
        break;
      case ColumnType::VARCHAR:
        offset = align_up(offset, 4);
        schema.col_offsets[i] = offset;
        offset += 4 + static_cast<uint32_t>(col.max_chars) * 4;
        break;
      case ColumnType::VECTOR:
        offset = align_up(offset, 32);
        schema.col_offsets[i] = offset;
        offset += static_cast<uint32_t>(col.dim) * 4;
        break;
    }
    // Bound per-row buffers before a large column list can wrap the offset.
    if (offset > 16 * 1024 * 1024)
      throw_error("INVALID_SCHEMA", "row layout exceeds 16 MiB");
  }
  schema.row_size = align_up(offset, 64);
  schema.page_size = kPageSize;
  schema.rows_per_page =
      (kPageSize - kPageHeaderSize) / schema.row_size;
  const uint32_t payload=kPageSize-kPageHeaderSize;
  schema.pages_per_row=(schema.row_size+payload-1)/payload;
}

int find_column(const TableSchema& schema,const std::string& name){auto key=lower_copy(name);for(size_t i=0;i<schema.columns.size();++i)if(lower_copy(schema.columns[i].name)==key)return int(i);return -1;}
}
