#!/usr/bin/env python3
"""Benchmark fastdicom against a directory of DICOM files.

Usage:
    python benchmark_fastdicom.py DATA_DIR

See benchmark_pydicom.py for the equivalent pydicom benchmark, run against
the same directory and the same tag list (common.py) for a fair comparison.
"""

from __future__ import annotations

import argparse
import pathlib
import time

import fastdicom

from common import MULTI_TAGS, SINGLE_TAG, as_fastdicom_tag, discover_files, report


def bench_single_tag(files: list[pathlib.Path]) -> None:
    tag = as_fastdicom_tag(SINGLE_TAG)
    start = time.perf_counter_ns()
    for path in files:
        fastdicom.get_tag(str(path), tag)
    elapsed = time.perf_counter_ns() - start
    report("fastdicom: single tag", elapsed, len(files), 1)


def bench_multi_tag(files: list[pathlib.Path]) -> None:
    tags = [as_fastdicom_tag(tag) for tag in MULTI_TAGS]
    start = time.perf_counter_ns()
    for path in files:
        fastdicom.get_tags(str(path), tags)
    elapsed = time.perf_counter_ns() - start
    report("fastdicom: multiple tags", elapsed, len(files), len(tags))


def bench_directory_traversal(data_dir: pathlib.Path) -> None:
    tags = [as_fastdicom_tag(tag) for tag in MULTI_TAGS]
    start = time.perf_counter_ns()
    count = 0
    for path in data_dir.rglob("*"):
        if not path.is_file():
            continue
        fastdicom.get_tags(str(path), tags)
        count += 1
    elapsed = time.perf_counter_ns() - start
    report("fastdicom: directory traversal", elapsed, count, len(tags))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "data_dir", type=pathlib.Path,
        help="Directory of DICOM files, searched recursively")
    args = parser.parse_args()

    files = discover_files(args.data_dir)
    print(f"fastdicom benchmark: {len(files)} file(s) under {args.data_dir}\n")

    # DCMTK lazily initializes its data dictionary on first use; warm it up
    # so that cost doesn't land on whichever scenario happens to run first.
    fastdicom.get_tag(str(files[0]), as_fastdicom_tag(SINGLE_TAG))

    bench_single_tag(files)
    bench_multi_tag(files)
    bench_directory_traversal(args.data_dir)


if __name__ == "__main__":
    main()
