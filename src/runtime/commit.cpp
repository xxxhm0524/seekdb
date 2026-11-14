#include "../runtime/database.h"
#include "../runtime/shared.h"
#include <algorithm>
namespace mvd::detail {
void Database::commit_transaction() {
  if (!tx_.active()) return;
  uint64_t txid = wal_.allocate_txid();
  uint64_t sync_offset = static_cast<uint64_t>(wal_.size());
  std::vector<PageImage> all_images;
  std::vector<uint8_t> compact_images;
  bool compact_cache_enabled = true;
  for (auto &kv : tx_.tables()) {
    auto it = tables_.find(kv.first);
    if (it == tables_.end()) continue;
    it->second->build_commit_images(kv.second, [&](PageImage img) {
      img.wal_offset = static_cast<uint64_t>(wal_.size());
      if (!wal_.append_page(txid, img.table_id, img.page_no, *img.page, &img.wal_crc))
        throw_error("IO_ERROR", "failed to write WAL page image");
      uint64_t written = static_cast<uint64_t>(wal_.size());
      if (written - sync_offset >= kWalWriteWindow) {
        // Persisting an uncommitted prefix is safe: recovery still requires
        // the COMMIT record. Bound dirty kernel cache as well as user buffers.
        if (!wal_.sync()) throw_error("IO_ERROR", "failed to sync WAL prefix");
        wal_.discard_clean_cache(sync_offset, written - sync_offset);
        sync_offset = written;
      }
      // Keep the first MiB raw. Highly compressible later images may share
      // a bounded arena; dense pages and exhausted/failed cache allocations
      // retain the authoritative WAL-offset path.
      if (all_images.size() >= 16) {
        if (compact_cache_enabled && compact_images.size()+kCompactPageMaxBytes <= kCompactCommitBudget) {
          try {
            auto packed = pack_page_image(img.page.get());
            if (!packed.empty()) {
              if (!compact_images.capacity()) compact_images.reserve(kCompactCommitBudget);
              uint32_t begin = static_cast<uint32_t>(compact_images.size());
              compact_images.insert(compact_images.end(), packed.begin(), packed.end());
              img.compact_offset = begin;
              img.compact_size = static_cast<uint32_t>(packed.size());
            }
          } catch (const std::bad_alloc &) {
            compact_cache_enabled = false;
          }
        }
        img.page.reset();
      }
      all_images.push_back(std::move(img));
    });
  }
  // A failed commit-record write or sync can also leave its outcome uncertain.
  // Replay, rather than later flushing a potentially incomplete in-memory view,
  // decides whether this transaction has a complete commit record.
  lifecycle_=Lifecycle::recovery_required;
  if (!all_images.empty()) {
    if (!wal_.append_commit(txid) || !wal_.sync())
      throw_error("IO_ERROR", "failed to commit WAL");
  } else {
    // Still emit a commit record for an empty transaction; harmless.
    if (!wal_.append_commit(txid) || !wal_.sync())
      throw_error("IO_ERROR", "failed to commit WAL");
  }
  uint64_t committed_end = static_cast<uint64_t>(wal_.size());
  wal_.discard_clean_cache(sync_offset, committed_end - sync_offset);
  // Install images and update in-memory maps.
  uint64_t replay_offset = 0;
  for (auto &img : all_images) {
    auto it = tables_.find(img.table_id);
    if (it != tables_.end()) {
      if (!img.page && img.compact_size && img.compact_offset <= compact_images.size() &&
          img.compact_size <= compact_images.size()-img.compact_offset) {
        auto restored = make_page();
        std::string_view packed(reinterpret_cast<const char *>(compact_images.data()+img.compact_offset),
                                img.compact_size);
        if (unpack_page_image(packed, restored.get())) {
          uint32_t metadata[] = {img.table_id, img.page_no, kPageSize};
          uint32_t checksum = crc32_update(crc32(metadata, sizeof(metadata)), restored.get(), kPageSize);
          if (checksum == img.wal_crc) img.page = std::move(restored);
        }
      }
      if (!img.page) {
        img.page = wal_.read_page_image(img.wal_offset);
        uint64_t consumed = img.wal_offset + sizeof(WalRecordHeader) + 12 + kPageSize;
        if (consumed - replay_offset >= kWalWriteWindow) {
          wal_.discard_clean_cache(replay_offset, consumed - replay_offset);
          replay_offset = consumed;
        }
      }
      cache_.install(it->second.get(), img.page_no, img.page);
      img.page.reset();
    }
  }
  wal_.discard_clean_cache(replay_offset, committed_end - replay_offset);
  std::vector<uint8_t>().swap(compact_images);
  for (auto &kv : tx_.tables()) {
    auto it = tables_.find(kv.first);
    if (it == tables_.end()) continue;
    Table &table = *it->second;
    table.set_next_pos(kv.second.next_pos);
    for (auto &rk : kv.second.rows) {
      const RowState &state = rk.second;
      if(table.schema().pages_per_row>1&&(!state.deleted||state.existed)){
        uint32_t end=table.wide_row_page(state.pos)+table.schema().pages_per_row;
        table.page_count_=std::max(table.page_count_,end);
      }
      if (state.deleted) {
        if (state.existed) table.erase_pk(rk.first);
      } else {
        table.put_pk(rk.first, state.pos);
      }
    }
    update_vector_index(table, kv.second);
  }
  tx_.clear();
  maybe_checkpoint();
  lifecycle_=Lifecycle::ready;
}

void Database::maybe_checkpoint() {
  if (wal_.size() < kWalCheckpointBytes) return;
  cache_.flush_all(tables_);
  if (!wal_.truncate()) throw_error("IO_ERROR", "failed to truncate checkpoint WAL");
}

void Database::rollback_transaction() { tx_.clear(); }


} // namespace mvd::detail
