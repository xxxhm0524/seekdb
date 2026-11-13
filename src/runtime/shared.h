#pragma once
#include "../common.h"
#include "../model/codec.h"
#include "../core/text.h"
namespace mvd {
void parse_vector_for_column(const std::string&,const ColumnDef&,std::vector<float>&);
namespace detail {
inline constexpr size_t kVectorIndexBudget=128*1024*1024;
inline constexpr size_t kCompactCommitBudget=16*1024*1024;
inline constexpr int64_t kWalCheckpointBytes=32*1024*1024;
inline constexpr uint64_t kWalWriteWindow=8*1024*1024;
}}
