"""Shared tag list and helpers used by both benchmark scripts.

Keeping the tag list and file discovery logic in one module guarantees
benchmark_fastdicom.py and benchmark_pydicom.py measure identical work.
"""

from __future__ import annotations

import pathlib

# (group, element) tuples. pydicom's specific_tags= accepts these directly;
# fastdicom's string API wants "gggg,eeee" hex, produced by as_fastdicom_tag().
SINGLE_TAG = (0x0010, 0x0010)  # Patient Name

MULTI_TAGS = [
    (0x0008, 0x0016),  # SOP Class UID
    (0x0008, 0x0018),  # SOP Instance UID
    (0x0008, 0x0060),  # Modality
    (0x0010, 0x0010),  # Patient Name
    (0x0010, 0x0020),  # Patient ID
    (0x0020, 0x000D),  # Study Instance UID
    (0x0020, 0x000E),  # Series Instance UID
]


def as_fastdicom_tag(tag: tuple[int, int]) -> str:
    group, element = tag
    return f"{group:04x},{element:04x}"


def discover_files(data_dir: pathlib.Path) -> list[pathlib.Path]:
    """Recursively list regular files under data_dir.

    DICOM files in the wild are frequently extensionless, so this matches
    on any regular file rather than filtering by suffix.
    """
    files = sorted(p for p in data_dir.rglob("*") if p.is_file())
    if not files:
        raise SystemExit(f"no files found under {data_dir}")
    return files


def report(name: str, elapsed_ns: int, num_files: int, tags_per_file: int) -> None:
    elapsed_s = elapsed_ns / 1e9
    total_tags = num_files * tags_per_file
    files_per_sec = num_files / elapsed_s if elapsed_s > 0 else float("inf")
    tags_per_sec = total_tags / elapsed_s if elapsed_s > 0 else float("inf")
    print(
        f"{name:<32} elapsed={elapsed_s:9.4f}s  "
        f"files/sec={files_per_sec:10.1f}  tags/sec={tags_per_sec:10.1f}"
    )
