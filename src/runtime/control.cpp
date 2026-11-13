#include "../runtime/database.h"
#include "../runtime/shared.h"
#include <algorithm>
#include "../query/vector_index.h"
namespace mvd::detail {
Database::Database(std::string data_dir, size_t cache_pages, size_t tx_memory, size_t ann_min_rows, size_t sort_memory)
    : data_dir_(std::move(data_dir)),
      catalog_path_(data_dir_ + "/catalog.dat"),
      wal_path_(data_dir_ + "/wal.log"),
      cache_(cache_pages),
      tx_(data_dir_+"/txn.tmp",tx_memory,[this](uint32_t id,const RowState& state,std::vector<uint8_t>& row){tables_.at(id)->materialize_state(state,nullptr,row);}), ann_min_rows_(ann_min_rows), sort_memory_(sort_memory) {}

Database::~Database() {
  try {
    shutdown();
  } catch (...) {
  }
}

void Database::open() {
  if(lifecycle_==Lifecycle::ready)return;
  if(lifecycle_==Lifecycle::recovery_required){recover_commit_publication();return;}
  tx_.clear();cache_.discard();tables_.clear();table_ids_.clear();
  indexes_.clear();vector_search_bytes_=0;wal_.close();
  bootstrap();
}

void Database::bootstrap() {
  make_dir(data_dir_);
  std::remove((data_dir_ + "/txn.tmp").c_str());
  std::remove((data_dir_ + "/sort.tmp").c_str());
  load_catalog();
  // Create/open all tables from catalog.
  for (auto &kv : tables_) kv.second->open(false);
  if (!wal_.open(wal_path_))
    throw_error("IO_ERROR", "cannot open WAL");
  wal_.recover([&](uint32_t table_id, uint32_t page_no,
                   const std::shared_ptr<Page> &page) {
    auto it = tables_.find(table_id);
    if (it == tables_.end()) return;
    it->second->write_page_to_disk(page_no, *page);
  });
  for (auto &kv : tables_) {
    kv.second->refresh_after_recovery();
    if (!kv.second->fsync_file()) throw_error("IO_ERROR", "failed to sync recovered table");
  }
  // WAL is fully applied and durable now.
  if (!wal_.truncate()) throw_error("IO_ERROR", "failed to truncate recovered WAL");
  for (auto &entry : tables_)
    if (entry.second->schema().vector_index_created) rebuild_vector_index(*entry.second);
  lifecycle_=Lifecycle::ready;
}

void Database::shutdown() {
  if (lifecycle_==Lifecycle::recovery_required) {
    // The committed WAL is authoritative when cache/map installation failed.
    // Do not flush a partial view and truncate the only complete copy.
    rollback_transaction();
    wal_.close();
    return;
  }
  if (lifecycle_==Lifecycle::closed) { wal_.close(); return; }
  if (tx_.active()) rollback_transaction();
  if (!tables_.empty()) {
    cache_.flush_all(tables_);
  }
  if (!wal_.truncate()) throw_error("IO_ERROR", "failed to truncate WAL at shutdown");
  wal_.close();
  lifecycle_=Lifecycle::closed;
}

void Database::recover_commit_publication() {
  tx_.clear();
  cache_.discard();
  tables_.clear();
  table_ids_.clear();
  indexes_.clear();vector_search_bytes_ = 0;
  wal_.close();
  // Leave the flag set if replay itself fails. Every subsequent request must
  // retry recovery instead of observing partially rebuilt maps or caches.
  bootstrap();
}

Table *Database::find_table(const std::string &name) {
  std::string lower = lower_copy(name);
  auto it = table_ids_.find(lower);
  if (it == table_ids_.end()) return nullptr;
  auto t = tables_.find(it->second);
  return t == tables_.end() ? nullptr : t->second.get();
}

Table &Database::require_table(const std::string &name) {
  Table *t = find_table(name);
  if (!t) throw_error("TABLE_NOT_FOUND", "table not found: " + name);
  return *t;
}


} // namespace mvd::detail
