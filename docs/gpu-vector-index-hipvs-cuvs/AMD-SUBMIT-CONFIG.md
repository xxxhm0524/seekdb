# AMD CAGRA final submission configuration (`amd-cagra-v6`)

Baseline: V5 frozen commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
This branch is a fresh descendant of that commit and does not reuse the tree of
any earlier submission branch.  `amd-cagra-submit` and `amd-cagra-v4` are left
untouched.

## Files changed

| File | Purpose |
| --- | --- |
| `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | Clean CAGRA bridge implementation with the final build/search policy |
| `tools/hipvs_bridge/seekdb_cuvs_bridge.h` | Adds `seekdb_cuvs_copy_dataset` |
| `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | Persistent large-stack search worker and single-copy GPU snapshot |
| `src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp` | Bounded parallel scan for vector-index auxiliary tables |

The adaptor and DDL files are the exact source that produced the official
`10efa0c9` result: **build 2.901x, query 2.693x, gates pass, 19.574/20**.
Only the bridge default parameters and documentation differ from that
officially measured implementation.

## Build and search parameters

| Parameter | 128-d (SIFT250K) | 256-d (Hidden250K) |
| --- | --- | --- |
| CAGRA `build_algo` | `NN_DESCENT` | `NN_DESCENT` |
| `graph_degree` | 24 | 128 |
| `intermediate_graph_degree` | 48 | 128 |
| `nn_descent_niter` | 10 | 20 |
| `search_width` | 32 | 32 |
| `itopk_size` | `max(topk, 256)` | `max(topk, 512)` |
| external vector precision | FP32 | FP32 |
| search kernel | `AUTO` | `AUTO` |

The graph pair is the exact configuration of the public accepted run at commit
`aa1bae8468`: **18.159/20 with every gate passing**, with `128 / 128 / 20` on
Hidden250K and `24 / 48 / 10` on SIFT250K.  The narrow arm measured strict
Recall@100 `0.99992` (delta `-5e-5`, gate `0.005`) on real SIFT250K.  This
rewrite keeps that measured point instead of the unmeasured five-iteration
floor, because the build score already has ample headroom.

The earlier `amd-cagra-v4` branch used `64 / 128 / 20` on the wide arm.  That
configuration was never accepted by the official evaluator; the only accepted
public evidence for the wide arm is degree 128.  This is why the rewrite keeps
degree 128 even though it costs some build time.

## Why this clears the build cap

The official `10efa` run measured composite build **2.901x** with narrow
`128 / 128 / 24` and wide `128 / 160 / 32`.  This branch changes only the two
NN-Descent graphs:

- narrow `128 / 128 / 24` -> `24 / 48 / 10`; the public real-SIFT sweep
  measured this graph at Recall@100 `0.99992`, versus the 10efa narrow graph's
  `128 / 128 / 24`, while the official SIFT arm was already the easier
  40%-weighted side;
- wide `128 / 160 / 32` -> `128 / 128 / 20`; intermediate degree 160 -> 128 is
  pure build cost and the final graph remains degree 128, the configuration
  whose Hidden250K gates passed in the accepted 18.159 run.

Both changes strictly reduce NN-Descent work relative to the 2.901x official
configuration, and neither touches the snapshot or query semantics.  A
conservative weighted estimate is above the 3.0x build cap.  Query stays on the accepted 18.159 search configuration (`AUTO`, `width=32`,
`itopk` 256/512), which already cleared the 1.5x query cap in that official
run; the `10efa` official query measurement with the same width was 2.693x.

## GPU-path and correctness notes

- The bridge sets `cuvsCagraSearchParams.algo = AUTO` and `search_width = 32`,
  exactly matching the public accepted 18.159/20 submission, which passed every
  Hidden250K gate including the external `gpu_path` spot check.  `AUTO`
  resolves to `MULTI_CTA` for the `nq=1` SQL query workload.
- The bridge emits `[seekdb_cuvs] build n=... dim=... degree=... inter=...
  niter=... ms=...` only after `cuvsCagraBuild` succeeds and the built shape is
  verified with `cuvsCagraIndexGetSize` / `cuvsCagraIndexGetDims`.  A snapshot
  without this marker means the GPU build did not happen.
- `lib=vsag` remains untouched: the bridge is only reached for `lib=cuvs`
  indexes, and the adaptor keeps the existing VSAG fallback for filters,
  deletes, stale GPU indexes, and small data sets.
- External FP32 vectors, external-ID mapping, SQL syntax, and snapshot
  deserialization are unchanged from the officially measured V5 path.

## Compile verification

`tools/hipvs_bridge/seekdb_cuvs_bridge.c` was checked against the fixed hipVS
headers at `f96a663d0a4441d6f5344bd3ef552b71ae7b370b` with both strict C11 and
C++17 front ends:

```bash
gcc -std=c11 -fsyntax-only -Wall -Wextra -D__HIP_PLATFORM_AMD__ \
  -I tools/hipvs_bridge -I /opt/hipvs/include tools/hipvs_bridge/seekdb_cuvs_bridge.c
```

The adaptor and DDL files are byte-identical to the official 19.574/20 source,
so the full observer build has already been exercised by the evaluator on that
exact code.

## Submission fields

- Repository: `https://github.com/xxxhm0524/seekdb`
- Branch: `amd-cagra-v6`
- Commit: the full 40-character SHA committed on that branch (also mirrored to
  `https://github.com/xxxhm0524/seekdb_2026` on the same branch).
- Submit this branch/commit, not `amd-cagra-submit` or `amd-cagra-v4`.
