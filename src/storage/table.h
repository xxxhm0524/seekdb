#pragma once
#include "layout.h"
#include "file.h"
#include "../model/schema.h"
#include "../transaction/state.h"
#include <unordered_map>
#include <functional>
namespace mvd {class PageCache;namespace detail {class Database;}
class Table {
 public:
  Table(TableSchema schema, std::string data_path, PageCache *cache);
  ~Table();

  const TableSchema &schema() const { return schema_; }
  TableSchema &schema() { return schema_; }
  uint32_t id() const { return schema_.id; }
  const std::string &name() const { return schema_.name; }
  const std::string &data_path() const { return data_path_; }
  PageCache *cache() const { return cache_; }

  // Opens the data file and computes the next allocation position.
  void open(bool rebuild = true, bool fresh = false);
  void close_file();

  uint32_t page_count() const { return page_count_; }
  uint64_t next_pos() const { return next_pos_; }
  void set_next_pos(uint64_t p) { next_pos_ = p; }

  std::shared_ptr<Page> get_page(uint32_t page_no);
  bool load_page_raw(uint32_t page_no, Page &page);
  uint32_t load_pages_raw(uint32_t first_page, uint32_t count, void *pages);
  void refresh_after_recovery();
  bool read_row(uint64_t pos, std::vector<uint8_t> &row);
  void write_page_to_disk(uint32_t page_no, const Page &page);
  void write_pages_to_disk(uint32_t first_page, const void *pages, uint32_t count);
#ifdef __linux__
  void write_scattered_pages(uint32_t first_page, const Page *const *pages, uint32_t count);
#endif
  bool fsync_file();

  // Primary key map (id -> row position), rebuilt on open.
  bool find_pk(int64_t pk, uint64_t &pos) const;
  void put_pk(int64_t pk, uint64_t pos);
  void erase_pk(int64_t pk);
  size_t row_count() const { return pk_pos_.size(); }
  void rebuild_pk_index();

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
  bool compare(const uint8_t *row, const BoundCondition &cond) const;

  // Builds full page images for a transaction overlay.
  void build_commit_images(const TxTable &tx,
                           const std::function<void(PageImage)> &visit);

 private:
  friend class detail::Database;
  TableSchema schema_;
  std::string data_path_;
  PageCache *cache_ = nullptr;
  FileHandle file_;
  uint32_t page_count_ = 0;
  uint64_t next_pos_ = 0;
  bool direct_io_ = false;
  std::unordered_map<int64_t, uint64_t> pk_pos_;
  bool reopen_buffered();
  void locate_end_of_file();
  uint32_t wide_row_page(uint64_t pos) const;
};

}
