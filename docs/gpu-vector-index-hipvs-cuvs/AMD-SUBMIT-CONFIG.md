# Final submission configuration (`amd-cagra-v3`)

Baseline: V5 frozen commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
This document defines the exact configuration on the submission branch; the
older reports in this directory are experiment logs and describe other
candidates unless they explicitly refer to this file.

## Configuration

| Parameter | 128-d (SIFT250K) | 256-d (Hidden250K) |
| --- | --- | --- |
| CAGRA `build_algo` | `NN_DESCENT` | `NN_DESCENT` |
| `graph_degree` | 32 | 160 |
| `intermediate_graph_degree` | 64 | 160 |
| `nn_descent_niter` | 10 | 16 |
| `search_width` | 32 | 16 |
| `itopk_size` | `max(topk, 256)` | `max(topk, 512)` |
| vector precision (external) | FP32 | FP32 |

The 128-d row follows the aggressive public-reference setting that was
independently measured on real SIFT250K: CAGRA 32/64/10 built in 1.68-1.83s
versus 17.6s for the frozen 128/256/100 graph (9.6-10.4x), with strict
Recall@100 0.99975. The 256-d row is the build-margin candidate measured in
[AMD-BUILD-MARGIN.md](AMD-BUILD-MARGIN.md): paired DDL **16.963s -> 12.310s
(1.378x)** at Recall@10/@100 **0.9274/0.8747** versus the 10efa reference's
**0.9266/0.8696**, with search width 16.

Bridge source MD5 for this exact configuration (LF blob as committed):
`f7bcf6ae33a407e9d933991b2a34f9f1`.

## Why the split

The official 19.574 result is 0.426 points short of 20 because build 2.901x
sits just below the 3.0x build cap. SIFT250K is the easier 128-d dataset and
carries 40% of the build weight, so the aggressive 32/64/10 graph there
creates a large build margin while the harder 256-d Hidden250K arm keeps the
recall-safe 160/160/16 graph. The search kernel is pinned to the frozen
baseline's `SINGLE_CTA`; the official GPU-path spot check rejects the
AUTO/MULTI_CTA path even when local GPU logs look clean.

## Score projection

The 128-d arm has an external public measurement of 9.6-10.4x build over the
frozen baseline; the 256-d arm has the paired 1.378x build increment measured
against the 10efa reference. Even discounting the 128-d result heavily, the
weighted build ratio is far above the 3.0x cap. Query uses width 32 with a
256-itopk floor on 128-d (public SIFT measurement 1.58x SQL QPS) and the
existing 10efa query path on 256-d. The projected score is the full **20/20**
(4 hard-gate + 14 build + 2 query). The exact cross-team number is a
projection; the platform result must still be recorded.

## Recall and gate evidence

- 128-d SIFT250K, 32/64/10 public-reference measurement: strict Recall@100
  `0.99975` against the frozen baseline (delta `-2.2e-4`), comfortably inside
  both the 0.95 absolute floor and the 0.005 budget.
- 256-d 160/16 graph: paired synthetic 250Kx256 Recall@10/@100
  `0.9274/0.8747` against reference `0.9266/0.8696` (deltas
  `+0.0008/+0.0051`). The official Hidden250K baseline is easier
  (`0.985/0.914`), so this configuration is expected to clear its absolute
  floors and the paired-delta gate.
- The `AMD-BUILD-MARGIN.md` acceptance run on the same bridge code verified:
  GPU-built snapshot, no VSAG fallback, restart reload with zero rebuild, 256
  pre/post-restart Top-10 result sets identical, insert/delete semantics, and
  the `lib=vsag` CPU path. Peak VRAM was 0.89 GB, well below the 1.25x cap.
- No change to external FP32 semantics, external-ID mapping, SQL syntax, or
  the CPU `lib=vsag` path. All GPU work remains on hipVS/cuVS CAGRA.

## Compile verification

- `tools/hipvs_bridge/seekdb_cuvs_bridge.c` compiles against the fixed hipVS
  `f96a663d0a4441d6f5344bd3ef552b71ae7b370b` C headers.
- A Linux clang-17 tree with the frozen third-party bundle successfully built
  `oblib_lib` (includes `ob_vsag_adaptor.cpp`) and `ob_rootserver` (includes
  `ob_ddl_common_rs_impl.cpp`) with `OB_BUILD_CUVS=ON`. The remaining observer
  build on that particular test host stopped on an unrelated missing
  `sqlite/sqlite3.h` from its local deps cache; the official frozen deps
  include it.

## Reproduction

Compile exactly as the contest requires (the bridge and observer are separate
artifacts):

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

The end-to-end paired harness and deterministic data generator used for the
256-d measurements are in `tools/hipvs_bridge/bench/e2e_pair.py` and
`tools/hipvs_bridge/bench/gen_vectors.c`; their exact invocation is documented
in [AMD-BUILD-MARGIN.md](AMD-BUILD-MARGIN.md).
