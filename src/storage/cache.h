#pragma once
#include "layout.h"
#include "compact_cache.h"
#include <list>
#include <mutex>
#include <unordered_map>
namespace mvd {class Table;
class PageCache {
 public:
  explicit PageCache(size_t max_pages = 1024) : max_pages_(max_pages) {}

  // Returns a cached page or loads it from table's file.  Never returns null.
  std::shared_ptr<Page> get(Table *table, uint32_t page_no);
  // Installs a commit image in the cache and marks it dirty.
  void install(Table *table, uint32_t page_no, std::shared_ptr<Page> page);
  void flush_all(const std::unordered_map<uint32_t, std::unique_ptr<Table>> &tables);
  void drop_table(uint32_t table_id);
  void discard();
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
  void insert(uint64_t key, Entry entry);
  void evict_if_needed();
  void write_entries(std::vector<Entry *> entries);

  std::unique_ptr<PageIoBuffer> write_buffer_;
  std::unique_ptr<PageIoBuffer> read_buffer_;

  size_t max_pages_;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Entry> pages_;
  std::list<uint64_t> lru_;  // front = most recently used
  CompactBlockCache compact_;
};

}
