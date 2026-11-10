// MiniVectorDB - engine interfaces
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "sql.h"

namespace mvd {

constexpr uint32_t kPageSize = 65536;
constexpr uint32_t kPageHeaderSize = 16;
constexpr uint32_t kPageMagic = 0x5044564D;  // "MVDP"

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

struct alignas(4096) Page {
  uint32_t magic = kPageMagic;
  uint32_t nrows = 0;
  uint32_t page_no = 0;
  uint32_t crc = 0;
  uint8_t payload[kPageSize - kPageHeaderSize] = {0};

  uint8_t *row_ptr(uint32_t slot, uint32_t row_size) {
    return payload + static_cast<size_t>(slot) * row_size;
  }
  const uint8_t *row_ptr(uint32_t slot, uint32_t row_size) const {
    return payload + static_cast<size_t>(slot) * row_size;
  }
  uint32_t capacity(uint32_t row_size) const {
    return (kPageSize - kPageHeaderSize) / row_size;
  }
};

// Reuse aligned page buffers across cache eviction, WAL replay and commits.
// Returning them to the general allocator on every scan can fragment its heap
// when small transaction nodes are allocated between aligned 64 KiB pages.
std::shared_ptr<Page> make_page(const Page *source = nullptr);

struct OutValue {
  enum class Kind { INT, DOUBLE, STRING, VECTOR } kind = Kind::INT;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<float> vec;
};

struct Response {
  bool ok = true;
  bool is_query = false;
  bool has_affected = false;
  int64_t affected_rows = 0;
  std::vector<std::string> columns;
  std::vector<std::vector<OutValue>> rows;
  std::string error_code;
  std::string error_message;
};

using RowPatch = std::vector<std::pair<int, Value>>;

struct RowState {
  bool existed = false;   // existed in the base table when first touched
  bool deleted = false;
  uint64_t pos = 0;
  std::vector<uint8_t> row;
  // UPDATE literals are shared across matching records. Existing rows retain
  // patches instead of a full fixed-width copy of every VARCHAR/VECTOR slot.
  std::vector<std::shared_ptr<const RowPatch>> patches;
};

struct TxTable {
  std::unordered_map<int64_t, RowState> rows;
  uint64_t next_pos = 0;
};

struct Transaction {
  bool active = false;
  std::unordered_map<uint32_t, TxTable> tables;
  void clear() {
    active = false;
    tables.clear();
  }
};

// A full page image produced while committing a transaction.
struct PageImage {
  uint32_t table_id = 0;
  uint32_t page_no = 0;
  std::shared_ptr<Page> page;
  uint64_t wal_offset = 0;
};

class Table;

class PageCache {
 public:
  explicit PageCache(size_t max_pages = 1024) : max_pages_(max_pages) {}

  // Returns a cached page or loads it from table's file.  Never returns null.
  std::shared_ptr<Page> get(Table *table, uint32_t page_no);
  // Installs a commit image in the cache and marks it dirty.
  void install(Table *table, uint32_t page_no, std::shared_ptr<Page> page);
  void flush_all(const std::unordered_map<uint32_t, std::unique_ptr<Table>> &tables);
  void drop_table(uint32_t table_id);
  size_t size() const;

 private:
  struct Entry {
    std::shared_ptr<Page> page;
    std::list<uint64_t>::iterator lru_it;
    bool dirty = false;
    Table *owner = nullptr;
    uint32_t page_no = 0;
  };
  uint64_t key(uint32_t table_id, uint32_t page_no) const {
    return (static_cast<uint64_t>(table_id) << 32) | page_no;
  }
  void touch(std::unordered_map<uint64_t, Entry>::iterator it);
  void evict_if_needed();
  void write_entries(std::vector<Entry *> entries);

  struct alignas(4096) WriteBuffer {
    uint8_t bytes[kPageSize * 16];
  };
  std::unique_ptr<WriteBuffer> write_buffer_;

  size_t max_pages_;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Entry> pages_;
  std::list<uint64_t> lru_;  // front = most recently used
};

class Table {
 public:
  Table(TableSchema schema, std::string data_path, PageCache *cache);

  const TableSchema &schema() const { return schema_; }
  TableSchema &schema() { return schema_; }
  uint32_t id() const { return schema_.id; }
  const std::string &name() const { return schema_.name; }
  const std::string &data_path() const { return data_path_; }
  PageCache *cache() const { return cache_; }

  // Opens the data file and computes the next allocation position.
  void open(bool rebuild = true);
  void close_file();

  uint32_t page_count() const { return page_count_; }
  uint64_t next_pos() const { return next_pos_; }
  void set_next_pos(uint64_t p) { next_pos_ = p; }
  uint64_t allocate_pos() { return next_pos_++; }

  std::shared_ptr<Page> get_page(uint32_t page_no) {
    return cache_->get(this, page_no);
  }
  bool load_page_raw(uint32_t page_no, Page &page);
  void refresh_after_recovery();
  bool read_row(uint64_t pos, std::vector<uint8_t> &row);
  void write_page_to_disk(uint32_t page_no, const Page &page);
  void write_pages_to_disk(uint32_t first_page, const void *pages, uint32_t count);
  void flush_all_dirty();
  bool fsync_file();

  // Primary key map (id -> row position), rebuilt on open.
  bool find_pk(int64_t pk, uint64_t &pos) const;
  void put_pk(int64_t pk, uint64_t pos);
  void erase_pk(int64_t pk);
  size_t row_count() const { return pk_pos_.size(); }
  void rebuild_pk_index();
  void clear_pk_index() { pk_pos_.clear(); }

  const uint8_t *row_data(const Page &page, uint32_t slot) const {
    return page.row_ptr(slot, schema_.row_size);
  }
  uint8_t *row_data(Page &page, uint32_t slot) const {
    return page.row_ptr(slot, schema_.row_size);
  }
  bool row_is_valid(const uint8_t *row) const { return row[0] != 0; }
  int64_t row_id(const uint8_t *row) const {
    int64_t v;
    std::memcpy(&v, row + 8, sizeof(v));
    return v;
  }
  void set_row_id(uint8_t *row, int64_t id) const {
    std::memcpy(row + 8, &id, sizeof(id));
  }
  void set_row_valid(uint8_t *row, bool valid) const { row[0] = valid ? 1 : 0; }

  // Encodes/decodes values in a row.
  void encode_value(uint8_t *row, int col_index, const Value &value);
  void apply_patches(uint8_t *row, const RowState &state);
  void materialize_state(const RowState &state, const uint8_t *base,
                          std::vector<uint8_t> &row);
  Value decode_value(const uint8_t *row, int col_index) const;
  int column_index(const std::string &name) const;
  bool compare(const uint8_t *row, const Condition &cond) const;

  // Builds full page images for a transaction overlay.
  void build_commit_images(const TxTable &tx,
                           const std::function<void(PageImage)> &visit);

 private:
  TableSchema schema_;
  std::string data_path_;
  PageCache *cache_ = nullptr;
  FileHandle file_;
  uint32_t page_count_ = 0;
  uint64_t next_pos_ = 0;
  bool direct_io_ = false;
  std::unordered_map<int64_t, uint64_t> pk_pos_;
  bool reopen_buffered();
};

struct WalRecordHeader {
  uint32_t magic = 0x4D564457;  // "MVDW"
  uint8_t type = 0;             // 1 = page image, 2 = commit
  uint8_t reserved[3] = {0, 0, 0};
  uint32_t payload_len = 0;
  uint64_t txid = 0;
  uint32_t crc = 0;
};
static_assert(sizeof(WalRecordHeader) == 32, "wal header layout");

class Wal {
 public:
  bool open(const std::string &path);
  void close();
  bool append_page(uint64_t txid, uint32_t table_id, uint32_t page_no,
                   const Page &page);
  bool append_commit(uint64_t txid);
  bool sync();
  bool truncate();
  std::shared_ptr<Page> read_page_image(uint64_t offset);
  int64_t size() const { return file_.size(); }
  uint64_t next_txid() const { return next_txid_; }
  uint64_t allocate_txid() { return next_txid_++; }

  // Replays committed page images.  `apply` is called for each committed
  // page image in WAL order.
  template <typename ApplyFn>
  void recover(ApplyFn apply) {
    int64_t size = file_.size();
    if (size < 0) throw_error("IO_ERROR", "cannot stat WAL");
    std::string payload;
    std::unordered_set<uint64_t> committed;
    uint64_t pos = 0;
    WalRecordHeader h;
    while (read_record(pos, static_cast<uint64_t>(size), h, payload)) {
      if (h.type == 2 && h.payload_len == 0) committed.insert(h.txid);
      if (h.txid >= next_txid_) next_txid_ = h.txid + 1;
      pos += sizeof(h) + h.payload_len;
    }
    pos = 0;
    while (read_record(pos, static_cast<uint64_t>(size), h, payload)) {
      if (h.type == 1 && committed.count(h.txid) &&
          h.payload_len >= 12) {
        uint32_t table_id, page_no, page_size;
        std::memcpy(&table_id, payload.data(), 4);
        std::memcpy(&page_no, payload.data() + 4, 4);
        std::memcpy(&page_size, payload.data() + 8, 4);
        if (page_size == kPageSize && h.payload_len == 12 + kPageSize) {
          auto page = make_page();
          std::memcpy(page.get(), payload.data() + 12, kPageSize);
          apply(table_id, page_no, page);
        }
      }
      pos += sizeof(h) + h.payload_len;
    }
  }

 private:
  bool read_record(uint64_t offset, uint64_t size, WalRecordHeader &header,
                    std::string &payload);
  FileHandle file_;
  uint64_t next_txid_ = 1;
};

class Engine {
 public:
  Engine(std::string data_dir, size_t cache_pages = 1024);
  ~Engine();

  void open();
  void shutdown();

  Response execute(const std::string &sql);

  bool ann_mode() const { return ann_mode_; }
  void set_ann_mode(bool v) { ann_mode_ = v; }
  bool in_transaction() const { return tx_.active; }

 private:
  std::string data_dir_;
  std::string catalog_path_;
  std::string wal_path_;
  PageCache cache_;
  Wal wal_;
  std::unordered_map<uint32_t, std::unique_ptr<Table>> tables_;
  std::unordered_map<std::string, uint32_t> table_ids_;
  Transaction tx_;
  bool ann_mode_ = false;
  bool opened_ = false;
  uint32_t next_table_id_ = 1;

  // catalog
  void load_catalog();
  void save_catalog();
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
  TxTable &tx_table_ref(uint32_t table_id) {
    auto [it, inserted] = tx_.tables.try_emplace(table_id);
    if (inserted) it->second.next_pos = tables_.at(table_id)->next_pos();
    return it->second;
  }
  const TxTable *find_tx_table(uint32_t table_id) const;
  void commit_transaction();
  void rollback_transaction();
  void maybe_checkpoint();

  // values
  Value coerce_value(const TableSchema &schema, int col_index, const Value &v);
  void compile_conditions(Table &table, std::vector<Condition> &where);
  OutValue out_value_from_row(const Table &table, const uint8_t *row, int col_index);
  std::string format_vector(const float *v, int dim) const;

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
  void for_each_visible(Table &table, const std::vector<Condition> &where,
                        const std::function<bool(const VisibleRow &)> &visit);
  bool row_matches(const Table &table, const uint8_t *row,
                   const std::vector<Condition> &where) const;

};

}  // namespace mvd
