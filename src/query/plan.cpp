#include "plan.h"
#include "../model/codec.h"
#include "../common.h"
#include "../core/text.h"
namespace mvd::detail {
std::vector<BoundCondition> bind_conditions(const TableSchema& schema,const std::vector<Condition>& where){
 std::vector<BoundCondition> result;result.reserve(where.size());
 for(const auto& condition:where){int index=find_column(schema,condition.column);if(index<0)throw_error("COLUMN_NOT_FOUND","column not found: "+condition.column);if(schema.columns[index].type==ColumnType::VECTOR)throw_error("UNSUPPORTED","VECTOR columns cannot be compared in WHERE");result.push_back(BoundCondition{index,condition.op,coerce_value(schema,index,condition.value)});}return result;
}
SelectPlan bind_select(const TableSchema& schema,const SelectStmt& stmt){
  auto where=bind_conditions(schema,stmt.where);

  // Resolve output columns.
  std::vector<ItemPlan> plans;
  std::vector<std::string> out_columns;
  for (const auto &item : stmt.items) {
    if (item.star) {
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        ItemPlan q;
        q.column_index = static_cast<int>(i);
        q.name = schema.columns[i].name;
        plans.push_back(q);
        out_columns.push_back(q.name);
      }
      continue;
    }
    ItemPlan p;
    if (item.expr.kind == Expr::Kind::COLUMN) {
      int ci = find_column(schema,item.expr.column);
      if (ci < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + item.expr.column);
      p.column_index = ci;
      p.name = item.alias.empty() ? schema.columns[ci].name : item.alias;
    } else if (item.expr.kind == Expr::Kind::L2_DISTANCE) {
      int ci = find_column(schema,item.expr.column);
      if (ci < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + item.expr.column);
      if (schema.columns[ci].type != ColumnType::VECTOR)
        throw_error("INVALID_ARGUMENT", "L2_DISTANCE requires a VECTOR column");
      p.is_distance = true;
      p.vector_column = ci;
      parse_vector_for_column(item.expr.vec_text, schema.columns[ci], p.query_vec);
      p.name = item.alias.empty() ? "L2_DISTANCE" : item.alias;
    } else {
      throw_error("UNSUPPORTED", "unsupported select expression");
    }
    plans.push_back(p);
    out_columns.push_back(p.name);
  }

  // Resolve ORDER BY before reading any rows, including empty tables and
  // LIMIT 0. An alias is an exact binding, never a guessed name such as "d".
  std::vector<OrderPlan> orders;
  for (const auto &key : stmt.order) {
    OrderPlan order;
    order.descending=key.desc;
    if (key.is_distance) {
      order.column = find_column(schema,key.expr.column);
      if (order.column < 0)
        throw_error("COLUMN_NOT_FOUND", "ORDER BY column not found: " + key.expr.column);
      if (schema.columns[order.column].type != ColumnType::VECTOR)
        throw_error("INVALID_ARGUMENT", "L2_DISTANCE requires a VECTOR column");
      order.distance = true;
      parse_vector_for_column(key.expr.vec_text, schema.columns[order.column], order.query);
    } else {
      if (key.ordinal > 0) {
        order.output = key.ordinal - 1;
        if (static_cast<size_t>(order.output) >= plans.size())
          throw_error("COLUMN_NOT_FOUND", "ORDER BY ordinal out of range");
      } else {
        for (size_t i = 0; i < plans.size(); ++i) {
          if (lower_copy(plans[i].name) == lower_copy(key.name)) {
            order.output = static_cast<int>(i);
            break;
          }
        }
      }
      if (order.output >= 0) {
        const auto &plan = plans[order.output];
        order.distance = plan.is_distance;
        order.column = plan.is_distance ? plan.vector_column : plan.column_index;
        order.query = plan.query_vec;
      } else {
        order.column = find_column(schema,key.name);
        if (order.column < 0)
          throw_error("COLUMN_NOT_FOUND", "ORDER BY column not found: " + key.name);
      }
      if (!order.distance && schema.columns[order.column].type == ColumnType::VECTOR)
        throw_error("UNSUPPORTED", "ORDER BY VECTOR requires L2_DISTANCE");
    }
    // A later occurrence of the same value cannot break a tie left by its
    // first occurrence, even with the opposite direction. Validate it first.
    bool duplicate=std::any_of(orders.begin(),orders.end(),[&](const OrderPlan &previous) {
      return previous.column==order.column && previous.distance==order.distance &&
          (!order.distance || previous.query==order.query);
    });
    if (!duplicate) orders.push_back(std::move(order));
  }
  bool topk_path = false;
  int64_t topk = 0;
  int topk_vector_column = -1;
  std::vector<float> topk_query_vec;
  if (stmt.has_limit && stmt.offset == 0 && stmt.order.size() == 1 &&
      !stmt.order[0].desc) {
    if (orders[0].distance) {
      topk_vector_column = orders[0].column;
      topk_query_vec = orders[0].query;
      topk_path = true;
    }
    if (topk_path) topk = stmt.limit;
  }

  return SelectPlan{std::move(plans),std::move(out_columns),std::move(orders),std::move(where),topk_path,topk,topk_vector_column,std::move(topk_query_vec)};
}
}
