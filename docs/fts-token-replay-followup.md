# FTS token-cache replay follow-up

Parent: d998e5686e4afb5393fa4109e09c9d8a93e1c6e4. The user reports 43/50 for
that parent; no per-metric platform report has been supplied.

Source inspection found one allocation per distinct token on a shared-cache hit.
This increment records total word bytes while creating each in-process cache
entry, then allocates one caller-owned contiguous word buffer per nonempty hit.
Replay order, token bytes, counts, collation, dictionary eligibility and cache
locking are preserved. An empty token map requires no word-buffer allocation.
The allocation failure still falls back through the existing ordinary parser
path; no returned token points into evictable cache memory.

The added byte count is in process memory only, not in an on-disk format.
Each cache entry grows by eight bytes, within the existing payload cap; eviction
boundaries can therefore differ slightly. This reduces allocation calls on hits
but has no measured end-to-end speedup claim and may have negligible impact if
cache misses, SQL overhead or index I/O dominate.

Validation: diff whitespace checks and incremental release compilation/linking
with two compile tasks. No SQL tests or performance benchmarks were run for this
increment. Benchmark inputs, SQL, timing, baseline and scoring are unchanged.
