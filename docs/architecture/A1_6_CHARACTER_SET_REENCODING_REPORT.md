# A1.6 — Charset-Aware Text Encoding and Mutation: Freeze Report

## 1. Starting frozen state

Commit `656edf4` (A1.5 closure freeze), verified directly: `git status` clean, **215/215** C++
tests (`ctest`), **39/39** Python tests (`pytest`), charset-table generation confirmed
byte-deterministic and matching the committed table.

## 2. Pre-A1.6 limitation

Confirmed by inspection: `fds::charset` exposed only `resolve_character_set_context()` and
`decode_text()` (read-only). No encode/set/mutate entry point existed anywhere in the codebase for
charset-aware text. A caller wanting to change `Riesmeier^Jörg` to a different Latin-1 name, or set
a UTF-8 `InstitutionName`, or write ISO 2022 multi-repertoire text, had exactly one option: supply
already-correctly-encoded raw bytes to `DICOMStructure::set_value()` directly — the library could
*read* rich text but could not safely *write* it.

## 3. API design

Free functions in the existing `fds::charset` subsystem (not new `DICOMStructure` members, matching
A1.5's own precedent):

```cpp
EncodedText encode_text(const std::vector<std::string>& values, VR vr,
                         const CharacterSetContext& context);          // pure, no DICOMStructure

SetTextStatus set_text(DICOMStructure&, const ElementPath&,
                        const std::vector<std::string>& values);       // existing element

SetTextStatus set_new_top_level_text(DICOMStructure&, Tag, VR,
                                      const std::vector<std::string>& values);  // new top-level element
```

`encode_text()`'s input/output type (`std::vector<std::string>`, UTF-8, one entry per VM component,
`^`/`=` embedded literally for PN) is identical to `decode_text()`'s output type — by construction,
`encode_text(decode_text(e, ctx).values, e.vr(), ctx)` round-trips. Raw-byte mutation
(`DICOMStructure::set_value` with caller-supplied bytes) is untouched and still means exactly what
it always meant; `set_text()` is a new, explicit, impossible-to-confuse-with-raw-bytes entry point,
per this task's §4 requirement.

## 4. Status/result model

`EncodeStatus`: `Success, NotATextVR, UnsupportedCharset, MalformedCharsetDeclaration,
InvalidUnicodeInput, UnrepresentableCharacter, ValueTooLong`. `SetTextStatus` is the same set plus
`PathNotFound` and `IsSequenceElement` (structural failures only `set_text()` can hit, since
`encode_text()` never touches a structure). No exceptions, no boolean collapse — matching A1.5's
established convention.

## 5. Supported charset envelope

Unchanged from A1.5, exactly: default repertoire, all ten PS3.5 single-byte repertoires, UTF-8
(`ISO_IR 192`). No broadening attempted or introduced — confirmed by inspection (the encoder's
`RepertoireId` enum is the identical closed set A1.5 already defined; no new repertoire identity was
added).

## 6. Default-repertoire behavior

Strict 7-bit ASCII, matching A1.5's decode-side strictness exactly (same `encode_in_repertoire`
codepoint<0x80 passthrough, same hard rejection above it). Confirmed by test: ASCII input under no
declaration succeeds; a single non-ASCII character under no declaration fails with
`UnrepresentableCharacter`, structure completely unchanged. A1.6 never auto-promotes a file to
UTF-8 or auto-inserts `(0008,0005)` — confirmed by inspection (`encode_text` only ever reads
`context`, never writes it) and by this being structurally impossible: `encode_text` has no access
to a `DICOMStructure` at all.

## 7. Ten single-byte repertoires

Reverse (`char32_t` → `uint8_t`) tables generated in the *same* `tools/generate_charset_tables.py`
run as A1.5's forward tables — never a second, independently-authored mapping — by deterministically
inverting each forward table, with injectivity (no two bytes decoding to the same code point)
verified for all ten during generation (none has a collision; if one ever did, generation aborts
rather than silently picking one). All ten repertoires round-trip a real, discriminating non-ASCII
character (`tests/integration/test_charset_encode.cpp`), independently confirmed by **pydicom**
(15/15 encoder-output files correctly decoded — §22) and **DCMTK** (6/6 spot-checked — §23). A
character genuinely outside the declared repertoire (Cyrillic under a Latin1-only declaration) fails
with `UnrepresentableCharacter`, never transliterated or dropped.

## 8. UTF-8 encoder

Strict: input is validated with the same RFC 3629 state machine A1.5 decode uses
(`is_valid_strict_utf8`); for a stand-alone `ISO_IR 192` context, already-validated bytes pass
through unchanged (no re-encoding transformation needed, since input and wire format are both
UTF-8). ASCII, 2/3/4-byte scalars, and a mixed string all tested; invalid/truncated input is
rejected (`InvalidUnicodeInput`), never partially encoded.

## 9. ISO 2022 encoder architecture

`encode_with_delimiter_reset` (the exact structural mirror of A1.5's `decode_with_delimiter_reset`):
walks the input code point sequence, tracking one "currently active repertoire" state variable
(initialized to `encodings[0]`, the same default `decode_text()` starts from); for each code point,
determines the target repertoire per §10's policy, emits an ISO 2022 escape sequence
(`escape_bytes_for`, a *forward* lookup into the identical `kEscapeTable` decode already uses — one
table, both directions, so encoder and decoder can never disagree about which bytes designate which
repertoire) only when the target differs from the current state, then the encoded byte. Verified
directly, not just via successful decode: a dedicated test inspects the actual wire bytes and
confirms the expected `ESC - A` / `ESC - L` sequences are present.

## 10. Deterministic repertoire-selection policy

For each code point: **(1)** if the currently-active repertoire can represent it, stay there — no
escape emitted; **(2)** otherwise, scan `context.terms`' declared order and use the first repertoire
that can represent it; **(3)** otherwise, fail with `UnrepresentableCharacter`. Rule (1) is not just
documented but tested directly: a code point representable in *both* the active repertoire (Latin3,
just switched into) and an earlier-declared one (Latin1) is confirmed to stay in Latin3 (exactly one
escape sequence emitted, not two) — proving the policy actually avoids gratuitous switching, not
merely that *a* legal encoding was produced. Determinism itself is tested: encoding the same input
under the same context twice produces byte-identical output.

## 11. Delimiter/reset semantics

Exact mirror of decode: state resets to `encodings[0]` at `\` between VM components (handled by the
caller's `values` vector, joined with a literal `\` byte — no separate reset logic needed, since
each component starts its own fresh `encode_with_delimiter_reset` call), at CR/LF/TAB/FF within
ordinary text, at `^` within one PN component group, and independently at `=` between PN component
groups (handled by `encode_pn_component` splitting on `=` first, mirroring `decode_pn_component`
exactly). All four reset points are dedicated-tested.

## 12. PN behavior

Component (`^`) and component-group (`=`) delimiters round-trip correctly, including the real name
`Riesmeier^Jörg` from A1.5's real corpus and a synthetic ideographic-group case
(`Yamada^Tarou=` + escaped Latin1). Multi-valued PN (two people, `\`-separated) round-trips each
independently. Cross-validated against pydicom (§22) and DCMTK (§23), both of which independently
recover the intended text from this library's own encoded bytes.

## 13. VM behavior

`LO`/`SH`/`PN`/`UC` multi-valued encoding preserves value boundaries and empty values (a three-value
`LO` with an empty middle value round-trips exactly). `ST`/`LT`/`UT` are enforced as VM=1 at the
encoder: passing more than one component for these three VRs is rejected as `InvalidUnicodeInput`
before any encoding work begins.

## 14. ST/LT/UT result

The A1.5 decode-direction fix (a literal backslash in `ST`/`LT`/`UT` content is not a value
delimiter) has its exact mirror here: a Windows-style file path
(`C:\Storage Card\var\patient\file.xml`) containing four literal backslashes encodes as one
contiguous value for all three VRs and decodes back to the identical string — proven with a
dedicated regression test naming the A1.5 bug class explicitly.

## 15. Padding

PS3.5 6.4: text values pad to even length with **SPACE** (`0x20`), never NUL (NUL is `VR::UI`-
specific, and UI is not Specific-Character-Set-governed — excluded by `is_text_vr()` before padding
logic is ever reached). Tested for both an odd-length ASCII result and an odd-length single-byte-
repertoire-encoded result (3 encoded bytes → 4, trailing byte `0x20`). Padding happens strictly
*after* encoding, on the final byte count — never on the input Unicode character count, which can
differ (confirmed structurally: padding logic operates on `joined.size()`, the byte vector, not on
any character-count value).

## 16. Representability failure

Every unrepresentable-character case (default repertoire + non-ASCII, single-byte repertoire +
character outside it, ISO 2022 + character needing an undeclared repertoire) returns
`UnrepresentableCharacter` (or `SetTextStatus::UnrepresentableCharacter` through `set_text`) and
is confirmed, by direct byte comparison, to leave the structure's raw bytes and `is_modified()` flag
completely untouched.

## 17. Atomicity result

`encode_text()` is a pure function — full validation (VR, context mode, UTF-8 well-formedness,
representability, padding, length ceiling) completes before it ever returns a byte vector; `set_text
()`/`set_new_top_level_text()` only call the underlying `DICOMStructure::set_value()`/`set()`
primitive *after* `encode_text()` reports `Success`. Every failure path (`PathNotFound`,
`IsSequenceElement`, all six `EncodeStatus` failure variants) is dedicated-tested to leave the
structure's raw bytes, `is_modified()`, and element identity completely unchanged — not merely
assumed from the code's structure.

## 18. Nested charset-context behavior

`set_text()` calls the *same* `resolve_character_set_context()` A1.5 already froze — no second
inheritance algorithm. Proven directly: a nested element with no local override correctly uses the
inherited root declaration; a nested element with its own `(0008,0005)` override uses that override
(a Cyrillic-only character succeeds where the root's Latin1 alone could not); two sibling Items, one
with an override and one without, do not leak into each other (the non-overriding sibling correctly
falls back to the *root's* declaration, not its sibling's).

## 19. Specific Character Set mutation policy

`(0008,0005)`'s own VR is `CS`, which is not one of the seven Specific-Character-Set-governed VRs —
so `set_text()` on it returns `NotATextVR` automatically, with zero special-casing required (this
followed directly from `is_text_vr()`'s existing, A1.5-frozen classification, confirmed by a
dedicated test). No dataset-transcoding operation (re-encoding every descendant text element after
changing the declaration) was implemented or attempted — explicitly out of this increment's scope
per the authorizing brief's §12, and documented here as the deliberate boundary: **charset-aware
element mutation uses the existing effective declaration; changing the declaration itself does not
automatically transcode the dataset** (no such operation exists in this codebase to accidentally
trigger it, either).

## 20–21. Round-trip property results

**Unicode → encode → decode == original**, within supported scope: proven for every repertoire
category (single-byte ×10, UTF-8, ISO 2022 multi-value, PN with both delimiter types, multi-valued
text) via write()+reparse()+`decode_text()` chains in `test_charset_encode.cpp`, and independently
via pydicom/DCMTK reading this library's own encoded output (§22–23) — not merely this library's own
decoder validating its own encoder. **bytes → decode → encode**: not required to be byte-identical
for ISO 2022 input (multiple legal escape placements can represent the same text, e.g. this
encoder's "prefer active repertoire" policy may choose different escape points than the original
source did) — this is stated explicitly, not left ambiguous, per the authorizing brief's §23
requirement to define the distinction rather than assume byte identity.

## 22. pydicom differential result

15 real encoded files (one per single-byte repertoire, one real-name PN, UTF-8 with a 4-byte
scalar, ISO 2022 multi-repertoire switching, PN component groups, multi-valued PN) were generated by
this library and read *independently* by pydicom (a fresh `dcmread`, no shared code path) —
**15/15 exact matches** between pydicom's decoded text and the intended Unicode input. Zero
unexplained disagreements.

## 23. DCMTK result

`dcmdump +U8` independently read 6 of the same encoder-output files (Latin1 PN with a real name,
Latin1, Cyrillic, UTF-8, ISO 2022 switching, PN component groups) — **6/6 exact matches**. The same
informational (non-error) warning A1.5 already documented — DCMTK noting an escape sequence in a
PN's first component group, then decoding it correctly anyway — reappeared here and is not treated
as a defect, consistent with the A1.5 report's own disposition of it.

## 24. Real-world mutation result

`test-SR.dcm` (A1.5's real corpus, containing the genuine German name `Riesmeier^Jörg`) had its
`PatientName` mutated to `Müller^Anna` via `set_text()`, written to a **local, non-redistributed**
temp file, and independently verified with pydicom: the target field changed correctly,
`SpecificCharacterSet` (`ISO_IR 100`) is unchanged, and a full pydicom-side comparison of every
other top-level tag (`StudyDate`, `SOPInstanceUID`, `CompletionFlag`, and every remaining tag's
value) found **zero unrelated differences** and an identical tag set. No file containing this
mutated content was committed to the repository or otherwise distributed.

## 25. Japanese negative control

`J2K_pixelrep_mismatch.dcm` (A1.5's real corpus negative case, declaring `['ISO 2022 IR 13',
'ISO 2022 IR 87']`) was used directly: `set_text()` on a real `LO` element returns
`UnsupportedCharset`, `structure.is_modified()` is `false`, and the element's raw bytes are
byte-identical before and after the attempt — confirmed on genuine real-world data, not only a
synthetic fixture.

## 26. Implicit-VR-origin result

A bare Implicit-VR-LE fixture (both `(0008,0005)` and the target `PatientName` written in genuine
Implicit VR wire form — tag+4-byte-length, no VR field) was parsed (A1.4 dictionary-resolves its VR
to `PN`), mutated via `set_text()` to the real name `Riesmeier^Jörg`, and written: the output is
re-encoded as Explicit VR per the existing, unmodified writer contract (A1.4's own documented
behavior — no new Implicit-VR output mode was introduced), and reparsing + `decode_text()` recovers
the mutated text exactly.

## 27. Pixel Data / unrelated-element preservation

A fixture with `Modality`, `PatientName`, and native Pixel Data was mutated (`PatientName` only via
`set_text()`) and written: reparsing confirms `Modality` is untouched and Pixel Data's native span
length (16 bytes) is unchanged — the semantic-reconstruction write contract (A1.3's ordering, A1.4's
resolution) is otherwise undisturbed by this increment.

## 28. Tests before/after

Before: **215 C++ tests**, **39 Python tests** (A1.5 closure baseline). After: **244 C++ tests**
(29 new — `tests/integration/test_charset_encode.cpp`), **39 Python tests unchanged**. All pass.

## 29. Performance/resource result

| Workload | Result |
|---|---|
| Single-byte encode, 5,000 small values | 1.78 ms (≈2,815 encodes/ms) |
| UTF-8 encode, one 160 KB value | 0.49 ms (≈324 MB/s) |
| ISO 2022 encode, one 30 KB switching value | 0.45 ms (≈66 MB/s) |

Encoding is entirely caller-invoked — confirmed by inspection, no encode code runs on any parse
path. **A real, self-introduced performance regression was found and fixed within this same
increment**: an early refactor made `is_valid_strict_utf8` share an implementation with the new
code-point-extracting `decode_utf8_to_codepoints`, which meant every UTF-8 validation (called
unconditionally by *both* `decode_text()` and `encode_text()`) started allocating and filling a full
`std::vector<char32_t>` just to answer a yes/no question — a measured ~15× UTF-8 throughput
regression (caught by this increment's own benchmarking, not by luck). Fixed by restoring
`is_valid_strict_utf8` to its original, allocation-free, validate-only state machine, kept
deliberately separate from the code-point extractor despite near-identical logic. Confirmed fixed:
A1.5's decode UTF-8 throughput is back to ≈3.5 GB/s, matching the figure the A1.5 freeze report
originally recorded; A1.6's own UTF-8 encode improved from ≈220 to ≈324 MB/s. A1.5's actual frozen
commit (`656edf4`) was never affected by this — the regression existed only transiently within
A1.6's own commit history and was caught and fixed before A1.6 itself was proposed for freeze.

## 30. Generated-table determinism result

The A1.5-closure determinism guarantee is preserved and extended: `tools/generate_charset_tables.py`
now emits both the decode (forward) and encode (reverse) tables in one deterministic run, with
injectivity verified during generation. `tools/verify_charset_tables_deterministic.py` and the two
`pytest` regressions in `tests/python/test_charset_tables_generation.py` (unchanged from A1.5
closure, still passing) confirm: committed file hash, regeneration #1, and regeneration #2 all
agree (`9b5534c24cf42b8ccb0448d839f8625a419c9cb240a3eea3ff3efeb0f4dd5128`). No volatile content
(timestamp, machine path, hostname) was reintroduced.

## 31. ABI/Python decision

**Deferred**, for the same reason A1.5 deferred it: the native C++ API is the complete A1.6
deliverable, proven exhaustively above. No ABI functions or Python bindings were added for
`encode_text`/`set_text`/`set_new_top_level_text`. This is not an oversight — A1.7 owns Python/ABI
ergonomics per the same boundary A1.4/A1.5 already established, and this increment does not disturb
it.

## 32. Any defect discovered in A1.1–A1.5

One, found and fixed within this same increment before ever being proposed for freeze (§29): a
self-introduced UTF-8 validation performance regression. It existed only in A1.6's own in-progress
commit history, never in a frozen commit — A1.1 through A1.5's frozen commits (`6f05d9d`, `e651c74`,
`140d850`, `87e79fe`, `656edf4`) are unaffected. No *semantic* defect was found in any frozen
increment; A1.5's decode results are unchanged by this fix (only the validation function's
performance characteristics were restored, not its logic — it was never semantically wrong, only
transiently slower).

## 33. Attrs Contract change

None required. A1.5's own "text VRs decode to raw bytes only... encode direction is A1.6" framing
(already corrected in `docs/roundtrip-contract.md` at the A1.5 freeze) now has its encode-direction
half delivered; no further contract-document correction was identified as stale.

## 34. Remaining limitations before A1.7

- No Python/ABI exposure for the encode direction (§31) — deferred, not designed against.
- Nested new-element insertion (a brand-new element inside a Sequence Item, as opposed to a new
  top-level element, which `set_new_top_level_text()` already supports) remains out of scope — the
  current structural mutation API has no primitive for it at any layer, charset-aware or not; adding
  one is a structural-API question for A1.7, not a charset-specific gap.
- No dataset-wide charset transcoding operation (§19) — a legitimate, larger future capability,
  explicitly not attempted here.
- Multi-byte code-extension repertoires (Japanese/Korean/Chinese) remain unsupported for both decode
  (A1.5) and encode (A1.6), unchanged.

## 35. Freeze recommendation

**PASS.** Every criterion in the authorizing brief's §36 is met with direct evidence: every A1.5-
supported repertoire has a working, independently-verified encode path; representability is
validated, never guessed; unsupported/unrepresentable input never silently degrades and always
leaves the structure provably unchanged (atomicity tested directly, not assumed); the default
repertoire remains strict ASCII; UTF-8 output is strict and independently confirmed valid; single-
byte reverse mappings are generated (never hand-authored) from the same source as the frozen decode
tables, with injectivity verified; ISO 2022 switching is standards-aware, deterministic, and its
wire bytes were directly inspected, not just decoded back; PN and VM delimiter semantics are
correct, including the ST/LT/UT literal-backslash mirror of the A1.5 fix; padding is correct and
applied after encoding; output reparses and A1.5 decode recovers the original Unicode in every
tested case; pydicom and DCMTK both independently corroborate with zero unexplained disagreement,
on this library's actual encoded bytes, not merely its own decoder checking its own encoder; a
genuine real-world file was mutated and independently verified with zero unrelated changes; the
Japanese-unsupported real file fails safely with zero mutation; Implicit-VR-origin mutation works
under the existing write contract; Pixel Data and unrelated content are unaffected; generated-table
determinism is preserved; all 244 C++ and 39 Python tests pass; a real performance regression was
caught and fixed within the increment itself; no A1.7 work began.

```text
A1.1: FROZEN at 6f05d9d
A1.2: FROZEN at e651c74
A1.3: FROZEN at 140d850
A1.4: FROZEN at 87e79fe
A1.5: FROZEN at 656edf4
A1.6: PASS, candidate freeze commit 278578a (pending this report's own commit)
A1.7: NOT STARTED
```
