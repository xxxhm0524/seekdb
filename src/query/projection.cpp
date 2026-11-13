#include "../runtime/database.h"
#include "../runtime/shared.h"
namespace mvd::detail {
OutValue Database::out_value_from_row(const Table &table, const uint8_t *row,
                                    int col_index) {
  OutValue out;
  const ColumnDef &col = table.schema().columns[col_index];
  const uint8_t *base = row + table.schema().col_offsets[col_index];
  if (col.type == ColumnType::BIGINT) {
    int64_t v;
    std::memcpy(&v, base, sizeof(v));
    out.kind = OutValue::Kind::INT;
    out.i = v;
  } else if (col.type == ColumnType::VARCHAR) {
    uint32_t len;
    std::memcpy(&len, base, 4);
    out.kind = OutValue::Kind::STRING;
    out.s.assign(reinterpret_cast<const char *>(base + 4), len);
  } else {
    const float *fv = reinterpret_cast<const float *>(base);
    out.kind = OutValue::Kind::VECTOR;
    out.vec.assign(fv, fv + col.dim);
  }
  return out;
}


}
