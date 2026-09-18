#!/usr/bin/env python3
"""Benchmarks fastDICOMattrs against pydicom on the same task: read a
DICOM file, apply README.md's worked-example policy (erase PatientName,
hash PatientID, erase private elements -- everything else untouched), and
write the result. This is the concrete measurement behind the README's
"benchmark CPU time, throughput, and peak memory usage against a
conventional implementation" success criterion.

Two separate measurement passes, for two different reasons:

* Timing runs many files **in-process** (no subprocess-per-file overhead),
  so wall-clock read/transform/write times reflect the library, not
  interpreter startup.
* Peak memory runs each sampled file in its **own fresh subprocess** (via
  `resource.getrusage(RUSAGE_SELF).ru_maxrss` after one file), because RSS
  high-water-mark is cumulative for a process's whole lifetime -- measuring
  many files in one process would report yesterday's peak, not this file's.
  This needs fewer samples (subprocess spawn cost dominates), so it uses a
  smaller, size-stratified subsample of the timing sample.

Requires pydicom (see python/requirements-dev.txt) and this project's own
Python binding on PYTHONPATH. Run from a source checkout, e.g.:

    PYTHONPATH=python python3 bench/bench_compare.py /path/to/corpus
"""

from __future__ import annotations

import argparse
import hashlib
import math
import resource
import shlex
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))


# ---------------------------------------------------------------------------
# The policy under test -- README.md's own worked example, applied via each
# library's native API. Kept deliberately parallel to
# python/fastdicomattrs/corpus.py's _apply_readme_example_policy, but
# duplicated rather than imported: this script must also run against a
# pydicom-only environment, and keeping the two policy implementations
# side-by-side here is the point of a benchmark script.
# ---------------------------------------------------------------------------

def _apply_policy_fds(structure) -> None:
    structure.erase((0x0010, 0x0010))
    patient_id = structure.get((0x0010, 0x0020))
    if patient_id is not None:
        # Match pydicom's logical text value rather than hashing the raw
        # DICOM padding byte retained by the structural API.
        logical_id = patient_id.value.rstrip(b" \x00").decode("utf-8")
        digest = hashlib.sha256(logical_id.encode("utf-8")).hexdigest().encode("ascii")
        structure.set_value((0x0010, 0x0020), digest)
    structure.erase_private()


def _apply_policy_pydicom(dataset) -> None:
    if "PatientName" in dataset:
        del dataset.PatientName
    if "PatientID" in dataset:
        digest = hashlib.sha256(str(dataset.PatientID).encode("utf-8")).hexdigest()
        dataset.PatientID = digest
    dataset.remove_private_tags()


def run_once_fastdicomattrs(path: str, out_path: str) -> tuple[float, float, float]:
    import fastdicomattrs as fds

    t0 = time.perf_counter()
    structure = fds.read(path, fidelity="lossless")
    t1 = time.perf_counter()
    try:
        _apply_policy_fds(structure)
        t2 = time.perf_counter()
        structure.write(out_path)
        t3 = time.perf_counter()
    finally:
        structure.close()
    return t1 - t0, t2 - t1, t3 - t2


def run_once_pydicom(path: str, out_path: str) -> tuple[float, float, float]:
    import pydicom

    t0 = time.perf_counter()
    dataset = pydicom.dcmread(path)  # full read, including Pixel Data -- see module docstring
    t1 = time.perf_counter()
    _apply_policy_pydicom(dataset)
    t2 = time.perf_counter()
    dataset.save_as(out_path)
    t3 = time.perf_counter()
    return t1 - t0, t2 - t1, t3 - t2


_RUNNERS: dict[str, Callable[[str, str], tuple[float, float, float]]] = {
    "fastdicomattrs": run_once_fastdicomattrs,
    "pydicom": run_once_pydicom,
}


# ---------------------------------------------------------------------------
# Subprocess worker mode: one file (or a bare import for a baseline
# reading), one fresh process, one clean peak-RSS number on stdout.
# ---------------------------------------------------------------------------

def _worker_main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--impl", required=True,
                        choices=["fastdicomattrs", "pydicom",
                                 "baseline-fastdicomattrs", "baseline-pydicom"])
    parser.add_argument("path", nargs="?")
    parser.add_argument("out_path", nargs="?")
    args = parser.parse_args(argv)

    if args.impl == "baseline-fastdicomattrs":
        import fastdicomattrs  # noqa: F401  (import cost is the measurement)
        timings = (0.0, 0.0, 0.0)
    elif args.impl == "baseline-pydicom":
        import pydicom  # noqa: F401
        timings = (0.0, 0.0, 0.0)
    else:
        timings = _RUNNERS[args.impl](args.path, args.out_path)

    peak_rss_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss  # Linux: KB
    print(f"{timings[0]!r} {timings[1]!r} {timings[2]!r} {peak_rss_kb}")
    return 0


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------

def _discover_dcm_files(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*.dcm") if p.is_file())


def _stratify_by_size(paths: list[Path], n: int) -> list[Path]:
    """Picks `n` files evenly spaced across the size-sorted distribution,
    so the sample spans small-to-large files rather than whatever order
    the filesystem happens to hand back."""
    if n >= len(paths):
        return list(paths)
    ordered = sorted(paths, key=lambda p: p.stat().st_size)
    step = len(ordered) / n
    return [ordered[int(i * step)] for i in range(n)]


def _median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def _p95(values: list[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def _time_in_process(impl: str, paths: list[Path], out_path: Path) -> dict[str, list[float]]:
    run = _RUNNERS[impl]
    reads, transforms, writes = [], [], []
    for path in paths:
        r, t, w = run(str(path), str(out_path))
        reads.append(r)
        transforms.append(t)
        writes.append(w)
    return {"read": reads, "transform": transforms, "write": writes}


def _run_measurement_batch(entries: list[tuple[str, str]], out_path: Path, script: Path) -> list[int]:
    """Runs `--worker --impl <impl> [<path> <out_path>]` once per (impl, path)
    entry (path may be "" for a baseline/import-only entry), all as children
    of a SINGLE bash loop rather than directly from this Python process.
    Returns each entry's peak RSS in KB, in order.

    Why bash and not subprocess.run() in a Python loop: on Linux, a forked
    child's reported ru_maxrss is seeded from its *immediate parent's*
    current RSS at fork time, and this is NOT reset by the child's own
    execve() -- verified empirically (inflate a parent to ~200MB, spawn a
    bare `python3 -c "print(getrusage(...))"` child, and it reports ~200MB
    too, despite doing nothing itself). A long-lived Python orchestrator
    making dozens of sequential subprocess.run(capture_output=True) calls
    can itself creep upward in RSS across the run (allocator arena
    retention), silently inflating -- and converging -- every measurement
    taken later in the sequence, for both implementations equally, masking
    the real per-file signal. Routing every worker invocation through one
    bash process avoids this: bash's own RSS stays low and stable across
    many loop iterations (also verified empirically -- grandchildren
    spawned by a bash loop, itself launched from a ~200MB-inflated Python
    parent, correctly report ~11MB, not ~200MB), so it -- not this
    possibly-creeping Python process -- is the immediate parent every
    worker actually forks from.
    """
    # Trailing "\n" matters: bash's `while read` silently drops a final
    # line that isn't newline-terminated, which would otherwise lose the
    # last entry's measurement without any error.
    stdin_lines = "".join(f"{impl}\t{path}\n" for impl, path in entries)
    python_q = shlex.quote(sys.executable)
    script_q = shlex.quote(str(script))
    out_q = shlex.quote(str(out_path))
    bash_script = (
        "while IFS=$'\\t' read -r impl path; do\n"
        f"  {python_q} {script_q} --worker --impl \"$impl\" \"$path\" {out_q}\n"
        "done\n"
    )
    result = subprocess.run(["bash", "-c", bash_script], input=stdin_lines,
                             capture_output=True, text=True, check=True)
    lines = [ln for ln in result.stdout.splitlines() if ln.strip()]
    if len(lines) != len(entries):
        raise RuntimeError(f"expected {len(entries)} measurement results, got {len(lines)}; "
                            f"bash stderr: {result.stderr}")
    return [int(line.split()[-1]) for line in lines]


def _report(impl: str, timings: dict[str, list[float]], memory_samples: list[int],
            baseline_kb: int, file_count: int) -> str:
    lines = [f"== {impl} ({file_count} files timed, {len(memory_samples)} memory-sampled) =="]
    for phase in ("read", "transform", "write"):
        ms = [v * 1000 for v in timings[phase]]
        lines.append(f"  {phase:<10} median {_median(ms):8.3f} ms   p95 {_p95(ms):8.3f} ms")
    total_ms = [sum(v) * 1000 for v in zip(timings["read"], timings["transform"], timings["write"])]
    lines.append(f"  {'total':<10} median {_median(total_ms):8.3f} ms   p95 {_p95(total_ms):8.3f} ms")
    if memory_samples:
        lines.append(f"  peak RSS   median {_median(memory_samples) / 1024:8.2f} MB   "
                      f"max {max(memory_samples) / 1024:8.2f} MB   "
                      f"(baseline/import-only: {baseline_kb / 1024:.2f} MB)")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    if argv and argv[0] == "--worker":
        return _worker_main(argv[1:])

    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=Path, help="DICOM corpus directory to benchmark against")
    parser.add_argument("--timing-sample", type=int, default=300,
                        help="files to time in-process per implementation (default: 300)")
    parser.add_argument("--memory-sample", type=int, default=20,
                        help="files to measure peak memory for, in isolated subprocesses, per "
                             "implementation (default: 20)")
    args = parser.parse_args(argv)

    root = args.root.expanduser()
    if not root.exists():
        print(f"bench_compare: {root} does not exist", file=sys.stderr)
        return 2

    all_files = _discover_dcm_files(root)
    if not all_files:
        print(f"bench_compare: no .dcm files found under {root}", file=sys.stderr)
        return 2

    timing_sample = _stratify_by_size(all_files, args.timing_sample)
    memory_sample = _stratify_by_size(timing_sample, args.memory_sample)
    script = Path(__file__).resolve()

    print(f"Corpus: {root} ({len(all_files)} .dcm files discovered)")
    print(f"Timing sample: {len(timing_sample)} files (size-stratified)")
    print(f"Memory sample: {len(memory_sample)} files (size-stratified subset, isolated "
          f"subprocess each)\n")

    with tempfile.TemporaryDirectory(prefix="fastdicomattrs-bench-") as directory:
        out_path = Path(directory, "out.dcm")

        # All memory measurement (both implementations' baseline and every
        # per-file sample) goes through one _run_measurement_batch() call --
        # see its docstring for why a single shared bash loop, rather than
        # this process spawning workers directly, is required for accurate
        # peak-RSS numbers.
        entries: list[tuple[str, str]] = [
            (f"baseline-{impl}", "") for impl in ("fastdicomattrs", "pydicom")
        ]
        for impl in ("fastdicomattrs", "pydicom"):
            entries.extend((impl, str(path)) for path in memory_sample)
        peaks = _run_measurement_batch(entries, out_path, script)

        baseline_kb = {"fastdicomattrs": peaks[0], "pydicom": peaks[1]}
        offset = 2
        memory_samples: dict[str, list[int]] = {}
        for impl in ("fastdicomattrs", "pydicom"):
            memory_samples[impl] = peaks[offset:offset + len(memory_sample)]
            offset += len(memory_sample)

        for impl in ("fastdicomattrs", "pydicom"):
            timings = _time_in_process(impl, timing_sample, out_path)
            print(_report(impl, timings, memory_samples[impl], baseline_kb[impl], len(timing_sample)))
            print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
