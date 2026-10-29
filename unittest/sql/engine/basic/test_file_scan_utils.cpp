// Unit tests for the file scan utilities used by the file-as-database feature.
//
// Covers CSV inference and read options, JSONL field union / missing fields /
// nested rejection, Parquet decoding (Thrift footer + Snappy + PLAIN) and the
// glob() file discovery helper.
//
// Standalone build (no server required):
//   g++ -std=c++20 -O1 -Wall -o t_scan \
//       unittest/sql/engine/basic/test_file_scan_utils.cpp \
//       src/sql/engine/basic/file_table/ob_file_table_csv.cpp \
//       src/sql/engine/basic/file_table/ob_file_table_parquet.cpp \
//       -I src/sql/engine/basic/file_table
// Run with the fixture dir as argv[1] (defaults to the mysqltest data dir).

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ob_file_table_csv.h"
#include "ob_file_table_parquet.h"

using namespace oceanbase::sql::filetable;

namespace
{

int g_failures = 0;

void check(bool ok, const std::string &what)
{
  std::printf("%s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
  if (!ok) {
    ++g_failures;
  }
}

std::string read_file(const std::string &path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void test_csv()
{
  // header, duplicate name -> id__2, NULL, multiline quoted field
  const char *text = "id,name,id\n1,\"a,b\",2\n3,,4\n5,\"x\ny\",6\n";
  FileTableData d;
  std::string err;
  check(0 == parse_csv(text, static_cast<int64_t>(std::strlen(text)), CsvOptions(), d, err),
        "csv: parses");
  check(d.columns_.size() == 3, "csv: 3 columns");
  if (d.columns_.size() == 3) {
    check(d.columns_[0].name_ == "id", "csv: first column name");
    check(d.columns_[2].name_ == "id__2", "csv: duplicate header renamed to id__2");
    check(d.columns_[0].type_ == FileColumnType::BIGINT, "csv: id inferred BIGINT");
    check(d.columns_[1].type_ == FileColumnType::VARCHAR, "csv: name inferred VARCHAR");
  }
  check(d.rows_.size() == 3, "csv: 3 rows");
  if (d.rows_.size() == 3) {
    check(d.rows_[0][1].value_ == "a,b", "csv: quoted delimiter kept");
    check(d.rows_[1][1].is_null_, "csv: empty unquoted field is NULL");
    check(d.rows_[2][1].value_ == "x\ny", "csv: newline inside quotes kept");
  }

  // UTF-8 BOM must not leak into the first column name
  const char *bom = "\xef\xbb\xbfid,name\n1,a\n";
  FileTableData b;
  std::string be;
  check(0 == parse_csv(bom, static_cast<int64_t>(std::strlen(bom)), CsvOptions(), b, be),
        "csv bom: parses");
  check(!b.columns_.empty() && b.columns_[0].name_ == "id", "csv bom: column name is clean");

  // header = false: every line is data
  CsvOptions no_header;
  no_header.has_header_ = false;
  FileTableData n;
  std::string ne;
  const char *plain = "1,2\n3,4\n";
  check(0 == parse_csv(plain, static_cast<int64_t>(std::strlen(plain)), no_header, n, ne),
        "csv header=false: parses");
  check(n.rows_.size() == 2, "csv header=false: both lines are rows");

  // custom delimiter
  CsvOptions semi;
  semi.field_delimiter_ = ';';
  FileTableData s;
  std::string se;
  const char *semi_text = "a;b\n1;2\n";
  check(0 == parse_csv(semi_text, static_cast<int64_t>(std::strlen(semi_text)), semi, s, se),
        "csv delimiter=';': parses");
  check(s.columns_.size() == 2 && s.rows_.size() == 1, "csv delimiter=';': split correctly");
}

void test_type_inference()
{
  // BIGINT range boundaries: values outside int64 must widen, not overflow.
  check(is_bigint("9223372036854775807"), "type: int64 max is BIGINT");
  check(is_bigint("-9223372036854775808"), "type: int64 min is BIGINT");
  check(!is_bigint("9223372036854775808"), "type: int64 max+1 is not BIGINT");
  check(!is_bigint("-9223372036854775809"), "type: int64 min-1 is not BIGINT");

  const char *overflow = "v\n9223372036854775808\n";
  FileTableData o;
  std::string oe;
  check(0 == parse_csv(overflow, static_cast<int64_t>(std::strlen(overflow)), CsvOptions(), o, oe),
        "type: overflow parses");
  check(!o.columns_.empty() && o.columns_[0].type_ == FileColumnType::DOUBLE,
        "type: overflowing integer widens to DOUBLE");

  // Mixing numbers with date-looking text has no lossless primitive type.
  const char *mixed = "v\n1\n2026-01-02\n";
  FileTableData m;
  std::string me;
  check(0 == parse_csv(mixed, static_cast<int64_t>(std::strlen(mixed)), CsvOptions(), m, me),
        "type: number+date parses");
  check(!m.columns_.empty() && m.columns_[0].type_ == FileColumnType::VARCHAR,
        "type: number+date widens to VARCHAR");

  // DATE + DATETIME widens to DATETIME; BIGINT + DOUBLE widens to DOUBLE.
  const char *temporal = "v\n2026-01-02\n2026-01-02 03:04:05\n";
  FileTableData t;
  std::string te;
  check(0 == parse_csv(temporal, static_cast<int64_t>(std::strlen(temporal)), CsvOptions(), t, te),
        "type: date+datetime parses");
  check(!t.columns_.empty() && t.columns_[0].type_ == FileColumnType::DATETIME,
        "type: date+datetime widens to DATETIME");

  const char *numeric = "v\n1\n2.5\n";
  FileTableData n;
  std::string ne;
  check(0 == parse_csv(numeric, static_cast<int64_t>(std::strlen(numeric)), CsvOptions(), n, ne),
        "type: int+double parses");
  check(!n.columns_.empty() && n.columns_[0].type_ == FileColumnType::DOUBLE,
        "type: int+double widens to DOUBLE");

  // Path normalization closes `..` traversal before the secure-path check.
  check(normalize_path("/allowed/../secret.csv") == "/secret.csv",
        "path: parent traversal is resolved");
  check(normalize_path("/allowed/./data/../basic.csv") == "/allowed/basic.csv",
        "path: dot components are resolved");
  check(normalize_path("/allowed/*.csv") == "/allowed/*.csv", "path: glob is preserved");
  check(normalize_path("a/../../b") == "../b", "path: relative parent is preserved");

  // Blank physical lines in the middle of a document are ignored.
  const char *blank = "a,b\n1,2\n\n3,4\n";
  FileTableData b;
  std::string be;
  check(0 == parse_csv(blank, static_cast<int64_t>(std::strlen(blank)), CsvOptions(), b, be),
        "csv blank line: parses");
  check(b.rows_.size() == 2, "csv blank line: skipped");
}

void test_jsonl()
{
  // union of keys, missing values are NULL
  const char *text = "{\"a\":1,\"b\":\"x\"}\n{\"a\":2,\"c\":true}\n";
  FileTableData d;
  std::string err;
  check(0 == parse_jsonl(text, static_cast<int64_t>(std::strlen(text)), d, err),
        "jsonl: parses");
  check(d.columns_.size() == 3, "jsonl: union of 3 keys");
  check(d.rows_.size() == 2, "jsonl: 2 rows");
  if (d.rows_.size() == 2 && d.rows_[0].size() == 3) {
    check(d.rows_[1][1].is_null_, "jsonl: missing key is NULL");
  }

  // nested objects/arrays are rejected
  const char *bad = "{\"a\":{\"b\":1}}\n";
  FileTableData n;
  std::string ne;
  check(0 != parse_jsonl(bad, static_cast<int64_t>(std::strlen(bad)), n, ne),
        "jsonl: nested object rejected");

  // JSON array input
  const char *arr = "[{\"a\":1},{\"a\":2}]";
  FileTableData a;
  std::string ae;
  check(0 == parse_jsonl(arr, static_cast<int64_t>(std::strlen(arr)), a, ae), "json array: parses");
  check(a.rows_.size() == 2, "json array: 2 rows");

  // Surrogate pairs become one 4-byte UTF-8 code point.
  const char *surrogate = "{\"s\":\"\\uD83D\\uDE00\"}\n";
  FileTableData s;
  std::string se;
  check(0 == parse_jsonl(surrogate, static_cast<int64_t>(std::strlen(surrogate)), s, se),
        "jsonl: surrogate pair parses");
  check(!s.rows_.empty() && s.rows_[0][0].value_ == "\xF0\x9F\x98\x80",
        "jsonl: surrogate pair is one code point");

  // Malformed JSON numbers must be rejected rather than treated as text.
  const char *bad_numbers[] = {
      "{\"n\":01}\n", "{\"n\":1.}\n", "{\"n\":1e}\n", "{\"n\":+1}\n", "{\"n\":1-2}\n"};
  for (size_t i = 0; i < sizeof(bad_numbers) / sizeof(bad_numbers[0]); ++i) {
    FileTableData b;
    std::string be;
    check(0 != parse_jsonl(bad_numbers[i], static_cast<int64_t>(std::strlen(bad_numbers[i])), b, be),
          "jsonl: malformed number rejected");
  }
}

void test_parquet(const std::string &fixture)
{
  std::string content = read_file(fixture);
  if (content.empty()) {
    check(false, "parquet: fixture readable (" + fixture + ")");
    return;
  }
  FileTableData d;
  std::string err;
  check(0 == parse_parquet(content.data(), static_cast<int64_t>(content.size()), d, err),
        "parquet: parses");
  check(d.columns_.size() == 1, "parquet: 1 column");
  check(d.rows_.size() == 7, "parquet: 7 rows");
  long sum = 0;
  for (size_t i = 0; i < d.rows_.size(); ++i) {
    if (!d.rows_[i].empty() && !d.rows_[i][0].is_null_) {
      sum += std::atol(d.rows_[i][0].value_.c_str());
    }
  }
  check(21 == sum, "parquet: values sum to 21");

  // snappy: literal-only block encodes "abc"
  const char block[] = {0x03, 0x08, 'a', 'b', 'c'};
  std::string out;
  check(0 == snappy_decompress(block, sizeof(block), out), "snappy: literal block");
  check(out == "abc", "snappy: decoded value");
}

void test_parquet_dictionary_v2(const std::string &fixture)
{
  std::string content = read_file(fixture);
  if (content.empty()) {
    std::printf("skip: parquet dict v2 fixture not present (%s)\n", fixture.c_str());
    return;
  }
  FileTableData d;
  std::string err;
  check(0 == parse_parquet(content.data(), static_cast<int64_t>(content.size()), d, err),
        "parquet dict v2: parses");
  check(d.columns_.size() == 1 && d.rows_.size() == 4, "parquet dict v2: shape");
  if (d.columns_.size() == 1 && d.rows_.size() == 4) {
    check(d.columns_[0].type_ == FileColumnType::VARCHAR, "parquet dict v2: VARCHAR column");
    check(d.rows_[0][0].value_ == "alpha", "parquet dict v2: first dictionary value");
    check(d.rows_[1][0].value_ == "beta", "parquet dict v2: second dictionary value");
    check(d.rows_[2][0].value_ == "alpha", "parquet dict v2: repeated index");
    check(d.rows_[3][0].is_null_, "parquet dict v2: null definition level");
  }
}

void test_parquet_v1_plain(const std::string &fixture)
{
  std::string content = read_file(fixture);
  if (content.empty()) {
    std::printf("skip: parquet v1 plain fixture not present (%s)\n", fixture.c_str());
    return;
  }
  FileTableData d;
  std::string err;
  check(0 == parse_parquet(content.data(), static_cast<int64_t>(content.size()), d, err),
        "parquet v1 plain: parses");
  check(d.columns_.size() == 1 && d.rows_.size() == 3, "parquet v1 plain: shape");
  if (d.columns_.size() == 1 && d.rows_.size() == 3) {
    check(d.rows_[0][0].value_ == "1" && d.rows_[1][0].value_ == "2"
              && d.rows_[2][0].value_ == "3",
          "parquet v1 plain: values");
  }
}

void test_parquet_v1_dictionary(const std::string &fixture)
{
  std::string content = read_file(fixture);
  if (content.empty()) {
    std::printf("skip: parquet v1 dict fixture not present (%s)\n", fixture.c_str());
    return;
  }
  FileTableData d;
  std::string err;
  check(0 == parse_parquet(content.data(), static_cast<int64_t>(content.size()), d, err),
        "parquet v1 dict: parses");
  check(d.columns_.size() == 1 && d.rows_.size() == 3, "parquet v1 dict: shape");
  if (d.columns_.size() == 1 && d.rows_.size() == 3) {
    check(d.rows_[0][0].value_ == "10" && d.rows_[1][0].value_ == "20"
              && d.rows_[2][0].value_ == "10",
          "parquet v1 dict: values");
  }
}

void test_glob(const std::string &dir)
{
  std::vector<std::string> files;
  std::string err;
  check(0 == list_glob((dir + "/*").c_str(), files, err), "glob: succeeds");
  std::set<std::string> names;
  for (size_t i = 0; i < files.size(); ++i) {
    size_t slash = files[i].find_last_of('/');
    names.insert(files[i].substr(slash + 1));
  }
  check(names.count("basic.csv") == 1, "glob: finds basic.csv");
  check(names.count("basic.jsonl") == 1, "glob: finds basic.jsonl");
  check(names.count("simple.parquet") == 1, "glob: finds simple.parquet");
}

}  // namespace

int main(int argc, char **argv)
{
  const std::string dir = argc > 1 ? argv[1]
                                   : "/root/fts-run/data_for_mysqltest_file_sql";
  test_csv();
  test_type_inference();
  test_jsonl();
  test_parquet(dir + "/simple.parquet");
  test_parquet_dictionary_v2(dir + "/dict_v2.parquet");
  test_parquet_v1_plain(dir + "/plain_v1.parquet");
  test_parquet_v1_dictionary(dir + "/dict_v1.parquet");
  test_glob(dir);

  std::printf("\n%s (%d failure%s)\n", 0 == g_failures ? "ALL PASS" : "FAILURES", g_failures,
              g_failures == 1 ? "" : "s");
  return 0 == g_failures ? 0 : 1;
}
