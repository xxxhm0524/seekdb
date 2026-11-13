#include "cache.h"
#include "table.h"
#include "../core/error.h"
#include <algorithm>
#include <array>
namespace mvd {
size_t PageCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pages_.size();
}

void PageCache::discard() {
  std::lock_guard<std::mutex> lock(mutex_);
  pages_.clear();
  lru_.clear();
  compact_.erase_if([](uint64_t) { return true; });
  read_buffer_.reset();
  write_buffer_.reset();
}

void PageCache::touch(std::unordered_map<uint64_t, Entry>::iterator it) {
  // Relink the existing node: erase + push_front could leave a dangling
  // iterator if the replacement node allocation failed.
  lru_.splice(lru_.begin(), lru_, it->second.lru_it);
}

void PageCache::insert(uint64_t k, Entry entry) {
  lru_.push_front(k);
  entry.lru_it = lru_.begin();
  try {
    pages_.emplace(k, std::move(entry));
  } catch (...) {
    lru_.pop_front();
    throw;
  }
}

std::shared_ptr<Page> PageCache::get(Table *table, uint32_t page_no) {
  uint64_t k = key(table->id(), page_no);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = pages_.find(k);
  if (it != pages_.end()) {
    touch(it);
    return it->second.page;
  }
  if (!compact_.empty()) {
    auto page = make_page();
    if (compact_.get(k, page.get())) {
      Entry entry;
      entry.page = page; entry.owner = table; entry.page_no = page_no;
      insert(k, std::move(entry));
      evict_if_needed();
      return page;
    }
  }
  if (!read_buffer_) read_buffer_ = std::make_unique<PageIoBuffer>();
  uint32_t count = static_cast<uint32_t>(std::min<size_t>(kIoBatchPages,
      std::max<size_t>(1, max_pages_ / 2)));
  count = table->load_pages_raw(page_no, count, read_buffer_->bytes);
  std::shared_ptr<Page> requested;
  for (uint32_t i = 0; i < std::max<uint32_t>(1, count); ++i) {
    uint32_t current = page_no + i;
    uint64_t current_key = key(table->id(), current);
    // A prefetched disk image must never replace a newer dirty cache page.
    if (pages_.find(current_key) != pages_.end()) continue;
    auto page = make_page();
    if (count)
      std::memcpy(page.get(), read_buffer_->bytes + static_cast<size_t>(i) * kPageSize, kPageSize);
    else
      page->page_no = current;
    Entry entry;
    entry.page = page;
    entry.owner = table;
    entry.page_no = current;
    insert(current_key, std::move(entry));
    if (current == page_no) requested = std::move(page);
  }
  evict_if_needed();
  return requested;
}

void PageCache::install(Table *table, uint32_t page_no,
                        std::shared_ptr<Page> page) {
  uint64_t k = key(table->id(), page_no);
  std::lock_guard<std::mutex> lock(mutex_);
  compact_.erase(k);
  auto it = pages_.find(k);
  if (it != pages_.end()) {
    it->second.page = page;
    it->second.dirty = true;
    it->second.owner = table;
    it->second.page_no = page_no;
    touch(it);
  } else {
    Entry entry;
    entry.page = page;
    entry.dirty = true;
    entry.owner = table;
    entry.page_no = page_no;
    insert(k, std::move(entry));
  }
  evict_if_needed();
}

void PageCache::evict_if_needed() {
  while (pages_.size() > max_pages_ && !lru_.empty()) {
    std::vector<uint64_t> victims;
    std::vector<Entry *> dirty;
    size_t batch = std::min<size_t>(kIoBatchPages, std::max<size_t>(1, max_pages_ / 2));
    for (auto it = lru_.rbegin(); it != lru_.rend() && victims.size() < batch; ++it) {
      auto p = pages_.find(*it);
      if (p == pages_.end()) continue;
      if (p->second.page.use_count() > 1) continue;
      victims.push_back(*it);
      if (p->second.dirty && p->second.owner) dirty.push_back(&p->second);
    }
    if (victims.empty()) break;  // all pages are pinned
    write_entries(std::move(dirty));
    for (uint64_t victim : victims) {
      auto p = pages_.find(victim);
      if (!compact_.contains(victim)) compact_.put(victim, p->second.page.get());
      lru_.erase(p->second.lru_it);
      pages_.erase(p);
    }
  }
}

void PageCache::write_entries(std::vector<Entry *> entries) {
  std::sort(entries.begin(), entries.end(), [](const Entry *a, const Entry *b) {
    if (a->owner->id() != b->owner->id()) return a->owner->id() < b->owner->id();
    return a->page_no < b->page_no;
  });
  for (size_t begin = 0; begin < entries.size();) {
    size_t end = begin + 1;
    while (end < entries.size() && end - begin < kIoBatchPages &&
           entries[end]->owner == entries[begin]->owner &&
           entries[end]->page_no == entries[end - 1]->page_no + 1) ++end;
    if (end == begin + 1) {
      entries[begin]->owner->write_page_to_disk(entries[begin]->page_no, *entries[begin]->page);
    } else {
#ifdef __linux__
      std::array<const Page *, kIoBatchPages> pages;
      for (size_t i = begin; i < end; ++i) pages[i-begin] = entries[i]->page.get();
      entries[begin]->owner->write_scattered_pages(entries[begin]->page_no,
          pages.data(), static_cast<uint32_t>(end-begin));
#else
      if (!write_buffer_) write_buffer_ = std::make_unique<PageIoBuffer>();
      for (size_t i = begin; i < end; ++i)
        std::memcpy(write_buffer_->bytes + (i - begin) * kPageSize,
                    entries[i]->page.get(), kPageSize);
      entries[begin]->owner->write_pages_to_disk(entries[begin]->page_no,
          write_buffer_->bytes, static_cast<uint32_t>(end - begin));
#endif
    }
    for (size_t i = begin; i < end; ++i) entries[i]->dirty = false;
    begin = end;
  }
}

void PageCache::flush_all(
    const std::unordered_map<uint32_t, std::unique_ptr<Table>> &tables) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Entry *> dirty;
  for (auto &kv : pages_) {
    if (kv.second.dirty && kv.second.owner) dirty.push_back(&kv.second);
  }
  write_entries(std::move(dirty));
  for (auto &t : tables) {
    if (!t.second->fsync_file()) throw_error("IO_ERROR", "failed to sync table file");
  }
}

void PageCache::drop_table(uint32_t table_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  compact_.erase_if([&](uint64_t k) { return static_cast<uint32_t>(k >> 32) == table_id; });
  for (auto it = pages_.begin(); it != pages_.end();) {
    uint32_t tid = static_cast<uint32_t>(it->first >> 32);
    if (tid == table_id) {
      lru_.erase(it->second.lru_it);
      it = pages_.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------
}
