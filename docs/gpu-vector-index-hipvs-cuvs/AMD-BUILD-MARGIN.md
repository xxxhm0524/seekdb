# Build-margin candidate

This candidate targets the remaining build-score gap of the user-reported official result for `10efa0c9f2c84e9959d9154f602d9950421ee13b`: build 2.901x, query 2.693x, gates pass, 19.574/20. A new official evaluation is required to establish this candidate's score.

> Submission note: the `amd-cagra-v3` branch applies the all-dimension
> 160/16 graph measured here only to 256-d vectors; 128-d uses the aggressive
> 32/64/10 public-reference SIFT configuration. See
> [AMD-SUBMIT-CONFIG.md](AMD-SUBMIT-CONFIG.md) for the exact final split and
> score projection. The 256-d measurements in this file are the evidence for
> the 256-d half of that split.

## Implementation

The bridge uses NN-Descent with final graph degree 160, intermediate degree 160 and 16 construction iterations. Search uses width 16 and retains the existing itopk floor of 512. This policy applies to all dimensions. Compared with the submitted 10efa version, this retains more graph neighbors and performs fewer construction passes. It does not change vector precision, external-ID mapping, SQL behavior, snapshot format, the persistent query worker, or the CPU backend.

The previously tested 12-iteration graphs lost more than 0.005 Recall@10 against the frozen GPU baseline on the synthetic 256-dimensional dataset and were rejected. A width-24 search did not recover sufficient recall at that construction budget. These experiments are not submission results.

## Method and scope

The benchmark uses 250,000 synthetic vectors in 128 and 256 dimensions, with 4,096 separate Top10 and Top100 SQL queries per build. `bench/gen_vectors.c` generates the data deterministically from its recorded xorshift seed and computes exact FP32 L2 truth with hipVS brute force. These are synthetic datasets, not real SIFT or the official hidden dataset.

The reference bridge is fetched from the exact 10efa commit. The clean-built observer source has no `src` difference against that commit. Both bridges run on that identical observer with a process restart between switches. The bridge hash and mapped inode are checked. Each run imports the same raw vectors without an index, starts timing at CREATE VECTOR INDEX, and includes the first query in the reported build duration. Graph construction, synchronization, serialization, snapshot publication and any first-query work are included. Startup and raw-data import are recorded separately.

Three paired rounds alternate the reference/candidate order. Only the `official` and `margin` variants with run orders `p1`, `p2`, `p3` belong to the final comparison. Earlier `scored`/2932 runs and standalone GPU tuning results are excluded. Here `official` is a local label for the 10efa reference, not a new official platform run.

The harness validates row count, unique valid external IDs, strict first-K Recall@K, a GPU-built snapshot and absence of GPU error/VSAG fallback messages. Restart acceptance additionally records EXPLAIN, reload/no-rebuild evidence, and insert/delete behavior. Resource readings include whole-card VRAM, available process VRAM and process high-water RSS; process CPU seconds include startup and import and are not build-only CPU time.

## Measured results (2026-10-30)

The three raw build times (seconds) are:

| Dataset | 10efa reference | Candidate | Median reference / candidate |
| --- | --- | --- | --- |
| Synthetic 250K x 128 | 12.945, 13.260, 13.175 | 11.893, 11.994, 11.789 | 13.175 / 11.893 = 1.108x |
| Synthetic 250K x 256 | 17.156, 16.821, 16.963 | 12.224, 12.310, 12.387 | 16.963 / 12.310 = 1.378x |

The weighted incremental build improvement is **1.2628x**. Multiplying by the previously reported official 2.901x gives a **3.6633x projection**, not a measured official acceleration or score.

Median query and quality results:

| Dataset | Reference Recall@10 / @100 | Candidate Recall@10 / @100 | Reference QPS@10 / @100 | Candidate QPS@10 / @100 |
| --- | --- | --- | --- | --- |
| 128 dimensions | 0.9879 / 0.9645 | 0.9904 / 0.9710 | 619.09 / 514.35 | 493.51 / 421.82 |
| 256 dimensions | 0.9266 / 0.8696 | 0.9274 / 0.8747 | 420.43 / 374.48 | 351.20 / 311.70 |

Query throughput is lower than the already-optimized 10efa version: its weighted incremental ratio is 0.8236x. Applying that to the prior official query ratio of 2.693x gives a **2.2180x projection**, above the recorded 1.5x query cap. This is an explicit construction/query tradeoff; it is not a claim of faster queries than 10efa or a direct new measurement against the frozen baseline.

Whole-card VRAM was 0.73 / 0.86 GB for the 128 / 256-dimensional reference runs and 0.77 / 0.89 GB for the candidate. Both ratios are below 1.25. Peak candidate RSS was 3.12 GB. All twelve performance runs rebuilt a GPU snapshot and passed the error/fallback/ID checks. The environment was AMD gfx1100 with 48 GiB VRAM, ROCm 7.2.4, a 16-CPU container quota and 55 GiB container memory limit; seekdb used `cpu_count=24` for both sides. The CPU quota is a limitation relative to an official 24-CPU environment, not a claim of physical 24-core pinning.

GPU restart acceptance on 250K x 256: one deserialization, zero rebuilt snapshots, zero mismatches in 256 pre/post-restart Top10 result sets or ordering, first query 0.761 seconds, Recall@10/@100 0.9258/0.8746. Delete and insert probes passed. The deletion probe exercised the existing CPU compatibility path outside the build/performance window. The 10K `lib=vsag` CPU-index smoke test matched exact results before and after restart. EXPLAIN records a `VECTOR INDEX SCAN` on `items(idx)`.

Raw paired JSON, the benchmark console output, the query plan, hardware/data/artifact hashes, restart results and CPU acceptance output are under [evidence/build-margin](evidence/build-margin). The acceptance driver's final aggregation initially encountered a non-performance JSON record without `run_order`; that post-processing filter was corrected and aggregation rerun. The restart and CPU checks themselves had already completed successfully, as preserved in the raw log.

The observer was clean-built from the 3f05232af tree; its `src` is identical to 10efa. The measured candidate bridge source MD5 is `23021c33c358135ee9e9a860e4bf13bf`, and the shared library MD5 is `d6a15c2f01bce6d11a7d8118e235828a`. The reference shared library MD5 is `be5b3f9530881219ea1234dd1c7fe1be`. No real-SIFT or official-hidden-data validation is claimed for this candidate.

## Reproduction commands

Use an isolated Linux ROCm/hipVS environment. The following root directory must be dedicated to this benchmark: the runner recreates its own per-run directories and removes their database store after collecting results. Run sequentially because the benchmark shares its bridge path and SQL port 2881.

```bash
export SEEKDB_BENCH_ROOT=/root/amd-score-20261030
ROOT=$SEEKDB_BENCH_ROOT
mkdir -p "$ROOT"/{bridge/variants,data/uniform128,data/uniform256,run,results}
bash tools/hipvs_bridge/build.sh "$ROOT/bridge/variants/margin.so"
cp tools/hipvs_bridge/seekdb_cuvs_bridge.c "$ROOT/bridge/variants/margin.c"
cp "$ROOT/bridge/variants/margin.so" "$ROOT/bridge/libseekdb_cuvs_bridge.so"
./build.sh release -DOB_BUILD_CUVS=ON \
  -DCUVS_BRIDGE_LIB="$ROOT/bridge/libseekdb_cuvs_bridge.so" --make -j12
hipcc -O3 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  tools/hipvs_bridge/bench/gen_vectors.c -L/opt/hipvs/lib \
  -Wl,-rpath,/opt/hipvs/lib -lcuvs_c -o "$ROOT/gen_vectors"
"$ROOT/gen_vectors" "$ROOT/data/uniform128" 250000 128 0 4096
"$ROOT/gen_vectors" "$ROOT/data/uniform256" 250000 256 0 4096
```

In a separate source checkout of 10efa, run its `tools/hipvs_bridge/build.sh` to produce `$ROOT/bridge/variants/official.so` and copy that bridge source to `official.c`. Then, from the candidate checkout, repeat for each dimension and round, reversing variant order in the second round:

```bash
for rep in 1 2 3; do
  for dim in 128 256; do
    variants='official margin'
    [ "$rep" != 2 ] || variants='margin official'
    for variant in $variants; do
      python3 tools/hipvs_bridge/bench/e2e_pair.py \
        "$ROOT/data/uniform$dim" "$dim" "$variant" \
        "$PWD/build_release/src/observer/seekdb" "p$rep"
    done
  done
done
```

Use the median of each variant's three complete build times. The weighted incremental improvement is `gain128^0.4 * gain256^0.6`. Multiplying this by the prior official 2.901x is only a projection: synthetic-data improvement need not transfer identically to official data. Query throughput relative to 10efa must likewise not be confused with throughput relative to the frozen official baseline. Do not claim 20/20 until the platform confirms it.
