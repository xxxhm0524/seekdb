# Full-text hot-path optimization

## Scope and bottleneck evidence

Changes cover the IK character context and processor dispatch, and the shared
text token stream. The benchmark inputs, SQL,
timers, scoring program and frozen baseline are unchanged.

Source inspection identified the following repeated work. No profiling or
runtime measurement was performed.

| Path | Original work | Candidate work |
| --- | --- | --- |
| IK character classification | Each unsuccessful classification predicate decodes the same character again | Decode once, then apply the same predicates in the same order |
| Whitespace tokenizer delimiter check | Three predicates consult the same charset byte table | One lookup and the union of the same bit masks |
| Punctuation/control-only token filter | Count special bytes across the entire token | Stop when an ordinary byte establishes validity |
| English boundary trimming | Scan pure non-alnum tokens forwards and backwards | Forward scan rejects them; backward scan is only needed for valid tokens |

For UTF-8, the original IK path uses one decode for English letters, two for
digits, three for Chinese characters and four for remaining categories.
The candidate uses one decode per classification. UTF-16 surrogate categories
also retain their original ordering and charset restrictions.

These are reductions in source-level work, not measured speedup percentages.
Index building and query-term analysis can benefit from the same tokenizer
changes. Sorting, posting-list traversal, execution plans, dictionary data,
stop-word rules, case conversion and token storage ownership are unchanged.
Queries dominated by SQL overhead or posting-list access may see little benefit.

## Correctness reasoning

IK uses the existing decoder and Unicode classification functions. A decoder
failure is returned before classification, as before. Surrogate predicates
remain applicable only to UTF-16 and UTF-16LE.

The byte-table expressions preserve the existing charset macros' unsigned-byte
indexing and null-table behavior. Delimiters still mean space OR control OR
punctuation. Token filtering still rejects exactly tokens whose every byte is
punctuation or control; empty tokens remain invalid. English normalization still
uses the original charset-aware alnum predicate and lowercasing function.

## Second revision after platform feedback

The user reported successful platform execution of commit
`9f59c0b5e702e11e74f50fb95027b7be0b3fcdb7`, with a displayed score of 7.821.
The individual timing metrics were not supplied, so this is not evidence of a
particular bottleneck or a measured percentage improvement.

This revision adds three source-level optimizations:

* The IK parser passes its already validated character, length and type directly
  to the processors in the same order. Each processor still executes; the
  redundant context getter calls are removed. Existing processor implementations
  do not move the context cursor while processing a character.
* UTF-8 ASCII bytes bypass multibyte-length and Unicode conversion functions.
  IK classifies ASCII using the exact English/digit ranges from the existing
  Unicode predicates. Tokenizer delimiter classification is unchanged. Non-ASCII
  and other encodings use the original validating functions, including malformed
  byte handling. The UTF-8 eligibility flag is cached per context/tokenizer.
* In UTF-8, a token containing only ASCII bytes and no uppercase ASCII letter is
  already lowercase. The English normalizer borrows that input slice instead of
  allocating and converting a second copy. The BENG parser retains its owned
  token copy, and the grouping normalizer copies new keys, preserving lifetimes.
  Uppercase or non-ASCII tokens still use charset-aware conversion.

No caches of SQL results, input-specific rules, dictionary changes or query-plan
shortcuts are introduced. This revision is uncompiled and untested at the user's
request. It does not establish a 50% end-to-end time reduction. Nine per-metric
timings and correctness results from platform evaluation remain necessary to
determine whether further work should target build or query execution instead.

## Reproduction

Build parent and candidate with identical compiler options and run each on the
same dedicated instance configuration. The public SQL examples in
`fts-performance-regression.sql` can be run on both versions and their complete
outputs compared; they are supplied but have not been executed. Also run the
existing `fts_index` regression suite, including `tokenize_function.test`,
`parser_properties.test`, `cn_word.test` and `simple_query.test`.

For performance, run the official unmodified benchmark on both versions:

```bash
export MYSQL='mysql -h127.0.0.1 -P2881 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2881 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

The benchmark recreates its own `fts_large_bench` database. Use a new report file
for each run. Retain all nine scored metrics and compare complete token outputs,
MATCH result IDs and ordering, not only hit counts.

## Performance report status

At the user's request, compilation, regressions and performance tests were not
run. The following measurements remain to be supplied by CI/evaluation:

| Category | Required metrics | Before | After |
| --- | --- | --- | --- |
| Build | build_ik_all_sec, build_ik_content_sec, build_beng_en_sec | Unmeasured | Unmeasured |
| Tokenize | tokenize_ik_avg_ms, tokenize_beng_avg_ms | Unmeasured | Unmeasured |
| Query | query_cn_avg_ms, query_beng_avg_ms, query_mixed_avg_ms, query_limit_avg_ms | Unmeasured | Unmeasured |

No runtime correctness result, measured performance gain or contest score is
claimed. Git whitespace checks and source inspection do not replace compilation
or correctness tests. Parent-versus-candidate timing and the official score are
outstanding until the unified evaluation runs.
