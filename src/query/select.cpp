#include "../runtime/database.h"
#include "../runtime/shared.h"
#include "../query/vector_index.h"
#include "../query/distance.h"
#include "../query/plan.h"
#include "external_sort.h"
#include <exception>
#include <atomic>
#ifdef _OPENMP
#include <omp.h>
#endif
#include <algorithm>
namespace mvd::detail {
namespace {
struct SortKey {
  OutValue::Kind kind = OutValue::Kind::INT;
  int64_t i = 0;
  long double d = 0;
  std::string s;
};
int compare_sort_keys(const SortKey &a, const SortKey &b) {
  if (a.kind == OutValue::Kind::INT && b.kind == OutValue::Kind::INT)
    return (a.i < b.i) ? -1 : (a.i > b.i) ? 1 : 0;
  if (a.kind == OutValue::Kind::DOUBLE && b.kind == OutValue::Kind::DOUBLE)
    return (a.d < b.d) ? -1 : (a.d > b.d) ? 1 : 0;
  if (a.kind == OutValue::Kind::STRING && b.kind == OutValue::Kind::STRING)
    return a.s.compare(b.s);
  // Mixed types are not expected; fall back to type then text.
  if (a.kind != b.kind) return static_cast<int>(a.kind) - static_cast<int>(b.kind);
  if (a.kind == OutValue::Kind::INT) return (a.i < b.i) ? -1 : (a.i > b.i) ? 1 : 0;
  if (a.kind == OutValue::Kind::DOUBLE) return (a.d < b.d) ? -1 : (a.d > b.d) ? 1 : 0;
  return a.s.compare(b.s);
}
}  // namespace

Response Database::exec_select(SelectStmt &stmt) {
  Table &table = require_table(stmt.table);
  const TableSchema &schema = table.schema();
  const auto bound=bind_select(schema,stmt);
  const auto& plans=bound.projections;const auto& out_columns=bound.columns;
  const auto& orders=bound.order;const auto& where=bound.where;
  const bool topk_path=bound.topk;const int64_t topk=bound.limit;
  const int topk_vector_column=bound.vector_column;const auto& topk_query_vec=bound.query;

  Response resp;
  resp.is_query = true;
  resp.columns = out_columns;

  // All expressions and sort keys are validated above. The base identities
  // plus every overlay entry conservatively bound visible rows, even when
  // inserts/deletes are pending, so an excessive OFFSET needs no heap scan.
  if (stmt.has_limit && stmt.limit >= 0 && stmt.offset >= 0) {
    uint64_t maximum_rows = static_cast<uint64_t>(table.row_count());
    if (tx_.active()) {
      auto changes = tx_.tables().find(table.id());
      if (changes != tx_.tables().end()) {
        uint64_t extra = static_cast<uint64_t>(changes->second.rows.size());
        maximum_rows = extra > std::numeric_limits<uint64_t>::max() - maximum_rows
            ? std::numeric_limits<uint64_t>::max() : maximum_rows + extra;
      }
    }
    if (static_cast<uint64_t>(stmt.offset) >= maximum_rows) return resp;
  }

  auto project_row = [&](const uint8_t *row) -> std::vector<OutValue> {
    std::vector<OutValue> vals;
    vals.reserve(plans.size());
    for (const auto &p : plans) {
      if (p.is_distance) {
        const uint8_t *base = row + schema.col_offsets[p.vector_column];
        const float *vec = reinterpret_cast<const float *>(base);
        OutValue ov;
        ov.kind = OutValue::Kind::DOUBLE;
        ov.d = returned_l2_distance(vec, p.query_vec.data(), schema.columns[p.vector_column].dim);
        vals.push_back(std::move(ov));
      } else {
        vals.push_back(out_value_from_row(table, row, p.column_index));
      }
    }
    return vals;
  };


  auto load_candidate = [&](const Candidate &candidate, std::vector<uint8_t> &row) {
    if (tx_.active()) {
      const TxTable *overlay = tx_.find_table(table.id());
      if (overlay) {
        auto found = overlay->rows.find(candidate.pk);
        if (found != overlay->rows.end() && !found->second.deleted) {
          table.materialize_state(found->second, nullptr, row);
          return true;
        }
      }
    }
    return table.read_row(candidate.pos, row);
  };

  if (topk_path && topk >= 0) {
    if (topk == 0) return resp;
    auto* index=index_handle(table.id());
    auto refine = [&](const Candidate &candidate) {
      uint32_t cached=0;
      if (!tx_.active() && index && index->find(candidate.pk,cached))
        return returned_l2_distance(index->vector(cached),topk_query_vec.data(),schema.columns[topk_vector_column].dim);
      std::vector<uint8_t> row;
      if (!load_candidate(candidate, row))
        throw_error("CORRUPTION", "TopK candidate row missing");
      const float *vector = reinterpret_cast<const float *>(row.data() + schema.col_offsets[topk_vector_column]);
      return returned_l2_distance(vector, topk_query_vec.data(), schema.columns[topk_vector_column].dim);
    };
    // Parallel exact top-K scan.
    std::vector<CandidateHeap> heaps;
    int thread_count = 1;
#ifdef _OPENMP
    thread_count = omp_get_max_threads();
    if (thread_count < 1) thread_count = 1;
    heaps.resize(static_cast<size_t>(thread_count));
    for (auto &h : heaps) h.k = static_cast<size_t>(topk);
#endif
#ifndef _OPENMP
    heaps.resize(1);
    heaps[0].k = static_cast<size_t>(topk);
#endif
    for (auto &heap : heaps) {
      // Include extra room for distinct squared values coalescing after sqrt.
      heap.uncertainty_ulps = 16 * (static_cast<uint64_t>(schema.columns[topk_vector_column].dim) + 1) + 8;
      heap.refine = refine;
    }
    if (!tx_.active() && table.schema().vector_index_created && !index &&
        !index_unavailable(table.id())) rebuild_vector_index(table);
    index=index_handle(table.id());
    std::exception_ptr scan_error;
    std::atomic<bool> scan_failed{false};
    auto remember_scan_error = [&]() {
      // Exceptions must not unwind out of an OpenMP worker. Publish the first
      // failure, join every worker, then rethrow in execute's error boundary.
#ifdef _OPENMP
#pragma omp critical(mvd_topk_error)
#endif
      { if (!scan_error) scan_error = std::current_exception(); }
      scan_failed.store(true, std::memory_order_relaxed);
    };
    if (!tx_.active() && index && index->supports(where)) {
      bool approximate = false;
      if (ann_mode_ && where.empty() && index->approximate_available(static_cast<size_t>(topk))) {
        auto hits=index->search(topk_query_vec.data(),static_cast<size_t>(topk));
        // Search already uses squared_l2_checked and returns the ordered
        // live prefix. Keep its extra hit for the precision boundary check.
        // Dense ties need all eligible IDs to enforce the canonical PK order.
        if (hits.size() > static_cast<size_t>(topk)) {
          uint64_t first, second;
          std::memcpy(&first, &hits[static_cast<size_t>(topk)-1].distance, 8);
          std::memcpy(&second, &hits[static_cast<size_t>(topk)].distance, 8);
          uint64_t gap = first > second ? first-second : second-first;
          approximate = gap > heaps[0].uncertainty_ulps;
        }
        if (approximate) for (size_t i = 0; i < static_cast<size_t>(topk); ++i) {
          const auto &hit = hits[i];
          const auto &node = index->node(hit.node);
          heaps[0].push(hit.distance, node.pk, node.pos);
        }
      }
      if (!approximate) {
        const auto *matching = index->matching_nodes(where);
        size_t scan_count = matching ? matching->size() : index->count();
#ifdef _OPENMP
#pragma omp parallel for num_threads(thread_count) schedule(static)
#endif
        for (int64_t i = 0; i < static_cast<int64_t>(scan_count); ++i) {
          if (scan_failed.load(std::memory_order_relaxed)) continue;
          try {
            uint32_t id = matching ? (*matching)[static_cast<size_t>(i)] : static_cast<uint32_t>(i);
            if (!matching && !index->matches(id, where)) continue;
            int tid = 0;
#ifdef _OPENMP
            tid = omp_get_thread_num();
#endif
            const auto &node = index->node(id);
            heaps[static_cast<size_t>(tid)].push(squared_l2_checked(index->vector(id),
                topk_query_vec.data(), schema.columns[topk_vector_column].dim), node.pk, node.pos);
          } catch (...) { remember_scan_error(); }
        }
        if (scan_error) std::rethrow_exception(scan_error);
        for (size_t t = 1; t < heaps.size(); ++t) heaps[0].merge(heaps[t]);
      }
    } else if (tx_.active()) {
      for_each_visible(table, where, [&](const VisibleRow &vr) {
        const uint8_t *base = vr.row + schema.col_offsets[topk_vector_column];
        const float *vec = reinterpret_cast<const float *>(base);
        double sq = squared_l2_checked(vec, topk_query_vec.data(),
                                       schema.columns[topk_vector_column].dim);
        heaps[0].push(sq, vr.pk, vr.pos);
        return true;
      });
    } else {
      uint32_t pages = table.page_count();
#ifdef _OPENMP
#pragma omp parallel num_threads(thread_count)
      {
        int tid = omp_get_thread_num();
        CandidateHeap &heap = heaps[static_cast<size_t>(tid)];
#pragma omp for schedule(static)
        for (int64_t pi = 0; pi < static_cast<int64_t>(pages); ++pi) {
          if (scan_failed.load(std::memory_order_relaxed)) continue;
          try {
            auto page = table.get_page(static_cast<uint32_t>(pi));
            if (page->magic != kPageMagic) continue;
            uint32_t n = std::min(page->nrows, page->capacity(schema.row_size));
            for (uint32_t slot = 0; slot < n; ++slot) {
              const uint8_t *row = table.row_data(*page, slot);
              if (!table.row_is_valid(row)) continue;
              bool match = true;
              for (const auto &cond : where) {
                if (!table.compare(row, cond)) { match = false; break; }
              }
              if (!match) continue;
              const uint8_t *base = row + schema.col_offsets[topk_vector_column];
              const float *vec = reinterpret_cast<const float *>(base);
              double sq = squared_l2_checked(vec, topk_query_vec.data(),
                                             schema.columns[topk_vector_column].dim);
              int64_t pk = table.row_id(row);
              heap.push(sq, pk, static_cast<uint64_t>(pi) * schema.rows_per_page + slot);
            }
          } catch (...) { remember_scan_error(); }
        }
      }
#else
      for (uint32_t pi = 0; pi < pages; ++pi) {
        auto page = table.get_page(pi);
        if (page->magic != kPageMagic) continue;
        uint32_t n = std::min(page->nrows, page->capacity(schema.row_size));
        for (uint32_t slot = 0; slot < n; ++slot) {
          const uint8_t *row = table.row_data(*page, slot);
          if (!table.row_is_valid(row)) continue;
          bool match = true;
          for (const auto &cond : where) {
            if (!table.compare(row, cond)) { match = false; break; }
          }
          if (!match) continue;
          const uint8_t *base = row + schema.col_offsets[topk_vector_column];
          const float *vec = reinterpret_cast<const float *>(base);
          double sq = squared_l2_checked(vec, topk_query_vec.data(),
                                         schema.columns[topk_vector_column].dim);
          int64_t pk = table.row_id(row);
          heaps[0].push(sq, pk, static_cast<uint64_t>(pi) * schema.rows_per_page + slot);
        }
      }
#endif
      if (scan_error) std::rethrow_exception(scan_error);
      for (size_t t = 1; t < heaps.size(); ++t) heaps[0].merge(heaps[t]);
    }
    std::vector<Candidate> finalists = heaps[0].data;
    std::sort(finalists.begin(), finalists.end(),
              [&](const Candidate &a, const Candidate &b) { return heaps[0].less(a, b); });
    for (const auto &c : finalists) {
      uint32_t cached=0;
      bool covered=!tx_.active()&&index&&index->find(c.pk,cached);
      for (const auto &plan:plans)
        covered=covered&&(plan.is_distance||plan.column_index==schema.vector_index||index->relevant_column(plan.column_index));
      if (covered) {
        std::vector<OutValue> values;
        for (const auto &plan:plans) {
          OutValue value;
          if (plan.is_distance) {
            value.kind=OutValue::Kind::DOUBLE;
            value.d=returned_l2_distance(index->vector(cached),plan.query_vec.data(),schema.columns[plan.vector_column].dim);
          } else if (plan.column_index==schema.vector_index) {
            value.kind=OutValue::Kind::VECTOR;
            value.vec.assign(index->vector(cached),index->vector(cached)+schema.columns[plan.column_index].dim);
          } else if (schema.columns[plan.column_index].type==ColumnType::BIGINT) {
            value.kind=OutValue::Kind::INT;value.i=index->integer(cached,plan.column_index);
          } else {
            value.kind=OutValue::Kind::STRING;value.s=index->string(cached,plan.column_index);
          }
          values.push_back(std::move(value));
        }
        resp.rows.push_back(std::move(values));
        continue;
      }
      std::vector<uint8_t> row;
      if (!load_candidate(c, row)) continue;
      resp.rows.push_back(project_row(row.data()));
    }
    return resp;
  }

  // Streaming path for no-ORDER-BY queries with LIMIT: avoids materializing
  // the whole table when the dataset is larger than the page cache/memory.
  if (stmt.order.empty() && stmt.has_limit) {
    int64_t seen = 0;
    int64_t wanted = (stmt.offset > std::numeric_limits<int64_t>::max() - stmt.limit)
                         ? std::numeric_limits<int64_t>::max()
                         : stmt.offset + stmt.limit;
    if (stmt.limit == 0) return resp;
    for_each_visible(table, where, [&](const VisibleRow &vr) {
      if (seen >= stmt.offset) resp.rows.push_back(project_row(vr.row));
      ++seen;
      return seen < wanted;
    });
    return resp;
  }

  // Generic path.
  struct ResultRow {
    std::vector<OutValue> vals;
    std::vector<SortKey> keys;
    int64_t pk;
    uint64_t pos;
  };
  std::vector<bool> desc;
  for (const auto &o : orders) desc.push_back(o.descending);
  auto row_less = [&](const ResultRow &a, const ResultRow &b) {
    for (size_t i = 0; i < desc.size(); ++i) {
      int cmp = compare_sort_keys(a.keys[i], b.keys[i]);
      if (cmp != 0) return desc[i] ? cmp > 0 : cmp < 0;
    }
    // Ties are resolved by primary key ascending.
    return a.pk < b.pk;
  };
  if (stmt.has_limit && stmt.limit == 0) return resp;
  bool bounded = stmt.has_limit && !stmt.order.empty() && stmt.limit > 0;
  int64_t need = 0;
  if (bounded) {
    need = (stmt.offset > std::numeric_limits<int64_t>::max() - stmt.limit)
               ? std::numeric_limits<int64_t>::max()
               : stmt.offset + stmt.limit;
  }
  std::vector<ResultRow> results;
  const bool ordered=!orders.empty();
  size_t retained_extra=0;
  auto row_memory=[](const ResultRow &row) {
    size_t bytes=sizeof(ResultRow)+row.keys.capacity()*sizeof(SortKey);
    for (const auto &key:row.keys) if (key.kind==OutValue::Kind::STRING) bytes+=key.s.capacity();
    return bytes;
  };
  auto encode_row=[](const ResultRow &row) {
    std::string packet;
    auto append=[&](const auto &value) {
      packet.append(reinterpret_cast<const char *>(&value),sizeof(value));
    };
    append(row.pk);append(row.pos);
    if (row.keys.size()>std::numeric_limits<uint32_t>::max())
      throw_error("RESOURCE_LIMIT","too many query sort keys");
    uint32_t count=static_cast<uint32_t>(row.keys.size());append(count);
    for (const auto &key:row.keys) {
      uint8_t kind=static_cast<uint8_t>(key.kind);append(kind);
      if (key.kind==OutValue::Kind::INT) append(key.i);
      else if (key.kind==OutValue::Kind::DOUBLE) pack_sort_number(packet,key.d);
      else if (key.kind==OutValue::Kind::STRING) {
        if (key.s.size()>std::numeric_limits<uint32_t>::max()) throw_error("RESOURCE_LIMIT","sort string too large");
        uint32_t length=static_cast<uint32_t>(key.s.size());append(length);packet.append(key.s);
      } else throw_error("CORRUPTION","invalid query sort key kind");
    }
    return packet;
  };
  auto decode_row=[&](std::string_view packet) {
    ResultRow row{};size_t cursor=0;
    auto read=[&](auto &value) {
      if (sizeof(value)>packet.size()-cursor) throw_error("CORRUPTION","truncated query sort key");
      std::memcpy(&value,packet.data()+cursor,sizeof(value));cursor+=sizeof(value);
    };
    read(row.pk);read(row.pos);uint32_t count=0;read(count);
    if (count!=orders.size()) throw_error("CORRUPTION","query sort key count mismatch");
    row.keys.resize(count);
    for (auto &key:row.keys) {
      uint8_t kind=0;read(kind);
      if (kind>static_cast<uint8_t>(OutValue::Kind::STRING)) throw_error("CORRUPTION","invalid query sort key kind");
      key.kind=static_cast<OutValue::Kind>(kind);
      if (key.kind==OutValue::Kind::INT) read(key.i);
      else if (key.kind==OutValue::Kind::DOUBLE) {
        if (kSortNumberBytes>packet.size()-cursor ||
            !unpack_sort_number(packet.substr(cursor,kSortNumberBytes),key.d))
          throw_error("CORRUPTION","invalid query sort distance");
        cursor+=kSortNumberBytes;
      } else {
        uint32_t length=0;read(length);
        if (length>packet.size()-cursor) throw_error("CORRUPTION","truncated query sort string");
        key.s.assign(packet.data()+cursor,length);cursor+=length;
      }
    }
    if (cursor!=packet.size()) throw_error("CORRUPTION","trailing query sort record bytes");
    return row;
  };
  std::unique_ptr<ExternalSorter<ResultRow>> external;
  auto promote_sort=[&]() {
    uint64_t maximum_record=sizeof(int64_t)+sizeof(uint64_t)+sizeof(uint32_t);
    for (const auto &order:orders) {
      uint64_t bytes=1;
      if (order.distance) bytes+=kSortNumberBytes;
      else if (schema.columns[order.column].type==ColumnType::BIGINT) bytes+=sizeof(int64_t);
      else bytes+=sizeof(uint32_t)+static_cast<uint64_t>(schema.columns[order.column].max_chars)*4;
      maximum_record=std::min<uint64_t>(std::numeric_limits<uint32_t>::max(),maximum_record+bytes);
    }
    external=std::make_unique<ExternalSorter<ResultRow>>(data_dir_+"/sort.tmp",sort_memory_,
        sizeof(ResultRow)+orders.size()*sizeof(SortKey),static_cast<uint32_t>(maximum_record),
        row_less,encode_row,decode_row,row_memory);
    // Any rows previously rejected by the TopK heap cannot enter its final
    // prefix, so retaining the heap plus all subsequent rows is sufficient.
    for (auto &row:results) external->push(std::move(row));
    std::vector<ResultRow>().swap(results);retained_extra=0;
  };
  auto consume = [&](const VisibleRow &vr) {
    ResultRow rr;
    // Ordered LIMIT/OFFSET needs keys and locations, not every wide output
    // value. Project the selected visible rows only after sorting/slicing.
    if (!ordered) rr.vals = project_row(vr.row);
    rr.pk = vr.pk;
    rr.pos = vr.pos;
    for (const auto &order : orders) {
      SortKey key;
      if (order.distance) {
        int ci = order.column;
        const float *vector = reinterpret_cast<const float *>(vr.row + schema.col_offsets[ci]);
        key.kind = OutValue::Kind::DOUBLE;
        key.d = returned_l2_distance(vector, order.query.data(), schema.columns[ci].dim);
      } else {
        OutValue ov = out_value_from_row(table, vr.row, order.column);
        key.kind = ov.kind;
        key.i = ov.i;
        key.d = ov.d;
        key.s = std::move(ov.s);
      }
      rr.keys.push_back(std::move(key));
    }
    if (external) {
      external->push(std::move(rr));
      return true;
    }
    if (bounded) {
      if (static_cast<int64_t>(results.size()) < need) {
        retained_extra+=row_memory(rr)-sizeof(ResultRow);
        results.push_back(std::move(rr));
        std::push_heap(results.begin(), results.end(), row_less);
      } else if (row_less(rr, results.front())) {
        std::pop_heap(results.begin(), results.end(), row_less);
        retained_extra-=row_memory(results.back())-sizeof(ResultRow);
        retained_extra+=row_memory(rr)-sizeof(ResultRow);
        results.back() = std::move(rr);
        std::push_heap(results.begin(), results.end(), row_less);
      }
    } else {
      if (ordered) retained_extra+=row_memory(rr)-sizeof(ResultRow);
      results.push_back(std::move(rr));
    }
    if (ordered && (retained_extra>sort_memory_ ||
        results.capacity()>((sort_memory_-std::min(sort_memory_,retained_extra))/sizeof(ResultRow))))
      promote_sort();
    return true;
  };
  bool identity_only = where.empty() && !orders.empty() &&
      std::all_of(plans.begin(), plans.end(), [&](const auto &plan) {
        return !plan.is_distance && plan.column_index == schema.pk_index;
      }) && std::all_of(orders.begin(), orders.end(), [&](const auto &order) {
        if (order.distance) return false;
        return order.output >= 0 ? plans[static_cast<size_t>(order.output)].column_index == schema.pk_index
                                 : order.column == schema.pk_index;
      });
  if (identity_only) {
    // Project/order only the immutable primary key from the already visible
    // identity maps. All other projections, predicates and order keys still
    // use actual row data through the ordinary scan.
    std::vector<uint8_t> identity(schema.row_size, 0);
    table.set_row_valid(identity.data(), true);
    for_each_visible_key(table, {}, [&](int64_t pk, uint64_t pos) {
      table.set_row_id(identity.data(), pk);
      std::memcpy(identity.data()+schema.col_offsets[schema.pk_index], &pk, sizeof(pk));
      VisibleRow row;
      row.row = identity.data(); row.pk = pk; row.pos = pos;
      consume(row);
    });
  } else for_each_visible(table, where, consume);
  std::vector<uint8_t> selected_row;
  if (ordered && identity_only) {
    selected_row.assign(schema.row_size, 0);
    table.set_row_valid(selected_row.data(), true);
  }
  auto emit_result=[&](const ResultRow &result) {
    if (identity_only) {
      table.set_row_id(selected_row.data(), result.pk);
      std::memcpy(selected_row.data()+schema.col_offsets[schema.pk_index], &result.pk, sizeof(result.pk));
    } else {
      Candidate location{};
      location.pk = result.pk; location.pos = result.pos;
      if (!load_candidate(location, selected_row))
        throw_error("CORRUPTION", "ordered candidate row missing");
    }
    resp.rows.push_back(project_row(selected_row.data()));
  };
  if (external) {
    external->finish(stmt.has_limit?static_cast<uint64_t>(stmt.offset):0,
        stmt.has_limit?static_cast<uint64_t>(stmt.limit):std::numeric_limits<uint64_t>::max(),emit_result);
    return resp;
  }
  if (ordered) std::sort(results.begin(), results.end(), row_less);
  int64_t start = stmt.has_limit ? stmt.offset : 0;
  int64_t end = stmt.has_limit
                    ? ((stmt.offset > std::numeric_limits<int64_t>::max() - stmt.limit)
                           ? std::numeric_limits<int64_t>::max()
                           : stmt.offset + stmt.limit)
                    : static_cast<int64_t>(results.size());
  if (start < 0) start = 0;
  if (end > static_cast<int64_t>(results.size())) end = static_cast<int64_t>(results.size());
  for (int64_t i=start;i<end;++i) {
    auto &result=results[static_cast<size_t>(i)];
    if (ordered) emit_result(result);
    else resp.rows.push_back(std::move(result.vals));
  }
  return resp;
}


} // namespace mvd::detail
