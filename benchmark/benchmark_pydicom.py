#!/usr/bin/env python3
"""Benchmark pydicom against a directory of DICOM files.

Usage:
    python benchmark_pydicom.py DATA_DIR

Reads use stop_before_pixels=True and specific_tags= so pydicom decodes
only the requested attributes, not the full dataset or pixel data -- the
same scope fastdicom's ReadOptions restricts to. See benchmark_fastdicom.py
for the equivalent fastdicom benchmark, run against the same directory and
the same tag list (common.py) for a fair comparison.
"""

from __future__ import annotations

import argparse
import pathlib
import time

import pydicom

from common import MULTI_TAGS, SINGLE_TAG, discover_files, report


def bench_single_tag(files: list[pathlib.Path]) -> None:
    start = time.perf_counter_ns()
    for path in files:
        dataset = pydicom.dcmread(
            path, stop_before_pixels=True, specific_tags=[SINGLE_TAG])
        dataset.get(SINGLE_TAG)
    elapsed = time.perf_counter_ns() - start
    report("pydicom: single tag", elapsed, len(files), 1)


def bench_multi_tag(files: list[pathlib.Path]) -> None:
    start = time.perf_counter_ns()
    for path in files:
        dataset = pydicom.dcmread(
            path, stop_before_pixels=True, specific_tags=MULTI_TAGS)
        for tag in MULTI_TAGS:
            dataset.get(tag)
    elapsed = time.perf_counter_ns() - start
    report("pydicom: multiple tags", elapsed, len(files), len(MULTI_TAGS))


def bench_directory_traversal(data_dir: pathlib.Path) -> None:
    start = time.perf_counter_ns()
    count = 0
    for path in data_dir.rglob("*"):
        if not path.is_file():
            continue
        dataset = pydicom.dcmread(
            path, stop_before_pixels=True, specific_tags=MULTI_TAGS)
        for tag in MULTI_TAGS:
            dataset.get(tag)
        count += 1
    elapsed = time.perf_counter_ns() - start
    report("pydicom: directory traversal", elapsed, count, len(MULTI_TAGS))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "data_dir", type=pathlib.Path,
        help="Directory of DICOM files, searched recursively")
    args = parser.parse_args()

    files = discover_files(args.data_dir)
    print(f"pydicom benchmark: {len(files)} file(s) under {args.data_dir}\n")

    # Symmetric warm-up with benchmark_fastdicom.py's, so neither script's
    # first scenario absorbs one-time import/initialization costs.
    pydicom.dcmread(
        files[0], stop_before_pixels=True, specific_tags=[SINGLE_TAG])

    bench_single_tag(files)
    bench_multi_tag(files)
    bench_directory_traversal(args.data_dir)


if __name__ == "__main__":
    main()
