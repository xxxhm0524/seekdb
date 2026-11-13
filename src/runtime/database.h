#pragma once
#include "../storage/store.h"
#include "../syntax/ast.h"
#include "../model/result.h"
#include "../transaction/workspace.h"
namespace mvd {class VectorIndex;}
namespace mvd::detail {
class Database {
 public:
  Database(std::string data_dir, size_t cache_pages = 1024, size_t tx_memory = 8*1024*1024,
          size_t ann_min_rows = 40000, size_t sort_memory = 16*1024*1024);
  ~Database();

  void open();
  void shutdown();

  Response execute(const std::string &sql);

  bool ann_mode() const { return ann_mode_; }
  void set_ann_mode(bool v) { ann_mode_ = v; }
  bool in_transaction() const { return tx_.active(); }

 private:
  std::string data_dir_;
  std::string catalog_path_;
  std::string wal_path_;
  PageCache cache_;
  Wal wal_;
  std::unordered_map<uint32_t, std::unique_ptr<Table>> tables_;
  std::unordered_map<std::string, uint32_t> table_ids_;
  TransactionWorkspace tx_;
  size_t ann_min_rows_;
  size_t sort_memory_;
  bool ann_mode_ = false;
  enum class Lifecycle { closed, ready, recovery_required };
  Lifecycle lifecycle_=Lifecycle::closed;
  void bootstrap();
  void recover_commit_publication();
  uint32_t next_table_id_ = 1;
  struct IndexState {std::unique_ptr<VectorIndex> index;size_t bytes=0;bool unavailable=false;};
  std::unordered_map<uint32_t,IndexState> indexes_;
  size_t vector_search_bytes_ = 0;
  VectorIndex* index_handle(uint32_t id)const;
  bool index_unavailable(uint32_t id)const;
  void rebuild_vector_index(Table &table);
  void update_vector_index(Table &table, const TxTable &changes);
  void drop_vector_index(Table &table);

  // catalog
  void load_catalog();
  void save_catalog(uint32_t excluded_table = 0);
  Table *find_table(const std::string &name);
  Table &require_table(const std::string &name);

  // statement execution
  Response exec_create_table(CreateTableStmt &stmt);
  Response exec_drop_table(DropTableStmt &stmt);
  Response exec_create_vector_index(CreateVectorIndexStmt &stmt);
  Response exec_insert(InsertStmt &stmt);
  Response exec_select(SelectStmt &stmt);
  Response exec_update(UpdateStmt &stmt);
  Response exec_delete(DeleteStmt &stmt);
  Response exec_set(SetStmt &stmt);
  Response exec_begin();
  Response exec_commit();
  Response exec_rollback();

  // transaction helpers
  void commit_transaction();
  void rollback_transaction();
  void maybe_checkpoint();

  // values
  OutValue out_value_from_row(const Table &table, const uint8_t *row, int col_index);

  // scans
  struct VisibleRow {
    const uint8_t *row = nullptr;
    std::shared_ptr<Page> page;  // keeps base row alive
    int64_t pk = 0;
    uint64_t pos = 0;
    bool from_overlay = false;
  };
  // The callback must consume the row before returning. At most one heap page
  // is pinned, including scans that merge an active transaction's overlay.
  void for_each_visible(Table &table, const std::vector<BoundCondition> &where,
                        const std::function<bool(const VisibleRow &)> &visit);
  void for_each_visible_key(Table &table, const std::vector<BoundCondition> &where,
                            const std::function<void(int64_t, uint64_t)> &visit);
  bool row_matches(const Table &table, const uint8_t *row,
                   const std::vector<BoundCondition> &where) const;

};

}
