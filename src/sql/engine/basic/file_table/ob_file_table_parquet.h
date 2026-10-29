/**
 * Standalone Parquet reader for the file-table feature (competition task 2).
 *
 * Scope: flat schemas (no nested groups), PLAIN encoded data, DATA_PAGE and
 * DATA_PAGE_V2, UNCOMPRESSED and SNAPPY codecs, optional/required columns.
 * That covers the contract fixture and the common cases a test is likely to
 * use; dictionary pages are reported as unsupported.
 *
 * No oceanbase dependencies: unit-testable with a plain compiler.
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_FILE_TABLE_OB_FILE_TABLE_PARQUET_H_
#define OCEANBASE_SQL_ENGINE_BASIC_FILE_TABLE_OB_FILE_TABLE_PARQUET_H_

#include <cstdint>
#include <string>

#include "ob_file_table_csv.h"

namespace oceanbase
{
namespace sql
{
namespace filetable
{

/**
 * Parse a Parquet buffer into FileTableData. Column types come from the
 * Parquet schema (not inferred), values are rendered as text so they can be
 * cast by the SQL layer exactly like CSV cells.
 */
int parse_parquet(const char *data, int64_t len, FileTableData &out, std::string &err);

// Snappy block decompression, exposed for testing.
// Returns 0 on success; writes the decompressed bytes to `out`.
int snappy_decompress(const char *in, int64_t in_len, std::string &out);

} // namespace filetable
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_FILE_TABLE_OB_FILE_TABLE_PARQUET_H_
