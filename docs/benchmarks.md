# fastDICOMattrs — Benchmarks vs. pydicom

This document records the results of `bench/bench_compare.py`: fastDICOMattrs vs. pydicom (a
conventional, widely-used pure-Python DICOM implementation) performing the same task — read a file,
apply README.md's worked-example policy (erase PatientName, hash PatientID, erase private elements,
Pixel Data untouched), write the result. This is the concrete measurement behind the README's
"benchmark CPU time, throughput, and peak memory usage against a conventional implementation"
success criterion, and a direct test of the project's stated
[research hypothesis](../README.md#research-hypothesis): that structural, non-materializing parsing
should reduce memory pressure and processing overhead relative to full deserialization. Numbers
below are from an actual run, not aspirational.

## Methodology

* **Timing** runs many files **in-process**, one implementation at a time, so wall-clock
  read/transform/write times reflect the library, not interpreter startup.
* **Peak memory** runs each sampled file in its **own fresh subprocess**
  (`resource.getrusage(RUSAGE_SELF).ru_maxrss` after processing exactly one file), because RSS
  high-water-mark is cumulative for a process's whole lifetime — measuring many files in one
  process would report an earlier file's peak, not necessarily the current one's.
* All memory-measurement subprocesses are spawned from a single shared `bash` loop rather than
  directly from the (long-lived) Python orchestrator. This isn't cosmetic: on Linux, a forked
  child's reported `ru_maxrss` is seeded from its *immediate parent's* current RSS at fork time,
  and is not reset by the child's own `execve()`. A long-lived Python process making dozens of
  sequential `subprocess.run()` calls can itself creep upward in RSS (allocator arena retention),
  silently inflating — and converging — every measurement taken later in the sequence, for both
  implementations equally, masking the real per-file signal. This was caught empirically while
  building this benchmark (an early version reported *identical* peak-RSS numbers for both
  implementations, which turned out to be the polluted-parent artifact, not a real result) — see
  `_run_measurement_batch`'s docstring in `bench_compare.py` for the verified repro. Routing
  through one `bash` process (whose own RSS stays low and stable across many loop iterations) as
  the immediate parent avoids it.
* Both implementations apply the *same* policy to the same logical PatientID text via their own
  native API (the structural path removes DICOM space/NUL padding before hashing): this library's
  `erase`/`set_value`/`erase_private`, and pydicom's `del ds.PatientName` /
  `ds.PatientID = hashlib.sha256(...).hexdigest()` / `ds.remove_private_tags()`.
* pydicom is read with plain `dcmread()` (no `stop_before_pixels`): a conventional
  read-modify-write must materialize Pixel Data to write a complete file back out, which is
  exactly the case this library's structural, pass-through approach is designed to avoid — see
  the README's ["Why"](../README.md#why) section. Comparing pydicom in its normal, full-read mode
  against this library's structural mode is therefore the fair, realistic comparison, not a
  favorable cherry-pick for either side.

**Hardware/software:** Intel Core i7-6700 @ 3.40GHz (8 threads), Linux 6.8.0-136-generic,
Python 3.10.12, pydicom 3.0.2, Release build of the pre-publication working tree. The results were
refreshed after aligning PatientID normalization between implementations.

**Corpus:** CMB-MEL (see `docs/corpus-results.md` for provenance/license) — CT files ranging from
~200KB to ~500KB (typical) up to one series of ~97MB native-pixel-data ultrasound biopsy captures
(large). The two tables below come from **two separate invocations** of `bench_compare.py`
against two different subsets of the same corpus, not one run split after the fact: the "typical"
table points at the corpus root (`--timing-sample 500 --memory-sample 30`, size-stratified across
the whole 14GB/23,805-file collection, which skews heavily toward the ~200-500KB regime); the
"large" table points at a single large-pixel-data series subdirectory containing exactly 21 files
(a requested `--timing-sample` at or above 21 returns every file in that subset — see
`_stratify_by_size`'s early-return in `bench_compare.py` — hence "21 files timed" rather than a
round sample size). That series isn't identified by a portable path (the corpus itself lives only
on the machine it was downloaded to, per `docs/corpus-results.md`); to find an equivalent large
subset in your own copy of the corpus, look for the highest-resolution/largest-file series, e.g.
`find /path/to/corpus -name '*.dcm' -size +50M`.

**Caveats:** these are single-machine, single-corpus, single-run numbers — no repeated-run
variance is captured, and the size-stratified sampling is deterministic (same files every run
against the same corpus) rather than randomized. pydicom is one conventional, widely-used
pure-Python implementation; it is a reasonable reference point, not a general performance baseline
for "DICOM libraries" as a category.

## How to reproduce

```sh
cmake --build build --parallel
pip install -r python/requirements-dev.txt   # pydicom

# "typical" table: whole corpus, size-stratified sample
PYTHONPATH=python python3 bench/bench_compare.py /path/to/corpus --timing-sample 500 --memory-sample 30

# "large" table: a subdirectory containing only the large-pixel-data series
PYTHONPATH=python python3 bench/bench_compare.py /path/to/corpus/large-series-subdir --timing-sample 500 --memory-sample 15
```

## Results: typical files (~200KB–500KB CT slices, 500 files timed / 30 memory-sampled)

| | fastDICOMattrs | pydicom | ratio |
|---|---:|---:|---:|
| read (median) | 0.756 ms | 2.858 ms | 3.8x |
| transform (median) | 0.114 ms | 4.921 ms | 43.2x |
| write (median) | 1.919 ms | 7.279 ms | 3.8x |
| **total (median)** | **2.831 ms** | **15.649 ms** | **5.5x** |
| total (p95) | 4.084 ms | 21.338 ms | 5.2x |
| peak RSS (median) | 20.25 MB | 50.76 MB | 2.5x |
| peak RSS, baseline (import-only) | 19.12 MB | 49.42 MB | — |

At this size, both implementations' peak RSS sits close to their own interpreter/library import
baseline — a single ~300KB file's processing doesn't move the needle much against a ~23–32MB
baseline either way. The dominant, consistent effect at typical file sizes is **processing time**
(wall-clock, via `time.perf_counter()` — this benchmark does not separately measure CPU time):
structural, dictionary-free parsing plus a writer that reconstructs headers deterministically
without going through pydicom's full per-element decode/re-encode machinery is roughly **5.5x
faster end to end** at the median.

## Results: large files (~97MB native Pixel Data, 21 files timed / 15 memory-sampled)

| | fastDICOMattrs | pydicom | ratio |
|---|---:|---:|---:|
| read (median) | 0.300 ms | 90.731 ms | 302x |
| transform (median) | 0.146 ms | 5.648 ms | 38.7x |
| write (median) | 216.516 ms | 357.921 ms | 1.7x |
| **total (median)** | **217.006 ms** | **459.809 ms** | **2.1x** |
| peak RSS (median) | 112.25 MB | 234.93 MB | 2.1x |
| peak RSS, baseline (import-only) | 19.00 MB | 49.66 MB | — |
| peak RSS, attributable to this file (median − baseline) | ~93 MB | ~185 MB | 2.0x |

This is where the research hypothesis shows up clearly. **Read time is essentially free** for this
library (0.300 ms — it never touches Pixel Data bytes during structural parsing) versus pydicom's
90.7 ms to fully decode a 97MB file into a `Dataset`. Write time dominates the total for *both*
implementations here, for an unavoidable reason: writing a complete output file requires physically
copying ~97MB to disk regardless of how cheaply it was parsed — but even there, this library is
1.7x faster, and **total time is 2.1x faster**. **Peak memory attributable to processing one file is
about half** for this library (~93MB vs. ~185MB) — consistent with pydicom needing roughly the raw
file size once for its own read buffer and again for the materialized `PixelData` bytes in the
`Dataset`, against this library's single mapped view of the source.

## Interpretation

The hypothesis this project set out to test — "a C++ structural parser capable of identifying
element boundaries, preserving source byte ranges, selectively replacing or removing elements, and
passing bulk data through without decoding it should substantially reduce memory pressure and
processing overhead" (README, "Research hypothesis") — holds up under measurement, not just
assertion, on real-world data: consistently faster at every file size tested (5.5x on typical files,
2.1x on large-pixel-data files where write I/O dominates for both sides), and using meaningfully less
memory specifically where the hypothesis predicts it should matter most — large Pixel Data.
