# Storage DAAT optimization and local validation (2026-10-31)

## Evidence and change

An eight-second CPU-clock/IP sample of repeated English full-text queries found
the active merge implementation in `ObSRDaaTIterImpl`, including ID comparison,
filtering, bitmap operations and result projection. This is a storage iterator;
optimizing only the separate DAS merge implementation would miss this workload.
The short sample included background threads and had no call stacks, so it is
evidence of the executed path, not an exclusive-time breakdown.

This change makes two local optimizations in `ob_sparse_daat_iter.cpp`:

* Compare equal-length, non-NULL binary VARCHAR document IDs with `memcmp`.
  The binary charset implementation itself uses this comparison. NULLs,
  unequal lengths and all other metadata retain their original comparator.
* Mark the contiguous projected batch evaluated once, after filling its datums,
  instead of setting an individual bitmap bit and expression flag for every row.
  The range operation preserves bits outside the projected range. Scalar
  projection and relevance calculation are unchanged.

There is no result cache, skipped posting, tokenizer change, scoring-rule change,
DDL scheduling change, benchmark edit or baseline edit in this increment.

## Query-only cross-over measurements

Same WSL release environment, local Unix socket, same 20,000-row indexed data,
before/after/after/before order. Each run used 200 query repetitions, three
samples and 30 warmups, with the unmodified benchmark. `SKIP_LOAD=1` deliberately
isolated querying; these runs cannot produce a valid full competition score.

| Query | Before mean (ms) | After mean (ms) | Observed time reduction |
| --- | ---: | ---: | ---: |
| Chinese | 1.34475 | 1.23500 | 8.16% |
| English | 1.90925 | 1.75325 | 8.17% |
| Mixed | 1.52965 | 1.39715 | 8.66% |
| LIMIT | 1.75450 | 1.66855 | 4.90% |

The query-category mean reduction was 7.47%. SELECT 1 also varied (before mean
0.07345 ms, after 0.06960 ms), so some reduction may be environment noise.
No 50% overall improvement is established.

## Full rebuild cross-check

One additional before/after pair ran the unmodified full benchmark with
`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`.

| Metric | Before | After |
| --- | ---: | ---: |
| IK title/content build (s) | 7.478 | 6.769 |
| IK content build (s) | 5.746 | 5.238 |
| BENG build (s) | 2.937 | 2.939 |
| IK tokenize (ms) | 0.1505 | 0.1471 |
| BENG tokenize (ms) | 0.1029 | 0.1057 |
| Chinese query (ms) | 1.3140 | 1.2216 |
| English query (ms) | 1.9678 | 1.9087 |
| Mixed query (ms) | 1.4819 | 1.4555 |
| LIMIT query (ms) | 1.7587 | 1.7630 |

LIMIT and BENG tokenization regressed slightly in this pair. Build and tokenizer
paths were not changed; one pair cannot establish causation or a reliable gain
for those categories. The query-category mean reduction in this pair was 2.89%.
Raw reports for both experiments are in `fts-daat-results.json`; no official
score is inferred from the local measurements.

## Correctness and scope

All four query-only runs produced identical complete outputs (169,619 bytes,
SHA-256 `37ec1032844107c5bcc9ae38e6a27baca9a8a0a178bcb02ce9613bfb6cfbf80d`).
Coverage included sorted matching IDs for all four workloads, exact top-40
BM25 scores, Boolean exclusion, LIMIT/OFFSET around 64/128/256 boundaries,
and the existing tokenizer differential inputs. Three invalid tokenizer cases
failed identically in both executables; they are not counted as passing inputs.

Separately reloaded indexes had small BM25 differences. Re-running the original
executable against the candidate's rebuilt data reproduced the candidate's
exact scores and ordering, confirming the discrepancy was between rebuilds,
not between executables reading the same index.

The new `fts_index/daat_projection.test` covers 512 documents, merged postings,
Boolean exclusion, score filtering, empty matches, bitmap-boundary LIMITs and
correlated rescans. Its SQL succeeded on both executables with identical
results. Expected output was recorded from the original executable. These SQL
statements were executed directly; the mysqltest harness itself was not run.

The candidate compiled and linked successfully with at most two compile tasks,
reusing the existing native release build. This was not a clean whole-repository
build, and other sparse-retrieval users/vector-index suites were not run. No
cloud/GPU resources were used; the local instance was stopped after validation.
