#pragma once
#include "../model/value.h"
#include "spool.h"
#include <unordered_map>
namespace mvd {
using RowPatch = std::vector<std::pair<int, Value>>;

struct RowState {
  bool existed = false;   // existed in the base table when first touched
  bool deleted = false;
  uint64_t pos = 0;
  std::vector<uint8_t> row;
  std::shared_ptr<const TxSpool> spool;
  uint64_t spool_offset = 0;
  uint32_t spool_size = 0;
  bool has_raw_row() const { return !row.empty() || spool_size != 0; }
  void read_raw(std::vector<uint8_t> &output) const {
    if (!row.empty()) output = row;
    else if (spool_size) spool->read(spool_offset, spool_size, output);
  }
  // UPDATE literals are shared across matching records. Existing rows retain
  // patches instead of a full fixed-width copy of every VARCHAR/VECTOR slot.
  std::vector<std::shared_ptr<const RowPatch>> patches;
};

struct TxTable {
  std::unordered_map<int64_t, RowState> rows;
  uint64_t next_pos = 0;
};

}
