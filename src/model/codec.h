#pragma once
#include "schema.h"
namespace mvd {
void parse_vector_for_column(const std::string&,const ColumnDef&,std::vector<float>&);
Value coerce_value(const TableSchema&,int,const Value&);
}
