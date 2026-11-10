// Uncommitted row payloads. This file is never needed for crash recovery:
// committed page images are durable in WAL before a commit is acknowledged.
#pragma once
#include "common.h"
namespace mvd {
class TxSpool {
 public:
  explicit TxSpool(std::string path) : path_(std::move(path)) {
    file_ = FileHandle::open(path_, O_RDWR | O_CREAT | O_TRUNC | O_APPEND);
    if (!file_.valid()) throw_error("IO_ERROR", "cannot open transaction spool");
  }
  ~TxSpool() { file_.close(); std::remove(path_.c_str()); }
  uint64_t size() const {
    int64_t value = file_.size();
    if (value < 0) throw_error("IO_ERROR", "cannot stat transaction spool");
    return static_cast<uint64_t>(value);
  }
  uint64_t append(const std::vector<uint8_t> &row) {
    uint64_t offset = size();
    if (!file_.append(row.data(), row.size())) throw_error("IO_ERROR", "cannot write transaction spool");
    uint64_t end = offset + row.size();
    if (end - synced_ >= 8*1024*1024) {
      if (!file_.fsync()) throw_error("IO_ERROR", "cannot sync transaction spool");
      file_.discard_clean_cache(synced_, end - synced_);
      synced_ = end;
    }
    return offset;
  }
  void read(uint64_t offset, uint32_t length, std::vector<uint8_t> &row) const {
    row.resize(length);
    if (!file_.pread_full(row.data(), row.size(), offset))
      throw_error("IO_ERROR", "cannot read transaction spool");
    if (offset + length <= synced_)
      file_.discard_clean_cache(offset, length);
  }
  void truncate(uint64_t end) {
    if (!file_.ftruncate(end) || !file_.fsync()) throw_error("IO_ERROR", "cannot rewind transaction spool");
    file_.discard_clean_cache(0, end);
    synced_ = end;
  }
 private:
  std::string path_;
  FileHandle file_;
  uint64_t synced_ = 0;
};
}  // namespace mvd
