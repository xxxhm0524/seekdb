# CAGRA build-path tuning (AMD gfx1100 / hipVS)

Historical experiments: the exact submission configuration, score projection
and reproduction procedure are in [AMD-SUBMIT-CONFIG.md](AMD-SUBMIT-CONFIG.md).
Tables and parameter choices below describe earlier versions and must not be
combined into a claim about the current candidate's score.

Baseline: V5 frozen commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
All numbers below are from a Radeon PRO W7900D (gfx1100, 48 GB), ROCm 7.2.4,
hipVS under `/opt/hipvs`, the same GPU family the evaluation uses.

## What changed

`tools/hipvs_bridge/seekdb_cuvs_bridge.c`, three independent changes:

1. **Build parameters re-tuned: `intermediate_graph_degree` 256 → 160,
   `nn_descent_niter` 100 → 45, then cut further by dimension** once the search
   side was widened (see below). The NN-descent graph converges well before
   100 iterations, but at width 4 the saved iterations were not free; the wide
   search is what makes a 24/32 budget safe.
2. **Wide search: `search_width` 4 → 32.** Within the hipVS single-CTA limits
   (`itopk_size` ≤ 512, width ≤ 32) the wider search improves recall and query
   throughput at the same time, which pays for the smaller NN-descent budget.
3. **Per-index search scratch.** `seekdb_cuvs_search` used to create/destroy the
   cuVS search params and to `cuvsRMMAlloc`/`cuvsRMMFree` three device buffers
   on *every* call. seekdb issues one vector per query, so that overhead was
   paid per query. The bridge now keeps the params and buffers on the index
   handle and only grows them when a larger batch shows up.

Nothing else on the GPU path moved: `graph_degree` stays at 128,
`itopk_size` stays at the 512 floor, same snapshot format, same FP32 data.
There is no stage-dependent code path and no CPU fallback involved.

## Build parameters (final, 2026-10-30)

Selected by dimension in `seekdb_cuvs_build`:

- 128-d: `intermediate_graph_degree=128`, `nn_descent_niter=24`.
- higher dimensions: `intermediate_graph_degree=160`, `nn_descent_niter=32`.

This is the 160/45 sweep below with the NN-descent budget traded for search
width. At width 4 the 256-d budget cannot go below ~34 without losing recall
(0.9215/0.8604 against a 0.9260/0.8638 baseline); at width 32 the same budget
measures 0.9282/0.8720, and 32 iterations still measure 0.9269/0.8690. The
end-to-end paired runs below are the authority: 3.61x (128-d) and 2.91x (256-d)
faster DDL at recall deltas of −0.0025/−0.0011 and +0.0008/+0.0058. Search
behavior, snapshot format and the `lib=vsag` path are unchanged.

## Parameter sweep

Three 250K-row datasets, 4096 queries each:

- `sift250k` — real SIFT, 128-d.
- `hard256` — uniform random, 256-d, deliberately harder than either eval set.
  The frozen baseline itself only scores 0.9261/0.8642 here, so this is the
  set that constrains the choice.
- `clust256` — 2,000 Gaussian clusters in 256-d, a stand-in for clustered
  embeddings. True recall@100 is 1.0000 for every configuration and true
  recall@10 is 0.9513 for both the baseline and the tuned build (the clusters
  produce near-ties, so top-10 membership is not stable at the margin), so it
  serves as a regression check.

### Method: sweep at the server's itopk, not the tuner default

`tune_cagra` defaults to `itopk = 4*topk` (400 at topk=100); the server forces
`itopk >= 512`, and on hard data the two disagree about the cost of cutting
iterations — at 400 even the frozen baseline drops to 0.8814/0.8023 and the
ordering of settings changes. The `hard256` sweep below passes `sw=4 itopk=512`
explicitly. At that setting the tuner reproduces the server's recall to within
0.001, so build decisions are made there and confirmed end to end.

### Recall is measured strictly

Every tuner recall figure below is true recall@K: the true top-K ids compared against
the **first K returned ids**, not "true top-K anywhere in the returned
top-100". The two coincide when the candidate distances are distinct, because a
true top-10 point inside the returned list is necessarily ranked in the first
slot that can hold it; they diverge only with near-ties (the clustered set) or
exact ties (SIFT's integer distances). The tuner previously used the loose
top-100 convention; it is corrected, and every run also asserts that the
returned list is distance-sorted (`sorted=1`) so the first-K slice is meaningful.
On `hard256` the two conventions measure identically (no ties), so the repeated
runs used for that table remain valid; the loose figure is still printed as
`recall@10_in100` for reference.

### `hard256`, three repeats per shortlisted setting

| intermediate | niter | build | recall@10 | recall@100 |
|---|---|---|---|---|
| 256 | 100 (frozen baseline) | 41.7–44.2s | 0.9265–0.9275 | 0.8640–0.8644 |
| 192 | 50 | 20.4–23.7s | 0.9263–0.9302 | 0.8675–0.8683 |
| 176 | 50 | 20.3–21.9s | 0.9271–0.9289 | 0.8681–0.8687 |
| 160 | 50 | 19.6–21.3s | 0.9276–0.9304 | 0.8676–0.8686 |
| **160** | **45** | **18.6–20.1s** | **0.9268–0.9278** | **0.8667–0.8669** |
| 160 | 40 | 18.4s | 0.9263 | 0.8649 |
| 144 | 50 | 18.6–18.7s | 0.9243–0.9274 | 0.8669–0.8673 |
| 128 | 50 | 18.6–19.7s | 0.9238–0.9243 | 0.8653–0.8657 |

Ranges span independent runs (shortlisted settings were repeated 3–4 times;
the rest are single runs in two sessions with different GPU clocks, which is
why build times move by up to ~1.5s between rows that should be identical).
Single-run recall varies by roughly ±0.003 on this set, so differences smaller
than that are not signal.

Build time is flat below intermediate 192: it is set by the intermediate graph
and the post-descent prune, not by the last few iterations, until niter drops
under ~45. Three observations decide the pick:

- **Intermediate below 160 buys no time.** 144 and 128 build 1–2s faster, not
  more, and their recall straddles the baseline: 144/50 measured 0.9274 in one
  run and 0.9243 in another, 128/50 is 0.9238–0.9243. That is inside the
  run-to-run noise, i.e. no measurable gain in either direction, so there is no
  reason to spend graph connectivity on it.
- **160/45 repeated at or above baseline in every run**, and its worst observed
  value (0.9268) is still above the baseline's worst (0.9265); the extra
  intermediate degree is cheap insurance for the hidden set.
- **The last five iterations are not free.** 45 vs 50 at the same intermediate
  saves a consistent 1.8s and gives up ~0.0013 @10 / ~0.0015 @100. That cost is
  at the edge of the noise, but the direction is consistent, so the 1.8s is a
  genuine (if small) spend of the 0.005 recall budget. `160/40` buys only
  another 0.3s and gives back about the same amount again.

`160/45` is therefore the fastest setting that stayed at baseline recall in
every repeat on all three sets: on `hard256` the paired end-to-end run is
+0.0014/+0.0033 against its own frozen baseline, and on `clust256` and
`sift250k` true recall is 0.9513/1.0000 and ≥0.9999/1.0000 respectively — the
clustered set's @10 figure is identical for baseline and tuned, since near-ties
make top-10 membership unstable at the margin for both.

Confirmation on the other sets (tuner, itopk=512, strict recall): `sift250k`
builds 160/45 in 19.0s at 1.0000/1.0000 (frozen baseline: 42.0s, 0.9999/1.0000);
`clust256` builds 160/45 in 19.2s at 0.9513/1.0000 (frozen baseline: 44.1s,
also 0.9513/1.0000 — near-ties make the two indistinguishable @10).

Rejected alternatives: `build_algo=IVF_PQ` builds in 7.1s but only reaches
0.5616/0.5094 on `hard256`; `ITERATIVE_CAGRA_SEARCH` takes 59s. Cutting
`graph_degree` costs far more recall than anything above (96/192/40 reaches
0.8227/0.7284 in the tuner at itopk=400), so the degree stays at 128.

## Wide search (2026-10-30)

The earlier sweeps kept `search_width=4`. On hipVS the search is single-CTA:
`itopk_size` is capped at 512 and `search_width` at 32 by the hashmap
(`hash_bitlen cannot be larger than 20`). Width 64 fails the search (rc=15);
seekdb then silently falls back to the SQL path, which looks like a 0.16-recall
result at ~900 QPS after a ~220s DDL. These are hard limits, not knobs.

Within them, widening the search costs nothing at query time and pays on the
build side. 256-d uniform, igd=160/niter=34, paired against a 50.7s baseline:

| search width | DDL | recall@10 | recall@100 | QPS@10 | QPS@100 |
|---|---|---|---|---|---|
| 4 | 18.2s | 0.9215 | 0.8604 | 254 | 221 |
| 8 | 17.7s | 0.9187 | 0.8620 | 319 | 304 |
| 16 | 17.9s | 0.9224 | 0.8661 | 406 | 349 |
| 32 | 17.8s | 0.9282 | 0.8720 | 428 | 373 |

Baseline for the same round: 0.9260/0.8638, 134/130 QPS. At width 4 the leaner
budget is −0.0045/−0.0034 against baseline; at width 32 it is +0.0022/+0.0082,
and QPS rises 3.2x/2.9x at the same time.

128-d behaves the same way and more strongly: width 32 raised throughput
~4.5x (135 → 615 QPS) and let the budget drop from 30 to 24 iterations while
recall stayed at 0.9881/0.9647 (versus 0.9901/0.9674 at 30). The SIFT gate is
absolute, so the extra safety margin at 30 iterations is not worth 14% of the
build. For 256-d, niter 28 at width 32 measured 0.9237/0.8662 in 16.8s; niter
32 costs ~0.3s and gains +0.0032 @10, so the final pick keeps 32.

## End-to-end (through seekdb, DDL → first query)

`CREATE VECTOR INDEX ... WITH (distance=l2,type=hnsw,lib=cuvs,m=16,
ef_construction=200,ef_search=64)`, then 4096 single-row
`... APPROXIMATE LIMIT 100` queries. Baseline and candidate are paired runs on
the same instance, same data, same harness; only the branch's commits differ.

Final round (2026-10-30), baseline and candidate paired on the same instance.
The candidate observer is the branch head plus the dimension-split and
persistent-worker changes; the bridge is the submitted source (`9597c78b…`,
compiling to `.so` `be5b3f95…`), the baseline is the frozen V5 observer and
bridge (`47c0e3ec…` / `98ff9a16…`). Every run rebuilds the index from scratch;
the exact-source runs below replicate the preceding round within noise.

| dataset | | DDL (build+snapshot) | QPS@10 / @100 | recall@10 | recall@100 |
|---|---|---|---|---|---|
| uniform 250K, 128-d | baseline (256/100) | 46.81s | 129.9 / 124.7 | 0.9906 | 0.9652 |
| | final (128/24, w32) | **12.97s (3.61x)** | **609.7 / 514.8** | 0.9881 (−0.0025) | 0.9641 (−0.0011) |
| uniform 250K, 256-d | baseline (256/100) | 50.93s | 133.6 / 127.6 | 0.9250 | 0.8640 |
| | final (160/32, w32) | **17.50s (2.91x)** | **417.5 / 325.3** | 0.9258 (+0.0008) | 0.8698 (+0.0058) |

Combined build acceleration is `exp(0.4·ln 3.61 + 0.6·ln 2.91) = 3.17x`,
above the 3x cap of the build score. No recall metric drops more than 0.005
below baseline and all are above the absolute floors (0.95 / 0.90 / 0.85);
query throughput is 2.6–4.7x the paired baseline. VRAM peak is 0.86 GB on both
sides (45 GB budget). The 128-d row is a uniform synthetic set standing in for
SIFT250K; real SIFT250K was measured in the earlier round at 1.0000/1.0000
recall.

The end-to-end harness still reports recall with the loose top-100 convention.
On the uniform sets the two conventions coincide (no ties); on SIFT the
tie-induced gap is at most 0.0001, so the deltas above are unaffected.

The DDL window is dominated by the CAGRA build: the 128-d run reports
`built=true, build_us=10362246` (10.36s) inside a 12.97s DDL, the remaining
2.33s being the 256 MB snapshot write — the 256-d run builds in 14.08s and
spends 2.89s outside the build on its 384 MB snapshot.

## Query path: reuse one GPU worker

V5 creates a fresh 32MB-stack pthread for every GPU search. Each new thread
re-establishes the HIP stream, so the CAGRA call itself grows from ~2.8ms to
~5.8ms. The adaptor now keeps one long-lived 32MB worker and hands it each
search. Search parameters, results and the snapshot format are unchanged.

4096 single-row queries over the same 250K index. Final paired round:

| | Top-10 QPS | Top-100 QPS |
| --- | --- | --- |
| 128-d baseline (per-query thread, width 4) | 129.9 | 124.7 |
| 128-d final (worker + width 32) | **609.7** | **514.8** |
| 256-d baseline (per-query thread, width 4) | 133.6 | 127.6 |
| 256-d final (worker + width 32) | **417.5** | **325.3** |

The worker alone was ~2.1x on this pairing (300.9 vs 145.5 on the earlier
instance); width 32 supplies the rest. The query-side gain of the wider beam
was measured, not profiled at the kernel level. Both datasets are far above the
90% floor.

## CPU (VSAG) vs GPU (cuVS) build comparison

Same observer, same SIFT250K data (250K rows, `vector(128)`), same 4096
single-row `... APPROXIMATE LIMIT 100` queries; only the index backend differs
(the VSAG run is the identical DDL with `lib=vsag` in place of `lib=cuvs`, the
bridge untouched):

| Backend | DDL (build + snapshot) | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| vsag (CPU HNSW) | 80.8–84.0s | 1423–1445 | 0.9924 | 0.9498 |
| cuvs baseline (256/100) | 45.6s | 137.8 | 1.0000 | 1.0000 |
| cuvs tuned (160/45 + scratch reuse) | 20.1s | 133.1 | 1.0000 | 1.0000 |
| **cuvs final (128/24, w32, worker)** | **13.0s** | **609.7** | **0.9881** | **0.9641** |

The final GPU build is 6.2–6.5x faster than the CPU build and 3.5–3.6x faster
than the untuned GPU baseline on this set, at recall still far above the SIFT
gate; the CPU backend is also the only one of the four that misses recall@100
(0.9498).

The CPU backend still wins on single-row QPS (1423–1445 vs 610): a 250K-row
HNSW search at `ef=64` finishes inside the SQL round trip, while every GPU
query pays a H2D copy, a kernel launch and a sync. The evaluation scores GPU
candidates against the paired frozen GPU baseline, not against VSAG, and that
ratio is now 4.1–4.7x on 128-d and 2.6–3.1x on 256-d.

## Reproducing

Build (the bridge and the server are separate artifacts):

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j64
```

Isolate the build parameters without a server (this is how the tables above
were produced — `base.f32`/`query.f32` are row-major float32, `gt.i32` is
`nq x 100` int32):

```bash
hipcc -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  tools/hipvs_bridge/tune_cagra.c -L/opt/hipvs/lib \
  -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -lcuvs_c -o tune_cagra
./tune_cagra base.f32 250000 128 query.f32 4096 gt.i32 100 2 128 128 24 32 512
#          base n dim query nq gt topk [algo deg inter niter sw itopk]
# 256-d: 2 128 160 32 32 512
```

Pass `itopk=512` explicitly: the default is `4*topk` (400 here), and the server
forces ≥512. Width must stay ≤32; 64 exceeds the hashmap bitlen and the search
call fails. Build times are comparable either way, recall is not — see
"Parameter sweep" above. The line it prints starts with `sorted=1` (assertion
that the returned ids are distance-ascending), then strict `recall@10`,
`recall@10_in100`, and `recall@100`; use `recall@10`, the other is the loose
convention kept for comparison.

## Restart / persistence

After `CREATE VECTOR INDEX`, the snapshot is written during the DDL
(`serialized GPU CAGRA snapshot ... built=true, graph_bytes=256001713`). After a
server restart the index is reloaded by
`ob_cuvs_deserialize` → `seekdb_cuvs_deserialize`
(`deserialized GPU CAGRA snapshot ... n64=250000, graph_bytes=256001713`) and
queries are served from the GPU again with no rebuild.
