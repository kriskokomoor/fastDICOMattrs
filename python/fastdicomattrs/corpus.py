"""Probe a directory tree with the fastdicomattrs Python binding.

Run from a source checkout with::

    PYTHONPATH=python python -m fastdicomattrs.corpus /path/to/dicom

Pass ``--reference pydicom`` to additionally cross-check this library's
structural view of each file against pydicom's (an independent DICOM
implementation) -- see docs/roundtrip-contract.md "Known gaps". This
requires pydicom to be installed; it is a dev/test-only, optional
dependency of this one mode, not of the core build.

Pass ``--transform readme-example`` to instead apply README.md's own
worked-example policy (preserve Modality, remove PatientName, hash
PatientID, preserve SliceThickness, remove private elements, pass through
Pixel Data untouched) to every cleanly parsed file, write the result, and
check metadata-level readability with pydicom plus policy effects with this
library's reparse, plus a report of the
"quantify byte-level preservation of untouched content" criterion via
Structure.write_with_stats(). Also requires pydicom.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import statistics
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator

from . import FdsError, Structure, _FDS_STATUS_UNSUPPORTED, read

_PIXEL_DATA_TAG = (0x7FE0, 0x0010)


def _blocking_diagnostics(structure: Structure) -> list:
    """Diagnostics that make an input ineligible for transformation."""
    return [diagnostic for diagnostic in structure.diagnostics
            if diagnostic.severity != "info"]


@dataclass
class CorpusReport:
    files_discovered: int = 0
    parsed_successfully: int = 0
    success_with_warnings: int = 0
    failed: int = 0
    unsupported_transfer_syntax: int = 0
    explicit_vr_le: int = 0
    native_pixel_data: int = 0
    encapsulated_pixel_data: int = 0
    private_tags_present: int = 0
    sequences_present: int = 0
    maximum_nesting_depth: int = 0
    lossless_eligible: int = 0
    round_trip_identical: int = 0
    round_trip_failures: int = 0
    reference_compared: int = 0
    reference_matches: int = 0
    reference_mismatches: int = 0
    parse_times_ms: list[float] = field(default_factory=list, repr=False)
    errors: list[tuple[Path, str]] = field(default_factory=list, repr=False)

    @property
    def median_parse_ms(self) -> float:
        return statistics.median(self.parse_times_ms) if self.parse_times_ms else 0.0

    @property
    def p95_parse_ms(self) -> float:
        if not self.parse_times_ms:
            return 0.0
        ordered = sorted(self.parse_times_ms)
        return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]

    def __str__(self) -> str:
        count_rows = (
            ("Files discovered", self.files_discovered),
            ("Parsed successfully", self.parsed_successfully),
            ("Success with warnings", self.success_with_warnings),
            ("Failed", self.failed),
            ("Unsupported transfer syntax", self.unsupported_transfer_syntax),
            None,
            ("Explicit VR LE", self.explicit_vr_le),
            ("Native Pixel Data", self.native_pixel_data),
            ("Encapsulated Pixel Data", self.encapsulated_pixel_data),
            None,
            ("Private tags present", self.private_tags_present),
            ("Sequences present", self.sequences_present),
            ("Maximum nesting depth", self.maximum_nesting_depth),
            None,
            ("LOSSLESS eligible", self.lossless_eligible),
            ("Round-trip identical", self.round_trip_identical),
            ("Round-trip failures", self.round_trip_failures),
        )
        if self.reference_compared:
            count_rows = count_rows + (
                None,
                ("Reference-compared (pydicom)", self.reference_compared),
                ("Reference matches", self.reference_matches),
                ("Reference mismatches", self.reference_mismatches),
            )
        lines = ["" if row is None else f"{row[0]:<27}{row[1]:>7,}" for row in count_rows]
        lines.extend(("", f"{'Median parse':<29}{self.median_parse_ms:.2f} ms",
                      f"{'P95 parse':<29}{self.p95_parse_ms:.2f} ms"))
        return "\n".join(lines)


def discover_files(root: Path) -> Iterator[Path]:
    """Yield every regular file below *root* in deterministic path order."""
    if root.is_file():
        yield root
        return
    for directory, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        for filename in sorted(filenames):
            path = Path(directory, filename)
            if path.is_file():
                yield path


def _structure_features(structure: Structure) -> tuple[bool, bool, int]:
    has_private = False
    has_sequence = False
    maximum_depth = 0

    def visit(elements: Iterable, depth: int) -> None:
        nonlocal has_private, has_sequence, maximum_depth
        for element in elements:
            if element.tag[0] & 1:
                has_private = True
            if element.is_sequence:
                has_sequence = True
                sequence_depth = depth + 1
                maximum_depth = max(maximum_depth, sequence_depth)
                for item in element.items():
                    visit(item, sequence_depth)

    visit(structure, 0)
    return has_private, has_sequence, maximum_depth


def _pydicom_module():
    """Lazily imports pydicom. Raises ImportError with a clear message if
    it's not installed -- pydicom is an optional dependency of the
    --reference pydicom mode only, never of the core library or build.
    """
    try:
        import pydicom  # noqa: PLC0415 (deliberately lazy/optional)
    except ImportError as error:
        raise ImportError(
            "pydicom is required for --reference pydicom; install it with "
            "'pip install pydicom' (it is not a core dependency)"
        ) from error
    return pydicom


def _fds_tag_set(elements: Iterable) -> set[tuple[int, int]]:
    return {element.tag for element in elements}


def _pydicom_tag_set(elements: Iterable) -> set[tuple[int, int]]:
    return {(element.tag.group, element.tag.element) for element in elements
            if (element.tag.group, element.tag.element) != _PIXEL_DATA_TAG}


def _compare_structural(fds_elements: list, pydicom_elements: list, where: str) -> list[str]:
    """Compares one structural level (top-level, or one Sequence Item's
    elements) between this library's view and pydicom's. Returns a list of
    human-readable mismatch descriptions (empty means they agree at this
    level and everywhere recursed into below it).

    Only structure is compared -- tag sets and Sequence item counts/nesting
    -- not decoded values: this is a check that the two implementations
    agree on the *shape* of the object, which is what this library claims
    to get right (see docs/roundtrip-contract.md), not a claim about typed
    value decoding, which this library deliberately does not attempt.
    """
    fds_tags = _fds_tag_set(fds_elements)
    py_tags = _pydicom_tag_set(pydicom_elements)
    if fds_tags != py_tags:
        mismatches = []
        only_pydicom = py_tags - fds_tags
        only_fds = fds_tags - py_tags
        if only_pydicom:
            mismatches.append(f"{where}: tags pydicom sees but fastdicomattrs does not: "
                               f"{sorted(only_pydicom)}")
        if only_fds:
            mismatches.append(f"{where}: tags fastdicomattrs sees but pydicom does not: "
                               f"{sorted(only_fds)}")
        return mismatches

    mismatches = []
    py_by_tag = {(e.tag.group, e.tag.element): e for e in pydicom_elements}
    for element in fds_elements:
        if not element.is_sequence:
            continue
        py_element = py_by_tag.get(element.tag)
        if py_element is None or py_element.VR != "SQ":
            mismatches.append(f"{where} {element.tag}: fastdicomattrs sees a Sequence, "
                               f"pydicom does not")
            continue
        fds_items = list(element.items())
        py_items = list(py_element.value)
        if len(fds_items) != len(py_items):
            mismatches.append(f"{where} {element.tag}: item count differs "
                               f"(fastdicomattrs={len(fds_items)}, pydicom={len(py_items)})")
            continue
        for index, (fds_item, py_item) in enumerate(zip(fds_items, py_items)):
            mismatches.extend(_compare_structural(list(fds_item), list(py_item),
                                                   f"{where} {element.tag}[{index}]"))
    return mismatches


def _reference_compare(structure: Structure, path: Path) -> list[str]:
    """Cross-checks `structure` (already parsed by this library) against an
    independent pydicom parse of the same file. Pixel Data is excluded and
    never decoded on either side (stop_before_pixels=True) -- this is a
    structural check, not a pixel-decoding one.
    """
    pydicom = _pydicom_module()
    dataset = pydicom.dcmread(str(path), stop_before_pixels=True, force=False)
    pydicom_top_level = list(dataset.file_meta) + list(dataset)
    return _compare_structural(list(structure), pydicom_top_level, str(path))


def _files_identical(left: Path, right: Path, chunk_size: int = 1024 * 1024) -> bool:
    if left.stat().st_size != right.stat().st_size:
        return False
    with left.open("rb") as a, right.open("rb") as b:
        while True:
            left_chunk = a.read(chunk_size)
            right_chunk = b.read(chunk_size)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def probe(root: Path, reference: str | None = None) -> CorpusReport:
    if reference not in (None, "pydicom"):
        raise ValueError(f"unknown reference implementation {reference!r}; expected 'pydicom'")
    if reference == "pydicom":
        _pydicom_module()  # fail fast with a clear message if it's not installed

    root = root.expanduser()
    if not root.exists():
        raise FileNotFoundError(root)

    report = CorpusReport()
    paths = list(discover_files(root))
    report.files_discovered = len(paths)

    with tempfile.TemporaryDirectory(prefix="fastdicomattrs-corpus-") as temporary_dir:
        round_trip_path = Path(temporary_dir, "roundtrip.dcm")
        for path in paths:
            started = time.perf_counter_ns()
            try:
                structure = read(path, fidelity="lossless")
            except FdsError as error:
                report.parse_times_ms.append((time.perf_counter_ns() - started) / 1_000_000)
                if error.status == _FDS_STATUS_UNSUPPORTED:
                    report.unsupported_transfer_syntax += 1
                else:
                    report.failed += 1
                report.errors.append((path, str(error)))
                continue
            except (OSError, ValueError) as error:
                report.parse_times_ms.append((time.perf_counter_ns() - started) / 1_000_000)
                report.failed += 1
                report.errors.append((path, str(error)))
                continue

            report.parse_times_ms.append((time.perf_counter_ns() - started) / 1_000_000)
            try:
                warnings = [d for d in structure.diagnostics if d.severity != "info"]
                if warnings:
                    report.success_with_warnings += 1
                else:
                    report.parsed_successfully += 1

                if structure.is_explicit_vr and structure.is_little_endian:
                    report.explicit_vr_le += 1
                if structure.pixel_data_kind == "native":
                    report.native_pixel_data += 1
                elif structure.pixel_data_kind == "encapsulated":
                    report.encapsulated_pixel_data += 1

                has_private, has_sequence, depth = _structure_features(structure)
                report.private_tags_present += int(has_private)
                report.sequences_present += int(has_sequence)
                report.maximum_nesting_depth = max(report.maximum_nesting_depth, depth)

                # Byte-identical round-trip is only ever a meaningful expectation for a
                # *cleanly* parsed file (zero diagnostics): a warning means parsing
                # stopped early (e.g. a Pixel Data length that runs past a genuinely
                # truncated source file -- a real, corpus-discovered example, not a
                # hypothetical, is documented in docs/corpus-results.md), so the
                # output can never reproduce bytes the parser never had. Attempting
                # (and always "failing") round-trip on those files would conflate
                # "correctly detected malformed input" with an actual regression.
                if not warnings:
                    report.lossless_eligible += 1
                    try:
                        structure.write(round_trip_path)
                        if _files_identical(path, round_trip_path):
                            report.round_trip_identical += 1
                        else:
                            report.round_trip_failures += 1
                            report.errors.append((path, "round-trip output differs"))
                    except (FdsError, OSError) as error:
                        report.round_trip_failures += 1
                        report.errors.append((path, f"round-trip failed: {error}"))

                if reference == "pydicom":
                    try:
                        mismatches = _reference_compare(structure, path)
                        report.reference_compared += 1
                        if mismatches:
                            report.reference_mismatches += 1
                            report.errors.append((path, "; ".join(mismatches)))
                        else:
                            report.reference_matches += 1
                    except Exception as error:  # pydicom's exception surface isn't our contract
                        report.reference_compared += 1
                        report.reference_mismatches += 1
                        report.errors.append((path, f"pydicom reference read failed: {error}"))
            finally:
                structure.close()

    return report


@dataclass
class TransformReport:
    files_discovered: int = 0
    transformed: int = 0
    ineligible_diagnostics: int = 0
    failed: int = 0
    pydicom_valid: int = 0
    pydicom_invalid: int = 0
    total_source_backed_value_bytes: int = 0
    total_regenerated_value_bytes: int = 0
    errors: list[tuple[Path, str]] = field(default_factory=list, repr=False)

    @property
    def preserved_fraction(self) -> float:
        total = self.total_source_backed_value_bytes + self.total_regenerated_value_bytes
        return 1.0 if total == 0 else self.total_source_backed_value_bytes / total

    def __str__(self) -> str:
        count_rows = (
            ("Files discovered", self.files_discovered),
            ("Transformed", self.transformed),
            ("Ineligible (diagnostics)", self.ineligible_diagnostics),
            ("Failed", self.failed),
            None,
            ("pydicom metadata-readable", self.pydicom_valid),
            ("pydicom metadata-unreadable", self.pydicom_invalid),
        )
        lines = ["" if row is None else f"{row[0]:<27}{row[1]:>7,}" for row in count_rows]
        lines.extend((
            "",
            f"{'Source-backed value bytes':<27}{self.total_source_backed_value_bytes:>15,}",
            f"{'Regenerated value bytes':<27}{self.total_regenerated_value_bytes:>15,}",
            f"{'Preserved fraction':<27}{self.preserved_fraction:>14.2%}",
        ))
        return "\n".join(lines)


def _apply_readme_example_policy(structure: Structure) -> None:
    """Applies README.md's own worked-example policy, verbatim:

        preserve  (0008,0060) Modality
        remove    (0010,0010) PatientName
        hash      (0010,0020) PatientID
        preserve  (0018,0050) SliceThickness
        remove    private elements
        passthru  (7FE0,0010) Pixel Data

    Everything not named above is left untouched by construction -- this
    function only ever calls erase()/set_value() for the specific tags the
    policy names. "Preserve" and "passthru" therefore require no code: not
    touching an element already preserves it, and Pixel Data is never
    reachable through the mutation API at all.
    """
    structure.erase((0x0010, 0x0010))
    patient_id = structure.get((0x0010, 0x0020))
    if patient_id is not None:
        digest = hashlib.sha256(patient_id.value).hexdigest().encode("ascii")
        structure.set_value((0x0010, 0x0020), digest)
    structure.erase_private()


def _validate_transformed_output(written_path: Path) -> list[str]:
    """Checks that pydicom can read metadata through the point before Pixel
    Data and finds File Meta Information, and re-parsing with this library shows
    the policy was actually applied (no PatientName, no private elements)
    and tag order is still ascending. Returns problem descriptions (empty
    means valid).
    """
    problems = []
    pydicom = _pydicom_module()
    try:
        dataset = pydicom.dcmread(str(written_path), stop_before_pixels=True, force=False)
    except Exception as error:  # pydicom's exception surface isn't our contract
        problems.append(f"pydicom could not read the transformed output: {error}")
        return problems
    if len(list(dataset.file_meta)) == 0:
        problems.append("pydicom sees no File Meta Information in the transformed output")

    reparsed = read(written_path, fidelity="lossless")
    try:
        tags = [element.tag for element in reparsed]
        if tags != sorted(tags):
            problems.append("tag order is not ascending in the transformed output")
        if (0x0010, 0x0010) in reparsed:
            problems.append("PatientName is still present after the policy said to remove it")
        if any(group & 1 for group, _ in tags):
            problems.append("a private element survived erase_private()")
    finally:
        reparsed.close()
    return problems


def transform_and_validate(root: Path, policy: str = "readme-example") -> TransformReport:
    if policy != "readme-example":
        raise ValueError(f"unknown policy {policy!r}; expected 'readme-example'")
    _pydicom_module()  # fail fast with a clear message if it's not installed

    root = root.expanduser()
    if not root.exists():
        raise FileNotFoundError(root)

    report = TransformReport()
    paths = list(discover_files(root))
    report.files_discovered = len(paths)

    with tempfile.TemporaryDirectory(prefix="fastdicomattrs-transform-") as temporary_dir:
        out_path = Path(temporary_dir, "transformed.dcm")
        for path in paths:
            try:
                structure = read(path, fidelity="lossless")
            except (FdsError, OSError, ValueError) as error:
                report.failed += 1
                report.errors.append((path, f"read failed: {error}"))
                continue
            try:
                blocking_diagnostics = _blocking_diagnostics(structure)
                if blocking_diagnostics:
                    report.ineligible_diagnostics += 1
                    messages = "; ".join(d.message for d in blocking_diagnostics)
                    report.errors.append((path, f"not transformed because parsing produced "
                                                 f"diagnostics: {messages}"))
                    continue
                _apply_readme_example_policy(structure)
                stats = structure.write_with_stats(out_path)
                report.transformed += 1
                report.total_source_backed_value_bytes += stats.source_backed_value_bytes
                report.total_regenerated_value_bytes += stats.regenerated_value_bytes

                problems = _validate_transformed_output(out_path)
                if problems:
                    report.pydicom_invalid += 1
                    report.errors.append((path, "; ".join(problems)))
                else:
                    report.pydicom_valid += 1
            except (FdsError, OSError) as error:
                report.failed += 1
                report.errors.append((path, f"transform failed: {error}"))
            finally:
                structure.close()

    return report


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Probe and losslessly round-trip a DICOM corpus")
    parser.add_argument("root", type=Path, help="DICOM file or directory tree to probe")
    parser.add_argument("--details", action="store_true", help="print per-file failures to stderr")
    parser.add_argument("--strict", action="store_true",
                        help="exit nonzero when parsing or round-trip failures occur")
    parser.add_argument("--reference", choices=["pydicom"], default=None,
                        help="cross-check structural parsing against an independent DICOM "
                             "implementation (requires it to be installed)")
    parser.add_argument("--transform", choices=["readme-example"], default=None,
                        help="apply a transformation policy to every file, write the result, and "
                             "validate it with pydicom, instead of read-only probing (requires "
                             "pydicom to be installed)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.transform:
            report = transform_and_validate(args.root, policy=args.transform)
        else:
            report = probe(args.root, reference=args.reference)
    except (FileNotFoundError, NotADirectoryError, PermissionError) as error:
        print(f"fastdicomattrs.corpus: {error}", file=sys.stderr)
        return 2
    except ImportError as error:
        print(f"fastdicomattrs.corpus: {error}", file=sys.stderr)
        return 2
    print(report)
    if args.details:
        for path, message in report.errors:
            print(f"{path}: {message}", file=sys.stderr)
    if args.transform:
        if args.strict and (report.failed or report.ineligible_diagnostics
                            or report.pydicom_invalid):
            return 1
        return 0
    if args.strict and (report.failed or report.unsupported_transfer_syntax
                        or report.round_trip_failures or report.reference_mismatches):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
