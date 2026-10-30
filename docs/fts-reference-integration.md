# FTS reference integration (2026-10-31)

## Source and attribution

This increment adapts selected Apache-2.0 code from juanzhi-gcb/seekdb commit
[6062a1eccc732b9dfbd67f477c0e34f33fd092bc](https://github.com/juanzhi-gcb/seekdb/commit/6062a1eccc732b9dfbd67f477c0e34f33fd092bc),
published in [oceanbase/seekdb PR #1787](https://github.com/oceanbase/seekdb/pull/1787).
Original license headers are retained. The author reports a platform result of
47.647/50 for that original commit. This is not a score for this adapted branch.

## Incorporated modules

* Detect natural-language MATCH uses whose numerical score is not observable.
  Keep real scores for projections, numeric thresholds, Boolean/boosted matching,
  function lookups and unsupported shapes. Preserve prepared-parameter constraints.
* Complete the score-free retrieval path across code generation, legacy/DAS
  iterators, corpus-statistics estimation and DAAT/TAAT projection. Read document
  IDs without decoding BM25-only posting columns when scores are unnecessary.
* Merge small score-free posting unions through buffered document-ID cursors.
  Other modes, filters, top-k and larger unions retain the general implementation.
* Remove unobservable implicit relevance ordering from qualifying count-only
  derived tables, and push limits only through eligible unfiltered local scans.
* Generate FTS build rows in document-bounded batches, replicating only batch
  expressions with batch storage and preserving scalar slots.
* Reuse segmentation results through a sharded, locked process cache and reuse
  packed TOKENIZE JSON in a small thread-local memo. Hashes are verified against
  full input bytes and configuration before a hit is accepted.
* Use the ordinary parser defaults without a JSON reconstruction round trip when
  no properties were provided. Preserve property validation for explicit input.

## Local adaptations

The existing binary-ID comparator and batch evaluated-bit updates are retained.
Scalar score-free projection now avoids dereferencing an absent relevance
expression. The original out-of-line ObDocIdExt getter is retained.
The original dictionary-loading lifecycle checks are retained, and a generation
counter invalidates thread-local JSON memo entries when parser data is restarted.

Cache payload is limited to 64 KiB per entry and 256 KiB per shard (256 shards,
64 MiB serialized payload in total; allocator/hash bookkeeping is additional).
Oversized entries use ordinary segmentation. Shard reclamation happens under
its write lock; replayed words own their bytes independently of cache eviction.
Cache initialization or lookup/store failure does not replace a successful
ordinary parsing result with an error.

The segmentation cache accepts only built-in parser identities and built-in
dictionary bindings. The JSON memo accepts only built-in names without explicit
parser-property bindings. Entries also record the destination word-map bucket
count. Different layouts miss the cache; reverse-order record serialization
reconstructs the original within-bucket iteration order during replay, because
the existing map inserts each new node at the bucket head.

This is a selective integration, not a replacement with the reference tree.
Global charset-comparison changes, shared sort changes, dictionary-hub rewrites,
DDL parallelism changes and unrelated contest features were not imported.
Benchmark inputs, benchmark SQL, timers, scoring script, baseline and CI workflow
are unchanged.

## Validation scope

The user requested no tests. The reference regression SQL and expected output
are included as source coverage, but have not been executed on this adaptation.
No observer startup, SQL regression or performance benchmark is claimed.
Static diff checks and the incremental release compile/link succeeded using
the existing native WSL build tree with at most two compile tasks. The final
build returned exit code 0 (`cmake --build build_release --target seekdb -j2`).
This was not a clean build of the entire submitted commit. It does not establish
runtime correctness, current platform acceptance, the reference author's score
or a 50% improvement.
