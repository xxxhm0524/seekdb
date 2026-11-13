// Uncommitted row payloads. This file is never needed for crash recovery:
// committed page images are durable in WAL before a commit is acknowledged.
#pragma once
#include "../common.h"
#include "../storage/compact_cache.h"
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
    end_ = end;
    if (end - synced_ >= 8*1024*1024) {
      if (!file_.fsync()) throw_error("IO_ERROR", "cannot sync transaction spool");
      file_.discard_clean_cache(synced_, end - synced_);
      synced_ = end;
    }
    cache_append(offset, row.data(), row.size());
    return offset;
  }
  void read(uint64_t offset, uint32_t length, std::vector<uint8_t> &row) const {
    if (!file_.valid() || offset > end_ || length > end_-offset)
      throw_error("IO_ERROR", "transaction spool row outside file");
    row.resize(length);
    if (read_compact(offset, length, row.data())) {
      next_read_offset_ = offset + length;
      return;
    }
    if (offset < read_offset_ || offset + length > read_offset_ + read_buffer_.size()) {
      int64_t end = file_.size();
      if (end < 0 || offset > static_cast<uint64_t>(end) || length > static_cast<uint64_t>(end)-offset)
        throw_error("IO_ERROR", "transaction spool row outside file");
      // Migration and point reads can visit offsets in arbitrary order.
      // Prefetch only after an observed contiguous row, otherwise a tiny row
      // could force a fresh MiB read on every backward/random access.
      size_t wanted = offset == next_read_offset_ ? 1024*1024 : length;
      size_t window = static_cast<size_t>(std::min<uint64_t>(wanted, static_cast<uint64_t>(end)-offset));
      read_buffer_.resize(window);
      if (!file_.pread_full(read_buffer_.data(), window, offset)) {
        read_buffer_.clear();
        next_read_offset_ = std::numeric_limits<uint64_t>::max();
        throw_error("IO_ERROR", "cannot read transaction spool");
      }
      read_offset_ = offset;
      if (offset + window <= synced_)
        file_.discard_clean_cache(offset, window);
    }
    std::memcpy(row.data(), read_buffer_.data() + (offset-read_offset_), length);
    next_read_offset_ = offset + length;
  }
  void truncate(uint64_t end) {
    read_buffer_.clear();  // A later append may reuse the rewound offsets.
    next_read_offset_ = std::numeric_limits<uint64_t>::max();
    if (!file_.ftruncate(end)) throw_error("IO_ERROR", "cannot rewind transaction spool");
    // ftruncate has already changed the visible bytes, even if the following
    // sync fails. Invalidate decoded/packed tails before reporting that error:
    // a later append can reuse these offsets within the same transaction.
    end_ = end;
    synced_ = std::min(synced_, end);
    uint64_t first = end-end%kCompactPageBytes;
    compact_.erase_if([&](uint64_t offset) { return offset >= first; });
    decoded_offset_ = std::numeric_limits<uint64_t>::max();
    staging_offset_ = first;
    staging_size_ = static_cast<size_t>(end-first);
    staging_valid_ = !staging_size_ || file_.pread_full(staging_.data(), staging_size_, first);
    if (!file_.fsync()) throw_error("IO_ERROR", "cannot rewind transaction spool");
    synced_ = end;
    file_.discard_clean_cache(0, end);
  }
 private:
  void cache_append(uint64_t offset, const uint8_t *bytes, size_t length) {
    // Rows may straddle block boundaries. Cache complete logical blocks,
    // retaining a raw tail, so record offsets and the spool file stay intact.
    if (offset != staging_offset_+staging_size_) {
      staging_offset_ = offset-offset%kCompactPageBytes;
      staging_size_ = static_cast<size_t>(offset-staging_offset_);
      staging_valid_ = !staging_size_ || file_.pread_full(staging_.data(), staging_size_, staging_offset_);
    }
    while (length) {
      size_t take = std::min(length, kCompactPageBytes-staging_size_);
      std::memcpy(staging_.data()+staging_size_, bytes, take);
      staging_size_ += take; bytes += take; length -= take;
      if (staging_size_ == kCompactPageBytes) {
        if (staging_valid_) compact_.put(staging_offset_, staging_.data());
        staging_offset_ += kCompactPageBytes;
        staging_size_ = 0;
        staging_valid_ = true;
      }
    }
  }
  bool read_compact(uint64_t offset, size_t length, uint8_t *output) const {
    while (length) {
      uint64_t block = offset-offset%kCompactPageBytes;
      size_t within = static_cast<size_t>(offset-block);
      size_t take = std::min(length, kCompactPageBytes-within);
      if (staging_valid_ && block == staging_offset_ && within+take <= staging_size_) {
        std::memcpy(output, staging_.data()+within, take);
      } else {
        if (decoded_offset_ != block) {
          decoded_offset_ = std::numeric_limits<uint64_t>::max();
          if (!compact_.get(block, decoded_.data())) return false;
          decoded_offset_ = block;
        }
        std::memcpy(output, decoded_.data()+within, take);
      }
      offset += take; output += take; length -= take;
    }
    return true;
  }
  std::string path_;
  FileHandle file_;
  uint64_t end_ = 0;
  uint64_t synced_ = 0;
  mutable CompactBlockCache compact_;
  std::array<uint8_t, kCompactPageBytes> staging_{};
  uint64_t staging_offset_ = 0;
  size_t staging_size_ = 0;
  bool staging_valid_ = true;
  mutable std::array<uint8_t, kCompactPageBytes> decoded_{};
  mutable uint64_t decoded_offset_ = std::numeric_limits<uint64_t>::max();
  mutable uint64_t read_offset_ = 0;
  mutable uint64_t next_read_offset_ = std::numeric_limits<uint64_t>::max();
  mutable std::vector<uint8_t> read_buffer_;
};
}  // namespace mvd
