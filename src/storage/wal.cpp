#include "wal.h"
#include "../core/error.h"
namespace mvd {
bool Wal::open(const std::string &path) {
  reset_read_cache();
  int flags = O_RDWR | O_CREAT;
#ifndef __linux__
  flags |= O_APPEND;
#endif
  file_ = FileHandle::open(path, flags);
  end_offset_ = file_.size();
  return end_offset_ >= 0;
}

void Wal::close() { reset_read_cache(); file_.close(); }

bool Wal::read_cached(void *data, size_t length, uint64_t offset, uint64_t file_size) {
  if (!file_.valid() || offset > file_size || length > file_size - offset) return false;
  if (!length) return true;
  uint64_t within = offset >= read_cache_begin_ ? offset - read_cache_begin_
      : std::numeric_limits<uint64_t>::max();
  if (within > read_cache_.size() || length > read_cache_.size() - static_cast<size_t>(within)) {
    bool sequential = offset >= next_read_offset_ &&
        offset - next_read_offset_ <= sizeof(WalRecordHeader) + 12;
    size_t wanted = sequential ? std::max<size_t>(length, 4*1024*1024) : length;
    size_t window = static_cast<size_t>(std::min<uint64_t>(wanted, file_size - offset));
    read_cache_.resize(window);
    if (!file_.pread_full(read_cache_.data(), window, offset)) {
      reset_read_cache();
      return false;
    }
    read_cache_begin_ = offset;
    within = 0;
  }
  std::memcpy(data, read_cache_.data() + static_cast<size_t>(within), length);
  next_read_offset_ = offset + length;
  return true;
}

bool Wal::read_record(uint64_t offset, uint64_t size, WalRecordHeader &h,
                      std::string &payload) {
  if (offset + sizeof(h) > size) return false;
  if (!read_cached(&h, sizeof(h), offset, size))
    throw_error("IO_ERROR", "cannot read WAL header");
  if (h.magic != 0x4D564457 || h.payload_len > 1024 * 1024 ||
      offset + sizeof(h) + h.payload_len > size) return false;
  payload.resize(h.payload_len);
  if (!payload.empty() && !read_cached(payload.data(), payload.size(), offset + sizeof(h), size))
    throw_error("IO_ERROR", "cannot read WAL record");
  return crc32(payload.data(), payload.size()) == h.crc;
}

std::shared_ptr<Page> Wal::read_page_image(uint64_t offset) {
  auto page = make_page();
  if (!read_cached(page.get(), kPageSize, offset + sizeof(WalRecordHeader) + 12,
                   static_cast<uint64_t>(end_offset_)))
    throw_error("IO_ERROR", "cannot read committed WAL page");
  return page;
}

bool Wal::append_page(uint64_t txid, uint32_t table_id, uint32_t page_no,
                      const Page &page, uint32_t *payload_crc) {
  if (!file_.valid()) return false;
  uint32_t metadata[] = {table_id, page_no, kPageSize};
  WalRecordHeader h{};
  h.type = 1;
  h.payload_len = sizeof(metadata) + kPageSize;
  h.txid = txid;
  h.crc = crc32_update(crc32(metadata, sizeof(metadata)), &page, kPageSize);
  const int64_t record_bytes = sizeof(h) + h.payload_len;
  if (end_offset_ > std::numeric_limits<int64_t>::max() - record_bytes) return false;

#ifdef __linux__
  // WAL only appends into new extents. A later record/COMMIT materializes
  // skipped zeros without changing the full page image or its checksum.
  const auto *bytes = reinterpret_cast<const uint8_t *>(&page);
  const uint64_t page_offset = end_offset_ + sizeof(h) + sizeof(metadata);
  bool prefix_written = false;
  bool written = write_nonzero_extents(&page, kPageSize, page_offset,
      [&](size_t begin, size_t length) {
        if (!prefix_written && begin == 0) {
          iovec buffers[] = {{&h, sizeof(h)}, {metadata, sizeof(metadata)},
              {const_cast<uint8_t *>(bytes), length}};
          prefix_written = true;
          return file_.pwritev_full(buffers, 3, end_offset_);
        }
        if (!prefix_written) {
          iovec prefix[] = {{&h, sizeof(h)}, {metadata, sizeof(metadata)}};
          if (!file_.pwritev_full(prefix, 2, end_offset_)) return false;
          prefix_written = true;
        }
        return file_.pwrite_full(bytes + begin, length, page_offset + begin);
      });
  if (written && !prefix_written) {
    iovec prefix[] = {{&h, sizeof(h)}, {metadata, sizeof(metadata)}};
    written = file_.pwritev_full(prefix, 2, end_offset_);
  }
#else
  std::array<uint8_t, sizeof(h) + sizeof(metadata)> prefix;
  std::memcpy(prefix.data(), &h, sizeof(h));
  std::memcpy(prefix.data() + sizeof(h), metadata, sizeof(metadata));
  bool written = file_.append(prefix.data(), prefix.size()) && file_.append(&page, kPageSize);
#endif
  if (!written) {
    // A partial record can invalidate the tail for recovery.
    close();
    return false;
  }
  end_offset_ += record_bytes;
  if (payload_crc) *payload_crc = h.crc;
  return true;
}

bool Wal::append_commit(uint64_t txid) {
  WalRecordHeader h{};
  h.type = 2;
  h.payload_len = 0;
  h.txid = txid;
  h.crc = crc32(nullptr, 0);
  if (!file_.valid() ||
      end_offset_ > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(sizeof(h)))
    return false;
#ifdef __linux__
  bool written = file_.pwrite_full(&h, sizeof(h), end_offset_);
#else
  bool written = file_.append(&h, sizeof(h));
#endif
  if (!written) {
    close();
    return false;
  }
  end_offset_ += sizeof(h);
  return true;
}

bool Wal::sync() {
  if (file_.fsync()) return true;
  close();
  return false;
}

bool Wal::truncate() {
  reset_read_cache();
  if (!file_.ftruncate(0)) return false;
  end_offset_ = 0;
  return sync();
}


} // namespace mvd
