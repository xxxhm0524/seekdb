// Unit driver for the standalone CSV reader / type inference.
// Prints the parsed schema and rows in the exact format asserted by
// file_sql_basic.result, then runs a few focused unit checks.
#include "ob_file_table_csv.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace oceanbase::sql::filetable;

static int failures = 0;

static void check(bool ok, const char *what)
{
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  } else {
    std::printf("ok:   %s\n", what);
  }
}

static std::string read_file(const char *path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static std::string render(const FileTableField &f)
{
  return f.is_null_ ? "NULL" : f.value_;
}

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : "/root/fts-run/data_for_mysqltest_file_sql/basic.csv";
  std::string content = read_file(path);
  if (content.empty()) {
    std::printf("cannot read %s\n", path);
    return 2;
  }

  // ---- unit checks: name de-duplication ----
  std::vector<std::string> names;
  std::string n1 = make_unique_column_name(names, "id");
  names.push_back(n1);
  std::string n2 = make_unique_column_name(names, "id");
  names.push_back(n2);
  std::string n3 = make_unique_column_name(names, "id");
  check(n1 == "id", "first id keeps its name");
  check(n2 == "id__2", "second id becomes id__2");
  check(n3 == "id__3", "third id becomes id__3");

  // ---- unit checks: type predicates ----
  check(is_bigint("87"), "87 is BIGINT");
  check(!is_bigint("98.5"), "98.5 is not BIGINT");
  check(is_double("98.5") && is_double("87"), "87 and 98.5 are DOUBLE-compatible");
  check(is_boolean("true") && is_boolean("false"), "true/false are BOOLEAN");
  check(is_date("2026-08-01"), "2026-08-01 is DATE");
  check(!is_date("2026-08-01 10:11:12"), "datetime is not DATE");
  check(is_datetime("2026-08-01 10:11:12"), "space datetime");
  check(is_datetime("2026-08-03T12:13:14"), "ISO-T datetime");
  check(!is_datetime("2026-08-03T12:13:14 "), "trailing space rejected");

  // ---- parse the fixture ----
  CsvOptions opt;
  FileTableData data;
  std::string err;
  int ret = parse_csv(content.data(), static_cast<int64_t>(content.size()), opt, data, err);
  check(ret == 0, "parse_csv returns success");
  if (ret != 0) {
    std::printf("parse error: %s\n", err.c_str());
    return 1;
  }

  std::printf("\n--- schema ---\n");
  for (size_t i = 0; i < data.columns_.size(); ++i) {
    std::printf("%s\t%s\t%s\n", data.columns_[i].name_.c_str(),
                file_column_type_name(data.columns_[i].type_),
                data.columns_[i].nullable_ ? "YES" : "NO");
  }

  std::printf("\n--- rows ---\n");
  for (size_t r = 0; r < data.rows_.size(); ++r) {
    for (size_t c = 0; c < data.rows_[r].size(); ++c) {
      std::string v = render(data.rows_[r][c]);
      // mimic replace(note, char(10), '<NL>') for readability
      std::string shown;
      for (size_t k = 0; k < v.size(); ++k) {
        if (v[k] == '\n') {
          shown += "<NL>";
        } else {
          shown += v[k];
        }
      }
      std::printf("%s%s", shown.c_str(), c + 1 == data.rows_[r].size() ? "" : "\t");
    }
    std::printf("\n");
  }

  // ---- assertions against file_sql_basic.result ----
  std::printf("\n--- contract checks ---\n");
  const char *expect_names[] = {"id", "name", "score", "active", "day", "created", "note", "id__2"};
  const FileColumnType expect_types[] = {FileColumnType::BIGINT, FileColumnType::VARCHAR,
                                         FileColumnType::DOUBLE, FileColumnType::BOOLEAN,
                                         FileColumnType::DATE, FileColumnType::DATETIME,
                                         FileColumnType::VARCHAR, FileColumnType::BIGINT};
  const bool expect_null[] = {false, false, false, false, true, true, false, false};

  check(data.columns_.size() == 8, "8 columns");
  for (size_t i = 0; i < data.columns_.size() && i < 8; ++i) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "column %zu is %s %s", i, expect_names[i],
                  expect_null[i] ? "YES" : "NO");
    check(data.columns_[i].name_ == expect_names[i]
              && data.columns_[i].type_ == expect_types[i]
              && data.columns_[i].nullable_ == expect_null[i],
          buf);
  }

  check(data.rows_.size() == 3, "3 data rows");
  if (data.rows_.size() == 3) {
    check(data.rows_[0][6].value_ == "hello,\nworld", "multiline quoted field preserved");
    check(data.rows_[2][6].value_ == "say \"hi\"", "doubled quotes unescaped");
    check(data.rows_[1][4].is_null_ && data.rows_[1][5].is_null_, "empty fields are NULL");
    check(data.rows_[0][7].value_ == "101", "duplicate header column holds its own data");
  }

  // ---- JSONL: union schema, missing keys, type unification ----
  const char *jsonl_path = argc > 2 ? argv[2]
                                    : "/root/fts-run/data_for_mysqltest_file_sql/basic.jsonl";
  std::string jsonl = read_file(jsonl_path);
  FileTableData jdata;
  std::string jerr;
  int jret = parse_jsonl(jsonl.data(), static_cast<int64_t>(jsonl.size()), jdata, jerr);
  check(jret == 0, "parse_jsonl returns success");
  if (jret != 0) {
    std::printf("jsonl error: %s\n", jerr.c_str());
  } else {
    std::printf("\n--- jsonl schema ---\n");
    for (size_t i = 0; i < jdata.columns_.size(); ++i) {
      std::printf("%s\t%s\t%s\n", jdata.columns_[i].name_.c_str(),
                  file_column_type_name(jdata.columns_[i].type_),
                  jdata.columns_[i].nullable_ ? "YES" : "NO");
    }
    std::printf("\n--- jsonl rows ---\n");
    for (size_t r = 0; r < jdata.rows_.size(); ++r) {
      for (size_t c = 0; c < jdata.rows_[r].size(); ++c) {
        std::printf("%s%s", render(jdata.rows_[r][c]).c_str(),
                    c + 1 == jdata.rows_[r].size() ? "" : "\t");
      }
      std::printf("\n");
    }

    const char *jnames[] = {"id", "name", "score", "active", "day", "note"};
    const FileColumnType jtypes[] = {FileColumnType::BIGINT, FileColumnType::VARCHAR,
                                     FileColumnType::DOUBLE, FileColumnType::BOOLEAN,
                                     FileColumnType::DATE, FileColumnType::VARCHAR};
    check(jdata.columns_.size() == 6, "jsonl: 6 union columns");
    for (size_t i = 0; i < jdata.columns_.size() && i < 6; ++i) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "jsonl column %zu is %s", i, jnames[i]);
      check(jdata.columns_[i].name_ == jnames[i] && jdata.columns_[i].type_ == jtypes[i], buf);
    }
    check(jdata.rows_.size() == 3, "jsonl: 3 rows");
    if (jdata.rows_.size() == 3 && jdata.columns_.size() == 6) {
      check(jdata.rows_[0][4].is_null_ && jdata.rows_[0][5].is_null_, "jsonl: missing keys are NULL");
      check(jdata.rows_[1][2].is_null_, "jsonl: missing score is NULL");
      check(jdata.rows_[1][4].value_ == "2026-08-02", "jsonl: day string kept");
      check(jdata.rows_[2][2].value_ == "90", "jsonl: integer 90 kept as text");
      check(jdata.rows_[2][5].value_ == "ok", "jsonl: note value");
    }
  }

  // ---- JSON array input: DuckDB's read_json also accepts [{...},{...}] ----
  {
    const char *arr = "[{\"id\":1,\"name\":\"a\"},{\"id\":2,\"name\":\"b\"}]";
    FileTableData adata;
    std::string aerr;
    const int aret = parse_jsonl(arr, static_cast<int64_t>(std::strlen(arr)), adata, aerr);
    check(0 == aret, "json array parses");
    check(adata.columns_.size() == 2, "json array: 2 columns");
    check(adata.rows_.size() == 2, "json array: 2 rows");
    if (adata.rows_.size() == 2 && adata.rows_[1].size() == 2) {
      check(adata.rows_[0][0].value_ == "1", "json array: first id");
      check(adata.rows_[1][1].value_ == "b", "json array: second name");
    }
    // commas inside strings must not split objects
    const char *quoted = "[{\"id\":1,\"n\":\"x,y\"}]";
    FileTableData qdata;
    std::string qerr;
    const int qret = parse_jsonl(quoted, static_cast<int64_t>(std::strlen(quoted)), qdata, qerr);
    check(0 == qret && qdata.rows_.size() == 1
              && qdata.rows_[0][1].value_ == "x,y",
          "json array: comma inside a string kept");
    // pretty-printed arrays (newlines inside the array) are flattened too
    const char *pretty = "[\n  {\"id\":1},\n  {\"id\":2}\n]";
    FileTableData pdata;
    std::string perr;
    const int pret = parse_jsonl(pretty, static_cast<int64_t>(std::strlen(pretty)), pdata, perr);
    check(0 == pret && pdata.rows_.size() == 2, "pretty-printed json array parses");
  }

  // ---- glob(): one column of full file paths, files only ----
  const char *glob_dir = argc > 3 ? argv[3] : "/root/fts-run/data_for_mysqltest_file_sql";
  std::string pattern = std::string(glob_dir) + "/*";
  std::vector<std::string> files;
  std::string gerr;
  int gret = list_glob(pattern.c_str(), files, gerr);
  check(gret == 0, "list_glob returns success");
  if (gret != 0) {
    std::printf("glob error: %s\n", gerr.c_str());
  } else {
    std::printf("\n--- glob(%s) ---\n", pattern.c_str());
    std::set<std::string> got;
    for (size_t i = 0; i < files.size(); ++i) {
      std::printf("%s\n", files[i].c_str());
      size_t slash = files[i].find_last_of('/');
      got.insert(files[i].substr(slash + 1));
    }
    check(files.size() >= 4, "glob finds the staged files");
    check(got.count("basic.csv") == 1 && got.count("basic.jsonl") == 1
              && got.count("invalid.txt") == 1 && got.count("simple.parquet") == 1,
          "glob returns the expected file set");
    check(files[0].find('/') != std::string::npos, "glob returns full paths");
  }

  std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures,
              failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
