# Full-text hot-path optimization

## Latest increment: filter dispatch and batch frame preparation

The latest increment bypasses the per-document filter call when the merge has
no filter expression, and prepares batch-result datum buffers once per batch
using the existing `locate_datums_for_update` API. Non-batch expressions keep
their original per-row path. Each datum's writable result pointer is still
restored before assignment, including shallow document IDs.

Validation for this increment was limited, at the user's request, to source
inspection, `git diff --check`, and a successful incremental release compile/link
(24.44 seconds, at most two compile tasks). No SQL regression or performance
benchmark was run for this increment. Earlier measured reductions below apply
to the earlier candidate only; they do not measure this change or explain the
latest platform zero score. A 50% overall reduction remains unproven.

## Current candidate: conservative parser and storage DAAT projection

The parser/dictionary source retains commit
`9f59c0b5e702e11e74f50fb95027b7be0b3fcdb7`, the user's first scored candidate.
Subsequent speculative fast paths, character caches, chain-weight caches and
dictionary lookup changes are withdrawn. Their descriptions below are historical
development notes and do not describe the current source.

After the user authorized bounded local testing, the FTS child-completion reuse
change was also withdrawn: the A/B/B/A runs showed essentially unchanged build
time. A separate stopword/hash update experiment showed no reliable aggregate
benefit and was not submitted. Differential tests caught and helped correct a
representative-word difference for accent-insensitive keys in that experiment;
the entire experiment was subsequently withdrawn rather than stacked onto the
candidate.

The latest increment additionally optimizes binary document-ID comparison and
batch projection in the storage DAAT iterator identified by local CPU sampling.
Its query-only cross-over runs observed a 7.47% query-category time reduction;
the full-rebuild pair observed 2.89%, with a slight LIMIT regression. These
measurements do not establish a 50% overall gain. See the
[DAAT validation report](fts-daat-validation.md) for raw results, regression
coverage and limitations.

Incremental compilation and local functional/performance validation have
now been performed using the existing WSL build tree. This was not a clean build
of the entire submission commit, and local results do not establish the platform
score. See [the local validation report](fts-local-validation.md) for measured
results and scope of the prior parser experiments. The fixed benchmark and
scoring files remain unchanged.

## Development history (earlier candidates, partly withdrawn)

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

## Third revision: dictionary lookup

With no per-metric report available, further source inspection identified a
linear scan of all dictionary ranges for every initial-character match. The
candidate validates range ordering and strict non-overlap using the original
UTF8MB4_BIN comparator when loading the per-parser dictionary. It then finds
the first range whose end is at least the input using binary search, followed by
the original inclusive start check. Unsorted, overlapping or invalid ranges keep
the original first-match linear scan. There is no reordering, memoization of
query results, mutable shared lookup state or change to dictionary contents.

The DAT character-code hash map also used integer remainder at every collision
step. With capacity at least three, adding three requires at most one wraparound
subtraction. Lookup and insertion now both use that equivalent probe sequence;
smaller capacities retain the remainder path. Hash functions, equality, table
layout and persisted dictionary format are unchanged.

These paths serve IK parsing for documents and query terms. Their benefit depends
on range count and collisions, and range validation adds initialization work.
No whole-query runtime reduction can be deduced from these complexity changes.
Compilation, correctness testing and timing are still intentionally unexecuted.
The 50% aggregate target remains unverified.

## Fourth revision: reuse within IK arbitration

Source inspection of `ObIKArbitrator::output_result` showed a second character
validation/classification pass after processor traversal. The token context now
keeps one packed metadata byte per source-byte offset, limited to the first
64 KiB. Only successful decodes with representable lengths are cached; invalid
input is not cached. Larger offsets and allocation failures use the original
decoder. The cache belongs to one immutable input context, survives only its
internal batches and is freed with that context. Token selection, compound
handling and output positions remain in the existing arbitration code.

Candidate-chain comparison repeatedly traversed the same best chain to calculate
x/p weights. The chain now computes each weight lazily and reuses it until a
mutation. Add, pop and mutable-list access invalidate the weights. The original
separate formulas and comparison order are retained, so unused weights are not
calculated speculatively. This is local intermediate-state reuse, not a cache of
TOKENIZE/MATCH results or persistent dictionary data.

The supplied platform reports showed BENG build time changing from 2.621 s to
6.375 s, while both IK builds, both tokenize metrics and all query metrics were
slightly faster. No commit IDs were included in those reports. The corresponding
aggregate improvements were +0.0782091784 and -0.0431528858. These observations
do not distinguish DDL scheduling variation from a source regression. The local
checked-in baseline also differs from the baseline implied by platform scores;
it has not been changed. No measurements exist for this fourth revision.

The DAAT result merger was inspected, including its one-row cache interface,
loser-tree merging and boolean filtering. It was not modified: batching across
that interface requires preserving per-token exhaustion, duplicate-document
merging, filter evaluation and LIMIT behavior. No query correctness or speedup
claim is made for an unimplemented change. No compilation or tests were run.

## Fifth revision: DAAT cached-row advancement and token grouping

The DAAT batch merger requests one row per token iterator through
`ObDASIter::get_next_rows`, even when `ObDASTRCacheIter` already holds the next
materialized row. A narrow fast path now advances only that cached cursor when
both initialization and token document-count aggregation are complete. The first
row, cache boundaries and uncompleted aggregates keep the original interface.
Result extraction, loser-tree insertion/rebuild, per-document coalescing, boolean
evaluation, relevance projection and LIMIT handling are unchanged. No disk reads
or candidate documents are skipped. This reduces call overhead within an existing
batch; it does not implement a new multi-document merge algorithm.

Token grouping previously looked up a key to retrieve its frequency, then looked
up the same key again to update it. The hash table's existing atomic callback now
updates frequency in the first lookup. Missing keys still take the owned-string
insertion path; lookup failures are still propagated. The grouping map belongs to
one analyzer and is not shared across queries.

No compilation or tests were run. Evaluation should cover single and multiple
query tokens, cache-boundary sizes, partial final batches, exhausted streams,
overlapping postings, boolean filters, relevancy and LIMIT/OFFSET. In particular,
cached advancement is not a substitute for checking returned IDs and complete
query results against the original version. No measured speedup is claimed.

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
