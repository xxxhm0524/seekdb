#include "engine.h"
#include "ann.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <queue>
#include <sstream>
#include <thread>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif

namespace mvd {

namespace {
double squared_l2_checked(const float *a, const float *b, int dim);
double squared_l2_approximate(const float *a, const float *b, int dim);
constexpr size_t kVectorIndexBudget = 128 * 1024 * 1024;
uint32_t align_up(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}
std::string lower_copy(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Keep the crash-recovery log bounded so a long write workload does not grow
// WAL on disk and in memory without limit.  Checkpointing flushes every
// committed dirty page and fsyncs table files before truncating the log; a
// crash at any point leaves either the old log or durable data files.
constexpr int64_t kWalCheckpointBytes = 32 * 1024 * 1024;
constexpr uint64_t kWalWriteWindow = 8 * 1024 * 1024;

// Parses a vector literal with detailed error classification matching the
// task package's semantic error names.
void parse_vector_for_column(const std::string &text, const ColumnDef &col,
                             std::vector<float> &out) {
  out.clear();
  out.reserve(static_cast<size_t>(col.dim));
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= text.size() || text[i] != '[')
    throw_error("INVALID_VECTOR", "expected numeric vector component");
  ++i;
  std::vector<std::string_view> parts;
  parts.reserve(static_cast<size_t>(col.dim));
  while (true) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < text.size() && text[i] == ']') { ++i; break; }
    size_t start = i;
    while (i < text.size() && text[i] != ',' && text[i] != ']') ++i;
    size_t end = i;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    if (end == start) throw_error("INVALID_VECTOR", "expected numeric vector component");
    parts.emplace_back(text.data()+start, end-start);
    if (i < text.size() && text[i] == ',') {
      ++i;
      size_t next = i;
      while (next < text.size() && std::isspace(static_cast<unsigned char>(text[next]))) ++next;
      if (next >= text.size() || text[next] == ']')
        throw_error("INVALID_VECTOR", "expected numeric vector component after comma");
      continue;
    }
    if (i < text.size() && text[i] == ']') { ++i; break; }
    throw_error("INVALID_VECTOR", "expected numeric vector component");
  }
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i != text.size()) throw_error("INVALID_VECTOR", "expected numeric vector component");
  for (const auto &part : parts) {
    float v = 0;
    Float32Parse status = parse_float32_value(part, v);
    if (status == Float32Parse::INVALID)
      throw_error("INVALID_VECTOR", "expected numeric vector component");
    if (status == Float32Parse::OVERFLOW)
      throw_error("VECTOR_OVERFLOW", "vector component overflows float32");
    out.push_back(v);
  }
  if (static_cast<int>(out.size()) != col.dim)
    throw_error("VECTOR_DIM_MISMATCH",
                col.name + " must have " + std::to_string(col.dim) + " dimensions");
}
}  // namespace

namespace {
class PagePool {
 public:
  PagePool() { spare_.reserve(kIoBatchPages); }
  ~PagePool() { for (Page *page : spare_) delete page; }
  Page *take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spare_.empty()) return new Page();
    Page *page = spare_.back();
    spare_.pop_back();
    return page;
  }
  void put(Page *page) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Retain a complete I/O batch. A smaller pool frees aligned pages between
    // small cache entries on every scan, fragmenting the allocator's heap.
    if (spare_.size() < kIoBatchPages) spare_.push_back(page);
    else delete page;
  }
 private:
  std::mutex mutex_;
  std::vector<Page *> spare_;
};
PagePool &page_pool() {
  static PagePool pool;
  return pool;
}
}  // namespace

std::shared_ptr<Page> make_page(const Page *source) {
  Page *page = page_pool().take();
  if (source) *page = *source;
  else *page = Page();
  return std::shared_ptr<Page>(page, [](Page *released) { page_pool().put(released); });
}

// ---------------------------------------------------------------------------
// Schema layout
// ---------------------------------------------------------------------------
void build_row_layout(TableSchema &schema) {
  int pk_count = 0;
  int vec_count = 0;
  std::unordered_set<std::string> names;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const auto &col = schema.columns[i];
    if (col.name.empty()) throw_error("INVALID_SCHEMA", "empty column name");
    std::string lower = lower_copy(col.name);
    if (!names.insert(lower).second)
      throw_error("INVALID_SCHEMA", "duplicate column name: " + col.name);
    if (col.type == ColumnType::VECTOR) {
      ++vec_count;
      schema.vector_index = static_cast<int>(i);
    }
  }
  if (vec_count > 1)
    throw_error("INVALID_SCHEMA", "at most one VECTOR column is allowed");
  schema.vector_index = -1;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (schema.columns[i].type == ColumnType::VECTOR)
      schema.vector_index = static_cast<int>(i);
  }
  // pk_index is set by the caller (parse_create_table leaves the primary_key
  // name).  If it is still -1, try to find a BIGINT column named "id".
  if (schema.pk_index < 0) {
    for (size_t i = 0; i < schema.columns.size(); ++i) {
      if (schema.columns[i].type == ColumnType::BIGINT) {
        schema.pk_index = static_cast<int>(i);
        break;
      }
    }
  }
  if (schema.pk_index < 0)
    throw_error("INVALID_SCHEMA", "a BIGINT PRIMARY KEY is required");
  if (schema.columns[schema.pk_index].type != ColumnType::BIGINT)
    throw_error("INVALID_SCHEMA", "primary key must be BIGINT");
  (void)pk_count;

  uint32_t offset = 16;  // valid byte + padding + int64 id
  schema.col_offsets.assign(schema.columns.size(), 0);
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const auto &col = schema.columns[i];
    switch (col.type) {
      case ColumnType::BIGINT:
        offset = align_up(offset, 8);
        schema.col_offsets[i] = offset;
        offset += 8;
        break;
      case ColumnType::VARCHAR:
        offset = align_up(offset, 4);
        schema.col_offsets[i] = offset;
        offset += 4 + static_cast<uint32_t>(col.max_chars) * 4;
        break;
      case ColumnType::VECTOR:
        offset = align_up(offset, 32);
        schema.col_offsets[i] = offset;
        offset += static_cast<uint32_t>(col.dim) * 4;
        break;
    }
    // Reject as soon as a row exceeds one page, before a very large column
    // list can wrap the uint32 layout offset back to a small valid-looking row.
    if (offset > kPageSize - kPageHeaderSize)
      throw_error("INVALID_SCHEMA", "row is too large for a page");
  }
  schema.row_size = align_up(offset, 64);
  schema.page_size = kPageSize;
  schema.rows_per_page =
      (kPageSize - kPageHeaderSize) / schema.row_size;
  if (schema.rows_per_page == 0)
    throw_error("INVALID_SCHEMA", "row is too large for a page");
}

// ---------------------------------------------------------------------------
// PageCache
// ---------------------------------------------------------------------------
size_t PageCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pages_.size();
}

void PageCache::touch(std::unordered_map<uint64_t, Entry>::iterator it) {
  lru_.erase(it->second.lru_it);
  lru_.push_front(it->first);
  it->second.lru_it = lru_.begin();
}

std::shared_ptr<Page> PageCache::get(Table *table, uint32_t page_no) {
  uint64_t k = key(table->id(), page_no);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = pages_.find(k);
  if (it != pages_.end()) {
    touch(it);
    return it->second.page;
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
    lru_.push_front(current_key);
    entry.lru_it = lru_.begin();
    pages_.emplace(current_key, std::move(entry));
    if (current == page_no) requested = std::move(page);
  }
  evict_if_needed();
  return requested;
}

void PageCache::install(Table *table, uint32_t page_no,
                        std::shared_ptr<Page> page) {
  uint64_t k = key(table->id(), page_no);
  std::lock_guard<std::mutex> lock(mutex_);
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
    lru_.push_front(k);
    entry.lru_it = lru_.begin();
    pages_.emplace(k, std::move(entry));
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
Table::Table(TableSchema schema, std::string data_path, PageCache *cache)
    : schema_(std::move(schema)),
      data_path_(std::move(data_path)),
      cache_(cache) {}
Table::~Table() = default;

void Table::open(bool rebuild, bool fresh) {
  int flags = O_RDWR | O_CREAT | (fresh ? O_TRUNC : 0);
#ifndef _WIN32
  file_ = FileHandle::open(data_path_, flags | O_DIRECT);
  direct_io_ = file_.valid();
  if (!file_.valid()) file_ = FileHandle::open(data_path_, flags);
#else
  file_ = FileHandle::open(data_path_, flags);
#endif
  if (!file_.valid())
    throw_error("IO_ERROR", "cannot open table file: " + data_path_);
  // Startup replays committed WAL before inspecting possibly interrupted page
  // writes. Also avoid scanning every table twice during normal recovery.
  if (!rebuild) return;
  int64_t size = file_.size();
  if (size < 0) throw_error("IO_ERROR", "cannot stat table file");
  uint32_t total_pages = static_cast<uint32_t>(size / kPageSize);
  if (static_cast<int64_t>(total_pages) * kPageSize != size)
    throw_error("CORRUPTION", "table file size is not page aligned");
  page_count_ = 0;
  // Find the highest page with a valid header.
  for (int64_t p = static_cast<int64_t>(total_pages) - 1; p >= 0; --p) {
    Page page;
    if (!load_page_raw(static_cast<uint32_t>(p), page)) break;
    if (page.magic == kPageMagic) {
      page_count_ = static_cast<uint32_t>(p) + 1;
      next_pos_ = static_cast<uint64_t>(p) * schema_.rows_per_page + page.nrows;
      break;
    }
  }
  rebuild_pk_index();
}

void Table::close_file() { file_.close(); }

bool Table::reopen_buffered() {
  direct_io_ = false;
  file_.close();
  file_ = FileHandle::open(data_path_, O_RDWR | O_CREAT);
  return file_.valid();
}

bool Table::load_page_raw(uint32_t page_no, Page &page) {
  if (!file_.valid()) return false;
  int64_t size = file_.size();
  uint64_t offset = static_cast<uint64_t>(page_no) * kPageSize;
  if (size < 0 || static_cast<int64_t>(offset + kPageSize) > size) {
    page = Page();
    page.magic = kPageMagic;
    page.nrows = 0;
    page.page_no = page_no;
    return true;
  }
  if (!file_.pread_full(&page, kPageSize, offset)) {
    // Some container/volume filesystems accept O_DIRECT at open but reject
    // aligned reads.  Fall back to buffered I/O once and retry.
    if (direct_io_ && reopen_buffered()) {
      return file_.pread_full(&page, kPageSize, offset);
    }
    return false;
  }
  return true;
}

uint32_t Table::load_pages_raw(uint32_t first_page, uint32_t count, void *pages) {
  int64_t bytes = file_.size();
  if (bytes < 0) throw_error("IO_ERROR", "cannot stat table file");
  uint64_t total = static_cast<uint64_t>(bytes) / kPageSize;
  if (first_page >= total || count == 0) return 0;
  count = static_cast<uint32_t>(std::min<uint64_t>(count, total - first_page));
  uint64_t offset = static_cast<uint64_t>(first_page) * kPageSize;
  size_t length = static_cast<size_t>(count) * kPageSize;
  if (!file_.pread_full(pages, length, offset)) {
    if (!direct_io_ || !reopen_buffered() || !file_.pread_full(pages, length, offset))
      throw_error("IO_ERROR", "cannot read table pages");
  }
  return count;
}

void Table::refresh_after_recovery() {
  int64_t size = file_.size();
  if (size < 0) throw_error("IO_ERROR", "cannot stat table file");
  uint32_t total_pages = static_cast<uint32_t>(size / kPageSize);
  if (static_cast<int64_t>(total_pages) * kPageSize != size)
    throw_error("CORRUPTION", "table file size is not page aligned after recovery");
  page_count_ = 0;
  next_pos_ = 0;
  for (int64_t p = static_cast<int64_t>(total_pages) - 1; p >= 0; --p) {
    Page page;
    if (!load_page_raw(static_cast<uint32_t>(p), page)) continue;
    if (page.magic == kPageMagic) {
      page_count_ = static_cast<uint32_t>(p) + 1;
      next_pos_ = static_cast<uint64_t>(p) * schema_.rows_per_page + page.nrows;
      break;
    }
  }
  rebuild_pk_index();
}

void Table::rebuild_pk_index() {
  pk_pos_.clear();
  if (page_count_ == 0) return;
  auto buffer = std::make_unique<PageIoBuffer>();
  for (uint32_t p = 0; p < page_count_;) {
    uint32_t count = load_pages_raw(p, std::min(kIoBatchPages, page_count_ - p), buffer->bytes);
    if (count == 0) throw_error("CORRUPTION", "table page missing during index rebuild");
    for (uint32_t i = 0; i < count; ++i) {
      Page page;
      std::memcpy(&page, buffer->bytes + static_cast<size_t>(i) * kPageSize, kPageSize);
      if (page.magic != kPageMagic) continue;
      uint32_t n = std::min(page.nrows, page.capacity(schema_.row_size));
      for (uint32_t slot = 0; slot < n; ++slot) {
        const uint8_t *row = page.row_ptr(slot, schema_.row_size);
        if (!row_is_valid(row)) continue;
        int64_t pk = row_id(row);
        pk_pos_[pk] = static_cast<uint64_t>(p + i) * schema_.rows_per_page + slot;
      }
    }
    p += count;
  }
}

bool Table::read_row(uint64_t pos, std::vector<uint8_t> &row) {
  uint32_t page_no = static_cast<uint32_t>(pos / schema_.rows_per_page);
  uint32_t slot = static_cast<uint32_t>(pos % schema_.rows_per_page);
  auto page = get_page(page_no);
  if (page->magic != kPageMagic || slot >= page->nrows) return false;
  const uint8_t *ptr = row_data(*page, slot);
  if (!row_is_valid(ptr)) return false;
  row.assign(ptr, ptr + schema_.row_size);
  return true;
}

void Table::write_page_to_disk(uint32_t page_no, const Page &page) {
  write_pages_to_disk(page_no, &page, 1);
}

#ifdef __linux__
void Table::write_scattered_pages(uint32_t first, const Page *const *pages, uint32_t count) {
  std::vector<iovec> vectors(count);
  for (size_t i = 0; i < count; ++i) {
    vectors[i].iov_base = const_cast<Page *>(pages[i]);
    vectors[i].iov_len = kPageSize;
  }
  uint64_t offset = static_cast<uint64_t>(first)*kPageSize;
  if (!file_.pwritev_full(vectors.data(), static_cast<int>(count), offset)) {
    if (!direct_io_ || !reopen_buffered() ||
        !file_.pwritev_full(vectors.data(), static_cast<int>(count), offset))
      throw_error("IO_ERROR", "failed to write table page batch");
  }
  if (first+count > page_count_) page_count_ = first+count;
}
#endif

void Table::write_pages_to_disk(uint32_t page_no, const void *pages, uint32_t count) {
  uint64_t offset = static_cast<uint64_t>(page_no) * kPageSize;
  size_t bytes = static_cast<size_t>(count) * kPageSize;
  if (!file_.pwrite_full(pages, bytes, offset)) {
    if (direct_io_ && reopen_buffered()) {
      if (!file_.pwrite_full(pages, bytes, offset))
        throw_error("IO_ERROR", "failed to write table page");
    } else {
      throw_error("IO_ERROR", "failed to write table page");
    }
  }
  if (page_no + count > page_count_) page_count_ = page_no + count;
}

void Table::flush_all_dirty() {}

bool Table::fsync_file() { return file_.fsync(); }

bool Table::find_pk(int64_t pk, uint64_t &pos) const {
  auto it = pk_pos_.find(pk);
  if (it == pk_pos_.end()) return false;
  pos = it->second;
  return true;
}
void Table::put_pk(int64_t pk, uint64_t pos) { pk_pos_[pk] = pos; }
void Table::erase_pk(int64_t pk) { pk_pos_.erase(pk); }

int Table::column_index(const std::string &name) const {
  std::string lower = lower_copy(name);
  for (size_t i = 0; i < schema_.columns.size(); ++i) {
    if (lower_copy(schema_.columns[i].name) == lower)
      return static_cast<int>(i);
  }
  return -1;
}

void Table::encode_value(uint8_t *row, int col_index, const Value &value) {
  const ColumnDef &col = schema_.columns[col_index];
  uint8_t *base = row + schema_.col_offsets[col_index];
  if (col.type == ColumnType::BIGINT) {
    int64_t v = 0;
    if (value.kind == Value::Kind::INT) {
      v = value.i;
    } else if (value.kind == Value::Kind::RAW ||
               value.kind == Value::Kind::STRING) {
      if (!parse_int64(value.s, v))
        throw_error("INVALID_VALUE",
                    "cannot convert value to BIGINT for column " + col.name);
    } else {
      throw_error("INVALID_VALUE", "invalid BIGINT value");
    }
    std::memcpy(base, &v, sizeof(v));
  } else if (col.type == ColumnType::VARCHAR) {
    if (value.kind != Value::Kind::STRING && value.kind != Value::Kind::RAW)
      throw_error("INVALID_VALUE", "invalid VARCHAR value");
    if (utf8_codepoint_count(value.s) < 0)
      throw_error("INVALID_VALUE", "invalid UTF-8 string");
    int cps = utf8_codepoint_count(value.s);
    if (cps > col.max_chars)
      throw_error("VARCHAR_TOO_LONG", col.name + " is too long");
    uint32_t bytes = static_cast<uint32_t>(value.s.size());
    std::memcpy(base, &bytes, 4);
    if (bytes) std::memcpy(base + 4, value.s.data(), bytes);
  } else if (col.type == ColumnType::VECTOR) {
    std::vector<float> vec;
    if (!value.vec.empty()) {
      vec = value.vec;
    } else if (value.kind == Value::Kind::VECTOR ||
               value.kind == Value::Kind::STRING ||
               value.kind == Value::Kind::RAW) {
      parse_vector_for_column(value.s, col, vec);
    } else {
      throw_error("INVALID_VECTOR", "expected numeric vector component");
    }
    if (static_cast<int>(vec.size()) != col.dim)
      throw_error("VECTOR_DIM_MISMATCH",
                  col.name + " must have " + std::to_string(col.dim) + " dimensions");
    std::memcpy(base, vec.data(), static_cast<size_t>(col.dim) * sizeof(float));
  }
}

Value Table::decode_value(const uint8_t *row, int col_index) const {
  const ColumnDef &col = schema_.columns[col_index];
  const uint8_t *base = row + schema_.col_offsets[col_index];
  if (col.type == ColumnType::BIGINT) {
    int64_t v;
    std::memcpy(&v, base, sizeof(v));
    return Value::make_int(v);
  }
  if (col.type == ColumnType::VARCHAR) {
    uint32_t len;
    std::memcpy(&len, base, 4);
    std::string s(reinterpret_cast<const char *>(base + 4), len);
    return Value::make_string(std::move(s));
  }
  std::vector<float> vec(static_cast<size_t>(col.dim));
  std::memcpy(vec.data(), base, static_cast<size_t>(col.dim) * sizeof(float));
  return Value::make_vector(std::move(vec));
}

bool Table::compare(const uint8_t *row, const Condition &cond) const {
  if (cond.resolved_column < 0) return false;
  const ColumnDef &col = schema_.columns[cond.resolved_column];
  const uint8_t *base = row + schema_.col_offsets[cond.resolved_column];
  int cmp = 0;
  if (col.type == ColumnType::BIGINT) {
    int64_t a;
    std::memcpy(&a, base, sizeof(a));
    int64_t b = cond.typed_value.i;
    if (cond.typed_value.kind != Value::Kind::INT) {
      if (!parse_int64(cond.typed_value.s, b)) return false;
    }
    cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
  } else if (col.type == ColumnType::VARCHAR) {
    uint32_t len;
    std::memcpy(&len, base, 4);
    std::string_view a(reinterpret_cast<const char *>(base + 4), len);
    const std::string &b = cond.typed_value.s;
    cmp = a.compare(b);
  } else {
    throw_error("UNSUPPORTED", "VECTOR columns cannot be compared in WHERE");
  }
  switch (cond.op) {
    case CompareOp::EQ: return cmp == 0;
    case CompareOp::NE: return cmp != 0;
    case CompareOp::LT: return cmp < 0;
    case CompareOp::LE: return cmp <= 0;
    case CompareOp::GT: return cmp > 0;
    case CompareOp::GE: return cmp >= 0;
  }
  return false;
}

void Table::apply_patches(uint8_t *row, const RowState &state) {
  for (const auto &patch : state.patches)
    for (const auto &change : *patch)
      encode_value(row, change.first, change.second);
}

void Table::materialize_state(const RowState &state, const uint8_t *base,
                              std::vector<uint8_t> &row) {
  if (state.has_raw_row()) state.read_raw(row);
  else if (base) row.assign(base, base + schema_.row_size);
  else if (!read_row(state.pos, row))
    throw_error("CORRUPTION", "transaction base row missing");
  apply_patches(row.data(), state);
}

void Table::build_commit_images(const TxTable &tx,
                                const std::function<void(PageImage)> &visit) {
  struct Change {
    uint64_t pos;
    bool deleted;
    const RowState *state;
  };
  std::unordered_map<uint32_t, std::vector<Change>> by_page;
  for (const auto &kv : tx.rows) {
    const RowState &state = kv.second;
    if (state.deleted && !state.existed) continue;  // insert then delete: no-op
    uint32_t page_no = static_cast<uint32_t>(state.pos / schema_.rows_per_page);
    by_page[page_no].push_back(Change{state.pos, state.deleted, &state});
  }
  std::vector<uint32_t> ordered_pages;
  ordered_pages.reserve(by_page.size());
  for (const auto &kv : by_page) ordered_pages.push_back(kv.first);
  std::sort(ordered_pages.begin(), ordered_pages.end());
  std::vector<uint8_t> spilled_row;
  for (uint32_t page_no : ordered_pages) {
    auto &changes = by_page.at(page_no);
    std::sort(changes.begin(), changes.end(),
              [](const Change &a, const Change &b) { return a.pos < b.pos; });
    // Complete raw replacements and tombstones do not depend on old bytes.
    // Verify coverage of every slot before omitting the old-page read.
    bool replaces_page = changes.size() == schema_.rows_per_page;
    for (size_t i = 0; replaces_page && i < changes.size(); ++i)
      replaces_page = changes[i].pos == static_cast<uint64_t>(page_no) * schema_.rows_per_page + i &&
          (changes[i].deleted || changes[i].state->has_raw_row());
    std::shared_ptr<Page> base;
    if (page_no < page_count_ && !replaces_page) {
      base = cache_->get(this, page_no);
    } else {
      base = make_page();
      base->magic = kPageMagic;
      base->nrows = replaces_page ? schema_.rows_per_page : 0;
      base->page_no = page_no;
    }
    auto image = make_page(base.get());
    image->page_no = page_no;
    image->magic = kPageMagic;
    uint32_t max_slots = image->capacity(schema_.row_size);
    for (const Change &ch : changes) {
      uint32_t slot = static_cast<uint32_t>(ch.pos % schema_.rows_per_page);
      if (slot >= max_slots)
        throw_error("CORRUPTION", "row slot outside page capacity");
      uint8_t *dst = image->row_ptr(slot, schema_.row_size);
      if (ch.deleted) {
        if (slot < image->nrows) set_row_valid(dst, false);
      } else {
        if (ch.state->has_raw_row()) {
          ch.state->read_raw(spilled_row);
          std::memcpy(dst, spilled_row.data(), schema_.row_size);
        }
        apply_patches(dst, *ch.state);
        set_row_valid(dst, true);
        if (slot + 1 > image->nrows) image->nrows = slot + 1;
        if (image->nrows > max_slots) image->nrows = max_slots;
      }
    }
    if (page_no + 1 > page_count_) page_count_ = page_no + 1;
    PageImage pi;
    pi.table_id = schema_.id;
    pi.page_no = page_no;
    pi.page = std::move(image);
    visit(std::move(pi));
  }
}

// ---------------------------------------------------------------------------
// WAL
// ---------------------------------------------------------------------------
bool Wal::open(const std::string &path) {
  file_ = FileHandle::open(path, O_RDWR | O_CREAT | O_APPEND);
  return file_.valid();
}

void Wal::close() { file_.close(); }

bool Wal::read_record(uint64_t offset, uint64_t size, WalRecordHeader &h,
                      std::string &payload) {
  if (offset + sizeof(h) > size) return false;
  if (!file_.pread_full(&h, sizeof(h), offset))
    throw_error("IO_ERROR", "cannot read WAL header");
  if (h.magic != 0x4D564457 || h.payload_len > 1024 * 1024 ||
      offset + sizeof(h) + h.payload_len > size) return false;
  payload.resize(h.payload_len);
  if (!payload.empty() && !file_.pread_full(payload.data(), payload.size(), offset + sizeof(h)))
    throw_error("IO_ERROR", "cannot read WAL record");
  return crc32(payload.data(), payload.size()) == h.crc;
}

std::shared_ptr<Page> Wal::read_page_image(uint64_t offset) {
  auto page = make_page();
  if (!file_.pread_full(page.get(), kPageSize, offset + sizeof(WalRecordHeader) + 12))
    throw_error("IO_ERROR", "cannot read committed WAL page");
  return page;
}

bool Wal::append_page(uint64_t txid, uint32_t table_id, uint32_t page_no,
                      const Page &page) {
  std::string payload;
  payload.reserve(12 + kPageSize);
  payload.append(reinterpret_cast<const char *>(&table_id), 4);
  payload.append(reinterpret_cast<const char *>(&page_no), 4);
  uint32_t ps = kPageSize;
  payload.append(reinterpret_cast<const char *>(&ps), 4);
  payload.append(reinterpret_cast<const char *>(&page), kPageSize);
  WalRecordHeader h;
  h.type = 1;
  h.payload_len = static_cast<uint32_t>(payload.size());
  h.txid = txid;
  h.crc = crc32(payload.data(), payload.size());
  if (!file_.append(&h, sizeof(h))) return false;
  if (!file_.append(payload.data(), payload.size())) return false;
  return true;
}

bool Wal::append_commit(uint64_t txid) {
  WalRecordHeader h;
  h.type = 2;
  h.payload_len = 0;
  h.txid = txid;
  h.crc = crc32(nullptr, 0);
  return file_.append(&h, sizeof(h));
}

bool Wal::sync() { return file_.fsync(); }

bool Wal::truncate() {
  if (!file_.ftruncate(0)) return false;
  return file_.fsync();
}

// ---------------------------------------------------------------------------
// Engine basic helpers
// ---------------------------------------------------------------------------
Engine::Engine(std::string data_dir, size_t cache_pages, size_t tx_memory, size_t ann_min_rows)
    : data_dir_(std::move(data_dir)),
      catalog_path_(data_dir_ + "/catalog.dat"),
      wal_path_(data_dir_ + "/wal.log"),
      cache_(cache_pages), tx_memory_(tx_memory), ann_min_rows_(ann_min_rows) {}

Engine::~Engine() {
  try {
    shutdown();
  } catch (...) {
  }
}

void Engine::open() {
  make_dir(data_dir_);
  std::remove((data_dir_ + "/txn.tmp").c_str());
  load_catalog();
  // Create/open all tables from catalog.
  for (auto &kv : tables_) kv.second->open(false);
  if (!wal_.open(wal_path_))
    throw_error("IO_ERROR", "cannot open WAL");
  wal_.recover([&](uint32_t table_id, uint32_t page_no,
                   const std::shared_ptr<Page> &page) {
    auto it = tables_.find(table_id);
    if (it == tables_.end()) return;
    it->second->write_page_to_disk(page_no, *page);
  });
  for (auto &kv : tables_) {
    kv.second->refresh_after_recovery();
    if (!kv.second->fsync_file()) throw_error("IO_ERROR", "failed to sync recovered table");
  }
  // WAL is fully applied and durable now.
  if (!wal_.truncate()) throw_error("IO_ERROR", "failed to truncate recovered WAL");
  for (auto &entry : tables_)
    if (entry.second->schema().vector_index_created) rebuild_vector_index(*entry.second);
  opened_ = true;
}

void Engine::shutdown() {
  if (!opened_) { wal_.close(); return; }
  if (tx_.active) rollback_transaction();
  if (!tables_.empty()) {
    cache_.flush_all(tables_);
  }
  if (!wal_.truncate()) throw_error("IO_ERROR", "failed to truncate WAL at shutdown");
  wal_.close();
  opened_ = false;
}

Table *Engine::find_table(const std::string &name) {
  std::string lower = lower_copy(name);
  auto it = table_ids_.find(lower);
  if (it == table_ids_.end()) return nullptr;
  auto t = tables_.find(it->second);
  return t == tables_.end() ? nullptr : t->second.get();
}

Table &Engine::require_table(const std::string &name) {
  Table *t = find_table(name);
  if (!t) throw_error("TABLE_NOT_FOUND", "table not found: " + name);
  return *t;
}

Value Engine::coerce_value(const TableSchema &schema, int col_index,
                           const Value &v) {
  const ColumnDef &col = schema.columns[col_index];
  if (col.type == ColumnType::BIGINT) {
    int64_t out;
    if (v.kind == Value::Kind::INT) return v;
    if ((v.kind == Value::Kind::RAW || v.kind == Value::Kind::STRING) &&
        parse_int64(v.s, out))
      return Value::make_int(out);
    throw_error("INVALID_VALUE", "invalid BIGINT literal");
  }
  if (col.type == ColumnType::VARCHAR) {
    if (v.kind == Value::Kind::STRING || v.kind == Value::Kind::RAW)
      return v;
    throw_error("INVALID_VALUE", "invalid VARCHAR literal");
  }
  if (col.type == ColumnType::VECTOR) {
    std::vector<float> vec;
    if (!v.s.empty()) parse_vector_for_column(v.s, col, vec);
    else if (!v.vec.empty()) vec = v.vec;
    else throw_error("INVALID_VECTOR", "expected numeric vector component");
    if (static_cast<int>(vec.size()) != col.dim)
      throw_error("VECTOR_DIM_MISMATCH",
                  col.name + " must have " + std::to_string(col.dim) + " dimensions");
    return Value::make_vector(std::move(vec));
  }
  return v;
}

void Engine::compile_conditions(Table &table, std::vector<Condition> &where) {
  for (auto &cond : where) {
    int idx = table.column_index(cond.column);
    if (idx < 0)
      throw_error("COLUMN_NOT_FOUND", "column not found: " + cond.column);
    if (table.schema().columns[idx].type == ColumnType::VECTOR)
      throw_error("UNSUPPORTED", "VECTOR columns cannot be compared in WHERE");
    cond.resolved_column = idx;
    cond.typed_value = coerce_value(table.schema(), idx, cond.value);
  }
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------
void Engine::load_catalog() {
  tables_.clear();
  table_ids_.clear();
  next_table_id_ = 1;
  std::string data;
  if (!read_file(catalog_path_, data) || data.empty()) return;
  Reader r(data);
  uint32_t magic, version, count;
  if (!r.u32(magic) || !r.u32(version) || !r.u32(count))
    throw_error("CORRUPTION", "invalid catalog header");
  if (magic != 0x4D564443 || version != 1)
    throw_error("CORRUPTION", "unsupported catalog version");
  // Last 4 bytes are a CRC over everything before them.
  if (data.size() < 12) throw_error("CORRUPTION", "catalog truncated");
  uint32_t stored_crc;
  std::memcpy(&stored_crc, data.data() + data.size() - 4, 4);
  if (crc32(data.data(), data.size() - 4) != stored_crc)
    throw_error("CORRUPTION", "catalog checksum mismatch");
  for (uint32_t i = 0; i < count; ++i) {
    TableSchema schema;
    uint32_t id;
    std::string name;
    uint32_t ncols;
    if (!r.u32(id) || !r.str(name) || !r.u32(ncols))
      throw_error("CORRUPTION", "invalid catalog entry");
    schema.id = id;
    schema.name = name;
    for (uint32_t c = 0; c < ncols; ++c) {
      ColumnDef col;
      uint8_t type;
      std::string cname;
      int32_t max_chars, dim;
      if (!r.u8(type) || !r.str(cname) || !r.i32(max_chars) || !r.i32(dim))
        throw_error("CORRUPTION", "invalid catalog column");
      col.type = static_cast<ColumnType>(type);
      col.name = std::move(cname);
      col.max_chars = max_chars;
      col.dim = dim;
      schema.columns.push_back(std::move(col));
    }
    int32_t pk_idx, vec_idx;
    uint8_t has_index;
    if (!r.i32(pk_idx) || !r.i32(vec_idx) || !r.u8(has_index))
      throw_error("CORRUPTION", "invalid catalog table");
    schema.pk_index = pk_idx;
    schema.vector_index = vec_idx;
    schema.vector_index_created = has_index != 0;
    build_row_layout(schema);
    std::string path = data_dir_ + "/t_" + std::to_string(id) + ".dat";
    auto table = std::make_unique<Table>(std::move(schema), path, &cache_);
    table_ids_[lower_copy(table->name())] = id;
    if (id >= next_table_id_) next_table_id_ = id + 1;
    tables_[id] = std::move(table);
  }
}

void Engine::save_catalog() {
  std::string data;
  write_u32(data, 0x4D564443);
  write_u32(data, 1);
  write_u32(data, static_cast<uint32_t>(tables_.size()));
  for (auto &kv : tables_) {
    TableSchema &schema = kv.second->schema();
    write_u32(data, schema.id);
    write_u32(data, static_cast<uint32_t>(schema.name.size()));
    data += schema.name;
    write_u32(data, static_cast<uint32_t>(schema.columns.size()));
    for (auto &col : schema.columns) {
      write_u8(data, static_cast<uint8_t>(col.type));
      write_u32(data, static_cast<uint32_t>(col.name.size()));
      data += col.name;
      write_i32(data, col.max_chars);
      write_i32(data, col.dim);
    }
    write_i32(data, schema.pk_index);
    write_i32(data, schema.vector_index);
    write_u8(data, schema.vector_index_created ? 1 : 0);
  }
  uint32_t crc = crc32(data.data(), data.size());
  write_u32(data, crc);
  if (!write_file_atomic(catalog_path_, data))
    throw_error("IO_ERROR", "failed to write catalog");
}

// ---------------------------------------------------------------------------
// Transaction commit / rollback
// ---------------------------------------------------------------------------
void Engine::commit_transaction() {
  if (!tx_.active) return;
  uint64_t txid = wal_.allocate_txid();
  uint64_t sync_offset = static_cast<uint64_t>(wal_.size());
  std::vector<PageImage> all_images;
  for (auto &kv : tx_.tables) {
    auto it = tables_.find(kv.first);
    if (it == tables_.end()) continue;
    it->second->build_commit_images(kv.second, [&](PageImage img) {
      img.wal_offset = static_cast<uint64_t>(wal_.size());
      if (!wal_.append_page(txid, img.table_id, img.page_no, *img.page))
        throw_error("IO_ERROR", "failed to write WAL page image");
      uint64_t written = static_cast<uint64_t>(wal_.size());
      if (written - sync_offset >= kWalWriteWindow) {
        // Persisting an uncommitted prefix is safe: recovery still requires
        // the COMMIT record. Bound dirty kernel cache as well as user buffers.
        if (!wal_.sync()) throw_error("IO_ERROR", "failed to sync WAL prefix");
        wal_.discard_clean_cache(sync_offset, written - sync_offset);
        sync_offset = written;
      }
      // Small batches keep their fast in-memory path. Large commits retain
      // only WAL offsets after the first MiB, instead of pinning every page.
      if (all_images.size() >= 16) img.page.reset();
      all_images.push_back(std::move(img));
    });
  }
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
  for (auto &kv : tx_.tables) {
    auto it = tables_.find(kv.first);
    if (it == tables_.end()) continue;
    Table &table = *it->second;
    table.set_next_pos(kv.second.next_pos);
    for (auto &rk : kv.second.rows) {
      const RowState &state = rk.second;
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
}

void Engine::maybe_checkpoint() {
  if (wal_.size() < kWalCheckpointBytes) return;
  cache_.flush_all(tables_);
  if (!wal_.truncate()) throw_error("IO_ERROR", "failed to truncate checkpoint WAL");
}

void Engine::rollback_transaction() { tx_.clear(); }

TxTable &Engine::tx_table_ref(uint32_t table_id) {
  auto old = tx_.tables.find(table_id);
  if (statement_undo_ && !statement_undo_->tables.count(table_id))
    statement_undo_->tables.emplace(table_id, UndoTable{old != tx_.tables.end(),
        old == tx_.tables.end() ? 0 : old->second.next_pos});
  auto [entry, inserted] = tx_.tables.try_emplace(table_id);
  if (inserted) entry->second.next_pos = tables_.at(table_id)->next_pos();
  return entry->second;
}

void Engine::spill_row(RowState &state) {
  if (state.row.empty()) return;
  uint32_t length = static_cast<uint32_t>(state.row.size());
  uint64_t offset = tx_.spool->append(state.row);
  state.spool = tx_.spool;
  state.spool_offset = offset;
  state.spool_size = length;
  tx_.encode_buffer.swap(state.row);
  std::vector<uint8_t>().swap(state.row);
}

void Engine::spill_state(Table &table, RowState &state) {
  if (state.deleted) return;
  if (!state.patches.empty()) {
    std::vector<uint8_t> materialized;
    materialized.swap(tx_.encode_buffer);
    table.materialize_state(state,nullptr,materialized);
    state.row=std::move(materialized);
    state.spool.reset();state.spool_size=0;
    state.patches.clear();
  }
  spill_row(state);
}

void Engine::store_row(Table &table, TxTable &changes, int64_t pk, RowState state) {
  auto old = changes.rows.find(pk);
  if (statement_undo_)
    statement_undo_->rows.push_back(UndoRow{table.id(), pk, old != changes.rows.end(),
        old == changes.rows.end() ? RowState{} : old->second});
  size_t previous = old == changes.rows.end() ? 0 : old->second.row.size();
  if (!tx_.spool && tx_.resident_payload - previous + state.row.size() + tx_.patch_budget->bytes > tx_memory_) {
    tx_.spool = std::make_shared<TxSpool>(data_dir_ + "/txn.tmp");
    for (auto &entry : tx_.tables) for (auto &row : entry.second.rows) {
      size_t bytes = row.second.row.size();
      spill_state(*tables_.at(entry.first),row.second);
      tx_.resident_payload -= bytes;
    }
    previous = 0;
  }
  if (tx_.spool) spill_state(table,state);
  tx_.resident_payload = tx_.resident_payload - previous + state.row.size();
  changes.rows[pk] = std::move(state);
}

void Engine::rollback_statement(StatementUndo &undo) {
  for (auto it = undo.rows.rbegin(); it != undo.rows.rend(); ++it) {
    auto &rows = tx_.tables.at(it->table).rows;
    if (it->existed) rows[it->pk] = std::move(it->before);
    else rows.erase(it->pk);
  }
  for (const auto &entry : undo.tables) {
    if (entry.second.existed) tx_.tables.at(entry.first).next_pos = entry.second.next_pos;
    else tx_.tables.erase(entry.first);
  }
  if (tx_.spool && !undo.had_spool) {
    // Keep migrated earlier work on disk. Reifying wide UPDATE rows could
    // otherwise expand a small patch budget into an entire table in RAM.
    uint64_t retained=0;
    for (const auto &entry:tx_.tables) for (const auto &row:entry.second.rows)
      if (row.second.spool_size) retained=std::max(retained,row.second.spool_offset+row.second.spool_size);
    if (retained) tx_.spool->truncate(retained);
    else tx_.spool.reset();
  } else if (tx_.spool) tx_.spool->truncate(undo.spool_end);
  tx_.resident_payload = 0;
  for (const auto &entry : tx_.tables) for (const auto &row : entry.second.rows)
    tx_.resident_payload += row.second.row.size();
}

const TxTable *Engine::find_tx_table(uint32_t table_id) const {
  auto it = tx_.tables.find(table_id);
  return it == tx_.tables.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Statement execution: dispatch
// ---------------------------------------------------------------------------
Response Engine::execute(const std::string &sql) {
  Response resp;
  std::unique_ptr<Statement> stmt;
  try {
    SqlParser parser(sql);
    stmt = parser.parse();
  } catch (const DbError &e) {
    resp.ok = false;
    resp.error_code = e.code;
    resp.error_message = e.message;
    return resp;
  }

  auto txn_guard = [&](auto fn) -> Response {
    bool autocommit = !tx_.active;
    if (autocommit) {
      tx_.clear();
      tx_.active = true;
    }
    StatementUndo undo;
    if (!autocommit) {
      undo.had_spool = static_cast<bool>(tx_.spool);
      if (tx_.spool) undo.spool_end = tx_.spool->size();
      statement_undo_ = &undo;
    }
    try {
      Response r = fn();
      statement_undo_ = nullptr;
      if (autocommit) commit_transaction();
      return r;
    } catch (...) {
      statement_undo_ = nullptr;
      if (autocommit) tx_.clear();
      else rollback_statement(undo);
      throw;
    }
  };

  try {
    switch (stmt->kind) {
      case Statement::Kind::CREATE_TABLE:
        if (tx_.active)
          throw_error("UNSUPPORTED", "DDL inside a transaction is not supported");
        return exec_create_table(*static_cast<CreateTableStmt *>(stmt.get()));
      case Statement::Kind::DROP_TABLE:
        if (tx_.active)
          throw_error("UNSUPPORTED", "DDL inside a transaction is not supported");
        return exec_drop_table(*static_cast<DropTableStmt *>(stmt.get()));
      case Statement::Kind::CREATE_VECTOR_INDEX:
        if (tx_.active)
          throw_error("UNSUPPORTED", "DDL inside a transaction is not supported");
        return exec_create_vector_index(
            *static_cast<CreateVectorIndexStmt *>(stmt.get()));
      case Statement::Kind::INSERT:
        return txn_guard([&]() { return exec_insert(*static_cast<InsertStmt *>(stmt.get())); });
      case Statement::Kind::UPDATE:
        return txn_guard([&]() { return exec_update(*static_cast<UpdateStmt *>(stmt.get())); });
      case Statement::Kind::DELETE:
        return txn_guard([&]() { return exec_delete(*static_cast<DeleteStmt *>(stmt.get())); });
      case Statement::Kind::SELECT:
        return exec_select(*static_cast<SelectStmt *>(stmt.get()));
      case Statement::Kind::BEGIN:
        return exec_begin();
      case Statement::Kind::COMMIT:
        return exec_commit();
      case Statement::Kind::ROLLBACK:
        return exec_rollback();
      case Statement::Kind::SET:
        return exec_set(*static_cast<SetStmt *>(stmt.get()));
    }
  } catch (const DbError &e) {
    resp.ok = false;
    resp.error_code = e.code;
    resp.error_message = e.message;
    return resp;
  } catch (const std::exception &e) {
    resp.ok = false;
    resp.error_code = "INTERNAL_ERROR";
    resp.error_message = e.what();
    return resp;
  }
  resp.ok = false;
  resp.error_code = "INTERNAL_ERROR";
  resp.error_message = "unhandled statement";
  return resp;
}

// ---------------------------------------------------------------------------
// DDL
// ---------------------------------------------------------------------------
Response Engine::exec_create_table(CreateTableStmt &stmt) {
  std::string lower = lower_copy(stmt.table);
  if (table_ids_.count(lower)) {
    if (stmt.if_not_exists) return Response{};
    throw_error("TABLE_EXISTS", "table already exists: " + stmt.table);
  }
  TableSchema schema;
  schema.id = next_table_id_++;
  schema.name = stmt.table;
  schema.columns = stmt.columns;
  // Resolve primary key.
  schema.pk_index = -1;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (lower_copy(schema.columns[i].name) == lower_copy(stmt.primary_key))
      schema.pk_index = static_cast<int>(i);
  }
  if (schema.pk_index < 0)
    throw_error("INVALID_SCHEMA", "PRIMARY KEY column not found: " + stmt.primary_key);
  build_row_layout(schema);
  std::string path = data_dir_ + "/t_" + std::to_string(schema.id) + ".dat";
  auto table = std::make_unique<Table>(schema, path, &cache_);
  // This ID is not referenced by the catalog. An interrupted DROP can leave
  // an orphan with the same path after restart; CREATE must start empty.
  table->open(false, true);
  // Make sure the table file is durable before the catalog references it.
  table->fsync_file();
  uint32_t id = schema.id;
  std::string name = schema.name;
  tables_[id] = std::move(table);
  table_ids_[lower] = id;
  try {
    save_catalog();
  } catch (...) {
    tables_.erase(id);
    table_ids_.erase(lower);
    std::remove(path.c_str());
    throw;
  }
  (void)name;
  Response resp;
  return resp;
}

Response Engine::exec_drop_table(DropTableStmt &stmt) {
  Table *table = find_table(stmt.table);
  if (!table) {
    if (stmt.if_exists) return Response{};
    throw_error("TABLE_NOT_FOUND", "table not found: " + stmt.table);
  }
  uint32_t id = table->id();
  std::string path = table->data_path();
  std::string lower = lower_copy(table->name());
  drop_vector_index(*table);
  cache_.drop_table(id);
  tables_.erase(id);
  table_ids_.erase(lower);
  try {
    save_catalog();
  } catch (...) {
    // Recovery of catalog failure is not critical here; rethrow.
    throw;
  }
  std::remove(path.c_str());
  Response resp;
  return resp;
}

Response Engine::exec_create_vector_index(CreateVectorIndexStmt &stmt) {
  Table &table = require_table(stmt.table);
  int col = table.column_index(stmt.column);
  if (col < 0)
    throw_error("COLUMN_NOT_FOUND", "column not found: " + stmt.column);
  if (table.schema().columns[col].type != ColumnType::VECTOR)
    throw_error("INVALID_ARGUMENT", "vector index requires a VECTOR column");
  table.schema().vector_index_created = true;
  save_catalog();
  rebuild_vector_index(table);
  return Response{};
}

// ---------------------------------------------------------------------------
// SET / transaction control
// ---------------------------------------------------------------------------
Response Engine::exec_set(SetStmt &stmt) {
  if (stmt.name == "vector_mode" || stmt.name == "search_mode") {
    if (stmt.value == "ann" || stmt.value == "approximate") ann_mode_ = true;
    else if (stmt.value == "exact") ann_mode_ = false;
    else throw_error("INVALID_VALUE", "search mode must be exact or ann");
    return Response{};
  }
  throw_error("UNSUPPORTED", "unsupported SET variable: " + stmt.name);
}

Response Engine::exec_begin() {
  if (tx_.active) throw_error("TRANSACTION_ERROR", "transaction already active");
  tx_.clear();
  tx_.active = true;
  return Response{};
}

Response Engine::exec_commit() {
  if (!tx_.active) return Response{};
  commit_transaction();
  return Response{};
}

Response Engine::exec_rollback() {
  rollback_transaction();
  return Response{};
}

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------
Response Engine::exec_insert(InsertStmt &stmt) {
  Table &table = require_table(stmt.table);
  const TableSchema &schema = table.schema();
  if (stmt.rows.empty()) throw_error("INVALID_VALUE", "INSERT requires values");
  // Determine source column order.
  std::vector<int> target_cols;
  if (stmt.columns.empty()) {
    if (stmt.rows[0].size() != schema.columns.size())
      throw_error("INVALID_VALUE", "column count does not match table");
    for (size_t i = 0; i < schema.columns.size(); ++i)
      target_cols.push_back(static_cast<int>(i));
  } else {
    std::vector<bool> columns_seen(schema.columns.size(), false);
    for (const auto &name : stmt.columns) {
      int idx = table.column_index(name);
      if (idx < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + name);
      if (columns_seen[idx])
        throw_error("INVALID_VALUE", "duplicate INSERT column: " + name);
      columns_seen[idx] = true;
      target_cols.push_back(idx);
    }
    if (target_cols.size() != stmt.rows[0].size())
      throw_error("INVALID_VALUE", "column count does not match values");
  }
  TxTable &tx_table = tx_table_ref(table.id());
  int64_t affected = 0;
  for (const auto &values : stmt.rows) {
    if (values.size() != target_cols.size())
      throw_error("INVALID_VALUE", "column count does not match values");
    std::vector<uint8_t> row;
    if (tx_.spool) row.swap(tx_.encode_buffer);
    row.assign(schema.row_size,0);
    table.set_row_valid(row.data(), true);
    // Validate/encode every provided value.  Missing columns stay zero; we
    // require every column to be provided except when a column list is used
    // and the omitted column is nullable -- this dialect does not support NULL,
    // so require completeness.
    std::vector<bool> present(schema.columns.size(), false);
    for (size_t vi = 0; vi < target_cols.size(); ++vi) {
      int ci = target_cols[vi];
      table.encode_value(row.data(), ci, values[vi]);
      present[ci] = true;
    }
    for (size_t ci = 0; ci < schema.columns.size(); ++ci) {
      if (!present[ci]) {
        if (static_cast<int>(ci) == schema.pk_index)
          throw_error("INVALID_VALUE", "primary key value is required");
        // Defaults: numeric zero / empty string / zero vector; the problem
        // does not define NULL, so an explicit value is required.
        throw_error("INVALID_VALUE",
                    "all columns must be provided in INSERT: " +
                        schema.columns[ci].name);
      }
    }
    int64_t pk;
    std::memcpy(&pk, row.data() + schema.col_offsets[schema.pk_index], sizeof(pk));
    table.set_row_id(row.data(), pk);
    auto tx_it = tx_table.rows.find(pk);
    if (tx_it != tx_table.rows.end()) {
      if (!tx_it->second.deleted)
        throw_error("DUPLICATE_KEY", "duplicate primary key: " + std::to_string(pk));
      // Insert after delete: reuse the existing slot.
      RowState state = tx_it->second;
      state.deleted = false;
      state.row = std::move(row);
      state.spool.reset(); state.spool_size = 0;
      state.patches.clear();
      store_row(table, tx_table, pk, std::move(state));
      ++affected;
      continue;
    }
    uint64_t base_pos;
    if (table.find_pk(pk, base_pos))
      throw_error("DUPLICATE_KEY", "duplicate primary key: " + std::to_string(pk));
    RowState state;
    state.existed = false;
    state.deleted = false;
    state.pos = tx_table.next_pos++;
    state.row = std::move(row);
    store_row(table, tx_table, pk, std::move(state));
    ++affected;
  }
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = affected;
  return resp;
}

// ---------------------------------------------------------------------------
// Visible row collection
// ---------------------------------------------------------------------------
void Engine::for_each_visible(
    Table &table, const std::vector<Condition> &where,
    const std::function<bool(const VisibleRow &)> &visit) {
  const TableSchema &schema = table.schema();
  const TxTable *tx_table = find_tx_table(table.id());
  std::vector<uint8_t> materialized;
  for (const auto &condition : where) {
    if (condition.resolved_column != schema.pk_index || condition.op != CompareOp::EQ) continue;
    int64_t pk = condition.typed_value.i;
    uint64_t pos = 0;
    bool overlay = false;
    if (tx_table) {
      auto entry = tx_table->rows.find(pk);
      if (entry != tx_table->rows.end()) {
        if (entry->second.deleted) return;
        table.materialize_state(entry->second, nullptr, materialized);
        pos = entry->second.pos;
        overlay = true;
      }
    }
    if (!overlay && (!table.find_pk(pk, pos) || !table.read_row(pos, materialized))) return;
    if (row_matches(table, materialized.data(), where)) {
      VisibleRow row;
      row.row = materialized.data(); row.pk = pk; row.pos = pos; row.from_overlay = overlay;
      visit(row);
    }
    return;
  }
  for (uint32_t p = 0; p < table.page_count(); ++p) {
    auto page = table.get_page(p);
    if (page->magic != kPageMagic) continue;
    uint32_t n = std::min(page->nrows, page->capacity(schema.row_size));
    for (uint32_t slot = 0; slot < n; ++slot) {
      const uint8_t *row = table.row_data(*page, slot);
      if (!table.row_is_valid(row)) continue;
      int64_t pk = table.row_id(row);
      const uint8_t *use = row;
      bool overlay = false;
      if (tx_table) {
        auto it = tx_table->rows.find(pk);
        if (it != tx_table->rows.end()) {
          if (it->second.deleted) continue;
          table.materialize_state(it->second, row, materialized);
          use = materialized.data();
          overlay = true;
        }
      }
      bool match = true;
      for (const auto &cond : where) {
        if (!table.compare(use, cond)) {
          match = false;
          break;
        }
      }
      if (!match) continue;
      VisibleRow vr;
      vr.row = use;
      vr.page = overlay ? nullptr : page;
      vr.pk = pk;
      vr.pos = static_cast<uint64_t>(p) * schema.rows_per_page + slot;
      vr.from_overlay = overlay;
      if (!visit(vr)) return;
    }
  }
  if (tx_table) {
    std::vector<std::pair<int64_t, const RowState *>> inserted;
    for (const auto &entry : tx_table->rows)
      if (!entry.second.deleted && !entry.second.existed) inserted.emplace_back(entry.first, &entry.second);
    if (tx_.spool) std::sort(inserted.begin(), inserted.end(), [](const auto &a, const auto &b) {
      return a.second->pos < b.second->pos;
    });
    for (const auto &entry : inserted) {
      const RowState &state = *entry.second;
      if (state.deleted || state.existed) continue;
      table.materialize_state(state, nullptr, materialized);
      bool match = true;
      for (const auto &cond : where) {
        if (!table.compare(materialized.data(), cond)) {
          match = false;
          break;
        }
      }
      if (!match) continue;
      VisibleRow vr;
      vr.row = materialized.data();
      vr.page = nullptr;
      vr.pk = entry.first;
      vr.pos = state.pos;
      vr.from_overlay = true;
      if (!visit(vr)) return;
    }
  }
}

void Engine::for_each_visible_key(
    Table &table, const std::vector<Condition> &where,
    const std::function<void(int64_t, uint64_t)> &visit) {
  if (!where.empty()) {
    for_each_visible(table, where, [&](const VisibleRow &row) {
      visit(row.pk, row.pos);
      return true;
    });
    return;
  }
  // An unfiltered identity walk needs no row payload. The committed PK map and
  // transaction overlay already contain that information; don't scan wide
  // records merely to rediscover it. Snapshot keys before callbacks mutate
  // the overlay, and keep any required spool reads in physical order.
  const TxTable *changes = find_tx_table(table.id());
  std::vector<std::pair<int64_t, uint64_t>> keys;
  keys.reserve(table.row_count());
  for (const auto &entry : table.pk_pos_) {
    uint64_t pos = entry.second;
    if (changes) {
      auto changed = changes->rows.find(entry.first);
      if (changed != changes->rows.end()) {
        if (changed->second.deleted) continue;
        pos = changed->second.pos;
      }
    }
    keys.emplace_back(entry.first, pos);
  }
  if (changes) for (const auto &entry : changes->rows)
    if (!entry.second.deleted && !entry.second.existed)
      keys.emplace_back(entry.first, entry.second.pos);
  std::sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) {
    return a.second < b.second;
  });
  for (const auto &entry : keys) visit(entry.first, entry.second);
}

bool Engine::row_matches(const Table &table, const uint8_t *row,
                         const std::vector<Condition> &where) const {
  for (const auto &cond : where) {
    if (!table.compare(row, cond)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Vector kernels
// ---------------------------------------------------------------------------
namespace {
double squared_l2_scalar(const float *a, const float *b, int dim) {
  double sum = 0.0;
  for (int i = 0; i < dim; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2"))) double squared_l2_avx2(const float *a,
                                                          const float *b,
                                                          int dim) {
  __m256d sum0 = _mm256_setzero_pd();
  __m256d sum1 = _mm256_setzero_pd();
  __m256d sum2 = _mm256_setzero_pd();
  __m256d sum3 = _mm256_setzero_pd();
  int i = 0;
  for (; i + 16 <= dim; i += 16) {
    __m256d d0 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i)));
    __m256d d1 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+4)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i+4)));
    __m256d d2 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+8)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i+8)));
    __m256d d3 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+12)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i+12)));
    sum0 = _mm256_add_pd(sum0, _mm256_mul_pd(d0, d0));
    sum1 = _mm256_add_pd(sum1, _mm256_mul_pd(d1, d1));
    sum2 = _mm256_add_pd(sum2, _mm256_mul_pd(d2, d2));
    sum3 = _mm256_add_pd(sum3, _mm256_mul_pd(d3, d3));
  }
  __m256d sum = _mm256_add_pd(_mm256_add_pd(sum0, sum1), _mm256_add_pd(sum2, sum3));
  for (; i + 4 <= dim; i += 4) {
    __m256d va = _mm256_cvtps_pd(_mm_loadu_ps(a + i));
    __m256d vb = _mm256_cvtps_pd(_mm_loadu_ps(b + i));
    __m256d d = _mm256_sub_pd(va, vb);
    sum = _mm256_add_pd(sum, _mm256_mul_pd(d, d));
  }
  double tmp[4];
  _mm256_storeu_pd(tmp, sum);
  double total = tmp[0] + tmp[1] + tmp[2] + tmp[3];
  for (; i < dim; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    total += d * d;
  }
  return total;
}
#endif
double squared_l2_checked(const float *a, const float *b, int dim) {
#if defined(__x86_64__) || defined(_M_X64)
  static const bool has_avx2 =
      __builtin_cpu_supports("avx2");
  if (has_avx2) return squared_l2_avx2(a, b, dim);
#endif
  return squared_l2_scalar(a, b, dim);
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2,fma"))) float squared_l2_float_avx(const float *a,const float *b,int dim) {
  __m256 sum=_mm256_setzero_ps();
  int i=0;
  for (;i+8<=dim;i+=8) {
    __m256 difference=_mm256_sub_ps(_mm256_loadu_ps(a+i),_mm256_loadu_ps(b+i));
    sum=_mm256_fmadd_ps(difference,difference,sum);
  }
  float lanes[8];_mm256_storeu_ps(lanes,sum);
  float total=0;
  for (float lane:lanes) total+=lane;
  for (;i<dim;++i) {float d=a[i]-b[i];total+=d*d;}
  return total;
}
#endif
double squared_l2_approximate(const float *a,const float *b,int dim) {
  float total=0;
#if defined(__x86_64__) || defined(_M_X64)
  static const bool fast=__builtin_cpu_supports("avx2")&&__builtin_cpu_supports("fma");
  if (fast) total=squared_l2_float_avx(a,b,dim);
  else
#endif
    for (int i=0;i<dim;++i) {float d=a[i]-b[i];total+=d*d;}
  return total>0&&std::isfinite(total) ? static_cast<double>(total) : squared_l2_checked(a,b,dim);
}

// Promote stored float32 values before subtraction and squaring. All finite
// float32 distances fit in double, including subnormals whose squares would
// silently underflow in a float accumulator.

long double squared_l2_precise(const float *a, const float *b, int dim) {
  long double sum = 0, correction = 0;
  for (int i = 0; i < dim; ++i) {
    long double difference = static_cast<long double>(a[i]) - static_cast<long double>(b[i]);
    long double term = difference * difference;
    long double next = sum + term;
    if (std::fabs(sum) >= std::fabs(term)) correction += (sum - next) + term;
    else correction += (term - next) + sum;
    sum = next;
  }
  return sum + correction;
}

struct Candidate {
  double dist;
  int64_t pk;
  uint64_t pos;
  mutable bool precise_ready = false;
  mutable long double precise = 0;
};
// Max-heap where the top is the worst candidate.
struct CandidateHeap {
  std::vector<Candidate> data;
  size_t k = 10;
  uint64_t uncertainty_ulps = 0;
  std::function<long double(const Candidate &)> refine;
  long double precise(const Candidate &candidate) const {
    if (!candidate.precise_ready) {
      candidate.precise = refine(candidate);
      candidate.precise_ready = true;
    }
    return candidate.precise;
  }
  bool less(const Candidate &a, const Candidate &b) const {
    if (a.dist == 0 && b.dist == 0) return a.pk < b.pk;
    // Squared distances are nonnegative finite doubles. Their bit patterns
    // are ordered, so a conservative ULP band avoids extra work for ordinary
    // queries while resolving rounding-dependent ties with compensation.
    uint64_t first, second;
    std::memcpy(&first, &a.dist, sizeof(first));
    std::memcpy(&second, &b.dist, sizeof(second));
    uint64_t gap = first > second ? first - second : second - first;
    if (gap > uncertainty_ulps) return a.dist < b.dist;
    long double left = precise(a), right = precise(b);
    if (left != right) return left < right;
    return a.pk < b.pk;
  }
  void push(Candidate candidate) {
    auto compare = [this](const Candidate &a, const Candidate &b) { return less(a, b); };
    if (data.size() < k) {
      data.push_back(std::move(candidate));
      std::push_heap(data.begin(), data.end(), compare);
    } else if (less(candidate, data.front())) {
      std::pop_heap(data.begin(), data.end(), compare);
      data.back() = std::move(candidate);
      std::push_heap(data.begin(), data.end(), compare);
    }
  }
  void push(double dist, int64_t pk, uint64_t pos) { push(Candidate{dist, pk, pos}); }
  void merge(CandidateHeap &other) {
    for (const auto &c : other.data) push(c);
  }
};
}  // namespace

void Engine::drop_vector_index(Table &table) {
  vector_search_bytes_ -= table.vector_search_bytes_;
  table.vector_search_bytes_ = 0;
  table.vector_search_.reset();
}

void Engine::rebuild_vector_index(Table &table) {
  drop_vector_index(table);
  table.vector_search_unavailable_ = false;
  const auto &schema = table.schema();
  if (!schema.vector_index_created || schema.vector_index < 0 || tx_.active) return;
  try {
    auto index = std::make_unique<VectorIndex>(schema.columns[schema.vector_index].dim,
        schema.col_offsets[schema.vector_index], schema.columns, schema.col_offsets,
        squared_l2_approximate, table.row_count(), ann_min_rows_);
    size_t charge = index->estimate(table.row_count());
    if (charge > kVectorIndexBudget - vector_search_bytes_) {
      index = std::make_unique<VectorIndex>(schema.columns[schema.vector_index].dim,
          schema.col_offsets[schema.vector_index], schema.columns, schema.col_offsets,
          squared_l2_approximate, table.row_count(), ann_min_rows_, false);
      charge=index->estimate(table.row_count());
    }
    if (charge > kVectorIndexBudget - vector_search_bytes_) {
      table.vector_search_unavailable_ = true;
      return;
    }
    index->reserve(table.row_count());
    for_each_visible(table, {}, [&](const VisibleRow &row) {
      index->upsert(row.pk, row.pos, row.row);
      return true;
    });
    table.vector_search_bytes_ = charge;
    vector_search_bytes_ += charge;
    table.vector_search_ = std::move(index);
  } catch (const std::bad_alloc &) {
    table.vector_search_unavailable_ = true;
  }
}

void Engine::update_vector_index(Table &table, const TxTable &changes) {
  auto *index = table.vector_search_.get();
  if (!index) return;
  if (changes.rows.size()>4096) {
    bool rebuild=false;
    for (const auto &entry:changes.rows) {
      const auto &state=entry.second;
      rebuild=rebuild||state.deleted||state.has_raw_row();
      for (const auto &patch:state.patches) for (const auto &change:*patch)
        rebuild=rebuild||change.first==table.schema().vector_index||index->relevant_column(change.first);
      if (rebuild) break;
    }
    if (rebuild) {
      drop_vector_index(table);
      table.vector_search_unavailable_=false;
      return;
    }
  }
  try {
    for (const auto &entry : changes.rows) {
      const RowState &state = entry.second;
      if (state.deleted) { index->erase(entry.first); continue; }
      bool relevant = state.has_raw_row();
      for (const auto &patch : state.patches)
        for (const auto &change : *patch)
          relevant = relevant || change.first == table.schema().vector_index || index->relevant_column(change.first);
      if (!relevant) continue;
      size_t charge = index->estimate(index->count() + 1);
      if (charge > kVectorIndexBudget - vector_search_bytes_ + table.vector_search_bytes_) {
        drop_vector_index(table);
        table.vector_search_unavailable_ = true;
        return;
      }
      std::vector<uint8_t> row;
      if (!table.read_row(state.pos, row)) throw_error("CORRUPTION", "committed index row missing");
      index->upsert(entry.first, state.pos, row.data());
      vector_search_bytes_ = vector_search_bytes_ - table.vector_search_bytes_ + charge;
      table.vector_search_bytes_ = charge;
    }
    if (index->needs_compaction()) {
      drop_vector_index(table);
      table.vector_search_unavailable_ = false;  // Rebuild after the transaction has cleared.
    }
  } catch (const std::bad_alloc &) {
    drop_vector_index(table);
    table.vector_search_unavailable_ = true;
  }
}

std::string Engine::format_vector(const float *v, int dim) const {
  std::string out = "[";
  char buf[64];
  for (int i = 0; i < dim; ++i) {
    if (i) out.push_back(',');
    std::snprintf(buf, sizeof(buf), "%.9g", v[i]);
    out += buf;
  }
  out.push_back(']');
  return out;
}

OutValue Engine::out_value_from_row(const Table &table, const uint8_t *row,
                                    int col_index) {
  OutValue out;
  const ColumnDef &col = table.schema().columns[col_index];
  const uint8_t *base = row + table.schema().col_offsets[col_index];
  if (col.type == ColumnType::BIGINT) {
    int64_t v;
    std::memcpy(&v, base, sizeof(v));
    out.kind = OutValue::Kind::INT;
    out.i = v;
  } else if (col.type == ColumnType::VARCHAR) {
    uint32_t len;
    std::memcpy(&len, base, 4);
    out.kind = OutValue::Kind::STRING;
    out.s.assign(reinterpret_cast<const char *>(base + 4), len);
  } else {
    const float *fv = reinterpret_cast<const float *>(base);
    out.kind = OutValue::Kind::VECTOR;
    out.vec.assign(fv, fv + col.dim);
    out.s = format_vector(fv, col.dim);  // textual form for ORDER BY fallback
  }
  return out;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------
namespace {
struct SortKey {
  OutValue::Kind kind = OutValue::Kind::INT;
  int64_t i = 0;
  long double d = 0;
  std::string s;
};
int compare_sort_keys(const SortKey &a, const SortKey &b) {
  if (a.kind == OutValue::Kind::INT && b.kind == OutValue::Kind::INT)
    return (a.i < b.i) ? -1 : (a.i > b.i) ? 1 : 0;
  if (a.kind == OutValue::Kind::DOUBLE && b.kind == OutValue::Kind::DOUBLE)
    return (a.d < b.d) ? -1 : (a.d > b.d) ? 1 : 0;
  if (a.kind == OutValue::Kind::STRING && b.kind == OutValue::Kind::STRING)
    return a.s.compare(b.s);
  // Mixed types are not expected; fall back to type then text.
  if (a.kind != b.kind) return static_cast<int>(a.kind) - static_cast<int>(b.kind);
  if (a.kind == OutValue::Kind::INT) return (a.i < b.i) ? -1 : (a.i > b.i) ? 1 : 0;
  if (a.kind == OutValue::Kind::DOUBLE) return (a.d < b.d) ? -1 : (a.d > b.d) ? 1 : 0;
  return a.s.compare(b.s);
}
}  // namespace

Response Engine::exec_select(SelectStmt &stmt) {
  Table &table = require_table(stmt.table);
  const TableSchema &schema = table.schema();
  compile_conditions(table, stmt.where);

  // Resolve output columns.
  struct ItemPlan {
    bool star = false;
    bool is_distance = false;
    int column_index = -1;
    int vector_column = -1;
    std::vector<float> query_vec;
    std::string name;
  };
  std::vector<ItemPlan> plans;
  std::vector<std::string> out_columns;
  for (const auto &item : stmt.items) {
    if (item.star) {
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        ItemPlan q;
        q.column_index = static_cast<int>(i);
        q.name = schema.columns[i].name;
        plans.push_back(q);
        out_columns.push_back(q.name);
      }
      continue;
    }
    ItemPlan p;
    if (item.expr.kind == Expr::Kind::COLUMN) {
      int ci = table.column_index(item.expr.column);
      if (ci < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + item.expr.column);
      p.column_index = ci;
      p.name = item.alias.empty() ? schema.columns[ci].name : item.alias;
    } else if (item.expr.kind == Expr::Kind::L2_DISTANCE) {
      int ci = table.column_index(item.expr.column);
      if (ci < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + item.expr.column);
      if (schema.columns[ci].type != ColumnType::VECTOR)
        throw_error("INVALID_ARGUMENT", "L2_DISTANCE requires a VECTOR column");
      p.is_distance = true;
      p.vector_column = ci;
      parse_vector_for_column(item.expr.vec_text, schema.columns[ci], p.query_vec);
      p.name = item.alias.empty() ? "L2_DISTANCE" : item.alias;
    } else {
      throw_error("UNSUPPORTED", "unsupported select expression");
    }
    plans.push_back(p);
    out_columns.push_back(p.name);
  }

  // Resolve ORDER BY before reading any rows, including empty tables and
  // LIMIT 0. An alias is an exact binding, never a guessed name such as "d".
  struct OrderPlan {
    int output = -1;
    int column = -1;
    bool distance = false;
    std::vector<float> query;
  };
  std::vector<OrderPlan> orders;
  for (const auto &key : stmt.order) {
    OrderPlan order;
    if (key.is_distance) {
      order.column = table.column_index(key.expr.column);
      if (order.column < 0)
        throw_error("COLUMN_NOT_FOUND", "ORDER BY column not found: " + key.expr.column);
      if (schema.columns[order.column].type != ColumnType::VECTOR)
        throw_error("INVALID_ARGUMENT", "L2_DISTANCE requires a VECTOR column");
      order.distance = true;
      parse_vector_for_column(key.expr.vec_text, schema.columns[order.column], order.query);
    } else {
      if (key.ordinal > 0) {
        order.output = key.ordinal - 1;
        if (static_cast<size_t>(order.output) >= plans.size())
          throw_error("COLUMN_NOT_FOUND", "ORDER BY ordinal out of range");
      } else {
        for (size_t i = 0; i < plans.size(); ++i) {
          if (lower_copy(plans[i].name) == lower_copy(key.name)) {
            order.output = static_cast<int>(i);
            break;
          }
        }
      }
      if (order.output >= 0) {
        const auto &plan = plans[order.output];
        order.distance = plan.is_distance;
        order.column = plan.is_distance ? plan.vector_column : plan.column_index;
        order.query = plan.query_vec;
      } else {
        order.column = table.column_index(key.name);
        if (order.column < 0)
          throw_error("COLUMN_NOT_FOUND", "ORDER BY column not found: " + key.name);
      }
      if (!order.distance && schema.columns[order.column].type == ColumnType::VECTOR)
        throw_error("UNSUPPORTED", "ORDER BY VECTOR requires L2_DISTANCE");
    }
    orders.push_back(std::move(order));
  }
  bool topk_path = false;
  int64_t topk = 0;
  int topk_vector_column = -1;
  std::vector<float> topk_query_vec;
  if (stmt.has_limit && stmt.offset == 0 && stmt.order.size() == 1 &&
      !stmt.order[0].desc) {
    if (orders[0].distance) {
      topk_vector_column = orders[0].column;
      topk_query_vec = orders[0].query;
      topk_path = true;
    }
    if (topk_path) topk = stmt.limit;
  }

  Response resp;
  resp.is_query = true;
  resp.columns = out_columns;

  auto project_row = [&](const uint8_t *row) -> std::vector<OutValue> {
    std::vector<OutValue> vals;
    vals.reserve(plans.size());
    for (const auto &p : plans) {
      if (p.star) {
        for (size_t ci = 0; ci < schema.columns.size(); ++ci)
          vals.push_back(out_value_from_row(table, row, static_cast<int>(ci)));
      } else if (p.is_distance) {
        const uint8_t *base = row + schema.col_offsets[p.vector_column];
        const float *vec = reinterpret_cast<const float *>(base);
        double sq = squared_l2_checked(vec, p.query_vec.data(),
                                       schema.columns[p.vector_column].dim);
        OutValue ov;
        ov.kind = OutValue::Kind::DOUBLE;
        ov.d = std::sqrt(sq);
        vals.push_back(std::move(ov));
      } else {
        vals.push_back(out_value_from_row(table, row, p.column_index));
      }
    }
    return vals;
  };


  if (topk_path && topk >= 0) {
    if (topk == 0) return resp;
    auto load_candidate = [&](const Candidate &candidate, std::vector<uint8_t> &row) {
      if (tx_.active) {
        const TxTable *overlay = find_tx_table(table.id());
        if (overlay) {
          auto found = overlay->rows.find(candidate.pk);
          if (found != overlay->rows.end() && !found->second.deleted) {
            table.materialize_state(found->second, nullptr, row);
            return true;
          }
        }
      }
      return table.read_row(candidate.pos, row);
    };
    auto refine = [&](const Candidate &candidate) {
      uint32_t cached=0;
      if (!tx_.active && table.vector_search_ && table.vector_search_->find(candidate.pk,cached))
        return squared_l2_precise(table.vector_search_->vector(cached),topk_query_vec.data(),schema.columns[topk_vector_column].dim);
      std::vector<uint8_t> row;
      if (!load_candidate(candidate, row))
        throw_error("CORRUPTION", "TopK candidate row missing");
      const float *vector = reinterpret_cast<const float *>(row.data() + schema.col_offsets[topk_vector_column]);
      return squared_l2_precise(vector, topk_query_vec.data(), schema.columns[topk_vector_column].dim);
    };
    // Parallel exact top-K scan.
    std::vector<CandidateHeap> heaps;
    int thread_count = 1;
#ifdef _OPENMP
    thread_count = omp_get_max_threads();
    if (thread_count < 1) thread_count = 1;
    heaps.resize(static_cast<size_t>(thread_count));
    for (auto &h : heaps) h.k = static_cast<size_t>(topk);
#endif
#ifndef _OPENMP
    heaps.resize(1);
    heaps[0].k = static_cast<size_t>(topk);
#endif
    for (auto &heap : heaps) {
      heap.uncertainty_ulps = 16 * (static_cast<uint64_t>(schema.columns[topk_vector_column].dim) + 1);
      heap.refine = refine;
    }
    if (!tx_.active && table.schema().vector_index_created && !table.vector_search_ &&
        !table.vector_search_unavailable_) rebuild_vector_index(table);
    auto *index = table.vector_search_.get();
    if (!tx_.active && index && index->supports(stmt.where)) {
      bool approximate = false;
      if (ann_mode_ && stmt.where.empty() && index->approximate_available(static_cast<size_t>(topk))) {
        auto hits=index->search(topk_query_vec.data(),static_cast<size_t>(topk));
        for (auto &hit:hits) hit.distance=squared_l2_checked(index->vector(hit.node),topk_query_vec.data(),schema.columns[topk_vector_column].dim);
        std::sort(hits.begin(),hits.end(),[&](const auto&a,const auto&b){
          return a.distance!=b.distance ? a.distance<b.distance : index->node(a.node).pk<index->node(b.node).pk;
        });
        hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const VectorIndex::Hit &hit) {
          return !index->node(hit.node).active;
        }), hits.end());
        // Dense ties need all eligible IDs to enforce the canonical PK order.
        if (hits.size() > static_cast<size_t>(topk)) {
          uint64_t first, second;
          std::memcpy(&first, &hits[static_cast<size_t>(topk)-1].distance, 8);
          std::memcpy(&second, &hits[static_cast<size_t>(topk)].distance, 8);
          uint64_t gap = first > second ? first-second : second-first;
          approximate = gap > heaps[0].uncertainty_ulps;
        }
        if (approximate) for (const auto &hit : hits) {
          const auto &node = index->node(hit.node);
          heaps[0].push(hit.distance, node.pk, node.pos);
        }
      }
      if (!approximate) {
        const auto *matching = index->matching_nodes(stmt.where);
        size_t scan_count = matching ? matching->size() : index->count();
#ifdef _OPENMP
#pragma omp parallel for num_threads(thread_count) schedule(static)
#endif
        for (int64_t i = 0; i < static_cast<int64_t>(scan_count); ++i) {
          uint32_t id = matching ? (*matching)[static_cast<size_t>(i)] : static_cast<uint32_t>(i);
          if (!matching && !index->matches(id, stmt.where)) continue;
          int tid = 0;
#ifdef _OPENMP
          tid = omp_get_thread_num();
#endif
          const auto &node = index->node(id);
          heaps[static_cast<size_t>(tid)].push(squared_l2_checked(index->vector(id),
              topk_query_vec.data(), schema.columns[topk_vector_column].dim), node.pk, node.pos);
        }
        for (size_t t = 1; t < heaps.size(); ++t) heaps[0].merge(heaps[t]);
      }
    } else if (tx_.active) {
      for_each_visible(table, stmt.where, [&](const VisibleRow &vr) {
        const uint8_t *base = vr.row + schema.col_offsets[topk_vector_column];
        const float *vec = reinterpret_cast<const float *>(base);
        double sq = squared_l2_checked(vec, topk_query_vec.data(),
                                       schema.columns[topk_vector_column].dim);
        heaps[0].push(sq, vr.pk, vr.pos);
        return true;
      });
    } else {
      uint32_t pages = table.page_count();
#ifdef _OPENMP
#pragma omp parallel num_threads(thread_count)
      {
        int tid = omp_get_thread_num();
        CandidateHeap &heap = heaps[static_cast<size_t>(tid)];
#pragma omp for schedule(static)
        for (int64_t pi = 0; pi < static_cast<int64_t>(pages); ++pi) {
          auto page = table.get_page(static_cast<uint32_t>(pi));
          if (page->magic != kPageMagic) continue;
          uint32_t n = std::min(page->nrows, page->capacity(schema.row_size));
          for (uint32_t slot = 0; slot < n; ++slot) {
            const uint8_t *row = table.row_data(*page, slot);
            if (!table.row_is_valid(row)) continue;
            bool match = true;
            for (const auto &cond : stmt.where) {
              if (!table.compare(row, cond)) { match = false; break; }
            }
            if (!match) continue;
            const uint8_t *base = row + schema.col_offsets[topk_vector_column];
            const float *vec = reinterpret_cast<const float *>(base);
            double sq = squared_l2_checked(vec, topk_query_vec.data(),
                                           schema.columns[topk_vector_column].dim);
            int64_t pk = table.row_id(row);
            heap.push(sq, pk, static_cast<uint64_t>(pi) * schema.rows_per_page + slot);
          }
        }
      }
#else
      for (uint32_t pi = 0; pi < pages; ++pi) {
        auto page = table.get_page(pi);
        if (page->magic != kPageMagic) continue;
        uint32_t n = std::min(page->nrows, page->capacity(schema.row_size));
        for (uint32_t slot = 0; slot < n; ++slot) {
          const uint8_t *row = table.row_data(*page, slot);
          if (!table.row_is_valid(row)) continue;
          bool match = true;
          for (const auto &cond : stmt.where) {
            if (!table.compare(row, cond)) { match = false; break; }
          }
          if (!match) continue;
          const uint8_t *base = row + schema.col_offsets[topk_vector_column];
          const float *vec = reinterpret_cast<const float *>(base);
          double sq = squared_l2_checked(vec, topk_query_vec.data(),
                                         schema.columns[topk_vector_column].dim);
          int64_t pk = table.row_id(row);
          heaps[0].push(sq, pk, static_cast<uint64_t>(pi) * schema.rows_per_page + slot);
        }
      }
#endif
      for (size_t t = 1; t < heaps.size(); ++t) heaps[0].merge(heaps[t]);
    }
    std::vector<Candidate> finalists = heaps[0].data;
    std::sort(finalists.begin(), finalists.end(),
              [&](const Candidate &a, const Candidate &b) { return heaps[0].less(a, b); });
    for (const auto &c : finalists) {
      uint32_t cached=0;
      bool covered=!tx_.active&&index&&index->find(c.pk,cached);
      for (const auto &plan:plans)
        covered=covered&&(plan.is_distance||plan.column_index==schema.vector_index||index->relevant_column(plan.column_index));
      if (covered) {
        std::vector<OutValue> values;
        for (const auto &plan:plans) {
          OutValue value;
          if (plan.is_distance) {
            value.kind=OutValue::Kind::DOUBLE;
            value.d=std::sqrt(squared_l2_checked(index->vector(cached),plan.query_vec.data(),schema.columns[plan.vector_column].dim));
          } else if (plan.column_index==schema.vector_index) {
            value.kind=OutValue::Kind::VECTOR;
            value.vec.assign(index->vector(cached),index->vector(cached)+schema.columns[plan.column_index].dim);
          } else if (schema.columns[plan.column_index].type==ColumnType::BIGINT) {
            value.kind=OutValue::Kind::INT;value.i=index->integer(cached,plan.column_index);
          } else {
            value.kind=OutValue::Kind::STRING;value.s=index->string(cached,plan.column_index);
          }
          values.push_back(std::move(value));
        }
        resp.rows.push_back(std::move(values));
        continue;
      }
      std::vector<uint8_t> row;
      if (!load_candidate(c, row)) continue;
      resp.rows.push_back(project_row(row.data()));
    }
    return resp;
  }

  // Streaming path for no-ORDER-BY queries with LIMIT: avoids materializing
  // the whole table when the dataset is larger than the page cache/memory.
  if (stmt.order.empty() && stmt.has_limit) {
    int64_t seen = 0;
    int64_t wanted = (stmt.offset > std::numeric_limits<int64_t>::max() - stmt.limit)
                         ? std::numeric_limits<int64_t>::max()
                         : stmt.offset + stmt.limit;
    if (stmt.limit == 0) return resp;
    for_each_visible(table, stmt.where, [&](const VisibleRow &vr) {
      if (seen >= stmt.offset) resp.rows.push_back(project_row(vr.row));
      ++seen;
      return seen < wanted;
    });
    return resp;
  }

  // Generic path.
  struct ResultRow {
    std::vector<OutValue> vals;
    std::vector<SortKey> keys;
    int64_t pk;
    uint64_t pos;
  };
  std::vector<bool> desc;
  for (const auto &o : stmt.order) desc.push_back(o.desc);
  auto row_less = [&](const ResultRow &a, const ResultRow &b) {
    for (size_t i = 0; i < desc.size(); ++i) {
      int cmp = compare_sort_keys(a.keys[i], b.keys[i]);
      if (cmp != 0) return desc[i] ? cmp > 0 : cmp < 0;
    }
    // Ties are resolved by primary key ascending.
    return a.pk < b.pk;
  };
  if (stmt.has_limit && stmt.limit == 0) return resp;
  bool bounded = stmt.has_limit && !stmt.order.empty() && stmt.limit > 0;
  int64_t need = 0;
  if (bounded) {
    need = (stmt.offset > std::numeric_limits<int64_t>::max() - stmt.limit)
               ? std::numeric_limits<int64_t>::max()
               : stmt.offset + stmt.limit;
  }
  std::vector<ResultRow> results;
  auto consume = [&](const VisibleRow &vr) {
    ResultRow rr;
    rr.vals = project_row(vr.row);
    rr.pk = vr.pk;
    rr.pos = vr.pos;
    for (const auto &order : orders) {
      SortKey key;
      if (order.distance) {
        int ci = order.column;
        const float *vector = reinterpret_cast<const float *>(vr.row + schema.col_offsets[ci]);
        key.kind = OutValue::Kind::DOUBLE;
        key.d = squared_l2_precise(vector, order.query.data(), schema.columns[ci].dim);
      } else if (order.output >= 0) {
        const OutValue &ov = rr.vals[order.output];
        key.kind = ov.kind;
        key.i = ov.i;
        key.d = ov.d;
        key.s = ov.s;
      } else {
        OutValue ov = out_value_from_row(table, vr.row, order.column);
        key.kind = ov.kind;
        key.i = ov.i;
        key.d = ov.d;
        key.s = ov.s;
      }
      rr.keys.push_back(std::move(key));
    }
    if (bounded) {
      if (static_cast<int64_t>(results.size()) < need) {
        results.push_back(std::move(rr));
        std::push_heap(results.begin(), results.end(), row_less);
      } else if (row_less(rr, results.front())) {
        std::pop_heap(results.begin(), results.end(), row_less);
        results.back() = std::move(rr);
        std::push_heap(results.begin(), results.end(), row_less);
      }
    } else {
      results.push_back(std::move(rr));
    }
    return true;
  };
  bool identity_only = stmt.where.empty() && !orders.empty() &&
      std::all_of(plans.begin(), plans.end(), [&](const auto &plan) {
        return !plan.is_distance && plan.column_index == schema.pk_index;
      }) && std::all_of(orders.begin(), orders.end(), [&](const auto &order) {
        if (order.distance) return false;
        return order.output >= 0 ? plans[static_cast<size_t>(order.output)].column_index == schema.pk_index
                                 : order.column == schema.pk_index;
      });
  if (identity_only) {
    // Project/order only the immutable primary key from the already visible
    // identity maps. All other projections, predicates and order keys still
    // use actual row data through the ordinary scan.
    std::vector<uint8_t> identity(schema.row_size, 0);
    table.set_row_valid(identity.data(), true);
    for_each_visible_key(table, {}, [&](int64_t pk, uint64_t pos) {
      table.set_row_id(identity.data(), pk);
      std::memcpy(identity.data()+schema.col_offsets[schema.pk_index], &pk, sizeof(pk));
      VisibleRow row;
      row.row = identity.data(); row.pk = pk; row.pos = pos;
      consume(row);
    });
  } else for_each_visible(table, stmt.where, consume);
  if (!stmt.order.empty())
    std::sort(results.begin(), results.end(), row_less);
  int64_t start = stmt.has_limit ? stmt.offset : 0;
  int64_t end = stmt.has_limit
                    ? ((stmt.offset > std::numeric_limits<int64_t>::max() - stmt.limit)
                           ? std::numeric_limits<int64_t>::max()
                           : stmt.offset + stmt.limit)
                    : static_cast<int64_t>(results.size());
  if (start < 0) start = 0;
  if (end > static_cast<int64_t>(results.size()))
    end = static_cast<int64_t>(results.size());
  for (int64_t i = start; i < end; ++i)
    resp.rows.push_back(std::move(results[static_cast<size_t>(i)].vals));
  return resp;
}

// ---------------------------------------------------------------------------
// UPDATE / DELETE
// ---------------------------------------------------------------------------
Response Engine::exec_update(UpdateStmt &stmt) {
  Table &table = require_table(stmt.table);
  const TableSchema &schema = table.schema();
  compile_conditions(table, stmt.where);
  // Resolve SET columns and typed values.
  auto patch = std::make_shared<RowPatch>();
  std::vector<uint8_t> validation_row(schema.row_size, 0);
  for (auto &kv : stmt.sets) {
    int ci = table.column_index(kv.first);
    if (ci < 0)
      throw_error("COLUMN_NOT_FOUND", "column not found: " + kv.first);
    if (ci == schema.pk_index)
      throw_error("UNSUPPORTED", "updating the primary key is not supported");
    Value v = coerce_value(schema, ci, kv.second);
    // Deferred patches must be valid when UPDATE returns, including VARCHAR
    // lengths, rather than failing at COMMIT or a later SELECT.
    table.encode_value(validation_row.data(), ci, v);
    patch->emplace_back(ci, std::move(v));
  }
  auto tracked_patch=tx_.track_patch(std::move(*patch));
  TxTable &tx_table = tx_table_ref(table.id());
  int64_t matched = 0;
  for_each_visible_key(table, stmt.where, [&](int64_t pk, uint64_t pos) {
    RowState state;
    auto it = tx_table.rows.find(pk);
    if (it != tx_table.rows.end()) {
      state = it->second;
    } else {
      state.existed = true;
      state.pos = pos;
    }
    state.deleted = false;
    state.patches.push_back(tracked_patch);
    if (state.patches.size()>1) {
      std::vector<int> covered;
      std::vector<std::shared_ptr<const RowPatch>> kept;
      for (auto candidate=state.patches.rbegin();candidate!=state.patches.rend();++candidate) {
        bool needed=false;
        for (const auto &change:**candidate)
          needed=needed||std::find(covered.begin(),covered.end(),change.first)==covered.end();
        if (!needed) continue;
        kept.push_back(*candidate);
        for (const auto &change:**candidate)
          if (std::find(covered.begin(),covered.end(),change.first)==covered.end()) covered.push_back(change.first);
      }
      std::reverse(kept.begin(),kept.end());state.patches=std::move(kept);
    }
    store_row(table, tx_table, pk, std::move(state));
    ++matched;
  });
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = matched;
  return resp;
}

Response Engine::exec_delete(DeleteStmt &stmt) {
  Table &table = require_table(stmt.table);
  compile_conditions(table, stmt.where);
  TxTable &tx_table = tx_table_ref(table.id());
  int64_t matched = 0;
  for_each_visible_key(table, stmt.where, [&](int64_t pk, uint64_t pos) {
    RowState state;
    auto it = tx_table.rows.find(pk);
    if (it != tx_table.rows.end()) {
      state = it->second;
    } else {
      state.existed = true;
      state.pos = pos;
    }
    state.deleted = true;
    std::vector<uint8_t>().swap(state.row);  // Tombstones retain no raw capacity.
    state.spool.reset(); state.spool_size = 0;
    state.patches.clear();
    store_row(table, tx_table, pk, std::move(state));
    ++matched;
  });
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = matched;
  return resp;
}

}  // namespace mvd
