#include "engine.h"

#include <algorithm>
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

// Parses a vector literal with detailed error classification matching the
// task package's semantic error names.
void parse_vector_for_column(const std::string &text, const ColumnDef &col,
                             std::vector<float> &out) {
  out.clear();
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= text.size() || text[i] != '[')
    throw_error("INVALID_VECTOR", "expected numeric vector component");
  ++i;
  std::vector<std::string> parts;
  while (true) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < text.size() && text[i] == ']') { ++i; break; }
    size_t start = i;
    while (i < text.size() && text[i] != ',' && text[i] != ']') ++i;
    size_t end = i;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    if (end == start) throw_error("INVALID_VECTOR", "expected numeric vector component");
    parts.emplace_back(text.substr(start, end - start));
    if (i < text.size() && text[i] == ',') { ++i; continue; }
    if (i < text.size() && text[i] == ']') { ++i; break; }
    throw_error("INVALID_VECTOR", "expected numeric vector component");
  }
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i != text.size()) throw_error("INVALID_VECTOR", "expected numeric vector component");
  for (const auto &part : parts) {
    char *endp = nullptr;
    errno = 0;
    double v = std::strtod(part.c_str(), &endp);
    if (endp == part.c_str() || *endp != '\0' || !std::isfinite(v))
      throw_error("INVALID_VECTOR", "expected numeric vector component");
    if (errno == ERANGE || v > FLT_MAX || v < -FLT_MAX)
      throw_error("VECTOR_OVERFLOW", "vector component overflows float32");
    out.push_back(static_cast<float>(v));
  }
  if (static_cast<int>(out.size()) != col.dim)
    throw_error("VECTOR_DIM_MISMATCH",
                col.name + " must have " + std::to_string(col.dim) + " dimensions");
}
}  // namespace

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
  auto page = std::shared_ptr<Page>(new Page());
  if (!table->load_page_raw(page_no, *page)) {
    page->magic = kPageMagic;
    page->nrows = 0;
    page->page_no = page_no;
  }
  Entry entry;
  entry.page = page;
  entry.dirty = false;
  entry.owner = table;
  entry.page_no = page_no;
  lru_.push_front(k);
  entry.lru_it = lru_.begin();
  pages_.emplace(k, std::move(entry));
  evict_if_needed();
  return page;
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
    bool evicted = false;
    for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
      auto p = pages_.find(*it);
      if (p == pages_.end()) continue;
      if (p->second.page.use_count() > 1) continue;
      if (p->second.dirty && p->second.owner) {
        p->second.owner->write_page_to_disk(p->second.page_no,
                                            *p->second.page);
      }
      lru_.erase(std::next(it).base());
      pages_.erase(p);
      evicted = true;
      break;
    }
    if (!evicted) break;  // all pages are pinned; allow temporary overshoot
  }
}

void PageCache::flush_all(
    const std::unordered_map<uint32_t, std::unique_ptr<Table>> &tables) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &kv : pages_) {
    if (kv.second.dirty && kv.second.owner) {
      kv.second.owner->write_page_to_disk(kv.second.page_no, *kv.second.page);
      kv.second.dirty = false;
    }
  }
  for (auto &t : tables) t.second->fsync_file();
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

void Table::open() {
  int flags = O_RDWR | O_CREAT;
#ifndef _WIN32
  file_ = FileHandle::open(data_path_, flags | O_DIRECT);
  direct_io_ = file_.valid();
  if (!file_.valid()) file_ = FileHandle::open(data_path_, flags);
#else
  file_ = FileHandle::open(data_path_, flags);
#endif
  if (!file_.valid())
    throw_error("IO_ERROR", "cannot open table file: " + data_path_);
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

void Table::refresh_after_recovery() {
  int64_t size = file_.size();
  if (size < 0) throw_error("IO_ERROR", "cannot stat table file");
  uint32_t total_pages = static_cast<uint32_t>(size / kPageSize);
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
  for (uint32_t p = 0; p < page_count_; ++p) {
    Page page;
    if (!load_page_raw(p, page)) continue;
    if (page.magic != kPageMagic) continue;
    uint32_t n = std::min(page.nrows, page.capacity(schema_.row_size));
    for (uint32_t slot = 0; slot < n; ++slot) {
      const uint8_t *row = page.row_ptr(slot, schema_.row_size);
      if (!row_is_valid(row)) continue;
      int64_t pk = row_id(row);
      pk_pos_[pk] = static_cast<uint64_t>(p) * schema_.rows_per_page + slot;
    }
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
  uint64_t offset = static_cast<uint64_t>(page_no) * kPageSize;
  if (!file_.pwrite_full(&page, kPageSize, offset)) {
    if (direct_io_ && reopen_buffered()) {
      if (!file_.pwrite_full(&page, kPageSize, offset))
        throw_error("IO_ERROR", "failed to write table page");
    } else {
      throw_error("IO_ERROR", "failed to write table page");
    }
  }
  if (page_no >= page_count_) page_count_ = page_no + 1;
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

std::vector<PageImage> Table::build_commit_images(const TxTable &tx) {
  struct Change {
    uint64_t pos;
    bool deleted;
    const std::vector<uint8_t> *row;
  };
  std::unordered_map<uint32_t, std::vector<Change>> by_page;
  for (const auto &kv : tx.rows) {
    const RowState &state = kv.second;
    if (state.deleted && !state.existed) continue;  // insert then delete: no-op
    uint32_t page_no = static_cast<uint32_t>(state.pos / schema_.rows_per_page);
    by_page[page_no].push_back(Change{state.pos, state.deleted, &state.row});
  }
  std::vector<PageImage> images;
  images.reserve(by_page.size());
  for (auto &kv : by_page) {
    uint32_t page_no = kv.first;
    std::shared_ptr<Page> base;
    if (page_no < page_count_) {
      base = cache_->get(this, page_no);
    } else {
      base = std::shared_ptr<Page>(new Page());
      base->magic = kPageMagic;
      base->nrows = 0;
      base->page_no = page_no;
    }
    auto image = std::shared_ptr<Page>(new Page(*base));
    image->page_no = page_no;
    image->magic = kPageMagic;
    std::sort(kv.second.begin(), kv.second.end(),
              [](const Change &a, const Change &b) { return a.pos < b.pos; });
    uint32_t max_slots = image->capacity(schema_.row_size);
    for (const Change &ch : kv.second) {
      uint32_t slot = static_cast<uint32_t>(ch.pos % schema_.rows_per_page);
      if (slot >= max_slots)
        throw_error("CORRUPTION", "row slot outside page capacity");
      uint8_t *dst = image->row_ptr(slot, schema_.row_size);
      if (ch.deleted) {
        if (slot < image->nrows) set_row_valid(dst, false);
      } else {
        std::memcpy(dst, ch.row->data(), schema_.row_size);
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
    images.push_back(std::move(pi));
  }
  return images;
}

// ---------------------------------------------------------------------------
// WAL
// ---------------------------------------------------------------------------
bool Wal::open(const std::string &path) {
  file_ = FileHandle::open(path, O_RDWR | O_CREAT | O_APPEND);
  if (!file_.valid()) return false;
  std::string data;
  if (read_all(data)) {
    size_t pos = 0;
    while (pos + sizeof(WalRecordHeader) <= data.size()) {
      WalRecordHeader h;
      std::memcpy(&h, data.data() + pos, sizeof(h));
      if (h.magic != 0x4D564457 || h.payload_len > 1024 * 1024) break;
      if (pos + sizeof(h) + h.payload_len > data.size()) break;
      if (h.txid >= next_txid_) next_txid_ = h.txid + 1;
      pos += sizeof(h) + h.payload_len;
    }
  }
  return true;
}

void Wal::close() { file_.close(); }

bool Wal::read_all(std::string &out) {
  int64_t sz = file_.size();
  if (sz < 0) return false;
  out.resize(static_cast<size_t>(sz));
  if (sz == 0) return true;
  return file_.pread_full(out.data(), out.size(), 0);
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
Engine::Engine(std::string data_dir, size_t cache_pages)
    : data_dir_(std::move(data_dir)),
      catalog_path_(data_dir_ + "/catalog.dat"),
      wal_path_(data_dir_ + "/wal.log"),
      cache_(cache_pages) {}

Engine::~Engine() {
  try {
    shutdown();
  } catch (...) {
  }
}

void Engine::open() {
  make_dir(data_dir_);
  load_catalog();
  // Create/open all tables from catalog.
  for (auto &kv : tables_) kv.second->open();
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
    kv.second->fsync_file();
  }
  // WAL is fully applied and durable now.
  wal_.truncate();
}

void Engine::shutdown() {
  if (tx_.active) rollback_transaction();
  if (!tables_.empty()) {
    cache_.flush_all(tables_);
    for (auto &kv : tables_) kv.second->fsync_file();
  }
  wal_.truncate();
  wal_.close();
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
  std::vector<PageImage> all_images;
  for (auto &kv : tx_.tables) {
    auto it = tables_.find(kv.first);
    if (it == tables_.end()) continue;
    auto images = it->second->build_commit_images(kv.second);
    for (auto &img : images) all_images.push_back(std::move(img));
  }
  for (auto &img : all_images) {
    if (!wal_.append_page(txid, img.table_id, img.page_no, *img.page))
      throw_error("IO_ERROR", "failed to write WAL page image");
  }
  if (!all_images.empty()) {
    if (!wal_.append_commit(txid) || !wal_.sync())
      throw_error("IO_ERROR", "failed to commit WAL");
  } else {
    // Still emit a commit record for an empty transaction; harmless.
    if (!wal_.append_commit(txid) || !wal_.sync())
      throw_error("IO_ERROR", "failed to commit WAL");
  }
  // Install images and update in-memory maps.
  for (auto &img : all_images) {
    auto it = tables_.find(img.table_id);
    if (it != tables_.end()) cache_.install(it->second.get(), img.page_no, img.page);
  }
  for (auto &kv : tx_.tables) {
    auto it = tables_.find(kv.first);
    if (it == tables_.end()) continue;
    Table &table = *it->second;
    for (auto &rk : kv.second.rows) {
      const RowState &state = rk.second;
      if (state.deleted) {
        if (state.existed) table.erase_pk(rk.first);
      } else {
        table.put_pk(rk.first, state.pos);
      }
    }
  }
  tx_.clear();
  maybe_checkpoint();
}

void Engine::maybe_checkpoint() {
  if (wal_.size() < kWalCheckpointBytes) return;
  cache_.flush_all(tables_);
  wal_.truncate();
}

void Engine::rollback_transaction() { tx_.clear(); }

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
    Transaction backup;
    if (!autocommit) backup = tx_;
    try {
      Response r = fn();
      if (autocommit) commit_transaction();
      return r;
    } catch (...) {
      if (autocommit) tx_.clear();
      else tx_ = std::move(backup);
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
  table->open();
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
    for (const auto &name : stmt.columns) {
      int idx = table.column_index(name);
      if (idx < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + name);
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
    std::vector<uint8_t> row(schema.row_size, 0);
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
      tx_it->second.deleted = false;
      tx_it->second.row = std::move(row);
      ++affected;
      continue;
    }
    uint64_t base_pos;
    if (table.find_pk(pk, base_pos))
      throw_error("DUPLICATE_KEY", "duplicate primary key: " + std::to_string(pk));
    RowState state;
    state.existed = false;
    state.deleted = false;
    state.pos = table.allocate_pos();
    state.row = std::move(row);
    tx_table.rows[pk] = std::move(state);
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
std::vector<Engine::VisibleRow> Engine::collect_visible_locked(
    Table &table, const std::vector<Condition> &where) {
  std::vector<VisibleRow> out;
  const TableSchema &schema = table.schema();
  const TxTable *tx_table = find_tx_table(table.id());
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
          use = it->second.row.data();
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
      out.push_back(std::move(vr));
    }
  }
  if (tx_table) {
    for (const auto &kv : tx_table->rows) {
      const RowState &state = kv.second;
      if (state.deleted || state.existed) continue;
      bool match = true;
      for (const auto &cond : where) {
        if (!table.compare(state.row.data(), cond)) {
          match = false;
          break;
        }
      }
      if (!match) continue;
      VisibleRow vr;
      vr.row = state.row.data();
      vr.page = nullptr;
      vr.pk = kv.first;
      vr.pos = state.pos;
      vr.from_overlay = true;
      out.push_back(std::move(vr));
    }
  }
  return out;
}

std::vector<Engine::VisibleRow> Engine::collect_visible(
    Table &table, const std::vector<Condition> &where, bool need_sorted_by_pos) {
  auto rows = collect_visible_locked(table, where);
  if (need_sorted_by_pos) {
    std::sort(rows.begin(), rows.end(), [](const VisibleRow &a, const VisibleRow &b) {
      if (a.pos != b.pos) return a.pos < b.pos;
      return a.pk < b.pk;
    });
  }
  return rows;
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
float squared_l2_scalar(const float *a, const float *b, int dim) {
  float sum = 0.0f;
  for (int i = 0; i < dim; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2,fma"))) float squared_l2_avx2(const float *a,
                                                          const float *b,
                                                          int dim) {
  __m256 sum = _mm256_setzero_ps();
  int i = 0;
  for (; i + 8 <= dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    sum = _mm256_fmadd_ps(d, d, sum);
  }
  float tmp[8];
  _mm256_storeu_ps(tmp, sum);
  float total = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] +
                tmp[6] + tmp[7];
  for (; i < dim; ++i) {
    float d = a[i] - b[i];
    total += d * d;
  }
  return total;
}
#endif
float squared_l2(const float *a, const float *b, int dim) {
#if defined(__x86_64__) || defined(_M_X64)
  static const bool has_avx2 =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (has_avx2) return squared_l2_avx2(a, b, dim);
#endif
  return squared_l2_scalar(a, b, dim);
}

// float accumulation is the fast path; finite float32 components can still
// overflow the squared sum, in which case double accumulation is needed for a
// meaningful distance and correct TopK ordering.
double squared_l2_checked(const float *a, const float *b, int dim) {
  float fast = squared_l2(a, b, dim);
  if (std::isfinite(fast)) return static_cast<double>(fast);
  double total = 0.0;
  for (int i = 0; i < dim; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    total += d * d;
  }
  return total;
}

struct Candidate {
  double dist;
  int64_t pk;
  uint64_t pos;
};
bool candidate_worse(const Candidate &a, const Candidate &b) {
  if (a.dist != b.dist) return a.dist < b.dist;
  return a.pk < b.pk;
}
// Max-heap where the top is the worst candidate.
struct CandidateHeap {
  std::vector<Candidate> data;
  size_t k = 10;
  void push(double dist, int64_t pk, uint64_t pos) {
    if (data.size() < k) {
      data.push_back({dist, pk, pos});
      std::push_heap(data.begin(), data.end(), candidate_worse);
    } else if (dist < data.front().dist ||
               (dist == data.front().dist && pk < data.front().pk)) {
      std::pop_heap(data.begin(), data.end(), candidate_worse);
      data.back() = {dist, pk, pos};
      std::push_heap(data.begin(), data.end(), candidate_worse);
    }
  }
  void merge(CandidateHeap &other) {
    for (const auto &c : other.data) push(c.dist, c.pk, c.pos);
  }
};
}  // namespace

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
  OutValue::Kind kind;
  int64_t i;
  double d;
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
  std::vector<ItemPlan> star_plans;
  for (const auto &item : stmt.items) {
    if (item.star) {
      ItemPlan p;
      p.star = true;
      plans.push_back(p);
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        ItemPlan q;
        q.column_index = static_cast<int>(i);
        q.name = schema.columns[i].name;
        star_plans.push_back(q);
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

  // Fast path: one ascending vector-distance ordering with LIMIT.  The
  // ordering may reference a projected distance alias or the L2_DISTANCE
  // expression itself, even when the distance is not in the SELECT list.
  int distance_plan = -1;
  for (size_t i = 0; i < plans.size(); ++i) {
    if (!plans[i].star && plans[i].is_distance) {
      if (distance_plan < 0) distance_plan = static_cast<int>(i);
    }
  }
  bool topk_path = false;
  int64_t topk = 0;
  int topk_vector_column = -1;
  std::vector<float> topk_query_vec;
  if (stmt.has_limit && stmt.offset == 0 && stmt.order.size() == 1 &&
      !stmt.order[0].desc) {
    const OrderKey &ok = stmt.order[0];
    if (ok.is_distance) {
      int ci = table.column_index(ok.expr.column);
      if (ci < 0)
        throw_error("COLUMN_NOT_FOUND", "column not found: " + ok.expr.column);
      if (schema.columns[ci].type != ColumnType::VECTOR)
        throw_error("INVALID_ARGUMENT", "L2_DISTANCE requires a VECTOR column");
      topk_vector_column = ci;
      parse_vector_for_column(ok.expr.vec_text, schema.columns[ci],
                              topk_query_vec);
      topk_path = true;
    } else {
      std::string key = lower_copy(ok.name);
      int chosen = -1;
      for (size_t i = 0; i < plans.size(); ++i) {
        if (plans[i].is_distance && lower_copy(plans[i].name) == key) {
          chosen = static_cast<int>(i);
          break;
        }
      }
      if (chosen < 0 && distance_plan >= 0 &&
          (key == "distance" || key == "d" || key == "l2_distance"))
        chosen = distance_plan;
      if (chosen >= 0) {
        topk_vector_column = plans[chosen].vector_column;
        topk_query_vec = plans[chosen].query_vec;
        topk_path = true;
      }
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
    if (tx_.active) {
      auto rows = collect_visible_locked(table, stmt.where);
      for (const auto &vr : rows) {
        const uint8_t *base = vr.row + schema.col_offsets[topk_vector_column];
        const float *vec = reinterpret_cast<const float *>(base);
        double sq = squared_l2_checked(vec, topk_query_vec.data(),
                                       schema.columns[topk_vector_column].dim);
        heaps[0].push(sq, vr.pk, vr.pos);
      }
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
              [](const Candidate &a, const Candidate &b) {
                if (a.dist != b.dist) return a.dist < b.dist;
                return a.pk < b.pk;
              });
    for (const auto &c : finalists) {
      std::vector<uint8_t> row;
      bool have = false;
      if (tx_.active) {
        const TxTable *tt = find_tx_table(table.id());
        if (tt) {
          auto it = tt->rows.find(c.pk);
          if (it != tt->rows.end() && !it->second.deleted) {
            row = it->second.row;
            have = true;
          }
        }
      }
      if (!have && !table.read_row(c.pos, row)) continue;
      resp.rows.push_back(project_row(row.data()));
    }
    return resp;
  }

  // Streaming path for no-ORDER-BY queries with LIMIT: avoids materializing
  // the whole table when the dataset is larger than the page cache/memory.
  if (stmt.order.empty() && stmt.has_limit && !tx_.active) {
    int64_t seen = 0;
    int64_t wanted = (stmt.offset > std::numeric_limits<int64_t>::max() - stmt.limit)
                         ? std::numeric_limits<int64_t>::max()
                         : stmt.offset + stmt.limit;
    if (stmt.limit == 0) return resp;
    for (uint32_t pi = 0; pi < table.page_count(); ++pi) {
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
        if (seen >= stmt.offset) resp.rows.push_back(project_row(row));
        ++seen;
        if (seen >= wanted) return resp;
      }
    }
    return resp;
  }

  // Generic path.
  auto rows = collect_visible(table, stmt.where, true);
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
  if (!bounded) results.reserve(rows.size());
  for (const auto &vr : rows) {
    ResultRow rr;
    rr.vals = project_row(vr.row);
    rr.pk = vr.pk;
    rr.pos = vr.pos;
    for (const auto &okey : stmt.order) {
      SortKey key;
      if (okey.is_distance) {
        int ci = table.column_index(okey.expr.column);
        if (ci < 0)
          throw_error("COLUMN_NOT_FOUND", "column not found: " + okey.expr.column);
        if (schema.columns[ci].type != ColumnType::VECTOR)
          throw_error("INVALID_ARGUMENT", "L2_DISTANCE requires a VECTOR column");
        std::vector<float> qv;
        parse_vector_for_column(okey.expr.vec_text, schema.columns[ci], qv);
        const uint8_t *base = vr.row + schema.col_offsets[ci];
        const float *vec = reinterpret_cast<const float *>(base);
        key.kind = OutValue::Kind::DOUBLE;
        key.d = std::sqrt(
            squared_l2_checked(vec, qv.data(), schema.columns[ci].dim));
        rr.keys.push_back(std::move(key));
        continue;
      }
      if (okey.ordinal > 0) {
        size_t oi = static_cast<size_t>(okey.ordinal - 1);
        if (oi >= rr.vals.size())
          throw_error("COLUMN_NOT_FOUND", "ORDER BY ordinal out of range");
        const OutValue &ov = rr.vals[oi];
        key.kind = ov.kind;
        key.i = ov.i;
        key.d = ov.d;
        key.s = ov.s;
        rr.keys.push_back(std::move(key));
        continue;
      }
      std::string lower = lower_copy(okey.name);
      bool found = false;
      // Output column first.
      for (size_t i = 0; i < out_columns.size(); ++i) {
        if (lower_copy(out_columns[i]) == lower) {
          const OutValue &ov = rr.vals[i];
          key.kind = ov.kind;
          key.i = ov.i;
          key.d = ov.d;
          key.s = ov.s;
          found = true;
          break;
        }
      }
      if (!found) {
        int ci = table.column_index(okey.name);
        if (ci < 0)
          throw_error("COLUMN_NOT_FOUND", "ORDER BY column not found: " + okey.name);
        OutValue ov = out_value_from_row(table, vr.row, ci);
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
  }
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
  struct SetPlan {
    int col;
    Value value;
  };
  std::vector<SetPlan> sets;
  for (auto &kv : stmt.sets) {
    int ci = table.column_index(kv.first);
    if (ci < 0)
      throw_error("COLUMN_NOT_FOUND", "column not found: " + kv.first);
    if (ci == schema.pk_index)
      throw_error("UNSUPPORTED", "updating the primary key is not supported");
    Value v = coerce_value(schema, ci, kv.second);
    sets.push_back(SetPlan{ci, std::move(v)});
  }
  auto rows = collect_visible(table, stmt.where, false);
  TxTable &tx_table = tx_table_ref(table.id());
  int64_t matched = 0;
  for (const auto &vr : rows) {
    RowState state;
    auto it = tx_table.rows.find(vr.pk);
    if (it != tx_table.rows.end()) {
      state = it->second;
    } else {
      state.existed = true;
      state.pos = vr.pos;
      state.row.assign(vr.row, vr.row + schema.row_size);
    }
    std::vector<uint8_t> new_row = state.row;
    for (const auto &sp : sets)
      table.encode_value(new_row.data(), sp.col, sp.value);
    state.deleted = false;
    state.row = std::move(new_row);
    tx_table.rows[vr.pk] = std::move(state);
    ++matched;
  }
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = matched;
  return resp;
}

Response Engine::exec_delete(DeleteStmt &stmt) {
  Table &table = require_table(stmt.table);
  const TableSchema &schema = table.schema();
  compile_conditions(table, stmt.where);
  auto rows = collect_visible(table, stmt.where, false);
  TxTable &tx_table = tx_table_ref(table.id());
  int64_t matched = 0;
  for (const auto &vr : rows) {
    RowState state;
    auto it = tx_table.rows.find(vr.pk);
    if (it != tx_table.rows.end()) {
      state = it->second;
    } else {
      state.existed = true;
      state.pos = vr.pos;
      state.row.assign(vr.row, vr.row + schema.row_size);
    }
    state.deleted = true;
    tx_table.rows[vr.pk] = std::move(state);
    ++matched;
  }
  Response resp;
  resp.has_affected = true;
  resp.affected_rows = matched;
  return resp;
}

}  // namespace mvd
