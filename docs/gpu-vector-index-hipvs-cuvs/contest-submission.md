# AMD CAGRA submission

## Base and scope

This submission starts at the V5 frozen commit
`634888d9b037153fa143d2c913ba4b25503e1d62`.
It optimizes the existing GPU CAGRA path in the seekdb adaptor and bridge.
It does not modify the fixed hipVS library, SQL interface, input precision,
evaluation datasets or evaluation programs.

## Source-level bottleneck analysis

The baseline creates and joins a 32 MiB-stack pthread for each GPU operation,
including each individual approximate SQL query. The bridge also allocates
three device buffers and frees them after every search. These operations repeat
for all 4,096 single-row evaluation queries, even when their shapes are identical.

The snapshot writer serializes CAGRA to a temporary file and copies that file
into the seekdb snapshot stream. Loading reverses this process. When the temporary
directory is disk-backed, this introduces intermediate filesystem I/O in addition
to the required persistent snapshot I/O. These are findings from source inspection,
not measurements from a profiler.

## Changes

* A synchronous, persistent GPU worker reuses the large pthread stack and GPU
  thread context. Submission waits for completion, preserving job pointer
  lifetimes. One outstanding job bounds the queue; index operations still obey
  the adaptor's registry lock. Thread creation failure is reported as operation
  failure; the large-stack requirement is retained.
* Each bridge index owns reusable query, neighbor and distance device buffers.
  Capacity grows only when needed; failed growth preserves the old allocation.
  An index-local mutex prevents concurrent searches from sharing scratch data.
  Destroying an index frees its buffers. Snapshot reload starts with empty scratch
  buffers; it does not reuse state from a previous index or evaluation run.
* Snapshot staging uses an anonymous Linux memory file when available, falling
  back to `tmpfile()` otherwise. The file is created and closed per operation.
  The OBCUVS02 format, persisted CAGRA graph, ID mapping and original FP32 vectors
  are unchanged. Copy chunks grow from 64 KiB to 1 MiB.
* Bridge inputs are checked for invalid sizes and byte-count overflow. A failed
  distance copy now returns an error instead of reporting a successful search.

The build configuration remains NN_DESCENT, graph degree 128, intermediate graph
degree 256 and 100 NN-descent iterations. Search retains the baseline's itopk and
search-width settings and calls the fixed hipVS CAGRA implementation. No work is
deferred beyond the first query and no graph is cached across evaluation runs.
The lib=vsag path does not submit GPU jobs.

## Reproducible build and evaluation method

Use the contest environment: ROCm 7.2.4, GCC 13.3, CMake 3.31, Rust 1.97,
the frozen offline dependencies and the organizer's fixed hipVS installation.
From the repository root:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

Run the organizer's fixed evaluation against both the frozen commit and this
submission, on the same GPU and resource limits. Import FP32 vectors before
timing, then execute the fixed offline index DDL:

```sql
CREATE VECTOR INDEX idx_items ON items(v)
  WITH (distance=l2, type=hnsw, lib=cuvs, m=16,
        ef_construction=200, ef_search=64);
```

Include the first fixed approximate query in build time. Measure all required
Top-10/Top-100 SQL queries and recall, returned-ID integrity, CPU-path behavior,
GPU-path profiler evidence, host memory and VRAM. Clear indices and caches between
runs and swap baseline/candidate order across the two datasets as prescribed.
The notebook's explicit `batch_knn` benchmark is not a substitute for this method.

## Performance report and validation status

At the user's request, no compilation, tests or GPU benchmarks were executed.
Only source inspection, Git ancestry and whitespace checks were performed.

| Metric | V5 baseline | Candidate |
| --- | --- | --- |
| GPU worker pthreads, successive operations | One new thread per operation | One reused worker |
| Query device allocations, unchanged shape | Three per successful query | Three initially, then reused |
| Intermediate graph file | Temporary filesystem file | Anonymous memory file, disk fallback |
| SIFT250K build time / recall / QPS | Not measured | Not measured |
| Hidden250K build time / recall / QPS | Not measured | Not measured |
| CPU/GPU build-time comparison | Not measured | Not measured |
| Peak host memory / VRAM | Not measured | Not measured |

No measured speedup, passing recall, gate result or contest score is claimed.
The notebook's CPU/GPU 12.65x batch result is not a result for this submission.
The CPU/GPU comparison report remains unmeasured pending evaluation.

Risks requiring evaluation: scratch buffers retain their high-water capacity
until index destruction; memory-backed graph staging is charged to host memory;
the single persistent worker serializes GPU jobs across indices; build time may
still be dominated by unchanged NN-descent work. These changes target overhead,
and do not establish that the contest's speedup targets have been reached.
