#!/usr/bin/env python3
"""Generates fastDICOMattrs' compiled-in single-byte character-repertoire
decode AND encode tables for A1.5/A1.6 (see docs/architecture/
A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md and
A1_6_CHARACTER_SET_REENCODING_REPORT.md).

The encode (codepoint -> byte) tables are generated in the same run, from
the same per-byte decode results, as a deterministic inversion -- never a
second, independently-authored mapping that could drift out of sync with
the decode tables. Injectivity (no two bytes mapping to the same code
point) is verified for every repertoire during generation; a collision
would make simple inversion ambiguous, and none of the ten repertoires has
one (each is a real 1:1 single-byte character set).

This is an offline, dev-time tool, not part of the CMake build -- the
generated output (src/charset_tables.generated.cpp) is committed to the
repository, exactly like tools/generate_dictionary.py's relationship to
src/dictionary_data.generated.cpp.

Source of truth: unlike the PS3.6 dictionary generator (which pins one
DICOM-specific XML artifact and deliberately avoids any toolkit
dependency), there is no DICOM-specific source for byte-to-Unicode mappings
of the ten PS3.5 single-byte character repertoires -- they *are* the
ISO/IEC 8859 family (plus TIS 620-2533 for Thai), standards that predate
and sit outside DICOM entirely and have been frozen, unchanged, for
decades. This script uses Python's own standard-library `codecs` module
(not a third-party DICOM toolkit) as the reference implementation of those
frozen ISO/IEC standards -- a deliberate, documented departure from A1.1's
"one pinned artifact" discipline, justified because no equivalent
DICOM-specific pinned source exists for this data and the underlying
standards do not change. Self-verified below against a handful of
reference code points as a sanity check on the generation pipeline itself
(catches a wrong codec name, an off-by-one, a copy/paste error) -- these
checks query the same Python codecs the tables are generated from, so
they are an internal-consistency check, not an independent second source;
genuine independent validation (pydicom, DCMTK) is described in the A1.5
freeze report, not here.

Usage:
    python3 tools/generate_charset_tables.py \\
        --output src/charset_tables.generated.cpp \\
        --report -
"""

from __future__ import annotations

import argparse
import codecs
import platform
import sys
from dataclasses import dataclass


# DICOM defined term (PS3.5 C.12.1.1.2 / PS3.3 Annex C.12) -> Python codec
# name. Each entry is one of PS3.5's ten single-byte repertoires. The
# non-code-extension form ("ISO_IR nnn") and the code-extension form
# ("ISO 2022 IR nnn") both refer to the *same* underlying repertoire table
# -- only how they may legally be combined in a (0008,0005) declaration
# differs, which is a parser-layer (not table-layer) concern.
REPERTOIRES = [
    # (repertoire_id, dicom_number, python_codec, description)
    ("Latin1", "100", "iso8859_1", "Western Europe (ISO 8859-1)"),
    ("Latin2", "101", "iso8859_2", "Central Europe (ISO 8859-2)"),
    ("Latin3", "109", "iso8859_3", "South Europe (ISO 8859-3)"),
    ("Latin4", "110", "iso8859_4", "North Europe (ISO 8859-4)"),
    ("Cyrillic", "144", "iso8859_5", "Cyrillic (ISO 8859-5)"),
    ("Arabic", "127", "iso8859_6", "Arabic (ISO 8859-6)"),
    ("Greek", "126", "iso8859_7", "Greek (ISO 8859-7)"),
    ("Hebrew", "138", "iso8859_8", "Hebrew (ISO 8859-8)"),
    ("Latin5", "148", "iso8859_9", "Turkish (ISO 8859-9)"),
    ("Thai", "166", "tis_620", "Thai (TIS 620-2533)"),
]

# Reference code points, used purely as a self-verification sanity check
# on the generation pipeline (catches a wrong codec name or an off-by-one)
# -- queried against the same Python codecs the tables themselves are
# generated from, so this is an internal-consistency check, not an
# independent second source the way A1.1's pydicom full-table diff was
# (pydicom's dictionary is a genuinely separate implementation from the
# pinned PS3.6 XML; these checks are not).
REFERENCE_CHECKS = {
    "Latin1": [(0xE9, 0x00E9), (0xC9, 0x00C9)],  # 'é' / 'É'
    "Cyrillic": [(0xD0, 0x0430)],  # CYRILLIC SMALL LETTER A
    "Greek": [(0xE1, 0x03B1)],  # GREEK SMALL LETTER ALPHA
    "Hebrew": [(0xE0, 0x05D0)],  # HEBREW LETTER ALEF
    "Arabic": [(0xC1, 0x0621)],  # ARABIC LETTER HAMZA
}


@dataclass
class RepertoireTable:
    repertoire_id: str
    dicom_number: str
    python_codec: str
    description: str
    # 128 entries for bytes 0x80-0xFF; 0 means "undefined in this
    # repertoire" (a safe sentinel -- none of these repertoires map any
    # byte to U+0000).
    high_half: list[int]
    # Deterministic inversion of high_half: (codepoint, byte) pairs for
    # every defined high-half byte, sorted ascending by codepoint (for
    # binary-search encode lookup). Excludes the 0x00-0x7F range, which
    # A1.6's encoder handles generically as an identity mapping.
    reverse: list[tuple[int, int]]


def build_table(repertoire_id: str, dicom_number: str, python_codec: str,
                 description: str) -> RepertoireTable:
    high_half: list[int] = []
    for byte in range(0x80, 0x100):
        try:
            decoded = bytes([byte]).decode(python_codec)
            codepoint = ord(decoded) if len(decoded) == 1 else 0
        except UnicodeDecodeError:
            codepoint = 0
        if codepoint == 0 and byte != 0:
            codepoint = 0  # explicit: unmapped
        high_half.append(codepoint)

    # Self-check: bytes 0x00-0x7F must be ASCII-identical for every
    # ISO-8859-family / TIS-620 repertoire -- verified directly, not
    # assumed, exactly the discipline this generator's own docstring
    # promises.
    for byte in range(0x00, 0x80):
        decoded = bytes([byte]).decode(python_codec)
        if ord(decoded) != byte:
            raise RuntimeError(
                f"{repertoire_id} ({python_codec}): byte 0x{byte:02X} does not decode "
                f"identically to its own value (got U+{ord(decoded):04X}) -- low-range "
                "ASCII-compatibility assumption violated, aborting generation"
            )

    seen_codepoints: dict[int, int] = {}
    for offset, codepoint in enumerate(high_half):
        if codepoint == 0:
            continue
        byte = 0x80 + offset
        if codepoint in seen_codepoints:
            raise RuntimeError(
                f"{repertoire_id} ({python_codec}): bytes 0x{seen_codepoints[codepoint]:02X} "
                f"and 0x{byte:02X} both decode to U+{codepoint:04X} -- not injective, cannot "
                "invert deterministically, aborting generation"
            )
        seen_codepoints[codepoint] = byte
    reverse = sorted(seen_codepoints.items())

    return RepertoireTable(repertoire_id, dicom_number, python_codec, description, high_half,
                            reverse)


def verify_table(table: RepertoireTable) -> None:
    for byte, expected_codepoint in REFERENCE_CHECKS.get(table.repertoire_id, []):
        actual = table.high_half[byte - 0x80]
        if actual != expected_codepoint:
            raise RuntimeError(
                f"{table.repertoire_id}: byte 0x{byte:02X} decoded to U+{actual:04X}, "
                f"expected U+{expected_codepoint:04X} -- reference check failed"
            )


def render_cpp(tables: list[RepertoireTable]) -> str:
    lines = []
    lines.append("// GENERATED FILE -- do not edit by hand.")
    lines.append("// Produced by tools/generate_charset_tables.py -- see that script's")
    lines.append("// docstring for provenance (Python's stdlib `codecs`, not a DICOM-specific")
    lines.append("// pinned artifact -- these are frozen ISO/IEC 8859 / TIS 620 standards,")
    lines.append("// not DICOM data) and docs/architecture/")
    lines.append("// A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md.")
    lines.append("//")
    lines.append("// Deterministic: regenerating this file from the same Python version and")
    lines.append("// the same REPERTOIRES table above always produces byte-identical output --")
    lines.append("// no timestamp, machine path, hostname, or other volatile content is")
    lines.append("// embedded (see tools/verify_charset_tables_deterministic.py). The exact")
    lines.append("// Python version used for the committed table is recorded once, in this")
    lines.append("// script's own commit message and the A1.5 freeze report, not per-generation")
    lines.append("// here, so re-running this script never changes the file's bytes on its own.")
    lines.append("")
    lines.append('#include "charset_tables.generated.hpp"')
    lines.append("")
    lines.append("namespace fds::charset::detail {")
    lines.append("")
    for t in tables:
        lines.append(f"// {t.description} -- DICOM ISO_IR {t.dicom_number} / "
                      f"ISO 2022 IR {t.dicom_number}. Python codec: {t.python_codec}.")
        lines.append(f"const char32_t k{t.repertoire_id}High[128] = {{")
        for i in range(0, 128, 8):
            row = ", ".join(f"0x{v:04X}" for v in t.high_half[i:i + 8])
            lines.append(f"    {row},")
        lines.append("};")
        lines.append("")
        lines.append(f"const std::size_t k{t.repertoire_id}ReverseCount = {len(t.reverse)};")
        lines.append(f"const ReverseEntry k{t.repertoire_id}Reverse[{len(t.reverse)}] = {{")
        for i in range(0, len(t.reverse), 4):
            chunk = t.reverse[i:i + 4]
            row = ", ".join(f"{{0x{cp:04X}, 0x{byte:02X}}}" for cp, byte in chunk)
            lines.append(f"    {row},")
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace fds::charset::detail")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--report", default=None)
    args = parser.parse_args()

    tables = []
    for repertoire_id, dicom_number, python_codec, description in REPERTOIRES:
        table = build_table(repertoire_id, dicom_number, python_codec, description)
        verify_table(table)
        tables.append(table)

    output = render_cpp(tables)
    with open(args.output, "w") as f:
        f.write(output)

    report_lines = [
        f"Generated {len(tables)} single-byte repertoire tables to {args.output}",
        f"Python: {platform.python_version()} ({sys.implementation.name})",
    ]
    for t in tables:
        undefined = sum(1 for v in t.high_half if v == 0)
        report_lines.append(
            f"  {t.repertoire_id:10s} ISO_IR {t.dicom_number:4s} codec={t.python_codec:12s} "
            f"undefined_high_bytes={undefined} reverse_entries={len(t.reverse)}"
        )
    report = "\n".join(report_lines)
    if args.report == "-" or args.report is None:
        print(report)
    else:
        with open(args.report, "w") as f:
            f.write(report + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
