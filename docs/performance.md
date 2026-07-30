# Performance

Phase 1 favors bounded, measurable behavior over speculative micro-optimization.

## Current data flow

- PNG input/output streams through `FILE` handles, while the simplified libpng
  API materializes decoded RGBA pixels.
- JPEG compressed input/output is streamed and processed by scanline.
- WebP compressed input is read through a Windows file mapping; decode writes
  directly into the allocated RGBA destination.
- WebP encoding may allocate an internal planar representation in addition to
  the RGBA source.
- Resize uses one destination RGBA buffer and releases the source after the
  transform.
- No complete video or audio pipeline exists in Phase 1.

The decoded safety limit is 100 million pixels (roughly 400 MiB RGBA). Default
job concurrency is conservative, but the scheduler currently limits job count
rather than aggregate memory.

## Measurement plan

Before changing codec parameters, buffer sizes, or resize parallelism, add
benchmarks for:

1. Signature detection latency on local and network-backed paths.
2. PNG/JPEG/WebP decode and encode throughput by image size.
3. Resize throughput and peak working set.
4. Batch makespan and peak working set at job limits 1, 2, 4, and 8.
5. Temporary commit and conflict-name generation in large directories.

Record codec versions, CPU, storage type, build preset, wall time, CPU time, and
peak working set. Use representative public-domain/generated images and never
commit large fixtures.

The next performance milestone should introduce a benchmark target using Google
Benchmark and a memory-weighted admission controller. Media work must separately
cap concurrent video jobs and let FFmpeg manage bounded streaming buffers.
