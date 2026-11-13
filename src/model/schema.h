#pragma once
#include "layout.h"
#include "value.h"
namespace mvd {
struct TableSchema {
  uint32_t id = 0;
  std::string name;
  std::vector<ColumnDef> columns;
  int pk_index = -1;
  int vector_index = -1;
  bool vector_index_created = false;
  uint32_t row_size = 0;
  std::vector<uint32_t> col_offsets;  // payload offset of each column
  uint32_t page_size = kPageSize;
  uint32_t rows_per_page = 0;
};

// Compute row layout.  Throws INVALID_SCHEMA when a row cannot fit a page.
void build_row_layout(TableSchema &schema);

int find_column(const TableSchema&,const std::string&);
}
