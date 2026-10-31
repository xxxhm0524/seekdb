# Final submission configuration (`amd-cagra-v2`)

Baseline: V5 frozen commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
This document defines the exact configuration on the submission branch; the
older reports in this directory are experiment logs and describe other
candidates unless they explicitly refer to this file.

## Configuration

| Parameter | 128-d (SIFT250K) | 256-d (Hidden250K) |
| --- | --- | --- |
| CAGRA `build_algo` | `NN_DESCENT` | `NN_DESCENT` |
| `graph_degree` | 128 | 160 |
| `intermediate_graph_degree` | 128 | 160 |
| `nn_descent_niter` | 24 | 16 |
| `search_width` | 32 | 16 |
| `itopk_size` | `max(4*topk, 512)` | `max(4*topk, 512)` |
| vector precision (external) | FP32 | FP32 |

The 128-d row is exactly the configuration that the official evaluator already
scored on commit `10efa0c9f2c84e9959d9154f602d9950421ee13b`: **build 2.901x,
query 2.693x, gates pass, 19.574/20**. The 256-d row is the build-margin
candidate measured in [AMD-BUILD-MARGIN.md](AMD-BUILD-MARGIN.md): paired DDL
**16.963s -> 12.310s (1.378x)** at Recall@10/@100 **0.9274/0.8747** versus the
10efa reference's **0.9266/0.8696**, with search width 16.

Bridge source MD5 for this exact configuration (LF blob as committed):
`5342d95675721b528ba42c7a455d3473`.

## Why the split

The official 19.574 result is exactly 0.426 points short of 20 because build
2.901x sits just below the 3.0x build cap. A uniform 160-degree/16-pass graph
(measured in `AMD-BUILD-MARGIN.md`) projects enough build margin, but it also
changes the 128-d path whose recall and query behaviour the official run had
already validated. Keeping 128-d on the official configuration removes that
recall risk entirely; only the 256-d path, which carries 60% of the build
weight, moves to the build-margin graph.

## Score projection

Using the measured per-dataset incremental ratios against `10efa`:

- build: `1.000^0.4 * 1.378^0.6 = 1.212x` -> `2.901 * 1.212 = 3.52x`
- query: `1.000^0.4 * 0.8338^0.6 = 0.897x` -> `2.693 * 0.897 = 2.41x`

Both exceed their score caps (build 3.0x, query 1.5x), so the projected score
is the full **20/20** (4 hard-gate + 14 build + 2 query). This is a projection
from paired local measurements, not a new official run; the platform result
must still be recorded.

## Recall and gate evidence

- 128-d SIFT250K, official configuration: Recall@10/@100 `0.9975/0.9989`
  against the frozen baseline `1.0000/1.0000` (deltas `-0.0025/-0.0011`,
  inside the 0.005 budget).
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
