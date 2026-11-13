#pragma once
#include "layout.h"
#include "file.h"
#include "../core/error.h"
#include "../core/crc32.h"
#include <unordered_set>
namespace mvd {
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
                   const Page &page, uint32_t *payload_crc = nullptr);
  bool append_commit(uint64_t txid);
  bool sync();
  void discard_clean_cache(uint64_t offset, uint64_t length) {
    file_.discard_clean_cache(offset, length);
  }
  bool truncate();
  std::shared_ptr<Page> read_page_image(uint64_t offset);
  int64_t size() const { return end_offset_; }
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
  bool read_cached(void *data, size_t length, uint64_t offset, uint64_t file_size);
  void reset_read_cache() {
    read_cache_.clear();
    read_cache_begin_ = 0;
    next_read_offset_ = std::numeric_limits<uint64_t>::max();
  }
  bool read_record(uint64_t offset, uint64_t size, WalRecordHeader &header,
                    std::string &payload);
  FileHandle file_;
  std::vector<uint8_t> read_cache_;
  uint64_t read_cache_begin_ = 0;
  uint64_t next_read_offset_ = std::numeric_limits<uint64_t>::max();
  // The next record begins after the full logical image. A zero suffix may
  // remain beyond the physical end of the file until the next record is written.
  int64_t end_offset_ = 0;
  uint64_t next_txid_ = 1;
};

}
