# File-table readers (competition task 2) — standalone, tested

These files implement the two hardest pure-logic pieces of `read_csv` / `read_json`:

| file | purpose |
|---|---|
| `ob_file_table_csv.h/.cpp` | RFC4180 CSV reader, newline-delimited JSON reader, deterministic type inference (`BIGINT/DOUBLE/BOOLEAN/DATE/DATETIME/VARCHAR`), DuckDB-style duplicate column renaming (`id__2`), single-level `glob()` expansion |
| `test_file_table_csv.cpp` | unit driver; asserts the exact schema/rows of `file_sql_basic.result` |

They deliberately have **no oceanbase dependencies** (only `<string>/<vector>`), so they can be
compiled and tested in seconds without rebuilding the observer:

```bash
g++ -std=c++20 -O1 -g -Wall -Wextra -o test_csv test_file_table_csv.cpp ob_file_table_csv.cpp -I.
./test_csv                                  # uses the staged fixtures by default
./test_csv /path/basic.csv /path/basic.jsonl
```

Fixtures are staged by `tools`-less helper (see the progress doc):
`/root/fts-run/data_for_mysqltest_file_sql/{basic.csv,basic.jsonl,simple.parquet}`.

Current status: **ALL PASS** — the parsed schema and rows match
`mysql_test/test_suite/file_sql/r/mysql/file_sql_basic.result` exactly for both CSV and JSONL
(including multiline quoted fields, `""` unescaping, empty→NULL, duplicate header `id__2`,
JSONL key-union with missing→NULL, and `int`+`float` widening to `DOUBLE`).

## Integration notes (next step)

- Move `ob_file_table_csv.*` to `src/sql/engine/basic/` and register them in
  `src/sql/sql_source_inventory.bzl` (see `tools/module_check/sql_source_ownership_check.py`).
- The resolver should build `ObValuesTableDef` rows from `FileTableData` (see progress doc §2.2).
- Remaining conversions for exact output: `BOOLEAN` must render as `1/0` and `DATETIME` as
  `YYYY-MM-DD HH:MM:SS` (the ISO `T` form in the input is accepted at parse time).
