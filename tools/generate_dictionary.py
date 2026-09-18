#!/usr/bin/env python3
"""Generates fastDICOMattrs' compiled-in DICOM data dictionary from the
pinned, official PS3.6 source (see thirdparty/dicom_standard/PROVENANCE.md).

This is an offline, dev-time tool. It is not part of the CMake build and no
normal user or CI job needs to run it -- the generated output
(src/dictionary_data.generated.cpp) is committed to the repository. Run it
only when adopting a new pinned PS3.6 edition.

Usage:
    python3 tools/generate_dictionary.py \\
        --source thirdparty/dicom_standard/PS3.6.xml \\
        --output src/dictionary_data.generated.cpp \\
        --report -

Design constraints this script exists to satisfy (see docs/architecture/
A1_ATTRS_V1_IMPLEMENTATION_PROGRESSION.md SS8 and the A1.1 task instructions):

  * consumes only the one pinned source artifact -- no network access, no
    dependency on pydicom/DCMTK/any other toolkit's data at generation time;
  * deterministic: identical input always produces byte-identical output;
  * rejects anything it does not understand (an unrecognized VR string, an
    unexpected row shape, a source row count that has drifted from what this
    script's logic was written against) rather than silently dropping rows;
  * represents ambiguous VRs (e.g. "US or SS") explicitly, never resolves
    them to a single guessed VR;
  * handles the three group-level repeating patterns (60xx/50xx/7Fxx)
    deliberately, as a small explicit rule table, not by expanding every
    concrete group into the main table;
  * explicitly excludes (and reports the count of) three narrow categories
    that do not belong in a data-element VR dictionary at all: structural
    delimiter pseudo-tags (FFFE,E000/E00D/E0DD), rows the standard itself
    lists with no VR at all (fully-retired placeholder tags kept reserved
    but otherwise blank), and a small set of fully-retired, obscure
    element-level wildcard patterns (see EXCLUDED_ELEMENT_PATTERNS below)
    that would require a third, more complex masking mechanism to support
    for zero real-world benefit.
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field

DOCBOOK_NS = "http://docbook.org/ns/docbook"
XML_NS = "http://www.w3.org/XML/1998/namespace"
NS = {"d": DOCBOOK_NS}
XMLID = f"{{{XML_NS}}}id"

# The table_6-1 "Registry of DICOM Data Elements" row count as of the pinned
# PS3.6 2026c edition. A generation run against a *different* edition is
# expected to see a different count -- that is not itself an error -- but a
# count this script's logic was not written against is exactly the kind of
# silent-data-loss risk SS12 of the task instructions asks to guard against.
# Bump this constant deliberately (and re-verify every category count below)
# when adopting a new edition; a mismatch aborts generation rather than
# silently proceeding against unreviewed data.
EXPECTED_TOTAL_ROWS = 5267

# Every VR string this generator is prepared to see, unambiguous case.
KNOWN_VRS = {
    "AE", "AS", "AT", "CS", "DA", "DS", "DT", "FL", "FD", "IS", "LO", "LT",
    "OB", "OD", "OF", "OL", "OV", "OW", "PN", "SH", "SL", "SQ", "SS", "ST",
    "SV", "TM", "UC", "UI", "UL", "UN", "UR", "US", "UT", "UV",
}

# Every explicitly-ambiguous VR string this generator is prepared to see, and
# the symbolic name it maps to in the generated VRAmbiguity enum. Discovering
# an ambiguous form not listed here is a generation error (see
# _classify_vr), not something to guess about.
AMBIGUOUS_VRS = {
    "US or SS": "USorSS",
    "OB or OW": "OBorOW",
    "US or OW": "USorOW",
    "US or SS or OW": "USorSSorOW",
}

# Structural pseudo-tags: PS3.6 lists these with VR "See Note" because they
# are pure encoding framing (item/sequence boundaries), not data elements --
# PS3.5 states plainly that they have no associated VR. The existing object
# model already treats these specially and structurally
# (Tag::is_item_or_delimiter(), include/fastdicomattrs/tag.hpp) with no
# dictionary involvement, so they are deliberately excluded here rather than
# given a fabricated dictionary entry.
STRUCTURAL_PSEUDO_TAGS = {(0xFFFE, 0xE000), (0xFFFE, 0xE00D), (0xFFFE, 0xE0DD)}

# Fully-retired, obscure element-level wildcard patterns (fixed group, "x"
# digits in the *element*), all from an early-1990s JPEG-adjacent
# compression mechanism retired in 2007. Handling these correctly would
# require a third, nibble-level masking mechanism distinct from the
# group-level masking the three MUST-support repeating-group patterns need,
# for tags with zero known real-world usage. Deliberately excluded --
# looking one of these tags up returns "not found", identical to how a
# genuinely unrecognized tag is treated, which is honest: this dictionary
# does not claim to resolve them, rather than silently mis-resolving them.
# Recorded here, by exact tag, so the exclusion is auditable and bounded
# rather than a vague "and some others too."
EXCLUDED_ELEMENT_PATTERNS = {
    "0020,31xx", "0028,04x0", "0028,04x1", "0028,04x2", "0028,04x3",
    "0028,08x0", "0028,08x2", "0028,08x3", "0028,08x4", "0028,08x8",
    "1000,xxx0", "1000,xxx1", "1000,xxx2", "1000,xxx3", "1000,xxx4",
    "1000,xxx5", "1010,xxxx",
}

# The three group-level repeating patterns this dictionary DOES support, and
# their cross-validated group ranges -- see PROVENANCE.md for the full
# reasoning (PS3.5 SS7.6's narrower cardinality text vs. the wider,
# tag-recognition range independently used by pydicom and DCMTK, both
# generated from the same PS3.6-2026c edition; the wider range is adopted
# deliberately, not by default).
REPEATING_GROUP_RANGES = {
    "50xx": (0x5000, 0x50FE),
    "60xx": (0x6000, 0x60FE),
    "7Fxx": (0x7F00, 0x7FFE),
}

HEX4 = re.compile(r"[0-9A-Fa-f]{4}")
TAG_RE = re.compile(r"\(([0-9A-Fa-fXx]{4}),([0-9A-Fa-fXx]{4})\)")
ZERO_WIDTH_SPACE = "​"


class GenerationError(Exception):
    """Raised for anything this generator does not understand well enough
    to proceed safely. Generation must fail loudly here, never drop a row
    silently -- see this module's docstring."""


@dataclass
class ExactEntry:
    group: int
    element: int
    vr: str  # one of KNOWN_VRS
    ambiguity: str | None  # None, or a value from AMBIGUOUS_VRS
    keyword: str
    retired: bool


@dataclass
class RepeatingRule:
    pattern: str  # "50xx" / "60xx" / "7Fxx"
    group_first: int
    group_last: int
    element: int
    vr: str
    ambiguity: str | None
    keyword: str
    retired: bool


@dataclass
class ExtractionResult:
    exact: list[ExactEntry] = field(default_factory=list)
    repeating: list[RepeatingRule] = field(default_factory=list)
    excluded_empty: int = 0
    excluded_delimiter: int = 0
    excluded_element_pattern: int = 0
    total_rows: int = 0


def _cell_text(td: ET.Element | None) -> str:
    if td is None:
        return ""
    return "".join(td.itertext()).replace(ZERO_WIDTH_SPACE, "").strip()


def _classify_vr(raw_vr: str, tag_str: str) -> tuple[str, str | None]:
    """Returns (vr, ambiguity_name_or_None). Raises GenerationError for
    anything not in KNOWN_VRS or AMBIGUOUS_VRS -- never guesses."""
    if raw_vr in KNOWN_VRS:
        return raw_vr, None
    if raw_vr in AMBIGUOUS_VRS:
        # "Unknown" here means fds::VR::Unknown, this codebase's own
        # catch-all sentinel for "no definitive VR" -- NOT the real,
        # distinct standard VR code "UN" ("Unknown" binary data). Callers
        # must check `ambiguity` before trusting this placeholder.
        return "Unknown", AMBIGUOUS_VRS[raw_vr]
    raise GenerationError(
        f"tag {tag_str}: unrecognized VR string {raw_vr!r} -- not in KNOWN_VRS or "
        f"AMBIGUOUS_VRS; refusing to guess. Add it deliberately if this is a genuine "
        f"new VR form in the pinned edition."
    )


def extract(source_path: str) -> ExtractionResult:
    tree = ET.parse(source_path)
    root = tree.getroot()
    table = None
    for t in root.iter(f"{{{DOCBOOK_NS}}}table"):
        if t.get(XMLID) == "table_6-1":
            table = t
            break
    if table is None:
        raise GenerationError("table_6-1 (Registry of DICOM Data Elements) not found in source")

    tbody = table.find("d:tbody", NS)
    rows = tbody.findall("d:tr", NS)
    result = ExtractionResult(total_rows=len(rows))

    if len(rows) != EXPECTED_TOTAL_ROWS:
        raise GenerationError(
            f"table_6-1 has {len(rows)} rows; this generator's category logic was "
            f"written against and verified for {EXPECTED_TOTAL_ROWS}. A different row "
            f"count means the pinned edition changed in a way this script has not been "
            f"re-verified against (rows added, removed, or restructured) -- refusing to "
            f"proceed silently. Re-run the full extraction/categorization/differential-"
            f"validation pass against the new edition, update EXPECTED_TOTAL_ROWS "
            f"deliberately, and update docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md."
        )

    seen_tags: set[tuple[int, int]] = set()

    for tr in rows:
        tds = tr.findall("d:td", NS)
        if len(tds) < 5:
            raise GenerationError(f"row with fewer than 5 cells: {ET.tostring(tr)[:200]!r}")
        tag_str = _cell_text(tds[0])
        name = _cell_text(tds[1])
        keyword = _cell_text(tds[2])
        vr_str = _cell_text(tds[3])
        note = _cell_text(tds[5]) if len(tds) > 5 else ""

        m = TAG_RE.match(tag_str)
        if not m:
            raise GenerationError(f"row's Tag cell does not match the expected pattern: {tag_str!r}")
        g_str, e_str = m.group(1), m.group(2)
        g_is_pattern = not HEX4.fullmatch(g_str)
        e_is_pattern = not HEX4.fullmatch(e_str)

        if g_is_pattern and e_is_pattern:
            raise GenerationError(f"tag {tag_str}: both group and element are patterns -- unhandled shape")

        if not vr_str:
            # Fully-retired placeholder rows with no recorded VR at all
            # (name/keyword/VR/VM all blank) -- nothing to resolve.
            result.excluded_empty += 1
            continue

        retired = note.startswith("RET")  # covers "RET", "RET (YYYY[x])", "RET (YYYY) - See Note"
        # DICONDE/DICOS-noted entries are companion-standard elements, not
        # retired -- explicitly not treated as retired.

        if not g_is_pattern and not e_is_pattern:
            g, e = int(g_str, 16), int(e_str, 16)
            if (g, e) in STRUCTURAL_PSEUDO_TAGS:
                result.excluded_delimiter += 1
                continue
            if (g, e) in seen_tags:
                raise GenerationError(f"duplicate exact tag ({g:04X},{e:04X}) -- source data integrity issue")
            seen_tags.add((g, e))
            vr, ambiguity = _classify_vr(vr_str, tag_str)
            result.exact.append(ExactEntry(g, e, vr, ambiguity, keyword, retired))
            continue

        if g_is_pattern and not e_is_pattern:
            if g_str not in REPEATING_GROUP_RANGES:
                raise GenerationError(
                    f"tag {tag_str}: unrecognized group-repeating pattern {g_str!r} -- "
                    f"not in REPEATING_GROUP_RANGES; refusing to guess its valid range."
                )
            group_first, group_last = REPEATING_GROUP_RANGES[g_str]
            vr, ambiguity = _classify_vr(vr_str, tag_str)
            result.repeating.append(RepeatingRule(
                g_str, group_first, group_last, int(e_str, 16), vr, ambiguity, keyword, retired
            ))
            continue

        # e_is_pattern and not g_is_pattern
        if tag_str.strip("()") not in EXCLUDED_ELEMENT_PATTERNS:
            raise GenerationError(
                f"tag {tag_str}: unrecognized element-repeating pattern -- not in "
                f"EXCLUDED_ELEMENT_PATTERNS. Either add it there deliberately (with "
                f"rationale) or implement element-level masking support; refusing to "
                f"silently drop an unrecognized pattern shape."
            )
        result.excluded_element_pattern += 1

    return result


def _cpp_string_literal(s: str) -> str:
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def generate_cpp(result: ExtractionResult, source_sha256: str, edition: str) -> str:
    exact_sorted = sorted(result.exact, key=lambda e: (e.group << 16) | e.element)

    # Deterministic, order-stable string pool: each distinct keyword appears
    # once, at the offset of its first occurrence in tag order.
    pool: list[str] = []
    pool_offset: dict[str, int] = {}
    running_offset = 0

    def intern(keyword: str) -> tuple[int, int]:
        nonlocal running_offset
        if keyword not in pool_offset:
            pool_offset[keyword] = running_offset
            pool.append(keyword)
            running_offset += len(keyword.encode("utf-8"))
        return pool_offset[keyword], len(keyword.encode("utf-8"))

    lines: list[str] = []
    lines.append("// GENERATED FILE -- DO NOT EDIT BY HAND.")
    lines.append("//")
    lines.append(f"// Generated by tools/generate_dictionary.py from thirdparty/dicom_standard/PS3.6.xml")
    lines.append(f"// Source edition: {edition}")
    lines.append(f"// Source SHA-256: {source_sha256}")
    # Deliberately no generation timestamp here: this file's content must be
    # byte-for-byte reproducible from the pinned source alone (see this
    # script's docstring and A1.1's determinism requirement). Generation
    # provenance (when/who) lives in git history and the --report output,
    # neither of which this determinism guarantee applies to.
    lines.append(f"// Exact entries: {len(exact_sorted)}")
    lines.append(f"// Repeating-group rules: {len(result.repeating)}")
    lines.append(f"// Excluded (no VR recorded, retired placeholder): {result.excluded_empty}")
    lines.append(f"// Excluded (structural delimiter pseudo-tag): {result.excluded_delimiter}")
    lines.append(f"// Excluded (obscure retired element-wildcard pattern): {result.excluded_element_pattern}")
    lines.append(f"// Source table_6-1 total rows: {result.total_rows}")
    lines.append("//")
    lines.append("// Regenerate with: python3 tools/generate_dictionary.py")
    lines.append("// See docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md for full provenance.")
    lines.append("")
    lines.append('#include "dictionary_data.generated.hpp"')
    lines.append("")
    lines.append("namespace fds::dictionary::detail {")
    lines.append("")

    # String pool
    pool_str = "".join(pool)
    for k in exact_sorted:
        intern(k.keyword)
    for r in result.repeating:
        intern(r.keyword)
    pool_str = "".join(pool)

    lines.append(f"const char kKeywordPool[] = {_cpp_string_literal(pool_str)};")
    lines.append("")

    def vr_expr(vr: str) -> str:
        return f"VR::{vr}"

    def ambiguity_expr(a: str | None) -> str:
        return f"VRAmbiguity::{a}" if a else "VRAmbiguity::None"

    lines.append(f"const RawEntry kExactEntries[{len(exact_sorted)}] = {{")
    for e in exact_sorted:
        off, length = pool_offset[e.keyword], len(e.keyword.encode("utf-8"))
        tag_u32 = (e.group << 16) | e.element
        lines.append(
            f"  {{0x{tag_u32:08X}u, {vr_expr(e.vr)}, {ambiguity_expr(e.ambiguity)}, "
            f"{'true' if e.retired else 'false'}, {off}u, {length}u}},"
        )
    lines.append("};")
    lines.append("")

    lines.append(f"const RepeatingRule kRepeatingRules[{len(result.repeating)}] = {{")
    for r in result.repeating:
        off, length = pool_offset[r.keyword], len(r.keyword.encode("utf-8"))
        lines.append(
            f"  {{0x{r.group_first:04X}u, 0x{r.group_last:04X}u, 0x{r.element:04X}u, "
            f"{vr_expr(r.vr)}, {ambiguity_expr(r.ambiguity)}, "
            f"{'true' if r.retired else 'false'}, {off}u, {length}u}},  // {r.pattern}"
        )
    lines.append("};")
    lines.append("")
    lines.append(f"const std::size_t kExactEntryCount = {len(exact_sorted)};")
    lines.append(f"const std::size_t kRepeatingRuleCount = {len(result.repeating)};")
    lines.append("")
    lines.append("}  // namespace fds::dictionary::detail")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default="thirdparty/dicom_standard/PS3.6.xml")
    parser.add_argument("--output", default="src/dictionary_data.generated.cpp")
    parser.add_argument("--report", default=None, help="write a plain-text summary here, or '-' for stdout")
    args = parser.parse_args()

    import hashlib
    with open(args.source, "rb") as f:
        source_bytes = f.read()
    source_sha256 = hashlib.sha256(source_bytes).hexdigest()

    tree = ET.parse(args.source)
    edition = tree.getroot().findtext("d:subtitle", namespaces=NS) or "(unknown edition)"

    try:
        result = extract(args.source)
    except GenerationError as exc:
        print(f"generation FAILED: {exc}", file=sys.stderr)
        return 1

    output = generate_cpp(result, source_sha256, edition)
    with open(args.output, "w") as f:
        f.write(output)

    report_lines = [
        f"edition: {edition}",
        f"source sha256: {source_sha256}",
        f"table_6-1 total rows: {result.total_rows}",
        f"exact entries: {len(result.exact)}",
        f"repeating-group rules: {len(result.repeating)}",
        f"  by pattern: " + ", ".join(
            f"{p}={sum(1 for r in result.repeating if r.pattern == p)}"
            for p in REPEATING_GROUP_RANGES
        ),
        f"ambiguous entries (exact + repeating): "
        f"{sum(1 for e in result.exact if e.ambiguity)} + "
        f"{sum(1 for r in result.repeating if r.ambiguity)}",
        f"retired entries (exact + repeating): "
        f"{sum(1 for e in result.exact if e.retired)} + "
        f"{sum(1 for r in result.repeating if r.retired)}",
        f"excluded (empty-VR retired placeholder): {result.excluded_empty}",
        f"excluded (structural delimiter pseudo-tag): {result.excluded_delimiter}",
        f"excluded (obscure element-wildcard pattern): {result.excluded_element_pattern}",
        f"output: {args.output}",
    ]
    report = "\n".join(report_lines) + "\n"
    if args.report == "-":
        sys.stdout.write(report)
    elif args.report:
        with open(args.report, "w") as f:
            f.write(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
