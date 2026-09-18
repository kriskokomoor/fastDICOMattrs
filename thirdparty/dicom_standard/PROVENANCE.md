# PS3.6 Source Provenance

> **Note:** as of the public-release remediation, `PS3.6.xml` itself is no longer committed
> alongside this file — see [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) for why and
> for the maintainer-only re-acquisition workflow (`tools/fetch_ps3.6_source.py`). Every fact
> below remains exactly as originally recorded and is what that workflow verifies against; nothing
> here was weakened or removed.

## Primary pinned artifact

| Field | Value |
|---|---|
| File | `PS3.6.xml` |
| Source | NEMA DICOM PS3.6 "Data Dictionary", official docbook XML |
| Retrieval URL | `https://dicom.nema.org/medical/dicom/current/source/docbook/part06/part06.xml` |
| Retrieved | 2026-09-13 |
| HTTP `Last-Modified` at retrieval | Fri, 19 Jun 2026 02:25:07 GMT |
| Edition, per the document's own `<subtitle>` | **DICOM PS3.6 2026c — Data Dictionary** |
| File size | 9,665,786 bytes |
| SHA-256 | `ff1dcdfb557d57db96420614fcaf6d739bb76aa74b73eba77f367be9fab0be3e` |

This file is committed byte-for-byte, untouched, exactly as retrieved — no trimming or
normalization was applied to the committed artifact itself, so its SHA-256 is a complete, direct
proof of provenance requiring no separate transformation record. `tools/generate_dictionary.py`
reads this exact file and performs all extraction/normalization at generation time, in one
deterministic, auditable pass (see that script for the extraction logic).

Only `chapter_6`'s `table_6-1` ("Registry of DICOM Data Elements") is consumed by the generator.
The file also contains chapters 7–9 and Annexes A–B (File Meta registry, Command registry, UID
registry, IOD context group tables) — none of that content is read or used; it remains in the
committed file only because splitting it out would itself be an unaudited transformation of the
official artifact, which this provenance record specifically avoids.

## Supplementary facts used for repeating-group range determination

`table_6-1` documents *which element numbers* participate in each of the three repeating-group
tag patterns (`50xx`, `60xx`, `7Fxx`) but does not, by itself, state the exact valid *group number*
range for each pattern in machine-readable form. Two supplementary sources were consulted,
specifically for this one fact, and are recorded here rather than silently assumed:

**PS3.5 §7.6 "Repeating Groups"** (normative text, same edition — see below), states explicitly:
> "Repeating Groups shall only be allowed in the even numbered Groups 6000-601E."

This is a *cardinality* statement (a compliant sender may create at most 16 Overlay Planes) rather
than a *tag-recognition* statement. It does not by itself say what a compliant reader should do
with a structurally-identical but numerically-higher group (e.g. `6020`) that a non-conformant
sender might still produce.

| Field | Value |
|---|---|
| Source | `PS3.5.xml`, same retrieval date/method as `PS3.6.xml` |
| Retrieval URL | `https://dicom.nema.org/medical/dicom/current/source/docbook/part05/part05.xml` |
| File size | 1,021,881 bytes |
| SHA-256 | `4dfd7b8cbc7c368b7cf03e9c4b8a0773bed91266fcdb0bbf8ed634b7287cacca` |
| Cited paragraph | `xml:id="para_e2e0f651-e42c-4404-8d97-e31b43de0b09"`, section `xml:id="sect_7.6"` |

**Cross-check against two independent, mature implementations** — both pydicom (3.0.2,
`pydicom.datadict.dictionary_VR`) and DCMTK (`dcmdata/data/dicom.dic`, retrieved from
`https://raw.githubusercontent.com/DCMTK/dcmtk/master/dcmdata/data/dicom.dic`,
SHA-256 `700f43b759eea9aa93b08478af8011027c87acffe5372c494f1f8ff99d439e7a`, whose own header states
it was "Generated automatically from DICOM PS 3.6-2026c and PS 3.7-2026c" — the same edition as
this project's own pinned source) — independently resolve the *practical, tag-recognition* range
for all three patterns as the full even-numbered byte range (`x000`–`xFFE`, i.e. every even group
from the pattern's base through base+`0xFE`), not the narrower cardinality-motivated `601E` bound.
Confirmed directly against pydicom (not merely inferred from documentation) for exact boundary
values: `6000`/`60FE` resolve, `6001`/`60FF`/`6100` do not; `5000`/`50FE` resolve, `5100` does not;
`7F00`/`7FFE` resolve; `8000` does not.

**Resolution adopted by this generator:** the wider, cross-validated practical range
(`{6000,5000,7F00}`–`{60FE,50FE,7FFE}`, even groups only), not the narrower PS3.5 §7.6 cardinality
text. Rationale: a reader that rejected a structurally well-formed but numerically-out-of-spec
Overlay/Curve/Variable-Pixel-Data element (which a non-conformant but not malicious sender could
still produce) would be *less* correct in practice than one that recognizes the tag shape — the
narrower bound governs what a compliant *sender* may create, not what a correct *reader* must
recognize. This is a deliberate, documented divergence from the primary source's own cardinality
prose, made because two independent mature implementations of the *same* edition agree with each
other and not with that narrower reading — not because either implementation was assumed
authoritative by default.

## Reproducing this provenance record

1. Fetch the same two URLs; confirm the SHA-256 values above match.
2. Fetch the DCMTK dictionary URL; confirm its SHA-256 matches.
3. Run `python3 -c "from pydicom.datadict import dictionary_VR; print(dictionary_VR(0x60FE0010))"` (expect `US`) and the boundary checks above against an installed `pydicom` to reproduce the cross-check independently of this document's own claims.
