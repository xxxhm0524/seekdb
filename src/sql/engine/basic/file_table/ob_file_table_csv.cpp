/**
 * Implementation of the standalone CSV reader / type inference.
 * See ob_file_table_csv.h for the contract.
 */

#include "ob_file_table_csv.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>

namespace oceanbase
{
namespace sql
{
namespace filetable
{

namespace
{

bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::string to_lower(const std::string &s)
{
  std::string out(s);
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
  }
  return out;
}

} // namespace

const char *file_column_type_name(FileColumnType type)
{
  switch (type) {
    case FileColumnType::BIGINT: return "BIGINT";
    case FileColumnType::DOUBLE: return "DOUBLE";
    case FileColumnType::BOOLEAN: return "BOOLEAN";
    case FileColumnType::DATE: return "DATE";
    case FileColumnType::DATETIME: return "DATETIME";
    case FileColumnType::VARCHAR: return "VARCHAR";
    default: return "VARCHAR";
  }
}

std::string make_unique_column_name(const std::vector<std::string> &existing,
                                    const std::string &candidate)
{
  bool used = false;
  for (size_t i = 0; i < existing.size(); ++i) {
    if (existing[i] == candidate) {
      used = true;
      break;
    }
  }
  if (!used) {
    return candidate;
  }
  for (int64_t suffix = 2;; ++suffix) {
    std::string next = candidate + "__" + std::to_string(suffix);
    bool taken = false;
    for (size_t i = 0; i < existing.size(); ++i) {
      if (existing[i] == next) {
        taken = true;
        break;
      }
    }
    if (!taken) {
      return next;
    }
  }
}

std::string normalize_path(const std::string &path)
{
  if (path.empty()) {
    return path;
  }
  const bool windows_style = path.find('\\') != std::string::npos
                             && path.find('/') == std::string::npos;
  const char sep = windows_style ? '\\' : '/';
  auto is_sep = [](char c) { return c == '/' || c == '\\'; };

  std::string prefix;
  size_t pos = 0;
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
    prefix = path.substr(0, 2);
    pos = 2;
    if (pos < path.size() && is_sep(path[pos])) {
      prefix.push_back(sep);
      ++pos;
    }
  } else if (is_sep(path[0])) {
    prefix.assign(1, sep);
    pos = 1;
  }
  const bool absolute = !prefix.empty();

  std::vector<std::string> parts;
  while (pos <= path.size()) {
    size_t end = pos;
    while (end < path.size() && !is_sep(path[end])) {
      ++end;
    }
    const std::string part = path.substr(pos, end - pos);
    if (part.empty() || part == ".") {
      // skip
    } else if (part == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else if (!absolute) {
        parts.push_back(part);
      }
    } else {
      parts.push_back(part);
    }
    if (end >= path.size()) {
      break;
    }
    pos = end + 1;
  }

  std::string out = prefix;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!out.empty() && out.back() != sep) {
      out.push_back(sep);
    }
    out += parts[i];
  }
  if (out.empty()) {
    out = absolute ? std::string(1, sep) : ".";
  }
  return out;
}

bool is_bigint(const std::string &s)
{
  if (s.empty()) {
    return false;
  }
  size_t i = 0;
  if (s[0] == '+' || s[0] == '-') {
    i = 1;
  }
  if (i >= s.size()) {
    return false;
  }
  for (; i < s.size(); ++i) {
    if (!is_digit(s[i])) {
      return false;
    }
  }
  // A syntactically integral value outside int64 must widen to DOUBLE rather
  // than being cast (and silently wrapped/truncated) as a BIGINT.
  errno = 0;
  char *end = nullptr;
  (void)strtoll(s.c_str(), &end, 10);
  return ERANGE != errno && nullptr != end && end == s.c_str() + s.size();
}

bool is_double(const std::string &s)
{
  if (s.empty()) {
    return false;
  }
  size_t i = 0;
  if (s[0] == '+' || s[0] == '-') {
    i = 1;
  }
  bool digits = false;
  bool dot = false;
  bool exp = false;
  bool exp_digits = false;
  for (; i < s.size(); ++i) {
    char c = s[i];
    if (is_digit(c)) {
      digits = true;
      if (exp) {
        exp_digits = true;
      }
    } else if (c == '.' && !dot && !exp) {
      dot = true;
    } else if ((c == 'e' || c == 'E') && digits && !exp) {
      exp = true;
      if (i + 1 < s.size() && (s[i + 1] == '+' || s[i + 1] == '-')) {
        ++i;
      }
    } else {
      return false;
    }
  }
  return digits && (!exp || exp_digits);
}

bool is_boolean(const std::string &s)
{
  std::string lower = to_lower(s);
  return lower == "true" || lower == "false";
}

namespace
{

bool read_fixed_digits(const std::string &s, size_t &pos, int n, int &out)
{
  int value = 0;
  for (int i = 0; i < n; ++i) {
    if (pos >= s.size() || !is_digit(s[pos])) {
      return false;
    }
    value = value * 10 + (s[pos] - '0');
    ++pos;
  }
  out = value;
  return true;
}

bool valid_date_parts(int year, int month, int day)
{
  if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) {
    return false;
  }
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max_day = days[month - 1];
  bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  if (month == 2 && leap) {
    max_day = 29;
  }
  return day <= max_day;
}

} // namespace

bool is_date(const std::string &s)
{
  size_t pos = 0;
  int year = 0, month = 0, day = 0;
  if (s.size() != 10) {
    return false;
  }
  if (!read_fixed_digits(s, pos, 4, year) || pos >= s.size() || s[pos++] != '-'
      || !read_fixed_digits(s, pos, 2, month) || pos >= s.size() || s[pos++] != '-'
      || !read_fixed_digits(s, pos, 2, day)) {
    return false;
  }
  return pos == s.size() && valid_date_parts(year, month, day);
}

bool is_datetime(const std::string &s)
{
  // YYYY-MM-DD[ T]HH:MM:SS[.fraction]
  if (s.size() < 19) {
    return false;
  }
  size_t pos = 0;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!read_fixed_digits(s, pos, 4, year) || pos >= s.size() || s[pos++] != '-'
      || !read_fixed_digits(s, pos, 2, month) || pos >= s.size() || s[pos++] != '-'
      || !read_fixed_digits(s, pos, 2, day) || pos >= s.size()
      || (s[pos] != ' ' && s[pos] != 'T')) {
    return false;
  }
  ++pos;
  if (!read_fixed_digits(s, pos, 2, hour) || pos >= s.size() || s[pos++] != ':'
      || !read_fixed_digits(s, pos, 2, minute) || pos >= s.size() || s[pos++] != ':'
      || !read_fixed_digits(s, pos, 2, second)) {
    return false;
  }
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    if (pos >= s.size()) {
      return false;
    }
    while (pos < s.size() && is_digit(s[pos])) {
      ++pos;
    }
  }
  return pos == s.size() && valid_date_parts(year, month, day)
         && hour >= 0 && hour < 24 && minute >= 0 && minute < 60
         && second >= 0 && second <= 60;
}

int parse_csv(const char *data,
              int64_t len,
              const CsvOptions &options,
              FileTableData &out,
              std::string &err)
{
  int ret = 0;
  out.reset();
  err.clear();
  if (NULL == data || len < 0) {
    err = "invalid csv buffer";
    return -1;
  }
  // Strip a UTF-8 BOM: otherwise it ends up in the first column name and
  // `select id from ...` fails with "unknown column".
  if (len >= 3 && 0 == memcmp(data, "\xef\xbb\xbf", 3)) {
    data += 3;
    len -= 3;
  }

  std::vector<std::vector<FileTableField>> records;
  std::vector<FileTableField> fields;
  std::string field;
  bool field_quoted = false;   // current field was quoted
  bool in_quotes = false;
  bool field_started = false;  // saw any char (incl. quote) for this field
  int64_t pos = 0;

  const char delim = options.field_delimiter_;

  auto finish_field = [&]() {
    FileTableField f;
    if (!field_quoted && field.empty()) {
      f.is_null_ = true;
    } else {
      f.value_.swap(field);
      f.is_null_ = false;
    }
    fields.push_back(f);
    field.clear();
    field_quoted = false;
    field_started = false;
  };

  auto finish_record = [&]() {
    finish_field();
    records.push_back(fields);
    fields.clear();
  };

  while (pos < len) {
    char c = data[pos];
    if (in_quotes) {
      if (c == '"') {
        if (pos + 1 < len && data[pos + 1] == '"') {
          field.push_back('"');
          pos += 2;
          continue;
        }
        in_quotes = false;
        ++pos;
        continue;
      }
      if (c == '\r' && pos + 1 < len && data[pos + 1] == '\n') {
        // Quoted fields use LF internally regardless of the record's CRLF
        // line ending, matching the standard CSV text projection.
        field.push_back('\n');
        pos += 2;
        continue;
      }
      field.push_back(c);
      ++pos;
      continue;
    }
    if (c == '"' && !field_started) {
      in_quotes = true;
      field_quoted = true;
      field_started = true;
      ++pos;
      continue;
    }
    if (c == delim) {
      finish_field();
      ++pos;
      continue;
    }
    if (c == '\r') {
      // CRLF or lone CR
      finish_record();
      if (pos + 1 < len && data[pos + 1] == '\n') {
        pos += 2;
      } else {
        ++pos;
      }
      continue;
    }
    if (c == '\n') {
      finish_record();
      ++pos;
      continue;
    }
    field.push_back(c);
    field_started = true;
    ++pos;
  }

  // Trailing record without newline.
  if (field_started || !field.empty() || field_quoted || !fields.empty()) {
    finish_record();
  }

  // Ignore physically blank lines (including the one a final newline would
  // otherwise turn into an all-NULL one-field row).
  std::vector<std::vector<FileTableField>> non_blank;
  non_blank.reserve(records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    std::vector<FileTableField> &rec = records[i];
    if (!(rec.size() == 1 && rec[0].is_null_ && rec[0].value_.empty())) {
      non_blank.push_back(std::move(rec));
    }
  }
  records.swap(non_blank);

  if (records.empty()) {
    err = "empty csv file";
    return -1;
  }

  int64_t first_row = 0;
  if (options.has_header_) {
    const std::vector<FileTableField> &header = records[0];
    std::vector<std::string> names;
    for (size_t i = 0; i < header.size(); ++i) {
      std::string raw = header[i].is_null_ ? std::string() : header[i].value_;
      std::string name = make_unique_column_name(names, raw);
      names.push_back(name);
      FileTableColumn col;
      col.name_ = name;
      out.columns_.push_back(col);
    }
    first_row = 1;
  }

  for (int64_t r = first_row; r < static_cast<int64_t>(records.size()); ++r) {
    std::vector<FileTableField> &rec = records[r];
    if (out.columns_.empty()) {
      for (size_t i = 0; i < rec.size(); ++i) {
        FileTableColumn col;
        col.name_ = "column_" + std::to_string(i);
        out.columns_.push_back(col);
      }
    }
    if (rec.size() > out.columns_.size()) {
      err = "row " + std::to_string(r) + " has more fields than the header";
      return -1;
    }
    while (rec.size() < out.columns_.size()) {
      FileTableField missing;
      missing.is_null_ = true;
      rec.push_back(missing);
    }
    // Move rows outright: the intermediate record buffer would otherwise
    // double peak memory for large files.
    out.rows_.push_back(std::move(rec));
    if (options.max_rows_ > 0 && static_cast<int64_t>(out.rows_.size()) >= options.max_rows_) {
      break;
    }
  }

  infer_column_types(out);
  return ret;
}

void infer_column_types(FileTableData &data)
{
  for (size_t c = 0; c < data.columns_.size(); ++c) {
    bool nullable = false;
    bool saw_bool = false;
    bool saw_int = false;
    bool saw_double = false;
    bool saw_date = false;
    bool saw_datetime = false;
    bool saw_text = false;
    bool saw_value = false;

    for (size_t r = 0; r < data.rows_.size(); ++r) {
      if (c >= data.rows_[r].size()) {
        continue;
      }
      const FileTableField &f = data.rows_[r][c];
      if (f.is_null_) {
        nullable = true;
        continue;
      }
      saw_value = true;
      const std::string &v = f.value_;
      if (f.from_json_string_) {
        // JSON strings never become numbers/booleans, but date-like strings
        // (e.g. "2026-08-02") do become DATE/DATETIME.
        if (is_datetime(v)) {
          saw_datetime = true;
        } else if (is_date(v)) {
          saw_date = true;
        } else {
          saw_text = true;
        }
      } else if (is_boolean(v)) {
        saw_bool = true;
      } else if (is_bigint(v)) {
        saw_int = true;
      } else if (is_double(v)) {
        saw_double = true;
      } else if (is_datetime(v)) {
        saw_datetime = true;
      } else if (is_date(v)) {
        saw_date = true;
      } else {
        saw_text = true;
      }
    }

    const bool saw_number = saw_int || saw_double;
    const bool saw_temporal = saw_date || saw_datetime;
    FileColumnType type = FileColumnType::VARCHAR;
    if (!saw_value) {
      type = FileColumnType::VARCHAR;
    } else if (saw_text) {
      type = FileColumnType::VARCHAR;
    } else if (saw_bool && (saw_number || saw_temporal)) {
      type = FileColumnType::VARCHAR;
    } else if (saw_number && saw_temporal) {
      // A column holding both numbers and date-looking strings has no single
      // lossless primitive type; VARCHAR preserves every value.
      type = FileColumnType::VARCHAR;
    } else if (saw_bool) {
      type = FileColumnType::BOOLEAN;
    } else if (saw_datetime) {
      // DATE and DATETIME mixed widens to DATETIME
      type = FileColumnType::DATETIME;
    } else if (saw_date) {
      type = FileColumnType::DATE;
    } else if (saw_double) {
      type = FileColumnType::DOUBLE;
    } else if (saw_int) {
      type = FileColumnType::BIGINT;
    }

    data.columns_[c].type_ = type;
    data.columns_[c].nullable_ = nullable;
  }
}

namespace
{

// Minimal cursor for flat JSON objects (the contract rejects nesting).
struct JsonReader
{
  const char *p_;
  const char *end_;

  void skip_ws()
  {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) {
      ++p_;
    }
  }

  bool consume(char c)
  {
    skip_ws();
    if (p_ < end_ && *p_ == c) {
      ++p_;
      return true;
    }
    return false;
  }

  static int hex_value(char h)
  {
    if (h >= '0' && h <= '9') {
      return h - '0';
    }
    if (h >= 'a' && h <= 'f') {
      return h - 'a' + 10;
    }
    if (h >= 'A' && h <= 'F') {
      return h - 'A' + 10;
    }
    return -1;
  }

  bool read_hex4(unsigned &code)
  {
    code = 0;
    for (int i = 0; i < 4; ++i) {
      if (p_ >= end_) {
        return false;
      }
      const int value = hex_value(*p_++);
      if (value < 0) {
        return false;
      }
      code = (code << 4) | static_cast<unsigned>(value);
    }
    return true;
  }

  static void append_utf8(std::string &out, unsigned code)
  {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool parse_string(std::string &out)
  {
    skip_ws();
    if (p_ >= end_ || *p_ != '"') {
      return false;
    }
    ++p_;
    while (p_ < end_) {
      const char c = *p_++;
      if (c == '"') {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        // Raw control characters are not allowed inside JSON strings.
        return false;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p_ >= end_) {
        return false;
      }
      const char e = *p_++;
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned code = 0;
          if (!read_hex4(code)) {
            return false;
          }
          // Combine a UTF-16 surrogate pair into one code point; a lone
          // surrogate is malformed JSON and is rejected.
          if (code >= 0xD800 && code <= 0xDBFF) {
            unsigned low = 0;
            if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') {
              return false;
            }
            p_ += 2;
            if (!read_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
              return false;
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return false;
          }
          append_utf8(out, code);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool parse_number(std::string &out)
  {
    skip_ws();
    const char *start = p_;
    if (p_ < end_ && *p_ == '-') {
      ++p_;
    }
    if (p_ >= end_) {
      return false;
    }
    if (*p_ == '0') {
      ++p_;
    } else if (is_digit(*p_)) {
      while (p_ < end_ && is_digit(*p_)) {
        ++p_;
      }
    } else {
      return false;
    }
    if (p_ < end_ && *p_ == '.') {
      ++p_;
      if (p_ >= end_ || !is_digit(*p_)) {
        return false;
      }
      while (p_ < end_ && is_digit(*p_)) {
        ++p_;
      }
    }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (p_ < end_ && (*p_ == '+' || *p_ == '-')) {
        ++p_;
      }
      if (p_ >= end_ || !is_digit(*p_)) {
        return false;
      }
      while (p_ < end_ && is_digit(*p_)) {
        ++p_;
      }
    }
    out.assign(start, static_cast<size_t>(p_ - start));
    return true;
  }

  bool parse_literal(const char *lit)
  {
    skip_ws();
    size_t n = strlen(lit);
    if (static_cast<size_t>(end_ - p_) < n || 0 != memcmp(p_, lit, n)) {
      return false;
    }
    p_ += n;
    return true;
  }
};

struct JsonLine
{
  std::vector<std::pair<std::string, FileTableField>> fields_;
};

} // namespace


// Rewrites a JSON array of objects into newline-delimited objects so that the
// per-line parser below handles both formats. Returns false when the buffer is
// not a JSON array (the common JSONL case), which keeps that path untouched.
static bool try_flatten_json_array(const char *data, int64_t len, std::string &out)
{
  out.clear();
  const char *p = data;
  const char *end = data + len;
  while (p < end && (' ' == *p || '\t' == *p || '\n' == *p || '\r' == *p)) {
    ++p;
  }
  if (p >= end || '[' != *p) {
    return false;
  }
  ++p;
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  std::string current;
  for (; p < end; ++p) {
    const char ch = *p;
    if (in_string) {
      current.push_back(ch);
      if (escaped) {
        escaped = false;
      } else if ('\\' == ch) {
        escaped = true;
      } else if ('"' == ch) {
        in_string = false;
      }
      continue;
    }
    if ('"' == ch) {
      in_string = true;
      current.push_back(ch);
    } else if ('{' == ch) {
      ++depth;
      current.push_back(ch);
    } else if ('}' == ch) {
      if (depth > 0) {
        --depth;
      }
      current.push_back(ch);
      if (0 == depth) {
        out += current;
        out.push_back('\n');
        current.clear();
      }
    } else if (']' == ch && 0 == depth) {
      break;
    } else if (0 == depth) {
      // whitespace / commas between array elements
      continue;
    } else if ('\n' == ch || '\r' == ch) {
      // keep one object per line even for pretty-printed input
      current.push_back(' ');
    } else {
      current.push_back(ch);
    }
  }
  return true;
}

int parse_jsonl(const char *data, int64_t len, FileTableData &out, std::string &err)
{
  out.reset();
  err.clear();
  if (NULL == data || len < 0) {
    err = "invalid jsonl buffer";
    return -1;
  }

  std::vector<JsonLine> lines;
  // JSON arrays are flattened into JSONL first; JSONL keeps the fast path.
  std::string flattened;
  const char *body = data;
  int64_t body_len = len;
  if (try_flatten_json_array(data, len, flattened)) {
    body = flattened.data();
    body_len = static_cast<int64_t>(flattened.size());
  }
  const char *p = body;
  const char *end = body + body_len;
  int64_t line_no = 0;

  while (p < end) {
    const char *line_end = p;
    while (line_end < end && *line_end != '\n') {
      ++line_end;
    }
    const char *trim = line_end;
    while (trim > p && (trim[-1] == '\r' || trim[-1] == ' ' || trim[-1] == '\t')) {
      --trim;
    }
    ++line_no;

    bool blank = true;
    for (const char *q = p; q < trim; ++q) {
      if (*q != ' ' && *q != '\t') {
        blank = false;
        break;
      }
    }

    if (!blank) {
      JsonReader r{p, trim};
      JsonLine line;
      if (!r.consume('{')) {
        err = "line " + std::to_string(line_no) + ": not a JSON object";
        return -1;
      }
      if (!r.consume('}')) {
        while (true) {
          std::string key;
          if (!r.parse_string(key)) {
            err = "line " + std::to_string(line_no) + ": malformed key";
            return -1;
          }
          if (!r.consume(':')) {
            err = "line " + std::to_string(line_no) + ": missing ':'";
            return -1;
          }
          FileTableField field;
          r.skip_ws();
          if (r.p_ < r.end_ && *r.p_ == '"') {
            std::string s;
            if (!r.parse_string(s)) {
              err = "line " + std::to_string(line_no) + ": malformed string";
              return -1;
            }
            field.value_ = s;
            field.from_json_string_ = true;
          } else if (r.p_ < r.end_ && (*r.p_ == '{' || *r.p_ == '[')) {
            err = "line " + std::to_string(line_no) + ": nested JSON values are not supported";
            return -1;
          } else if (r.parse_literal("true")) {
            field.value_ = "true";
          } else if (r.parse_literal("false")) {
            field.value_ = "false";
          } else if (r.parse_literal("null")) {
            field.is_null_ = true;
          } else {
            std::string num;
            if (!r.parse_number(num)) {
              err = "line " + std::to_string(line_no) + ": malformed value";
              return -1;
            }
            field.value_ = num;
          }
          line.fields_.push_back(std::make_pair(key, field));
          if (r.consume(',')) {
            continue;
          }
          if (r.consume('}')) {
            break;
          }
          err = "line " + std::to_string(line_no) + ": malformed object";
          return -1;
        }
      }
      r.skip_ws();
      if (r.p_ != r.end_) {
        err = "line " + std::to_string(line_no) + ": trailing characters";
        return -1;
      }
      lines.push_back(line);
    }
    p = line_end + (line_end < end ? 1 : 0);
  }

  // Union of keys in first-appearance order.
  std::vector<std::string> names;
  for (size_t i = 0; i < lines.size(); ++i) {
    for (size_t k = 0; k < lines[i].fields_.size(); ++k) {
      const std::string &key = lines[i].fields_[k].first;
      bool found = false;
      for (size_t n = 0; n < names.size(); ++n) {
        if (names[n] == key) {
          found = true;
          break;
        }
      }
      if (!found) {
        names.push_back(key);
      }
    }
  }

  for (size_t n = 0; n < names.size(); ++n) {
    FileTableColumn col;
    col.name_ = names[n];
    out.columns_.push_back(col);
  }

  for (size_t i = 0; i < lines.size(); ++i) {
    std::vector<FileTableField> row(names.size());
    for (size_t n = 0; n < row.size(); ++n) {
      row[n].is_null_ = true;  // keys absent from this line become NULL
      row[n].from_json_string_ = false;
    }
    for (size_t k = 0; k < lines[i].fields_.size(); ++k) {
      const std::string &key = lines[i].fields_[k].first;
      for (size_t n = 0; n < names.size(); ++n) {
        if (names[n] == key) {
          row[n] = lines[i].fields_[k].second;
          break;
        }
      }
    }
    out.rows_.push_back(row);
  }

  infer_column_types(out);
  return 0;
}

int list_glob(const char *pattern, std::vector<std::string> &files, std::string &err)
{
  files.clear();
  err.clear();
  if (NULL == pattern || '\0' == *pattern) {
    err = "empty glob pattern";
    return -1;
  }

  std::string pat(pattern);
  std::string dir;
  std::string base;
  size_t slash = pat.find_last_of('/');
  if (std::string::npos == slash) {
    dir = ".";
    base = pat;
  } else {
    dir = (0 == slash) ? "/" : pat.substr(0, slash);
    base = pat.substr(slash + 1);
  }
  if (base.empty()) {
    err = "glob pattern must name a file pattern";
    return -1;
  }

  DIR *dp = opendir(dir.c_str());
  if (NULL == dp) {
    err = "cannot open directory " + dir + ": " + strerror(errno);
    return -1;
  }

  struct dirent *entry = NULL;
  while (NULL != (entry = readdir(dp))) {
    if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")) {
      continue;
    }
    if (0 != fnmatch(base.c_str(), entry->d_name, 0)) {
      continue;
    }
    std::string full = dir;
    if (full != "/" && (full.empty() || full[full.size() - 1] != '/')) {
      full += "/";
    }
    full += entry->d_name;

    struct stat st;
    if (0 != stat(full.c_str(), &st)) {
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }
    files.push_back(full);
  }
  closedir(dp);

  return 0;
}

} // namespace filetable
} // namespace sql
} // namespace oceanbase
