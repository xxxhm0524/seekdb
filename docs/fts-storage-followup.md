# Storage decode follow-up (2026-10-31)

This increment adapts selected Apache-2.0 changes from juanzhi-gcb/seekdb:

* [835e79987c13362b2c234ff3733f1bf47fc98950](https://github.com/juanzhi-gcb/seekdb/commit/835e79987c13362b2c234ff3733f1bf47fc98950):
  small string copies, batch template reuse and width-sized decode buffers.
* [b9bc9ae0452ff6efea60084a50d4f60558857cb7](https://github.com/juanzhi-gcb/seekdb/commit/b9bc9ae0452ff6efea60084a50d4f60558857cb7):
  precompute fixed-length string-diff segment offsets once per batch.
* [472e45ebe12ed8586693cc8e7f87d8789700948e](https://github.com/juanzhi-gcb/seekdb/commit/472e45ebe12ed8586693cc8e7f87d8789700948e):
  avoid shared segmentation-cache overhead on the BENG path.
* [97ab48bb741f789e4e9d1d88380727f11cd03aaa](https://github.com/juanzhi-gcb/seekdb/commit/97ab48bb741f789e4e9d1d88380727f11cd03aaa):
  prefer existing raw encoding to string-template encodings for binary IDs.

Original source license headers remain intact. These upstream revisions have
local measurements but no published platform result; those measurements do not
apply to this adaptation without measurement.

## Changes and boundaries

The decoder no longer allocates at least 128 bytes per result when the actual
column width is smaller. It materializes common string bytes once per batch,
copies that template and patches row-specific segments. Fixed-length,
non-hex columns use at most 16 precomputed segments; more segments and hex
packing retain the original descriptor traversal. Small copies use bounded,
unaligned-safe memcpy operations and do not read outside the requested width.

The local adaptation handles zero and negative batch sizes before allocation,
checks allocation multiplication overflow, and initializes the common template
before copying its bytes. The existing NULL-row handling remains in place.

The encoding preference is narrower than upstream: all non-NULL values must
have fixed width 16, the maximum width must also be 16, and the type must be
binary-collated VARCHAR. Merely having a maximum width of 16 is insufficient.
This changes encoding selection among existing formats, not stored values or
the decoding format version. Existing compressed blocks still use their
decoders; the encoding preference takes effect when blocks are newly written
or rebuilt. Dictionary and other existing encoding choices remain available.

Raw encoding can increase storage size and I/O relative to compact string
templates. It trades compression for cheaper decoding on eligible columns,
which include native document IDs and other fixed-width binary VARCHAR values.
Its net benefit has not been measured here.

The shared segmentation cache is bypassed for BENG. Packed SQL TOKENIZE memo
reuse remains available under its existing eligibility checks; outputs and
normal parsing remain the same. Workloads that would otherwise repeatedly
reuse BENG segmentation entries may lose that shared-cache benefit.

Empty parser properties now reset a reused helper to constructor defaults,
avoiding retention of settings from its previous explicit configuration.

## Validation

Per the user's instruction, no SQL regressions or performance benchmarks are
run for this increment. Static diff checks and an incremental release build
passed: the incremental release build linked seekdb successfully (exit 0,
496.54 seconds, at most two compile tasks). The existing native WSL build tree
was reused; this was not a clean build of the entire submission. Runtime
correctness and speedup remain unverified.
All fixed benchmark data, SQL, timing logic, scoring files and CI workflow are
unchanged. Only local WSL compilation is used, with two concurrent tasks.
