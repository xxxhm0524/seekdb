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
  uint32_t pages_per_row = 1;
};

// Compute fixed row layout and its physical page mapping.
void build_row_layout(TableSchema &schema);

int find_column(const TableSchema&,const std::string&);
}
