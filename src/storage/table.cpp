#include "table.h"
#include "cache.h"
#include "../model/codec.h"
#include "../core/number.h"
#include "../core/utf8.h"
#include "../core/error.h"
#include <algorithm>
#include <array>
namespace mvd {
std::shared_ptr<Page> Table::get_page(uint32_t number){return cache_->get(this,number);}
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
  locate_end_of_file();
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
  if (size < 0) return false;
  if (static_cast<int64_t>(offset + kPageSize) > size) {
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
  locate_end_of_file();
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
      if (schema_.pages_per_row > 1) {
        const uint32_t page_no = p + i;
        if (page_no % schema_.pages_per_row == 0 &&
            page.nrows == 1 && row_is_valid(page.payload)) {
          pk_pos_[row_id(page.payload)] = page_no / schema_.pages_per_row;
        }
        continue;
      }
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

void Table::locate_end_of_file() {
  int64_t size = file_.size();
  if (size < 0) throw_error("IO_ERROR", "cannot stat table file");
  uint32_t total_pages = static_cast<uint32_t>(size / kPageSize);
  if (static_cast<int64_t>(total_pages) * kPageSize != size)
    throw_error("CORRUPTION", "table file size is not page aligned");
  page_count_ = 0;
  next_pos_ = 0;
  if (schema_.pages_per_row > 1) {
    if (total_pages % schema_.pages_per_row != 0)
      throw_error("CORRUPTION", "incomplete wide row extent after recovery");
    page_count_ = total_pages;
    next_pos_ = total_pages / schema_.pages_per_row;
    return;
  }
  // Find the highest page with a valid header.
  for (int64_t p = static_cast<int64_t>(total_pages) - 1; p >= 0; --p) {
    Page page;
    if (!load_page_raw(static_cast<uint32_t>(p), page))
      throw_error("IO_ERROR", "failed to read table page while locating end of file");
    if (page.magic == kPageMagic) {
      page_count_ = static_cast<uint32_t>(p) + 1;
      next_pos_ = static_cast<uint64_t>(p) * schema_.rows_per_page + page.nrows;
      break;
    }
  }
}


uint32_t Table::wide_row_page(uint64_t pos) const {
  if (pos >= std::numeric_limits<uint32_t>::max() / schema_.pages_per_row)
    throw_error("RESOURCE_LIMIT", "wide row exceeds the table page address space");
  return static_cast<uint32_t>(pos * schema_.pages_per_row);
}


bool Table::read_row(uint64_t pos, std::vector<uint8_t> &row) {
  if (schema_.pages_per_row > 1) {
    const uint32_t first = wide_row_page(pos);
    if (first >= page_count_) return false;
    auto page = get_page(first);
    if (page->magic != kPageMagic || page->nrows != 1 || !row_is_valid(page->payload))
      return false;
    row.resize(schema_.row_size);
    size_t offset = 0;
    for (uint32_t i = 0; i < schema_.pages_per_row; ++i) {
      if (i != 0) {
        page = get_page(first + i);
        if (page->magic != kPageMagic || page->page_no != first + i || page->nrows != 0)
          throw_error("CORRUPTION", "invalid wide row continuation");
      }
      const size_t bytes = std::min(sizeof(page->payload), row.size() - offset);
      std::memcpy(row.data() + offset, page->payload, bytes);
      offset += bytes;
    }
    return true;
  }

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
  if (!file_.pwritev_sparse(vectors.data(), static_cast<int>(count), offset)) {
    if (!direct_io_ || !reopen_buffered() ||
        !file_.pwritev_sparse(vectors.data(), static_cast<int>(count), offset))
      throw_error("IO_ERROR", "failed to write table page batch");
  }
  if (first+count > page_count_) page_count_ = first+count;
}
#endif

void Table::write_pages_to_disk(uint32_t page_no, const void *pages, uint32_t count) {
  uint64_t offset = static_cast<uint64_t>(page_no) * kPageSize;
  size_t bytes = static_cast<size_t>(count) * kPageSize;
#ifdef __linux__
  iovec vector{const_cast<void *>(pages), bytes};
  auto write = [&]() { return file_.pwritev_sparse(&vector, 1, offset); };
#else
  auto write = [&]() { return file_.pwrite_full(pages, bytes, offset); };
#endif
  if (!write()) {
    if (direct_io_ && reopen_buffered()) {
      if (!write())
        throw_error("IO_ERROR", "failed to write table page");
    } else {
      throw_error("IO_ERROR", "failed to write table page");
    }
  }
  if (page_no + count > page_count_) page_count_ = page_no + count;
}


bool Table::fsync_file() { return file_.fsync(); }

bool Table::find_pk(int64_t pk, uint64_t &pos) const {
  auto it = pk_pos_.find(pk);
  if (it == pk_pos_.end()) return false;
  pos = it->second;
  return true;
}
void Table::put_pk(int64_t pk, uint64_t pos) { pk_pos_[pk] = pos; }
void Table::erase_pk(int64_t pk) { pk_pos_.erase(pk); }

int Table::column_index(const std::string& name)const{return find_column(schema_,name);}

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
    int cps = utf8_codepoint_count(value.s);
    if (cps < 0)
      throw_error("INVALID_VALUE", "invalid UTF-8 string");
    if (cps > col.max_chars)
      throw_error("VARCHAR_TOO_LONG", col.name + " is too long");
    uint32_t bytes = static_cast<uint32_t>(value.s.size());
    std::memcpy(base, &bytes, 4);
    if (bytes) std::memcpy(base + 4, value.s.data(), bytes);
    // Old values may occupy the rest of this fixed-width slot, including
    // stale padding written by earlier versions. Keep the unused tail zero
    // so short values can actually use sparse page/WAL blocks.
    std::memset(base + 4 + bytes, 0,
                static_cast<size_t>(col.max_chars) * 4 - bytes);
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

bool Table::compare(const uint8_t *row, const BoundCondition &cond) const {
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
  if (schema_.pages_per_row > 1) {
    std::vector<const RowState *> changes;
    for (const auto &entry : tx.rows) {
      const RowState &state = entry.second;
      if (!state.deleted || state.existed) changes.push_back(&state);
    }
    std::sort(changes.begin(), changes.end(), [](const RowState *a, const RowState *b) {
      return a->pos < b->pos;
    });
    std::vector<uint8_t> row;
    for (const RowState *state : changes) {
      const uint32_t first = wide_row_page(state->pos);
      if (!state->deleted) {
        materialize_state(*state, nullptr, row);
        set_row_valid(row.data(), true);
      }
      size_t offset = 0;
      for (uint32_t i = 0; i < schema_.pages_per_row; ++i) {
        auto page = make_page();
        page->page_no = first + i;
        page->nrows = i == 0 ? 1 : 0;
        if (!state->deleted) {
          const size_t bytes = std::min(sizeof(page->payload), row.size() - offset);
          std::memcpy(page->payload, row.data() + offset, bytes);
          offset += bytes;
        }
        PageImage image;
        image.table_id = schema_.id;
        image.page_no = page->page_no;
        image.page = std::move(page);
        visit(std::move(image));
      }
    }
    return;
  }

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
        if (slot < image->nrows) std::memset(dst, 0, schema_.row_size);
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


}
