#pragma once
#include "../model/schema.h"
#include "../syntax/ast.h"
namespace mvd::detail {
struct ItemPlan {
    bool is_distance = false;
    int column_index = -1;
    int vector_column = -1;
    std::vector<float> query_vec;
    std::string name;
  };
struct OrderPlan {
    int output = -1;
    int column = -1;
    bool distance = false;
    bool descending = false;
    std::vector<float> query;
  };
struct SelectPlan {
 std::vector<ItemPlan> projections;std::vector<std::string> columns;
 std::vector<OrderPlan> order;std::vector<BoundCondition> where;
 bool topk=false;int64_t limit=0;int vector_column=-1;std::vector<float> query;
};
std::vector<BoundCondition> bind_conditions(const TableSchema&,const std::vector<Condition>&);
SelectPlan bind_select(const TableSchema&,const SelectStmt&);
}
