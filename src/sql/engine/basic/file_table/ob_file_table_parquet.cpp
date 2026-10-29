/**
 * Standalone Parquet reader for the file-table feature (competition task 2).
 *
 * Scope: flat schemas, PLAIN encoded values, DATA_PAGE_V2 and DATA_PAGE,
 * UNCOMPRESSED and SNAPPY codecs, optional/required columns. Dictionary pages
 * are skipped (their values are reported as unsupported).
 *
 * No oceanbase dependencies: unit-testable with a plain compiler.
 */

#include "ob_file_table_parquet.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace filetable
{

namespace
{

enum ThriftType
{
  TT_STOP = 0,
  TT_BOOL_TRUE = 1,
  TT_BOOL_FALSE = 2,
  TT_BYTE = 3,
  TT_I16 = 4,
  TT_I32 = 5,
  TT_I64 = 6,
  TT_DOUBLE = 7,
  TT_BINARY = 8,
  TT_LIST = 9,
  TT_SET = 10,
  TT_MAP = 11,
  TT_STRUCT = 12,
};

enum ParquetType
{
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED_LEN_BYTE_ARRAY = 7,
};

enum RepetitionType
{
  RT_REQUIRED = 0,
  RT_OPTIONAL = 1,
  RT_REPEATED = 2,
};

enum CompressionCodec
{
  CC_UNCOMPRESSED = 0,
  CC_SNAPPY = 1,
};

enum PageTypeId
{
  PAGE_DATA = 0,
  PAGE_INDEX = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2 = 3,
};

struct ThriftReader;
bool next_field(ThriftReader &r, int16_t &last, int16_t &id, uint8_t &type, bool &stop);

struct ThriftReader
{
  const uint8_t *p_ = nullptr;
  const uint8_t *end_ = nullptr;

  bool init(const char *data, int64_t len)
  {
    p_ = reinterpret_cast<const uint8_t *>(data);
    end_ = p_ + len;
    return nullptr != data && len > 0;
  }
  bool readByte(uint8_t &b)
  {
    if (p_ >= end_) {
      return false;
    }
    b = *p_++;
    return true;
  }
  bool readVarint(uint64_t &v)
  {
    v = 0;
    int shift = 0;
    while (p_ < end_ && shift < 64) {
      uint8_t b = *p_++;
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) {
        return true;
      }
      shift += 7;
    }
    return false;
  }
  bool readZigZag(int64_t &v)
  {
    uint64_t u = 0;
    if (!readVarint(u)) {
      return false;
    }
    v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    return true;
  }
  bool readBinary(const uint8_t *&ptr, int32_t &len)
  {
    uint64_t n = 0;
    if (!readVarint(n) || n > static_cast<uint64_t>(end_ - p_)) {
      return false;
    }
    ptr = p_;
    len = static_cast<int32_t>(n);
    p_ += n;
    return true;
  }
  bool skip(uint8_t type)
  {
    bool ret = true;
    switch (type) {
      case TT_BOOL_TRUE:
      case TT_BOOL_FALSE:
      case TT_STOP:
        break;
      case TT_BYTE: {
        uint8_t b = 0;
        ret = readByte(b);
        break;
      }
      case TT_I16:
      case TT_I32:
      case TT_I64: {
        int64_t v = 0;
        ret = readZigZag(v);
        break;
      }
      case TT_DOUBLE:
        if (end_ - p_ < 8) {
          ret = false;
        } else {
          p_ += 8;
        }
        break;
      case TT_BINARY: {
        const uint8_t *ptr = nullptr;
        int32_t len = 0;
        ret = readBinary(ptr, len);
        break;
      }
      case TT_LIST:
      case TT_SET: {
        uint8_t b = 0;
        if (!readByte(b)) {
          ret = false;
          break;
        }
        uint64_t size = (b >> 4) & 0x0f;
        uint8_t elem = b & 0x0f;
        if (15 == size) {
          if (!readVarint(size)) {
            ret = false;
            break;
          }
        }
        for (uint64_t i = 0; ret && i < size; ++i) {
          ret = skip(elem);
        }
        break;
      }
      case TT_MAP: {
        uint8_t b = 0;
        if (!readByte(b)) {
          ret = false;
          break;
        }
        uint64_t size = (b >> 4) & 0x0f;
        if (15 == size) {
          if (!readVarint(size)) {
            ret = false;
            break;
          }
        }
        for (uint64_t i = 0; ret && i < size; ++i) {
          ret = skip(TT_BINARY) && skip(TT_BINARY);
        }
        break;
      }
      case TT_STRUCT: {
        int16_t last = 0, id = 0;
        uint8_t t = 0;
        bool stop = false;
        while (ret) {
          if (!next_field(*this, last, id, t, stop)) {
            ret = false;
          } else if (stop) {
            break;
          } else {
            ret = skip(t);
          }
        }
        break;
      }
      default:
        ret = false;
        break;
    }
    return ret;
  }
};

bool next_field(ThriftReader &r, int16_t &last, int16_t &id, uint8_t &type, bool &stop)
{
  stop = false;
  uint8_t b = 0;
  if (!r.readByte(b)) {
    return false;
  }
  if (0 == b) {
    stop = true;
    return true;
  }
  type = b & 0x0f;
  int16_t delta = (b >> 4) & 0x0f;
  if (0 == delta) {
    int64_t zz = 0;
    if (!r.readZigZag(zz)) {
      return false;
    }
    id = static_cast<int16_t>(zz);
  } else {
    id = static_cast<int16_t>(last + delta);
  }
  last = id;
  return true;
}

bool read_i32(ThriftReader &r, int32_t &out)
{
  int64_t v = 0;
  if (!r.readZigZag(v)) {
    return false;
  }
  out = static_cast<int32_t>(v);
  return true;
}
bool read_i64(ThriftReader &r, int64_t &out) { return r.readZigZag(out); }
bool read_bool_value(ThriftReader &r, uint8_t type, bool &out)
{
  if (TT_BOOL_TRUE == type) {
    out = true;
    return true;
  }
  if (TT_BOOL_FALSE == type) {
    out = false;
    return true;
  }
  uint8_t b = 0;
  if (!r.readByte(b)) {
    return false;
  }
  out = (0 != b);
  return true;
}

bool read_list_header(ThriftReader &r, uint8_t &elem_type, uint64_t &size)
{
  uint8_t b = 0;
  if (!r.readByte(b)) {
    return false;
  }
  size = (b >> 4) & 0x0f;
  elem_type = b & 0x0f;
  if (15 == size) {
    if (!r.readVarint(size)) {
      return false;
    }
  }
  return true;
}

struct SchemaElement
{
  int32_t type_ = -1;
  int32_t repetition_ = -1;
  int32_t num_children_ = 0;
  std::string name_;
};

struct ColumnMeta
{
  int32_t type_ = -1;
  int32_t codec_ = CC_UNCOMPRESSED;
  int64_t num_values_ = 0;
  int64_t data_page_offset_ = 0;
  int64_t dictionary_page_offset_ = -1;
};

struct ColumnChunk
{
  ColumnMeta meta_;
  bool has_meta_ = false;
};

struct RowGroup
{
  std::vector<ColumnChunk> columns_;
  int64_t num_rows_ = 0;
};

struct FileMetaData
{
  std::vector<SchemaElement> schema_;
  int64_t num_rows_ = 0;
  std::vector<RowGroup> row_groups_;
};

bool parse_schema_element(ThriftReader &r, SchemaElement &e)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 1: if (!read_i32(r, e.type_)) return false; break;
      case 3: if (!read_i32(r, e.repetition_)) return false; break;
      case 4: {
        const uint8_t *ptr = nullptr;
        int32_t len = 0;
        if (!r.readBinary(ptr, len)) return false;
        e.name_.assign(reinterpret_cast<const char *>(ptr), static_cast<size_t>(len));
        break;
      }
      case 5: if (!read_i32(r, e.num_children_)) return false; break;
      default: if (!r.skip(t)) return false; break;
    }
  }
}

bool parse_column_meta(ThriftReader &r, ColumnMeta &m)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 1: if (!read_i32(r, m.type_)) return false; break;
      case 2: {  // list<Encoding>
        uint8_t elem = 0;
        uint64_t size = 0;
        if (!read_list_header(r, elem, size)) return false;
        for (uint64_t i = 0; i < size; ++i) {
          int64_t v = 0;
          if (!r.readZigZag(v)) return false;
        }
        break;
      }
      case 4: if (!read_i32(r, m.codec_)) return false; break;
      case 5: if (!read_i64(r, m.num_values_)) return false; break;
      case 9: if (!read_i64(r, m.data_page_offset_)) return false; break;
      case 11: if (!read_i64(r, m.dictionary_page_offset_)) return false; break;
      default: if (!r.skip(t)) return false; break;
    }
  }
}

bool parse_column_chunk(ThriftReader &r, ColumnChunk &c)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 3:
        if (TT_STRUCT != t) return false;
        if (!parse_column_meta(r, c.meta_)) return false;
        c.has_meta_ = true;
        break;
      default: if (!r.skip(t)) return false; break;
    }
  }
}

bool parse_row_group(ThriftReader &r, RowGroup &g)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 1: {
        uint8_t elem = 0;
        uint64_t size = 0;
        if (!read_list_header(r, elem, size)) return false;
        for (uint64_t i = 0; i < size; ++i) {
          ColumnChunk c;
          if (!parse_column_chunk(r, c)) return false;
          g.columns_.push_back(c);
        }
        break;
      }
      case 3: if (!read_i64(r, g.num_rows_)) return false; break;
      default: if (!r.skip(t)) return false; break;
    }
  }
}

bool parse_file_meta(ThriftReader &r, FileMetaData &m)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 2: {
        uint8_t elem = 0;
        uint64_t size = 0;
        if (!read_list_header(r, elem, size)) return false;
        for (uint64_t i = 0; i < size; ++i) {
          SchemaElement e;
          if (!parse_schema_element(r, e)) return false;
          m.schema_.push_back(e);
        }
        break;
      }
      case 3: if (!read_i64(r, m.num_rows_)) return false; break;
      case 4: {
        uint8_t elem = 0;
        uint64_t size = 0;
        if (!read_list_header(r, elem, size)) return false;
        for (uint64_t i = 0; i < size; ++i) {
          RowGroup g;
          if (!parse_row_group(r, g)) return false;
          m.row_groups_.push_back(g);
        }
        break;
      }
      default: if (!r.skip(t)) return false; break;
    }
  }
}

struct PageHeader
{
  int32_t type_ = -1;
  int32_t uncompressed_size_ = 0;
  int32_t compressed_size_ = 0;
  int32_t num_values_ = 0;
  int32_t encoding_ = -1;
  int32_t def_levels_len_ = 0;
  int32_t rep_levels_len_ = 0;
  bool is_compressed_ = false;
};

bool parse_data_page_header_v1(ThriftReader &r, PageHeader &h)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 1: if (!read_i32(r, h.num_values_)) return false; break;
      case 2: if (!read_i32(r, h.encoding_)) return false; break;
      default: if (!r.skip(t)) return false; break;
    }
  }
}

bool parse_data_page_header_v2(ThriftReader &r, PageHeader &h)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 1: if (!read_i32(r, h.num_values_)) return false; break;
      case 4: if (!read_i32(r, h.encoding_)) return false; break;
      case 5: if (!read_i32(r, h.def_levels_len_)) return false; break;
      case 6: if (!read_i32(r, h.rep_levels_len_)) return false; break;
      case 7: if (!read_bool_value(r, t, h.is_compressed_)) return false; break;
      default: if (!r.skip(t)) return false; break;
    }
  }
}

bool parse_page_header(ThriftReader &r, PageHeader &h)
{
  int16_t last = 0, id = 0;
  uint8_t t = 0;
  bool stop = false;
  while (true) {
    if (!next_field(r, last, id, t, stop)) {
      return false;
    }
    if (stop) {
      return true;
    }
    switch (id) {
      case 1: if (!read_i32(r, h.type_)) return false; break;
      case 2: if (!read_i32(r, h.uncompressed_size_)) return false; break;
      case 3: if (!read_i32(r, h.compressed_size_)) return false; break;
      case 5:
        if (TT_STRUCT != t) return false;
        if (!parse_data_page_header_v1(r, h)) return false;
        break;
      case 8:
        if (TT_STRUCT != t) return false;
        if (!parse_data_page_header_v2(r, h)) return false;
        break;
      default: if (!r.skip(t)) return false; break;
    }
  }
}

int read_varint32(const uint8_t *&p, const uint8_t *end, uint32_t &v)
{
  v = 0;
  int shift = 0;
  while (p < end && shift < 32) {
    uint8_t b = *p++;
    v |= static_cast<uint32_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      return 0;
    }
    shift += 7;
  }
  return -1;
}

// RLE/bit-packed hybrid, used for definition levels.
bool decode_levels(const uint8_t *data,
                   int64_t len,
                   int bit_width,
                   int64_t num_values,
                   std::vector<int32_t> &out)
{
  out.clear();
  out.reserve(static_cast<size_t>(num_values));
  const uint8_t *p = data;
  const uint8_t *end = data + len;
  const int byte_width = (bit_width + 7) / 8;
  while (p < end && static_cast<int64_t>(out.size()) < num_values) {
    uint32_t header = 0;
    if (0 != read_varint32(p, end, header)) {
      return false;
    }
    if (header & 1) {
      const uint32_t groups = header >> 1;
      for (uint32_t g = 0; g < groups && static_cast<int64_t>(out.size()) < num_values; ++g) {
        for (int i = 0; i < 8 && static_cast<int64_t>(out.size()) < num_values; ++i) {
          const int64_t bit_off = static_cast<int64_t>(g) * 8 * bit_width + i * bit_width;
          int32_t v = 0;
          for (int b = 0; b < bit_width; ++b) {
            const int64_t abs_bit = bit_off + b;
            const int64_t byte_idx = abs_bit / 8;
            if (p + byte_idx >= end) {
              return false;
            }
            if (p[byte_idx] & (1 << (abs_bit % 8))) {
              v |= (1 << b);
            }
          }
          out.push_back(v);
        }
      }
      p += (static_cast<int64_t>(groups) * 8 * bit_width + 7) / 8;
    } else {
      const uint32_t run = header >> 1;
      if (p + byte_width > end) {
        return false;
      }
      int32_t v = 0;
      for (int b = 0; b < byte_width; ++b) {
        v |= static_cast<int32_t>(p[b]) << (8 * b);
      }
      p += byte_width;
      for (uint32_t i = 0; i < run && static_cast<int64_t>(out.size()) < num_values; ++i) {
        out.push_back(v);
      }
    }
  }
  return static_cast<int64_t>(out.size()) == num_values;
}

std::string render_i64(int64_t v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  return std::string(buf);
}

std::string render_double(double v)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "%.15g", v);
  return std::string(buf);
}

int32_t read_le32(const uint8_t *p)
{
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                              | (static_cast<uint32_t>(p[2]) << 16)
                              | (static_cast<uint32_t>(p[3]) << 24));
}
int64_t read_le64(const uint8_t *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return static_cast<int64_t>(v);
}

// Decodes one PLAIN value; `pos` is a byte offset, except for BOOLEAN where it
// is a bit offset.
int decode_plain_value(int32_t ptype, const uint8_t *p, int64_t len, int64_t &pos,
                       FileTableField &f)
{
  int ret = 0;
  switch (ptype) {
    case PT_INT32: {
      if (pos + 4 > len) return -1;
      f.value_ = render_i64(read_le32(p + pos));
      pos += 4;
      break;
    }
    case PT_INT64: {
      if (pos + 8 > len) return -1;
      f.value_ = render_i64(read_le64(p + pos));
      pos += 8;
      break;
    }
    case PT_DOUBLE: {
      if (pos + 8 > len) return -1;
      double d = 0;
      const int64_t bits = read_le64(p + pos);
      memcpy(&d, &bits, sizeof(d));
      f.value_ = render_double(d);
      pos += 8;
      break;
    }
    case PT_FLOAT: {
      if (pos + 4 > len) return -1;
      float fl = 0;
      const int32_t bits = read_le32(p + pos);
      memcpy(&fl, &bits, sizeof(fl));
      f.value_ = render_double(static_cast<double>(fl));
      pos += 4;
      break;
    }
    case PT_BOOLEAN: {
      const int64_t byte_idx = pos / 8;
      if (byte_idx >= len) return -1;
      const bool v = (p[byte_idx] & (1 << (pos % 8))) != 0;
      f.value_ = v ? "true" : "false";
      ++pos;
      break;
    }
    case PT_BYTE_ARRAY: {
      if (pos + 4 > len) return -1;
      const int32_t n = read_le32(p + pos);
      pos += 4;
      if (n < 0 || pos + n > len) return -1;
      f.value_.assign(reinterpret_cast<const char *>(p + pos), static_cast<size_t>(n));
      pos += n;
      break;
    }
    default:
      ret = -1;
      break;
  }
  if (0 == ret) {
    f.is_null_ = false;
  }
  return ret;
}

} // namespace

// ----------------------------------------------------------- snappy --------
int snappy_decompress(const char *in, int64_t in_len, std::string &out)
{
  out.clear();
  const uint8_t *p = reinterpret_cast<const uint8_t *>(in);
  const uint8_t *end = p + in_len;
  uint32_t uncompressed_len = 0;
  if (0 != read_varint32(p, end, uncompressed_len)) {
    return -1;
  }
  out.reserve(uncompressed_len);
  while (p < end) {
    const uint8_t tag = *p++;
    uint32_t len = 0;
    uint32_t offset = 0;
    const int kind = tag & 3;
    if (0 == kind) {
      len = (tag >> 2) + 1;
      if (len > 60) {
        const int extra = static_cast<int>(len - 60);
        if (p + extra > end) {
          return -1;
        }
        uint32_t l = 0;
        for (int i = 0; i < extra; ++i) {
          l |= static_cast<uint32_t>(p[i]) << (8 * i);
        }
        p += extra;
        len = l + 1;
      }
      if (p + len > end) {
        return -1;
      }
      out.append(reinterpret_cast<const char *>(p), len);
      p += len;
    } else {
      if (1 == kind) {
        if (p >= end) {
          return -1;
        }
        len = ((tag >> 2) & 0x7) + 4;
        offset = (static_cast<uint32_t>(tag >> 5) << 8) | *p++;
      } else if (2 == kind) {
        if (p + 2 > end) {
          return -1;
        }
        len = (tag >> 2) + 1;
        offset = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
        p += 2;
      } else {
        if (p + 4 > end) {
          return -1;
        }
        len = (tag >> 2) + 1;
        offset = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                 | (static_cast<uint32_t>(p[2]) << 16)
                 | (static_cast<uint32_t>(p[3]) << 24);
        p += 4;
      }
      if (0 == offset || offset > out.size()) {
        return -1;
      }
      const size_t start = out.size() - offset;
      for (uint32_t i = 0; i < len; ++i) {
        out.push_back(out[start + i]);
      }
    }
  }
  return out.size() == uncompressed_len ? 0 : -1;
}

// ---------------------------------------------------------- parquet --------
int parse_parquet(const char *data, int64_t len, FileTableData &out, std::string &err)
{
  out.reset();
  err.clear();
  if (NULL == data || len < 12) {
    err = "file too small to be parquet";
    return -1;
  }
  const uint8_t *buf = reinterpret_cast<const uint8_t *>(data);
  if (0 != memcmp(buf, "PAR1", 4) || 0 != memcmp(buf + len - 4, "PAR1", 4)) {
    err = "missing parquet magic";
    return -1;
  }
  const uint32_t footer_len = static_cast<uint32_t>(buf[len - 8])
                              | (static_cast<uint32_t>(buf[len - 7]) << 8)
                              | (static_cast<uint32_t>(buf[len - 6]) << 16)
                              | (static_cast<uint32_t>(buf[len - 5]) << 24);
  if (static_cast<int64_t>(footer_len) + 8 > len - 4) {
    err = "invalid footer length";
    return -1;
  }
  const int64_t footer_off = len - 8 - static_cast<int64_t>(footer_len);
  FileMetaData meta;
  {
    ThriftReader r;
    if (!r.init(data + footer_off, footer_len) || !parse_file_meta(r, meta)) {
      err = "failed to parse parquet footer";
      return -1;
    }
  }

  struct Leaf
  {
    std::string name_;
    int32_t type_ = -1;
    int32_t repetition_ = -1;
  };
  std::vector<Leaf> leaves;
  for (size_t i = 1; i < meta.schema_.size(); ++i) {
    const SchemaElement &e = meta.schema_[i];
    if (e.num_children_ > 0) {
      err = "nested parquet schemas are not supported";
      return -1;
    }
    Leaf leaf;
    leaf.name_ = e.name_;
    leaf.type_ = e.type_;
    leaf.repetition_ = e.repetition_;
    leaves.push_back(leaf);
  }
  if (leaves.empty()) {
    err = "empty parquet schema";
    return -1;
  }
  for (size_t i = 0; i < leaves.size(); ++i) {
    FileTableColumn col;
    col.name_ = leaves[i].name_;
    col.nullable_ = (RT_OPTIONAL == leaves[i].repetition_);
    switch (leaves[i].type_) {
      case PT_INT32:
      case PT_INT64:
        col.type_ = FileColumnType::BIGINT;
        break;
      case PT_BOOLEAN:
        col.type_ = FileColumnType::BOOLEAN;
        break;
      case PT_FLOAT:
      case PT_DOUBLE:
        col.type_ = FileColumnType::DOUBLE;
        break;
      default:
        col.type_ = FileColumnType::VARCHAR;
        break;
    }
    out.columns_.push_back(col);
  }

  for (size_t g = 0; g < meta.row_groups_.size(); ++g) {
    const RowGroup &rg = meta.row_groups_[g];
    if (rg.columns_.size() != leaves.size()) {
      err = "column count mismatch in row group";
      return -1;
    }
    const size_t base_row = out.rows_.size();
    for (int64_t r = 0; r < rg.num_rows_; ++r) {
      out.rows_.push_back(std::vector<FileTableField>(leaves.size()));
    }
    for (size_t c = 0; c < rg.columns_.size(); ++c) {
      if (!rg.columns_[c].has_meta_) {
        err = "missing column metadata";
        return -1;
      }
      const ColumnMeta &cm = rg.columns_[c].meta_;
      const Leaf &leaf = leaves[c];
      int64_t offset = (cm.dictionary_page_offset_ >= 0 ? cm.dictionary_page_offset_
                                                        : cm.data_page_offset_);
      int64_t values_left = cm.num_values_;
      size_t row = base_row;
      while (values_left > 0) {
        if (offset < 0 || offset >= len) {
          err = "page offset out of range";
          return -1;
        }
        ThriftReader r;
        if (!r.init(data + offset, len - offset)) {
          err = "bad page header";
          return -1;
        }
        PageHeader ph;
        if (!parse_page_header(r, ph)) {
          err = "failed to parse page header";
          return -1;
        }
        const int64_t header_size = (r.p_ - reinterpret_cast<const uint8_t *>(data + offset));
        const uint8_t *body = reinterpret_cast<const uint8_t *>(data + offset + header_size);
        if (offset + header_size + ph.compressed_size_ > len) {
          err = "page body out of range";
          return -1;
        }
        if (PAGE_DATA_V2 == ph.type_) {
          const int32_t rep_len = ph.rep_levels_len_;
          const int32_t def_len = ph.def_levels_len_;
          const uint8_t *levels = body;
          const uint8_t *vals = body + rep_len + def_len;
          const int64_t vals_len = ph.compressed_size_ - rep_len - def_len;
          std::string plain;
          if (ph.is_compressed_ && CC_SNAPPY == cm.codec_) {
            if (0 != snappy_decompress(reinterpret_cast<const char *>(vals), vals_len, plain)) {
              err = "snappy decompression failed";
              return -1;
            }
          } else {
            plain.assign(reinterpret_cast<const char *>(vals), static_cast<size_t>(vals_len));
          }
          std::vector<int32_t> defs;
          if (def_len > 0 && RT_OPTIONAL == leaf.repetition_) {
            if (!decode_levels(levels + rep_len, def_len, 1, ph.num_values_, defs)) {
              err = "failed to decode definition levels";
              return -1;
            }
          }
          const uint8_t *vp = reinterpret_cast<const uint8_t *>(plain.data());
          const int64_t vlen = static_cast<int64_t>(plain.size());
          int64_t vpos = 0;
          for (int32_t i = 0; i < ph.num_values_; ++i) {
            const bool is_null = (RT_OPTIONAL == leaf.repetition_)
                                 && (static_cast<size_t>(i) < defs.size()) && (defs[i] <= 0);
            FileTableField f;
            if (is_null) {
              f.is_null_ = true;
            } else if (0 != decode_plain_value(leaf.type_, vp, vlen, vpos, f)) {
              err = "failed to decode plain value";
              return -1;
            }
            if (row < out.rows_.size()) {
              out.rows_[row][c] = f;
            }
            ++row;
          }
          values_left -= ph.num_values_;
          offset += header_size + ph.compressed_size_;
        } else if (PAGE_DICTIONARY == ph.type_ || PAGE_INDEX == ph.type_) {
          offset += header_size + ph.compressed_size_;
        } else {
          err = "unsupported parquet page type (only DATA_PAGE_V2 is supported)";
          return -1;
        }
      }
    }
  }
  return 0;
}

} // namespace filetable
} // namespace sql
} // namespace oceanbase
