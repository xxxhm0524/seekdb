// Parser-level unit tests for the file-as-database SQL surface.
//
// Covers the four table functions, aliases, named arguments, glob(), DESCRIBE
// and COPY. These tests only use the public parser API, so they do not depend
// on the storage or executor layers.
//
// Intended to be compiled by the repository unittest target; the standalone
// form used during development was:
//   g++ -std=c++20 -O1 -I src -I src/sql/parser -I src/objit/include \
//       unittest/sql/parser/test_file_sql_parser.cpp <ob_sql_parser objs>

#include <cstdio>
#include <cstring>
#include <string>

#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

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

bool find_node(const ParseNode *node, ObItemType type, int depth = 0)
{
  if (nullptr == node || depth > 64) {
    return false;
  }
  if (type == node->type_) {
    return true;
  }
  for (int64_t i = 0; nullptr != node->children_ && i < node->num_child_; ++i) {
    if (find_node(node->children_[i], type, depth + 1)) {
      return true;
    }
  }
  return false;
}

// Parses `sql`. Returns true on success and exposes the root node.
bool parse_sql(const char *sql,
               oceanbase::common::ObArenaAllocator &allocator,
               oceanbase::sql::ObParser &parser,
               ParseResult &result)
{
  oceanbase::common::ObString stmt(static_cast<int32_t>(std::strlen(sql)), sql);
  return OB_SUCCESS == parser.parse(stmt, result) && nullptr != result.result_tree_;
}

void expect_file_table(const char *sql, const char *label)
{
  oceanbase::common::ObArenaAllocator allocator("file_sql_parser_test");
  oceanbase::sql::ObParser parser(allocator, SQL_MODE_DEFAULT);
  ParseResult result;
  if (!parse_sql(sql, allocator, parser, result)) {
    check(false, std::string(label) + ": parses");
    return;
  }
  check(true, std::string(label) + ": parses");
  check(find_node(result.result_tree_, T_FILE_TABLE_EXPRESSION),
        std::string(label) + ": file table node");
  parser.free_result(result);
}

}  // namespace

int main()
{
  // ---- the four table functions, with and without alias ----
  expect_file_table("select id from read_csv('/tmp/a.csv')", "read_csv");
  expect_file_table("select id from read_csv('/tmp/a.csv') f", "read_csv alias");
  expect_file_table("select id from read_json('/tmp/a.jsonl') f", "read_json");
  expect_file_table("select id from read_ndjson('/tmp/a.jsonl') f", "read_ndjson alias");
  expect_file_table("select id from read_parquet('/tmp/a.parquet') p", "read_parquet");
  expect_file_table("select file from glob('/tmp/*')", "glob");

  // ---- named arguments ----
  expect_file_table("select * from read_csv('/tmp/a.csv', header = false)", "header=false");
  expect_file_table("select * from read_csv('/tmp/a.csv', delimiter = ';')", "delimiter=';'");
  expect_file_table("select * from read_csv('/tmp/a.csv', sample_size = -1)", "sample_size=-1");
  expect_file_table("select * from read_json('/tmp/a.jsonl', format = 'newline_delimited')",
                    "format=newline_delimited");

  // ---- DESCRIBE / COPY / JOIN ----
  expect_file_table("describe select * from read_csv('/tmp/a.csv')", "describe select");
  expect_file_table("copy (select id from read_csv('/tmp/a.csv')) to '/tmp/o.csv' "
                    "(format csv, header, nullstr 'NULL')",
                    "copy");
  expect_file_table("select a.id from read_csv('/tmp/a.csv') a "
                    "join read_parquet('/tmp/b.parquet') b on a.id = b.id",
                    "cross-format join");

  // ---- ordinary SQL must not regress ----
  {
    oceanbase::common::ObArenaAllocator allocator("file_sql_parser_test");
    oceanbase::sql::ObParser parser(allocator, SQL_MODE_DEFAULT);
    ParseResult result;
    check(parse_sql("select 1", allocator, parser, result), "plain select still parses");
    parser.free_result(result);
  }

  std::printf("\n%s (%d failure%s)\n", 0 == g_failures ? "ALL PASS" : "FAILURES", g_failures,
              g_failures == 1 ? "" : "s");
  return 0 == g_failures ? 0 : 1;
}
