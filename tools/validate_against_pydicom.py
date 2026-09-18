#!/usr/bin/env python3
"""Full-table differential validation of the generated dictionary against
pydicom's own DICOM data dictionary. See docs/architecture/
A1_1_DICTIONARY_SUBSTRATE_REPORT.md for the full results and triage of every
disagreement this script finds.

This is an offline, optional, dev-time tool (like corpus.py's `--reference
pydicom` mode) -- it requires `pip install pydicom` but nothing in the
runtime library or its normal build/test path depends on it. Not run by
CI or ctest.

Usage:
    python3 tools/validate_against_pydicom.py --source thirdparty/dicom_standard/PS3.6.xml

pydicom is not treated as automatically authoritative. Every disagreement
this script finds must be triaged by hand against the pinned PS3.6 source
(and, where useful, DCMTK's dictionary) -- see the generated report's
"UNEXPLAINED" section, which should always be empty at freeze time.
"""

from __future__ import annotations

import argparse
import sys

# Reuse the exact same extraction logic the generator uses, so this
# validator is comparing against precisely what was actually generated --
# not a second, independently-drifting re-implementation of the same
# parsing logic.
sys.path.insert(0, "tools")
from generate_dictionary import extract, GenerationError, REPEATING_GROUP_RANGES  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default="thirdparty/dicom_standard/PS3.6.xml")
    args = parser.parse_args()

    try:
        from pydicom.datadict import dictionary_VR, dictionary_keyword
        import pydicom
    except ImportError:
        print("pydicom is not installed -- pip install pydicom to run this validator.",
              file=sys.stderr)
        return 2

    try:
        result = extract(args.source)
    except GenerationError as exc:
        print(f"extraction failed: {exc}", file=sys.stderr)
        return 1

    print(f"pydicom version: {pydicom.__version__}")
    print(f"exact entries to check: {len(result.exact)}")

    # DICONDE/DICOS-scope and edition-recency are the two known, explained
    # categories of "pydicom doesn't have this tag at all" -- see
    # PROVENANCE.md and the A1.1 report for why both are expected, not bugs.
    # This script does not try to auto-detect which bucket a miss belongs
    # to (that requires reading the tag's own note field, already discarded
    # by `extract`'s ExactEntry -- deliberately, since the runtime dictionary
    # doesn't need it); it reports every miss and every VR mismatch, and the
    # human triage in the A1.1 report explains the pattern.
    from generate_dictionary import AMBIGUOUS_VRS
    ambiguity_to_str = {v: k for k, v in AMBIGUOUS_VRS.items()}

    n_checked = 0
    n_missing = 0
    n_vr_mismatch = 0
    mismatches = []
    missing = []

    for e in result.exact:
        tag_int = (e.group << 16) | e.element
        n_checked += 1
        try:
            py_vr = dictionary_VR(tag_int)
        except KeyError:
            n_missing += 1
            missing.append((e.group, e.element, e.keyword))
            continue
        # Reconstruct the original VR string for comparison: for an
        # ambiguous entry, e.vr is the "Unknown" placeholder (see
        # generate_dictionary._classify_vr) and the real comparable string
        # ("US or SS" etc.) lives in e.ambiguity instead.
        our_vr_str = ambiguity_to_str[e.ambiguity] if e.ambiguity else e.vr
        if our_vr_str != py_vr:
            n_vr_mismatch += 1
            mismatches.append((e.group, e.element, e.keyword, our_vr_str, py_vr))

    print(f"checked: {n_checked}")
    print(f"pydicom-missing (fastDICOMattrs has, pydicom does not): {n_missing}")
    print(f"VR mismatches: {n_vr_mismatch}")

    if mismatches:
        print("\n=== VR MISMATCHES (must be triaged; expect none at freeze) ===")
        for g, e2, kw, ours, theirs in mismatches:
            print(f"  ({g:04X},{e2:04X}) {kw}: ours={ours!r} pydicom={theirs!r}")

    if missing:
        print(f"\n=== pydicom-missing entries ({len(missing)} total, see report for bucket triage) ===")
        by_group = {}
        for g, e2, kw in missing:
            by_group.setdefault(g, []).append((e2, kw))
        for g in sorted(by_group):
            print(f"  group {g:04X}: {len(by_group[g])} entries")

    print("\n=== repeating-group boundary cross-check against pydicom ===")
    rule_by_pattern: dict[str, list] = {}
    for r in result.repeating:
        rule_by_pattern.setdefault(r.pattern, []).append(r)
    boundary_mismatches = 0
    for pattern, (first, last) in REPEATING_GROUP_RANGES.items():
        rules = rule_by_pattern.get(pattern, [])
        sample_element = rules[0].element if rules else 0x0010
        for label, group in (("first", first), ("last", last), ("out-of-range", last + 2)):
            tag_int = (group << 16) | sample_element
            try:
                py_vr = dictionary_VR(tag_int)
                py_found = True
            except KeyError:
                py_vr = None
                py_found = False
            expect_found = label != "out-of-range"
            status = "OK" if py_found == expect_found else "MISMATCH"
            if status == "MISMATCH":
                boundary_mismatches += 1
            print(f"  {pattern} {label} group {group:04X},{sample_element:04X}: "
                  f"pydicom={'found ' + str(py_vr) if py_found else 'not found'} [{status}]")

    print(f"\nTOTAL unexplained VR mismatches: {n_vr_mismatch}")
    print(f"TOTAL repeating-group boundary mismatches: {boundary_mismatches}")
    return 0 if (n_vr_mismatch == 0 and boundary_mismatches == 0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
