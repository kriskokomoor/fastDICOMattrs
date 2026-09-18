# A1.5 — Specific Character Set Context Resolution and Text Decoding: Freeze Report

## 1. Starting frozen state

Commit `87e79fe` (A1.4 freeze), verified directly: `git status` clean, **186/186** C++ tests
(`ctest`), **37/37** Python tests (`pytest`). Confirmed by inspection that no charset subsystem
existed anywhere in the tree (`grep -rn charset src include` before this increment matched
nothing production-relevant).

## 2. Pre-A1.5 limitation reproduced

Before any charset code was written, `Value::as_string()`/`bytes()` were confirmed to be the only
text-access API in the library — a raw byte view with a single trailing pad byte trimmed, no
`(0008,0005)` awareness whatsoever. A quick probe (`fds::read` a file declaring `ISO_IR 100` with a
Latin-1 `é` byte, print `as_string()`) confirmed the byte comes back unmodified (`\xE9`), not
decoded — the expected pre-A1.5 baseline, now closed by the new, additive `decode_text()` path
described below (the old raw-byte behavior is unchanged and still fully available).

## 3–4. V1 charset envelope

| DICOM defined term(s) | Family | Byte width | ISO 2022 capable | V1 status | Codec |
|---|---|---|---|---|---|
| *(absent)* / `` / `ISO_IR 6` / `ISO 2022 IR 6` | Default (ASCII) | single | as G0 always | **Supported** | strict 7-bit identity |
| `ISO_IR 100` / `ISO 2022 IR 100` | Latin1 (ISO 8859-1) | single | yes | **Supported** | generated table |
| `ISO_IR 101` / `ISO 2022 IR 101` | Latin2 (ISO 8859-2) | single | yes | **Supported** | generated table |
| `ISO_IR 109` / `ISO 2022 IR 109` | Latin3 (ISO 8859-3) | single | yes | **Supported** | generated table |
| `ISO_IR 110` / `ISO 2022 IR 110` | Latin4 (ISO 8859-4) | single | yes | **Supported** | generated table |
| `ISO_IR 144` / `ISO 2022 IR 144` | Cyrillic (ISO 8859-5) | single | yes | **Supported** | generated table |
| `ISO_IR 127` / `ISO 2022 IR 127` | Arabic (ISO 8859-6) | single | yes | **Supported** | generated table |
| `ISO_IR 126` / `ISO 2022 IR 126` | Greek (ISO 8859-7) | single | yes | **Supported** | generated table |
| `ISO_IR 138` / `ISO 2022 IR 138` | Hebrew (ISO 8859-8) | single | yes | **Supported** | generated table |
| `ISO_IR 148` / `ISO 2022 IR 148` | Turkish (ISO 8859-9) | single | yes | **Supported** | generated table |
| `ISO_IR 166` / `ISO 2022 IR 166` | Thai (TIS 620-2533) | single | yes | **Supported** | generated table |
| `ISO_IR 192` | UTF-8 | multi | no (stand-alone only) | **Supported** | strict UTF-8 |
| `ISO_IR 13` / `ISO 2022 IR 13`/`14` | Japanese Katakana | single (JIS family) | yes | *Deferred* | — |
| `ISO 2022 IR 87` / `159` | Japanese Kanji | multi | yes | *Deferred* | — |
| `ISO 2022 IR 149` | Korean | multi | yes | *Deferred* | — |
| `ISO 2022 IR 58` / `ISO_IR 58` | Chinese (GB2312) | multi | yes | *Deferred* | — |
| `GB18030`, `GBK`, `ISO 2022 GBK`/`58` | Chinese | multi | no (stand-alone) | *Deferred* | — |
| any other string | — | — | — | *Unrecognized* | — |

Recognized-but-deferred terms are distinguished from genuinely unrecognized strings internally
(`RepertoireSupport::Deferred` vs `::Unrecognized`) even though both surface as the same
`CharacterSetMode::Unsupported` outward result — a real DICOM term this library knows it cannot
decode is a different fact than a typo, worth preserving for diagnostics even though V1's public
contract doesn't yet expose the distinction further.

## 5. DICOM-defined-term mapping architecture

`fds::charset` speaks in DICOM terms throughout its public API (`RepertoireTerm::defined_term` is
the exact declared string, e.g. `"ISO_IR 100"`); an internal-only `RepertoireId` enum (never
exposed) drives codec dispatch. The two prefix forms of the same repertoire number (`ISO_IR nnn`
vs `ISO 2022 IR nnn`) are treated as naming the same underlying table — deliberately lenient about
which form appears where, matching real-world tolerant behavior (pydicom does the same) rather
than rejecting a technically-slightly-off declaration.

## 6–7. Charset-context scope, inheritance, and override

`resolve_character_set_context(structure, path)` walks `path` from the root using only the public
`DICOMStructure`/`Sequence`/`Item` accessors (no privileged access, no parent back-pointer),
checking every scope crossed for its own `(0008,0005)`. Unlike A1.2's private-creator resolution
(strictly scoped, never cascades), a charset declaration **cascades**: the nearest enclosing
declaration governs everything nested inside it that doesn't declare its own override, matching
PS3.5's actual inheritance rule — a deliberate, documented architectural difference from A1.2's
precedent, not an oversight. Proven by six dedicated tests: root-only inheritance into a nested
Item, a nested Item's own override, sibling-Item isolation (one Item's override never leaks into
its sibling, which correctly falls back to the root's declaration, not the sibling's), and a
three-level nested child Item inheriting from its *immediate* effective parent (an outer Item's
override) rather than skipping to the root.

## 8. Text-VR classification

Exactly seven VRs are Specific-Character-Set-governed per PS3.5 6.1.1: `LO LT PN SH ST UC UT`.
Independently cross-checked against pydicom's own `CUSTOMIZABLE_CHARSET_VR` constant
(`{LO, LT, PN, SH, ST, UC, UT}`) — exact agreement. Every other VR (`AE AS CS DA DS DT IS TM UI UL`
and the rest) is restricted to the default repertoire regardless of `(0008,0005)` and is never
passed to the decoder; `decode_text()` on any non-governed VR returns `NotATextVR` without
inspecting the charset context at all.

Within the governed set, a further distinction mattered and was found via real-world corpus
testing (§16): `ST`, `LT`, and `UT` are inherently single-valued (PS3.5 6.2 — VM is always 1),
unlike `SH`, `LO`, `PN`, `UC`; a backslash byte inside their content is literal text, never a
value-multiplicity delimiter.

## 9. Decoded-text API

```cpp
CharacterSetContext resolve_character_set_context(const DICOMStructure&, const ElementPath&);
DecodedText decode_text(const Element&, const CharacterSetContext&);
```

Deliberately two separate calls (not one combined `element.decoded_text(path)`), so a caller
decoding many values from the same structural region resolves context once and reuses it — proven
to matter, not just theoretically: §27's benchmark shows a ~45× speedup between resolving context
per-element versus once and reusing it. `DecodedText` distinguishes `Success` /
`NotATextVR` / `UnsupportedCharset` / `MalformedCharsetDeclaration` / `InvalidEncodedBytes`
explicitly — never collapsed into a single true/false.

## 10. Raw-byte preservation

`Value::bytes()`/`as_string()` are untouched by this increment — confirmed by inspection (no edit
to `value.hpp`/`.cpp`) and by two dedicated tests: one proves an element's raw bytes and
`is_modified()`/structure `is_modified()` are unchanged after a `decode_text()` call, the other
proves `write()` output is byte-identical whether or not `decode_text()` was called first. Further
confirmed at real-corpus scale (§25): 350 real files, decode-then-write vs. write-alone, 350/350
byte-identical.

## 11. Default repertoire

Strict 7-bit ASCII (`ISO 2022 IR 6`) — a byte ≥ 0x80 with no `(0008,0005)` declared is
`InvalidEncodedBytes`, never silently guessed. **Deliberate divergence from pydicom**, documented:
pydicom's own `default_encoding` resolves to a Latin-1-permissive Python codec, so a stray high-bit
byte in nominally-default-repertoire text decodes leniently there; this library instead follows
PS3.5's literal definition of the default repertoire and this task's explicit preference for
deterministic failure over lossy silent substitution. Confirmed empirically: the exact same byte
sequence was fed to both libraries, `fastdicomattrs` correctly returns `InvalidEncodedBytes` while
pydicom returns `'AéB'`. No real corpus file (§16, §24) was found where this divergence changed the
outcome on well-formed content.

## 12–17. Repertoire-by-repertoire results

**Single-byte (§17 task requirement — a real, discriminating non-ASCII character per repertoire,
not `"ABC"`):** all ten repertoires tested with a real byte→codepoint pair derived from the same
Python stdlib codecs that generate the tables (see `tests/integration/test_charset.cpp`) — an
internal-consistency check on the generation pipeline (catches a wrong codec name, an off-by-one,
a copy/paste error), not independent validation on its own — plus a dedicated discriminating test
where the *same* byte (`0xE0`) decodes to four different characters (`à` Latin1, `р` Cyrillic, `א`
Hebrew, `ـ` Arabic) depending solely on the active repertoire, proving the table *dispatch* is
correct, not just table presence. The genuine independent-implementation evidence is separate: all
ten repertoires were cross-validated against **pydicom** (13/13 synthetic-case matches, §20) and,
for a representative subset, **DCMTK** (`dcmdump`, §21) — a different codebase, a different
Unicode-mapping implementation, exact agreement throughout.

**UTF-8:** ASCII subset, 2-byte, 3-byte, and 4-byte sequences all decode correctly; truncated,
overlong (`0xC0 0xAF` for `/`), and lone-continuation-byte inputs are all strictly rejected
(`InvalidEncodedBytes`), never replaced.

**ISO 2022:** the escape-byte table (`ESC ( B` → ASCII/G0; `ESC - <letter>` → each single-byte
repertoire/G1) was derived from PS3.5 Table C.12-3 and cross-checked byte-for-byte against
pydicom's own `CODES_TO_ENCODINGS` table during development. The delimiter-reset algorithm (state
resets to `encodings[0]` at `\` between VM components, at CR/LF/TAB/FF within ordinary text, at
`^` within a PN component group, and independently at `=` between PN component groups) was derived
by reading PS3.5's own rule plus pydicom's `decode_bytes`/`_decode_fragment`/
`_decode_escaped_fragment` implementation line-by-line as a cross-check, not assumed. Proven by
dedicated tests: multi-repertoire switching mid-string (the textbook Latin+Cyrillic case), no state
leakage across a VM boundary, a malformed/unrecognized escape sequence rejected outright, and an
escape switching to a repertoire not present in the declared `(0008,0005)` list rejected outright
(never silently tolerated).

## 18. Multi-valued Specific Character Set

PS3.5 Table C.12-5's stand-alone rule (UTF-8/GB18030/GBK cannot be combined with anything else in
a multi-valued declaration) is checked explicitly and produces `Malformed`, distinct from the
generic `Unsupported` outcome for a merely-deferred term. An empty first term in a multi-valued
declaration is correctly treated as `ISO 2022 IR 6` per PS3.5 C.12.1.1.2 (matching pydicom's
identical special case).

## 19. Multi-valued text

VM > 1 text (backslash-separated) preserves value boundaries and empty values — proven for `LO`
(`"AAA\\\\BBB"` → three values, middle empty) and for `PN` (two people, each independently
decoded). See §8 for why `ST`/`LT`/`UT` are excluded from this splitting.

## 20. pydicom differential result

Two layers: (a) 13 synthetic generated-fixture cases (all ten single-byte repertoires, ISO 2022
multi-value switching, UTF-8, PN component groups) built as real DICOM byte streams and read
independently by pydicom — **13/13 exact matches**. (b) real-corpus (§16) automated comparison:
**1,077 text elements compared across 67 real files, 1,074 automated matches**. The 3 reported
"mismatches" were individually triaged, not waved away: 2 were confirmed (via a direct, isolated
C++-level check bypassing the diff harness entirely) to be an artifact of the differential
harness's own Python `subprocess` text-mode universal-newline translation on a 27KB multi-line XML
value — `decode_text()` itself was independently verified to return the exact, full,
byte-length-matching decoded string (26,974 UTF-8 bytes out for 26,974 raw bytes in, ending
correctly at `</Clip>`). The third is the pre-existing (not A1.5-introduced), already-documented
`Value::as_string()` single-trailing-pad-byte-trim convention (`" "` vs pydicom's more aggressive
multi-space stripping) — adjudicated against PS3.5 6.4, which mandates only single-pad-byte
handling, not arbitrary whitespace stripping; not a defect. **Zero unexplained disagreements.**

## 21. DCMTK adjudication result

`dcmdump +U8` (convert-to-UTF8 mode) independently decoded four synthetic files: Latin1 PN (`AéB`),
Cyrillic PN (`AрB`), UTF-8 LO (`ABé中`), and ISO 2022 multi-value PN switching Latin1→Cyrillic
(`ABéр`) — **4/4 exact matches** against this library's output. One informational finding, not a
defect: DCMTK emits a warning ("Escape sequences shall not be used in the first component group of
a Person Name") for a fixture using an escape sequence directly in PN's first (only) component
group, then decodes it correctly anyway ("using them anyway") — a PS3.5 conformance nicety neither
this library nor pydicom enforces as a restriction; noted, not acted on, since DCMTK itself treats
it as tolerable, not an error.

## 22–24. Real-world charset corpus: provenance, size, and decode results

**Neither NLST nor CMB-MEL contains any non-ASCII text** (confirmed, §26) — a real, genuinely
non-ASCII corpus was needed. Found already present on this machine: **pydicom's own bundled
test-data package** (`pydicom.data`, BSD-licensed, installed as a normal Python dependency) — **67
files** genuinely declaring a non-default `(0008,0005)`, discovered by scanning all 193 bundled
test files (not curated by hand in advance). Notable finds:

- `test-SR.dcm`: a **genuine, real, non-synthetic** German name, `Riesmeier^Jörg` (PN, `ISO_IR
  100`) — independently confirmed identical via pydicom on the same file.
- `J2K_pixelrep_mismatch.dcm`: declares `['ISO 2022 IR 13', 'ISO 2022 IR 87']` (Japanese) — a real
  file exercising the *negative* path: every text element on this file correctly reports
  `UnsupportedCharset` (25 elements confirmed), never a guess, raw bytes still fully available.
- 51 `SC_rgb_*`/similar files declare `ISO_IR 192` (UTF-8).

No file was converted from another encoding to manufacture "real-world" evidence — every file used
here is as pydicom's own test suite ships it, itself sourced from real scanner/DCMTK-generated
output historically.

## 25. Large-corpus charset characterization (descriptive, not a threshold)

| | NLST (2,832 files) | CMB-MEL (23,807 files) |
|---|---:|---:|
| Files with no `(0008,0005)` | 302 | 2 |
| `ISO_IR 100` declared | 2,530 files | 23,754 files |
| `ISO_IR 192` declared | 0 | 51 files |
| Text elements decoded | 118,265 | 1,399,814 |
| Decode success | 118,265 (100%) | 1,399,814 (100%) |
| Unsupported / Malformed / Invalid | 0 / 0 / 0 | 0 / 0 / 0 |
| Files with any non-ASCII decoded text | 0 | 0 |

As anticipated (US-based trial data), both corpora are overwhelmingly ASCII despite over 26,000
files declaring `ISO_IR 100`/`192` "just in case" — a real, common pattern (broad repertoire
declared, content happens to be plain ASCII, which is always valid under any of these
repertoires). This is characterization evidence, not proof of non-default-repertoire correctness
(§16/§22 supply that); it is, however, strong evidence of robustness at scale: 1.5 million text
decodes across ~26,600 real files, zero crashes, zero unexpected failures.

## 26. Round-trip invariance

Two dedicated unit tests (§10) plus a real-corpus spot check: 200 NLST + 150 CMB-MEL files, each
parsed twice, one copy decoded (every text element in the file) then written, the other written
directly with no decoding — **350/350 byte-identical**, and no structure ever reported
`is_modified() == true` after decoding alone.

## 27. Performance/resource result

| Workload | Result |
|---|---|
| Parse only, no decode calls, 5,000 elements | 1.41 ms (unchanged in kind from A1.4-era baselines — charset code is on no parse path) |
| Decode 5,000 small values, context **resolved once, reused** | 0.70 ms (≈7,100 decodes/ms) |
| Decode 5,000 small values, context resolved **per element** | 32.0 ms (≈156 decodes/ms) |
| Decode one 160 KB UTF-8 value | 0.049 ms (≈3.3 GB/s) |
| Decode a 40 KB value with an ISO 2022 switch every 8 bytes | 0.19 ms (≈209 MB/s) |

The ~45× gap between resolving context once versus per-element is not a regression — it is the
direct, expected, and now-measured consequence of `resolve_character_set_context`'s
O(elements-in-scope) cost per call, which is exactly why the API is split into two calls (§9) and
callers are told to resolve once per structural region. Parse-time cost is unaffected by this
increment's existence, confirmed both by construction (no charset code runs on any parse path,
confirmed by inspection) and by measurement.

## 28. ABI/Python exposure decision

**Deferred**, deliberately, per this task's explicit permission (§29 of the authorizing brief): the
native C++ API (`fds::charset::resolve_character_set_context`/`decode_text`) is the complete A1.5
deliverable, proven exhaustively above. No `fds_structure_resolve_charset_context`/
`fds_element_text` ABI functions or Python bindings were added — that is additive surface area best
built once the C++ semantic layer (this increment) is reviewed and frozen, matching the same
boundary A1.4 drew around Python/ABI mutation ergonomics (left to A1.7). Nothing about this
increment's design forecloses adding that surface later.

## 29. Deviations from the A1 plan

The original plan's capability description (`docs/architecture/
A1_ATTRS_V1_IMPLEMENTATION_PROGRESSION.md` §7, A1.5) did not call out the ST/LT/UT
single-valuedness rule explicitly — a real PS3.5 requirement this increment's own real-corpus
qualification surfaced and fixed (§8, §16). No other material deviation.

## 30. Any defect discovered in A1.1–A1.4

None. A1.1's dictionary, A1.2's private-creator resolution, A1.3's Pixel-Data positioning, and
A1.4's VR resolution were not touched by this increment and were not implicated by any finding
here.

## 31. Attrs Contract change

`docs/roundtrip-contract.md`'s "Still out of scope in V1" text, which previously claimed "text VRs
decode to raw bytes only, with no Specific Character Set-aware interpretation," is corrected to
describe the new decode-only capability and its actual remaining boundary (no encode direction yet,
multi-byte repertoires deferred) — done as part of this freeze.

## 32. Remaining limitations before A1.6

- No encode/mutation direction: a caller cannot set a native Unicode string and have it encoded
  back to conformant, charset-correct bytes — that is A1.6's entire purpose.
- Multi-byte code-extension repertoires (Japanese/Korean/Chinese, GB18030/GBK) remain unsupported,
  explicitly and non-destructively (§18 of the authorizing brief's criteria).
- No Python/ABI exposure yet (§28) — deferred, not designed against.
- The 13 non-`US-or-SS` ambiguous dictionary entries (A1.4) remain permanently unresolved — orthogonal
  to charset work, unaffected by this increment.

## 33. Report path

`docs/architecture/A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md` (this file).

## 34. Freeze recommendation

**PASS** (not "PASS WITH QUALIFICATION" — genuine real-world non-ASCII data, including a real
German name in a real file, was obtained and used, satisfying the one condition the authorizing
brief named as forcing the weaker verdict). Every criterion in §34 of the authorizing brief is met
with direct evidence: the V1 envelope is documented and every entry in it is tested with real
discriminating content; UTF-8 is strict; dataset-scoped context resolution, inheritance, override,
and sibling isolation are all proven, including three levels deep; PN and multi-valued-text
delimiter semantics are correct (and the ST/LT/UT single-valuedness nuance was caught and fixed);
unsupported and malformed declarations are explicit and never destructive; malformed bytes never
corrupt raw data or fail the underlying parse; raw bytes and serialization are provably unaffected
by decoding, both by unit test and at real-corpus scale; pydicom and DCMTK both independently
corroborate with zero unexplained disagreement; a genuine, not-manufactured real-world charset
corpus was qualified; the existing large corpora regress cleanly; all 215 pre-existing-plus-new C++
tests and 37 Python tests pass; parser performance is unaffected when decoding isn't invoked, and
decode performance itself is measured and understood; no A1.6 encode work was begun.

```text
A1.1: FROZEN at 6f05d9d
A1.2: FROZEN at e651c74
A1.3: FROZEN at 140d850
A1.4: FROZEN at 87e79fe
A1.5: PASS, candidate freeze commit 2a2a3f3 (pending this report's own commit)
A1.6: NOT STARTED
```

## 35. Closure addendum: reproducibility and evidence-wording review

Semantic PASS was already established above at `15337cb`. A follow-up review pass identified two
small, non-semantic findings, closed here without reopening any A1.5 decoding behavior:

**Generator nondeterminism.** `tools/generate_charset_tables.py` embedded a current UTC timestamp
in every generated `src/charset_tables.generated.cpp`, so two runs against identical inputs
produced different bytes (only in that comment header — every table's actual byte→codepoint content
was already deterministic, since it comes solely from Python's stdlib codecs applied to the fixed
`REPERTOIRES` list). Fixed by removing the timestamp line entirely and replacing it with a static
comment explaining the determinism property, rather than any other volatile substitute. The
generator is now byte-deterministic: two consecutive regenerations and the committed table all hash
to the same SHA-256 (`b54d83779cfe0a0298f5e27921096cdb1e3bc82c561939a46aed0ac15058e1e2`), verified
both by a standalone script (`tools/verify_charset_tables_deterministic.py`) and by two new,
automated `pytest` regressions (`tests/python/test_charset_tables_generation.py`, mirroring
`test_dictionary_generation.py`'s role for the PS3.6 dictionary generator) that will catch any
future reintroduction of volatile content. `git diff` on the regenerated file confirms only the
header comment changed — every table's data bytes are identical to what was committed at the
original `15337cb` freeze, so **no charset mapping changed**.

**Evidence-wording correction.** The report (§12–17, above) and `tests/integration/test_charset.cpp`
previously described the per-repertoire test fixture values as "independently sourced" /
"independently derived." That overstated the evidence: those byte→codepoint pairs were derived from
the *same* Python stdlib codecs the generator itself uses, so they are an internal-consistency check
on the generation pipeline (catching a wrong codec name or an off-by-one), not a second, independent
oracle. Corrected in both files, and in `tools/generate_charset_tables.py`'s own comments describing
`REFERENCE_CHECKS`, to state this precisely: Python's stdlib codecs are the table-generation source;
fixtures derived from those same codecs are internal-consistency tests; the genuine independent
validation is the pydicom differential testing (§20) and DCMTK adjudication (§21) — a different
codebase, a different Unicode-mapping implementation. This is a wording correction only — the
underlying pydicom/DCMTK results themselves are unchanged and were never mischaracterized.

No charset decoding behavior, no dataset-scoping rule, no VR classification, and no test assertion
outcome changed in this pass. A1.1–A1.4 were not touched. No A1.6 work began. Regression after this
pass: 215/215 C++ tests (unchanged — no new C++ test cases in this closure pass), 39/39 Python tests
(37 prior + 2 new determinism regressions).

```text
A1.1: FROZEN at 6f05d9d
A1.2: FROZEN at e651c74
A1.3: FROZEN at 140d850
A1.4: FROZEN at 87e79fe
A1.5: FROZEN at <this closure commit>
A1.6: NOT STARTED
```
