#include "storage.hpp"
#include <memory>

namespace mvd {
void Storage::cleanup_compaction_scratch() {
    const auto scratch = directory_ / ".compact-work";
    std::filesystem::remove(scratch / "database.mvd");
    std::error_code ignored;
    std::filesystem::remove(scratch, ignored);
}
bool Storage::compact_if_needed() {
    // Called only between transactions. The old file is the recovery source
    // until a fully synced replacement has been atomically installed.
    if (end_ != durable_end_ || end_ < (64ULL << 20)) return false;
    uint64_t estimate = PAGE_SIZE * 2;
    for (const auto& table : committed) {
        if (table.count > (UINT64_MAX - estimate) / 64) return false;
        estimate += table.count * 64;
        if (table.live_bytes > UINT64_MAX - estimate) return false;
        estimate += table.live_bytes;
    }
    if (estimate > UINT64_MAX / 2 || end_ <= std::max<uint64_t>(64ULL << 20, estimate * 2)) return false;

    const auto scratch = directory_ / ".compact-work";
    cleanup_compaction_scratch();
    std::filesystem::create_directories(scratch);
    Catalog replacement = committed;
    uint64_t replacement_end = 0, replacement_generation = 0;
    {
        Storage destination(scratch);
        for (size_t i = 0; i < replacement.size(); ++i) {
            const auto& source = committed[i]; auto& target = replacement[i];
            target.root = target.count = target.live_bytes = 0;
            BTree reader(*this);
            auto writer = std::make_unique<BTree>(destination);
            size_t batch = 0;
            reader.scan(source.root, [&](int64_t key, Ref ref) {
                Ref copied = destination.append(read(ref));
                writer->put(target.root, key, copied);
                ++target.count; target.live_bytes += copied.length;
                // Bound the set of statement-owned pages even for huge tables.
                if (++batch == 1024) { writer = std::make_unique<BTree>(destination); batch = 0; }
                return true;
            });
            if (target.count != source.count || target.live_bytes != source.live_bytes)
                throw IoError("inconsistent live counts during compaction");
        }
        destination.commit(replacement);
        replacement_end = destination.end_;
        replacement_generation = destination.generation_;
    }
    file_.replace_with(scratch / "database.mvd");
    pages_.clear(); lru_.clear();
    committed.swap(replacement);
    end_ = durable_end_ = replacement_end; generation_ = replacement_generation;
    cleanup_compaction_scratch();
    return true;
}
}
