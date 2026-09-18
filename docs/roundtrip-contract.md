# fastDICOMattrs — Round-Trip Contract

This document defines, precisely, what "lossless" means in this library, what the object model
retains to make it true, and exactly how far the current implementation actually gets — proven by
`tests/integration/test_roundtrip_lossless.cpp`, not asserted here.

> Do not claim lossless round-trip behavior until tests demonstrate it. This document is written
> to be falsifiable: every bullet below either has a passing byte-comparison test or is marked
> explicitly as not yet implemented.

## Two write contracts

`write()`/`write_file()` offer two distinct contracts, depending on whether the structure was
mutated (`is_modified()`):

**Unmodified — byte-identical.** Given a `DICOMStructure` parsed with `Fidelity::Lossless` from
source `S`, and never mutated (`erase`/`set`/`set_value` never called), `write()` reproduces the
DICOM Part-10 content of `S` **byte-for-byte** — including a source file that has an element
positioned *after* Pixel Data (e.g. a Data Set Trailing Padding element `(FFFC,FFFC)`, which
PS3.10 permits): `DICOMStructure::pixel_data_position()` records where Pixel Data actually sat
among the top-level elements, and the writer honors it (see "Known gaps" below, A1.3). This is
`Fidelity::Lossless`-only —
`Fidelity::Fast`/`Fidelity::Standard` don't retain the header-encoding detail (VR encoding mode,
length-field width, reserved bytes) this guarantee needs, so `write()` on an unmodified
FAST/STANDARD structure returns `WriteStatus::Unsupported`.

**Modified — valid reconstruction, not byte-identical.** Once any mutation has occurred,
byte-identical reproduction is not the goal (the edit itself changed the content); the contract
instead is that `write()` produces a **valid, semantically-correct DICOM Part-10 object**
reflecting the mutations, with untouched elements preserved and ascending tag order maintained for
any newly-inserted top-level elements. This is supported at both `Fidelity::Lossless` and
`Fidelity::Standard` — both retain every field the writer needs (`Tag`, `VR`, `LengthForm`,
undefined-length flag, and the current value bytes) to reconstruct headers deterministically, per
"Reconstruction, not verbatim copy" below. `Fidelity::Fast` stays `WriteStatus::Unsupported` even
when modified, since it's reserved for future short-circuiting that may not populate those fields.
`set`/`set_value` reject (return `false`, no-op) a mutation that can't be legally encoded — an
odd-length value (PS3.5 6.4 requires every value to have even length, regardless of VR), or one
that grows a short-form-VR value past what a 2-byte length field can represent — rather than let
it reach the writer and silently produce a non-conformant element.

If a `(0002,0000)` File Meta Information Group Length element is present, a modified write
recomputes its value from the actual encoded size of the rest of the File Meta group (see
`compute_file_meta_group_length` in `lossless_writer.cpp`) — an unmodified write leaves it
byte-identical to the source, which is already correct since nothing changed, but a modified write
can add, remove, or resize File Meta elements (directly, or indirectly via the
`(0002,0010)` TransferSyntaxUID rewrite described below), and a stale group length is a genuine
PS3.10 conformance defect, not just an unproven claim.

## What must be retained, and where it lives in the object model

Each row is one of the details the charter calls out as required for byte-for-byte reproduction.

| Detail | Retained as | Notes |
|---|---|---|
| Tag ordering | `DICOMStructure`/`Item` store `std::vector<Element>` in parse order | Never sorted, never re-keyed into a map. |
| VR representation | `Element::vr()` + `Element::vr_provenance()` (`has_explicit_vr_in_source()` remains as a derived `provenance == Explicit` check) | Distinguishes "VR was written in the header" from every Implicit-VR-sourced route to a VR (dictionary, structural inference, context-resolved, or still unknown — see "Implicit VR Little Endian" below and `docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md`). This table is about the byte-identical guarantee, which never applies to Implicit VR input regardless of how richly its VRs are now resolved. |
| VL representation | `Element` stores `LengthForm` (`Short16`/`Long32`) actually read from the header | A VR like `UN` can legally appear with either form in malformed/edge-case input; we record what was *there*, not what the VR table says should be there. |
| Byte order | `TransferSyntax::little_endian()` on the parsed structure | Only little-endian Transfer Syntaxes are parsed in this increment (see "Known gaps"). |
| Explicit vs. implicit VR | `TransferSyntax::explicit_vr()` + per-element `has_explicit_vr_in_source()` | Implicit VR datasets are parsed (see "Implicit VR Little Endian" below), but the byte-identical guarantee this table describes is Explicit-VR-only — the writer has no Implicit VR output mode. |
| Defined vs. undefined length | `Element::has_undefined_length()`, `Item::has_undefined_length()`, `Sequence::has_undefined_length()` | Each level tracks its own, independently — an outer sequence can be undefined-length while containing defined-length items. |
| Original raw values | `Value::bytes()` resolves to the exact source span; never re-encoded for unmodified elements | Includes trailing VR padding bytes (space/NUL) — padding is just part of the byte range, never stripped for `bytes()` (only `as_string()` trims it, and only for the *typed* view). |
| Value padding | Same as above | No separate field; it's implicit in "we store the raw span, not a semantically-trimmed copy." |
| Sequence nesting | `Sequence` → `vector<Item>` → `vector<Element>`, recursively | Arbitrary depth, bounded only by `ParseOptions::max_sequence_depth` (a malformed-input guard, not a structural limit). |
| Sequence/item headers | Reconstructed on write from `Tag` (`FFFE,E000`/`FFFE,E0DD`) + stored `LengthForm`/length/undefined-ness | Not stored as a separate raw blob — see "Reconstruction, not verbatim copy" below. |
| Item/sequence delimiters | `has_undefined_length()` on the relevant `Item`/`Sequence`; the writer emits `(FFFE,E00D)`/`(FFFE,E0DD)` length-0 delimiters exactly when that flag is set | |
| Unknown/private elements | Parsed and retained like any other element (VR `UN` when unrecognized, or the source-declared VR if explicit) | Never dropped at any fidelity level, per charter — this is a `Fidelity`-independent guarantee, not LOSSLESS-only. |
| File preamble / file meta | `DICOMStructure::has_file_preamble()` + `file_preamble()` (raw 128 bytes, verbatim, not assumed to be zero) | File Meta Information (group `0002`) is parsed as ordinary top-level elements — see `docs/architecture.md` §5. |
| Original source spans | `SourceSpan{offset, length}` on every source-backed `Value` | Exposed for inspection/debugging; not needed by the writer itself (see below), since the writer reconstructs headers rather than copying original header bytes. |

## Reconstruction, not verbatim copy

The writer does **not** work by locating one contiguous "original bytes" blob per element and
copying it. It reconstructs each element's header bytes deterministically from the structured
fields above (`Tag`, `VR`, `has_explicit_vr_in_source()`, `LengthForm`, undefined-length flag,
length value) and copies only the **value** bytes verbatim (from the source span, or from an
owned buffer if modified). Item and sequence headers/delimiters are reconstructed the same way
from their own stored flags.

This is a deliberate design choice over "store one opaque raw span per element and concatenate
them on write": reconstruction is what "regenerate only the representation that must change"
(the mutation rule in `docs/architecture.md` §9) requires anyway, and it turns the unmodified
round-trip test into a genuine end-to-end proof that the encoder and decoder implement the same
rules as inverses of each other — a bug in either one shows up as a byte mismatch, not just a
"looks plausible" pass. The cost is that every piece of information needed to reproduce a header
byte-for-byte must be an explicit field somewhere in the object model; anything not captured shows
up immediately as a failing round-trip test, which is exactly the intended feedback loop.

Consequence: any DICOM encoding freedom this parser does not yet track as a field is a
correctness bug in the LOSSLESS claim, not a rare edge case to shrug off. The table above is the
current, complete list of tracked fields. If a future fixture round-trips incorrectly, the fix is
to add the missing field to the model and document it here — never to weaken the test to semantic
equality.

## FAST

FAST retains exactly what's needed for navigation (`Tag`, `VR`, length, value `SourceSpan`) and
nothing about header encoding detail beyond that. `write()` on a FAST-parsed structure is
`Unsupported` unconditionally, whether modified or not — unlike STANDARD, which now supports
writing a *modified* structure (see "Two write contracts" above). FAST is not optimized for speed
yet beyond "don't retain the LOSSLESS-only fields"; per the charter, real FAST optimization (e.g.
skipping element bodies entirely, stopping before Pixel Data) is deferred until `bench/` produces
numbers that justify specific shortcuts.

## Implicit VR Little Endian

Implicit VR Little Endian (`1.2.840.10008.1.2`) elements are encoded as tag(4) + length(4) only —
no VR field on the wire, unlike Explicit VR's tag + VR + length. As of A1.4, the parser
(`src/parser/implicit_vr_le_parser.cpp`) recovers the real VR from the frozen PS3.6 dictionary
(`fds::dictionary::lookup`, see `docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md`) wherever
the dictionary has an unambiguous answer, for a standard tag at any nesting depth:

* An element with **undefined length** is always parsed as a `Sequence` — the only legal use of
  undefined length in Implicit VR is SQ (or encapsulated Pixel Data, handled separately), a
  spec-grounded structural fact independent of the dictionary (`VRProvenance::Structural` when the
  dictionary has no entry to confirm it, e.g. a private Sequence; `VRProvenance::Dictionary` when it
  does).
* A **defined-length** element whose dictionary entry is `VR::SQ` is *also* now recursively parsed
  as a Sequence, bounded by its declared length — this was the historical gap (see below) and is
  A1.4's central capability.
* A **defined-length** element with an unambiguous non-SQ dictionary entry gets that VR
  (`VRProvenance::Dictionary`).
* A tag with no dictionary entry (every private tag, by design — A1.1 — plus any genuinely
  unrecognized standard tag) gets `VR::Unknown`, kept as an opaque value, exactly as before.
* A tag whose dictionary entry is one of PS3.6's documented ambiguous forms (`US or SS`, `OB or OW`,
  `US or OW`, `US or SS or OW`) is never guessed: `US or SS` is resolved by inspecting Pixel
  Representation `(0028,0103)` at the top level of the dataset, in a pass that runs once the entire
  element tree is built (so it does not depend on which side of the ambiguous element Pixel
  Representation appears on); the other three forms are deliberately left `VR::Unknown` in V1 — see
  `docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md` for the full per-form
  rationale.

**Historical note (closed by A1.4):** before A1.4, no dictionary existed, so a *defined-length*
nested sequence could not be told apart from a large opaque value and was parsed as the latter.
That limitation is closed — `tests/integration/test_parse_implicit_vr_le.cpp`'s "a defined-length
standard Sequence is recursively parsed (A1.4)" test (the same fixture the old, removed
"NOT expanded" test used) proves the corrected behavior.

**Text decoding (A1.5, decode direction only):** `Value::as_string()`/`bytes()` remain exactly the
raw/default-repertoire view they always were -- A1.5 adds a separate, additive read path,
`fds::charset::resolve_character_set_context()` + `decode_text()` (`include/fastdicomattrs/
charset.hpp`), that resolves `(0008,0005)` at the correct dataset scope (root, or the nearest
enclosing Sequence Item's own override) and decodes the seven Specific-Character-Set-governed VRs
(`LO LT PN SH ST UC UT`) into Unicode text, for the default repertoire, all ten PS3.5 single-byte
repertoires with ISO 2022 code-extension switching, and UTF-8 (`ISO_IR 192`). See
`docs/architecture/A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md` for the full design. **Still
out of scope in V1:** encoding/mutation direction (setting a native string and having it encoded
back to conformant bytes) is A1.6, not implemented here; multi-byte code-extension repertoires
(Japanese/Korean/Chinese) remain explicitly deferred, reported as an explicit `Unsupported` decode
outcome rather than guessed; the 13 ambiguous dictionary entries outside the `US or SS` form remain
permanently `VR::Unknown`; no vendor-private dictionary of any kind exists or is planned.

**The LOSSLESS byte-identical guarantee never applies to Implicit VR input, modified or not.** The
writer (`src/writer/lossless_writer.cpp`) always emits Explicit VR headers — it has no Implicit VR
output mode — so:

* `write()` on an **unmodified** Implicit-VR-parsed structure is `WriteStatus::Unsupported`
  unconditionally (even at `Fidelity::Lossless`): the output would categorically differ from the
  input, not just risk differing, so there is nothing "lossless" to attempt.
* `write()` on a **modified** Implicit-VR-parsed structure follows the same modified-structure
  contract as any other input (see "Two write contracts" above) — but the output is always
  Explicit VR Little Endian. If a `(0002,0010)` TransferSyntaxUID element is present, the writer
  rewrites its value to `1.2.840.10008.1.2.1` so the output doesn't declare Implicit VR while
  actually being encoded as Explicit VR (a self-contradictory file no reader, including this
  library's own parser, could correctly interpret). If no File Meta is present at all (a bare
  Implicit VR dataset with no preamble), nothing is rewritten — same as any other bare-dataset
  write, the output has no Transfer Syntax declaration to begin with.

## Known gaps (honest limitations of this increment)

* **Only little-endian Transfer Syntaxes are parsed.** The retired Explicit VR Big Endian
  (`1.2.840.10008.1.2.2`) is detected and rejected with `DiagnosticSeverity::Unsupported`, not
  guessed at. Compressed Transfer Syntaxes (JPEG, JPEG 2000, RLE, etc.) *are* parsed structurally
  — their dataset encoding is still Explicit VR Little Endian per the standard; only their Pixel
  Data is encapsulated, which is handled generically (see `docs/architecture.md` §8) without
  decoding. Implicit VR Little Endian (`1.2.840.10008.1.2`) is also parsed — see "Implicit VR
  Little Endian" below for what that does and does not mean.
* **Deflated Explicit VR Little Endian (`1.2.840.10008.1.2.1.99`) is detected and rejected with
  `DiagnosticSeverity::Unsupported`.** Unlike the compressed Transfer Syntaxes above, its *entire
  dataset* — not just Pixel Data — is zlib-deflated after the preamble/File Meta; this library has
  no inflate step. It is special-cased in `TransferSyntax::from_uid()` specifically so it doesn't
  fall into the generic "any other `1.2.840.10008.1.2.*` UID" bucket those compressed syntaxes use,
  which would otherwise silently misread its deflated bytes as plain ones.
* **(A1.3, closed) The writer used to always emit Pixel Data last, regardless of its position in
  the source.** `DICOMStructure` now records `pixel_data_position()` — the count of ordinary
  top-level elements that preceded Pixel Data in the source — at parse time (both parser call
  sites: the Explicit VR LE top-level loop and the Implicit VR LE dataset loop, since both feed
  the same `elements`/`pixel_data` pair before one `DICOMStructure` is constructed). The
  unmodified write path honors that recorded position exactly, so a source file with an element
  positioned after Pixel Data (e.g. Data Set Trailing Padding `(FFFC,FFFC)`) now round-trips
  byte-identically. The modified (semantic-reconstruction) write path never consults the recorded
  position — it would go stale after insertion/removal — and instead places Pixel Data at the
  point its own tag `(7FE0,0010)` belongs in the current, already-ascending-tag-ordered element
  list, the same rule every other top-level element already follows on that path. See
  `docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md`. Proven by
  `tests/integration/test_roundtrip_lossless.cpp`'s "An element after Pixel Data round-trips
  byte-identically (A1.3)" test (the same fixture the old, now-corrected test used) plus
  `tests/integration/test_pixel_data_ordering.cpp`.
* **Round-trip is proven against synthetic, purpose-built fixtures constructed in
  `tests/integration/`** (see `tests/fixtures/README.md` for why no external sample files are
  vendored) **and against a real-world corpus** (26,636 real DICOM CT files from two TCIA
  collections, differentially cross-checked against pydicom — see `docs/corpus-results.md` for the
  actual numbers and methodology, not just the claim). The synthetic fixtures prove the
  encoder/decoder are correct inverses for every construct they exercise (explicit VR long/short
  form, defined/undefined length, nested sequences, private/unknown tags, native and encapsulated
  pixel data, group-length and file-meta elements); the corpus run proves the same holds up against
  real-world scanner output, not just deliberately-constructed cases. That corpus happened to be
  100% Explicit VR Little Endian, so it does not extend real-world proof to the Implicit VR LE
  parser — see "Implicit VR Little Endian" above.
* **Modified-structure output is proven semantically correct, not byte-identical** — by synthetic
  fixtures (`tests/integration/test_mutation_roundtrip.cpp`: parse, mutate, write, re-parse, assert
  on the result) and, for real-world data, by applying README.md's own worked-example policy to
  the same real-world corpus above and validating the output with pydicom (`--transform
  readme-example`; see `docs/corpus-results.md` "Transformation policy" for the actual numbers).
  Byte-level preservation of untouched content is measured via `WriteResult::source_backed_value_
  bytes`/`regenerated_value_bytes` (`Structure.write_with_stats()` in Python) — 99.99% preserved on
  that same real-world run.
