/**
 * Standalone CSV reader + deterministic type inference for the file-table
 * feature (competition task 2).
 *
 * Deliberately free of oceanbase dependencies so the logic can be unit tested
 * with a plain compiler; the resolver integration will wrap this.
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_CSV_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_CSV_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace filetable
{

enum class FileColumnType
{
  BIGINT = 0,
  DOUBLE,
  BOOLEAN,
  DATE,
  DATETIME,
  VARCHAR,
};

const char *file_column_type_name(FileColumnType type);

struct FileTableColumn
{
  std::string name_;
  FileColumnType type_ = FileColumnType::VARCHAR;
  bool nullable_ = false;
};

// One field of one row. is_null_ marks an empty (unquoted) CSV field or a JSON null.
struct FileTableField
{
  std::string value_;
  bool is_null_ = false;
  // true when the value came from a JSON string literal (vs a JSON number/bool).
  // Such values only ever infer as DATE/DATETIME/VARCHAR, never as numbers.
  bool from_json_string_ = false;
};

struct FileTableData
{
  std::vector<FileTableColumn> columns_;
  std::vector<std::vector<FileTableField>> rows_;

  void reset()
  {
    columns_.clear();
    rows_.clear();
  }
};

struct CsvOptions
{
  bool has_header_ = true;
  char field_delimiter_ = ',';
  char line_terminator_ = '\n';
  // max rows to materialize; 0 means no limit
  int64_t max_rows_ = 0;
};

/**
 * Parse a whole CSV buffer. Supports RFC4180 quoting: quoted fields may contain
 * delimiters and newlines, "" is a literal quote. CRLF is accepted. Unquoted
 * empty fields become NULL.
 *
 * Column names are de-duplicated the way DuckDB does: the second occurrence of
 * "id" becomes "id__2", the third "id__3", ...
 */
int parse_csv(const char *data,
              int64_t len,
              const CsvOptions &options,
              FileTableData &out,
              std::string &err);

/**
 * Parse newline-delimited JSON (one flat object per line).
 *
 * The schema is the union of all object keys, in first-appearance order; keys
 * missing from a line become NULL. Nested objects/arrays are rejected (the
 * contract forbids them). JSON strings only infer as DATE/DATETIME/VARCHAR;
 * JSON numbers infer as BIGINT/DOUBLE, booleans as BOOLEAN.
 */
int parse_jsonl(const char *data, int64_t len, FileTableData &out, std::string &err);

/**
 * Expand a single-level glob (POSIX glob(3)-style wildcards) into regular file
 * paths. Returns full paths in `files`; directories are skipped. The caller is
 * expected to sort if needed (the SQL contract sorts explicitly).
 */
int list_glob(const char *pattern, std::vector<std::string> &files, std::string &err);

// Duplicate-name suffixing, exposed for testing.
std::string make_unique_column_name(const std::vector<std::string> &existing,
                                    const std::string &candidate);

/**
 * Infer the column types from all parsed rows (full-file inference, never a
 * sample). Nullability is YES iff at least one field in the column is NULL.
 */
void infer_column_types(FileTableData &data);

// ---- value predicates / parsers used by inference ----
bool is_bigint(const std::string &s);
bool is_double(const std::string &s);
bool is_boolean(const std::string &s);
bool is_date(const std::string &s);
bool is_datetime(const std::string &s);

} // namespace filetable
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_CSV_H_
