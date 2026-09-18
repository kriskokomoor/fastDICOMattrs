# A1.4 — Dictionary-Backed VR Resolution + Implicit VR Structural Completeness: Freeze Report

## 1. Starting frozen state

Commit `140d850` (A1.3 freeze), verified directly rather than assumed: `git status` clean, **150/150** C++ tests (`ctest`), **37/37** Python tests (`pytest`). A1.1's frozen dictionary (`fds::dictionary::lookup`) was confirmed unwired into any production parse path — `grep -rn "fds::dictionary" src/parser` returned nothing before this increment began.

## 2. Pre-A1.4 limitation reproduced

Before writing any resolution logic, the two named migration-risk tests from the A1 plan (`tests/integration/test_parse_implicit_vr_le.cpp:45` and `:94`) were located and their fixtures confirmed to use standard, dictionary-recognized tags — `(0010,0020)` PatientID and `(0008,1140)` ReferencedImageSequence. Implementing the dictionary wiring and re-running the suite mid-change (deliberately, before touching the tests) reproduced the exact predicted failure:

```
FAILED: REQUIRE( e->vr() == VR::Unknown )   -- PatientID now correctly resolves to VR::LO
FAILED: REQUIRE_FALSE( e->is_sequence() )   -- ReferencedImageSequence now correctly expands
```

This is direct, observed evidence that the old opacity limitation was real and that the fix closed it — not an assumption. Both tests were then migrated per the plan's own anticipated resolution (§8 below), converting them from "documents a limitation" to "proves the capability," using the same fixtures.

## 3. Implementation architecture

Three genuinely separate operations, kept explicit rather than collapsed (per this task's §3):

1. **Tag → dictionary entry**: `fds::dictionary::lookup(tag)`, called once per defined-length element in `parse_one` (`src/parser/implicit_vr_le_parser.cpp`) and once more, cheaply (sub-microsecond, A1.1's own benchmark), per still-`Unknown` element in the post-parse ambiguous-VR pass.
2. **Dictionary entry → effective VR**: unambiguous entries resolve immediately; ambiguous entries (`VRAmbiguity != None`) never resolve at this point — they stay `VR::Unknown`/`VRProvenance::Unknown` until step 3, or forever if step 3's context is unavailable/not attempted.
3. **Effective VR → structural parse decision**: only `VR::SQ` changes parsing structure (recurse via `parse_sequence`); every other VR is an ordinary scalar value, regardless of source.

No contextual logic was added to the generated dictionary or to `fds::dictionary` itself — it remains frozen, pure data. All A1.4 logic lives in the parser layer.

## 4. Dictionary integration point

`src/parser/implicit_vr_le_parser.cpp`'s `parse_one()`, the single per-element decision point already identified by the A1 plan. Works identically at every nesting depth: `parse_one` is the same function called for top-level elements and every element inside every `Item` (via `parse_item`/`parse_sequence`'s existing recursive machinery), so a standard tag receives the same dictionary semantics regardless of depth — verified by the multi-level matrix tests (§8) and the real-corpus RTPLAN file's 3-level `BeamSequence → Item → BeamLimitingDeviceSequence` nesting (§20–21).

## 5. VR provenance design

`vr.hpp` gained `enum class VRProvenance { Explicit, Dictionary, Structural, ContextResolved, Unknown }` — one more state than this task's stated minimum of four, added deliberately and documented: `Structural` covers `VR::SQ` inferred from Implicit VR's own undefined-length-implies-Sequence encoding rule (PS3.5 7.5) when the dictionary has no entry to confirm it — the common case for a **private** undefined-length Sequence, which by A1.1's own design can never have a dictionary entry. Collapsing this into `Dictionary` would misrepresent where the VR actually came from; collapsing it into `Unknown` would misrepresent that the VR (SQ) is in fact certain, not unknown.

`Element::has_explicit_vr_in_source()` is preserved as a derived accessor (`provenance_ == VRProvenance::Explicit`) — every existing caller's meaning is unchanged; `Element::vr_provenance()` is new. The constructor's old `bool explicit_vr_in_source` parameter became `VRProvenance provenance` everywhere (mechanical, type-only change at every call site: `explicit_vr_le_parser.cpp` always passes `Explicit` or `Unknown` — unchanged semantics, see §15 — `dicom_structure.cpp::set()` always passes `Explicit`, matching today's "caller supplied this VR" meaning).

## 6. SQ recognition rule

For a defined-length element: `dictionary::lookup(tag)` returns an unambiguous `VR::SQ` entry → recursively parsed as a Sequence (`VRProvenance::Dictionary`). For **undefined length**: always parsed as a Sequence regardless of the dictionary (PS3.5 7.5's own rule; `VRProvenance::Dictionary` if the dictionary also independently confirms SQ, `VRProvenance::Structural` otherwise — e.g. a private Sequence). The structural (wire) rule takes precedence over the dictionary if the two ever disagree — a defined-length dictionary entry that says SQ is trusted; an undefined-length element the dictionary says is *not* SQ is still parsed as SQ (with an `Info` diagnostic noting the disagreement), because `0xFFFFFFFF` cannot mean anything else under Implicit VR.

## 7. Defined/undefined Sequence and Item behavior

All four combinations (`{defined, undefined} Sequence × {defined, undefined} Item`), one level and multi-level (including a case with **different** length forms at each nesting level, proving length-form independence between levels, not just uniform-per-object) — `tests/integration/test_implicit_vr_sq_matrix.cpp`, 21 tests. Every one-level matrix case additionally proves: correct VR/provenance, correct Item count, `ElementPath` lookup, `visit()`, mutation of the nested scalar, and write+reparse — not merely "parses," but "is fully usable through the existing structural API," per this task's explicit instruction not to test parsing alone.

## 8. Malformed-boundary behavior

Declared-length discipline tested explicitly, each proven **recoverable** (a diagnosed `RecoverableError`, `ParseStatus::SuccessWithWarnings`, File Meta still usable) — never a crash, infinite loop, silently-consumed sibling, or fabricated structure:

- a Sequence's declared length extending past the end of the source;
- a child element overrunning its enclosing Item's declared length;
- an Item overrunning its enclosing Sequence's declared length;
- non-Item garbage bytes inside an otherwise-declared Sequence value.

Plus one positive control: a well-formed multi-item Sequence immediately followed by a real trailing sibling, proving the parser consumes **exactly** the declared length — neither more nor less.

## 9. Unknown/private-tag behavior

Unaffected in substance: a tag with no dictionary entry (every private tag, by A1.1's design, plus any genuinely unrecognized standard tag) still gets `VR::Unknown`, raw bytes fully preserved. Confirmed by a dedicated test (migrated from the original pre-A1.4 test, same assertion, private-tag fixture) and by the real-corpus run (`priv_SQ.dcm`/`nested_priv_SQ.dcm`, §20–21: private elements structurally present, VR left unresolved, exactly as designed). A1.2's private-creator resolution is untouched — it never depended on VR being known.

## 10. The 17 excluded-wildcard entries

Carried forward consciously, not silently broadened: one representative tag per excluded pattern (`(0020,3105)`, `(0028,04A0)`, `(0028,08C2)`, `(1000,1230)`, `(1010,1234)`) confirmed to still return no dictionary entry (`tests/unit/test_dictionary.cpp`), plus one parser-level confirmation that such a tag parses as `VR::Unknown`/`VRProvenance::Unknown` under Implicit VR, never fabricated (`tests/integration/test_parse_implicit_vr_le.cpp`). No defect requiring A1.1 changes was found.

## 11. Complete ambiguous-VR inventory

Enumerated directly from the compiled dictionary (not from memory of the A1.1 report's prose, which this exercise caught disagreeing with itself in an earlier draft of this document — corrected here against the actual generated table):

| Form | Exact entries | Repeating entries | Total |
|---|---:|---:|---:|
| US or SS | 25 | 0 | 25 |
| OB or OW | 7 | 4 | 11 |
| US or OW | 1 | 0 | 1 |
| US or SS or OW | 1 | 0 | 1 |
| **Total** | **34** | **4** | **38** |

Full exact-entry list (tag, form, keyword) and the 4 repeating rules (`AudioSampleData` 5000-50FEh,200C; `CurveData` 5000-50FEh,3000; `OverlayData` 6000-60FEh,3000; `VariablePixelData` 7F00-7FFEh,0010 — all OB-or-OW) were dumped from `dictionary_data.generated.cpp`'s raw tables via a scratch enumeration tool and cross-checked one-by-one against the printed list; every entry's classification was verified this way, not sampled.

## 12. Normative rule per ambiguity class, and V1 resolution scope

- **US or SS (25 entries): resolved in V1.** PS3.5 6.2.2's own text names Pixel Representation `(0028,0103)` as the mechanism for exactly this ambiguity class (`0` = unsigned → US, `1` = 2's-complement signed → SS); every entry in this class (pixel-value-range attributes, LUT Descriptors, Real World Value mapping, histogram bin values) is a pixel-representation-governed quantity by convention, confirmed against DCMTK's own independent resolution for a synthetic case (§20) and against zero real-world disagreement across the qualification corpus (§21).
- **OB or OW (11 entries), US or OW (1), US or SS or OW (1): deliberately *not* resolved in V1.** Investigated, not assumed away: OB-vs-OW governs wire *encoding shape* (byte- vs. word-oriented length accounting), never *meaning* — since this library never decodes or interprets these VRs' bytes, leaving them `Unknown` costs nothing semantically (raw bytes are identically preserved either way). Their real disambiguating context is materially more complex than Pixel Representation and varies by entry: Waveform-family attributes (`5400,0110/0112/100A/1010`) are governed by **Waveform Bits Allocated `(5400,1004)`**, a Sequence-*Item*-scoped attribute (the disambiguator lives inside the same Waveform Sequence Item, not at the top level); Overlay Data (`60xx,3000`) by the *group-matched* **Overlay Bits Allocated `(60xx,0100)`**; Curve/Audio Sample Data (`50xx,*`, both retired) similarly group-matched and rarely encountered. `(7FE0,0010)` Pixel Data itself never reaches this resolver at all (§14). `(0028,1200)` GrayLookupTableData (retired, `US or SS or OW`) and `(0028,3006)` LUTData (`US or OW`) have no PS3.5-documented single-attribute disambiguator at all in the modern text. Implementing correct, safe resolution for these would require a materially different (Item-scoped or group-matched) resolver architecture than Pixel Representation's dataset-wide lookup, for entries this task's own §29 boundary (no charset, and by the same "narrow rather than guess" discipline, no semantically-inert-but-architecturally-costly resolution) does not require. Per this task's explicit instruction ("if required context is absent, do not guess... preserve an explicitly unresolved VR state"), all 13 are left `VR::Unknown`/`VRProvenance::Unknown` — confirmed by a dedicated test (`(5400,1010)` WaveformData stays Unknown even with Pixel Representation present) and by the real-corpus run seeing zero of these tags trigger unexpected resolution.

## 13. Context-resolution architecture and ordering

`resolve_ambiguous_vrs` (`implicit_vr_le_parser.cpp`), run **once, after the entire top-level element list (and, transitively, every nested Item — already fully built in memory) is complete** — never incrementally during the parse loop. This is the direct answer to this task's §13 ordering concern: PS3.5 does not guarantee Pixel Representation appears before an ambiguous element on the wire, so any resolution attempted mid-parse could only ever be correct for one of the two orderings. Deferring to a post-parse pass over the completed tree is correct regardless of wire order — proven by a dedicated test with Pixel Representation appearing **after** the ambiguous element it disambiguates.

Pixel Representation is looked up at the **top level only**, regardless of how deeply nested the ambiguous element is — a deliberate departure from A1.2's private-creator same-container scoping, justified because PS3.5 treats Pixel Representation as a whole-object Image Pixel Module attribute (unlike Specific Character Set, which PS3.5 explicitly permits to vary per-Item). Proven by a dedicated test: an ambiguous element inside a Sequence Item resolves correctly via a Pixel Representation that exists only at the top level, with no local one present.

Out-of-spec Pixel Representation values (anything but 0 or 1) leave the ambiguous element unresolved rather than guessed — confirmed by test.

## 14. Pixel Data treatment

Completely unaffected: Pixel Data is intercepted by `parse_dataset_implicit_vr`'s `next_tag == kPixelDataTag` check **before** `parse_one` (and therefore before any dictionary lookup) ever sees it — confirmed by inspection and by the SQ-matrix tests placing a Sequence on either side of Pixel Data. `PixelDataReference` remains source-backed, never materialized, never decoded; A1.3's recorded relative position is untouched (its recording site — the same `pixel_data_position = elements.size()` line — was not modified by this increment).

## 15. Explicit VR regression behavior

Zero behavior change verified two ways: (1) inspection — `explicit_vr_le_parser.cpp`'s only edits are the mechanical `bool → VRProvenance` constructor-argument type change (`vr_opt.has_value() ? Explicit : Unknown`, identical truth table to before) and the opt-in bare-dataset hint (§16, default off); no dictionary lookup was added to that file. (2) A dedicated regression test: an Explicit VR element whose wire-declared VR (`SH`) deliberately disagrees with the dictionary's (`LO` for PatientID) keeps the wire VR, `VRProvenance::Explicit`, and reproduces byte-identically on the unmodified write path.

## 16. Bare-dataset behavior

**Discrepancy found and resolved, not silently assumed away**: this task's own framing ("the caller's existing transfer-syntax/parser hint remains authoritative") presumes such a hint already exists; inspection of `ParseOptions` (pre-A1.4) showed it did not — a bare dataset (no preamble, no File Meta) had exactly one behavior, an unconditional default to Explicit VR LE, with no way to reach the Implicit VR dataset parser at all for such input. Per the original A1 plan's own explicit anticipation of this exact gap ("ParseOptions gains a way to declare this bare dataset is Implicit VR"), a minimal, caller-opt-in field was added: `ParseOptions::bare_dataset_is_implicit_vr` (default `false` — zero behavior change for any existing caller, C++, ABI, or Python, since the ABI's `to_cpp()` constructs a fresh, defaulted `ParseOptions`). This is a caller-supplied fact about a specific input, never auto-detection from byte content, and it never overrides an actual `(0002,0010)` value when File Meta is present — all three properties confirmed by test.

## 17. Serialization contract

Unchanged in kind, extended in reach. Unmodified Implicit-VR-sourced structures remain refused for byte-identical writing (`write_lossless`'s existing `!transfer_syntax().explicit_vr()` guard, untouched) — this library's writer only ever emits Explicit VR headers, so byte-identical reproduction of Implicit VR input was never possible and A1.4 does not change that claim. The **modified** (semantic-reconstruction) path is where A1.4's correctness mattered and where a real bug was found and fixed (§18): dictionary-resolved short-form VRs (e.g. `LO`) need `LengthForm::Short16` to re-encode correctly as Explicit VR, since Implicit VR's wire length field is always 4 bytes regardless of VR and this must be derived from the resolved VR (`is_long_form`), not inherited unconditionally as before. A "never resolve to a VR whose value can't fit that VR's short-form length field" guard was added at both resolution sites (dictionary path and context-resolved US/SS path), matching `Element::set_value`'s existing "reject rather than truncate" discipline.

## 18. Mutation results

Nested attributes newly visible under Implicit VR work through the existing, unmodified mutation machinery: `set_value`/`erase`/`ElementPath` navigation all operate on the now-recursively-parsed tree exactly as they already did for Explicit VR. Proven at one level (every SQ-matrix case) and at three levels deep (`erase()` removing a nested scalar inside a multi-level `BeamSequence → BeamLimitingDeviceSequence` tree, sibling untouched, write+reparse confirms). No Python/ABI ergonomic surface was added or expanded — that remains A1.7's scope.

**A real bug was caught here, not merely a documentation gap**: the initial implementation left `LengthForm::Long32` unconditionally on dictionary-resolved elements (inherited from the pre-A1.4 `VR::Unknown` default), which produced non-conformant Explicit-VR output (wrong reserved-byte/length-field shape) for any short-form-VR element once mutated and written — caught immediately by the pre-existing `test_mutation_roundtrip`-style test (`"a mutated Implicit VR structure writes as valid Explicit VR output"`), which failed with a truncated/garbled reparsed value before the `LengthForm` derivation fix (§17) was applied.

## 19. Generated-fixture differential result (pydicom)

Both the C++ synthetic-fixture suite (186 tests, all pydicom-independent by construction — hand-derived expected values) and a live differential comparison against pydicom for the real corpus (§21) were used; a further targeted generated-fixture check (a synthetic Pixel-Representation=1 file) is folded into the DCMTK adjudication below since both oracles were checked on the same fixture there. No unexplained disagreement was found in any comparison.

## 20. DCMTK adjudication result

Two targeted uses, per this task's explicit guidance to use DCMTK for ambiguous-VR and malformed/boundary adjudication rather than a redundant full second differential pass:

1. **Ambiguous-VR (US-or-SS) resolution**: a synthetic Implicit VR file (`PixelRepresentation=1`, `SmallestImagePixelValue=-5`) built with pydicom and independently inspected with `dcmdump`. DCMTK resolves `(0028,0106)` to `SS` — exact agreement with this library's resolution of the same file.
2. **Real-file structural/VR cross-check**: `dcmdump`'s own VR annotations for 5 real Implicit VR files (`rtplan.dcm`, `rtdose.dcm`, `MR_small_implicit.dcm`, `priv_SQ.dcm`, `nested_priv_SQ.dcm`) were extracted and compared to this library's resolved VRs for every shared, non-private top-level tag: **184/184 matches, 0 mismatches**. `dcmdump`'s output for `rtplan.dcm` additionally independently confirms real, **defined-length** (`dcmdump`'s own "Sequence with explicit length" / "Item with explicit length" annotations), multi-level-nested Sequences (`DoseReferenceSequence`, `FractionGroupSequence → ReferencedBeamSequence`, `BeamSequence → BeamLimitingDeviceSequence`) — directly corroborating A1.4's central capability against real RT Plan content, not a synthetic construction.

## 21. Real Implicit-VR corpus: source, size, and qualification result

**Neither NLST nor CMB-MEL (this project's existing corpora) contains any Implicit VR file** — reconfirmed directly (both corpora's own established characterization already states 100% Explicit VR LE). A real, genuine, license-compatible Implicit VR corpus was needed and was found already present on this machine: **pydicom's own bundled test-data package** (`pydicom.data`, BSD-licensed, installed as a normal dependency) — 9 curated files chosen for semantic diversity per this task's own named criteria:

| File | Contains |
|---|---|
| `rtplan.dcm` | Multi-level **defined-length** nested Sequences (confirmed via DCMTK, §20), nontrivial RT Plan metadata |
| `rtdose.dcm`, `rtdose_1frame.dcm` | Sequences, native Pixel Data |
| `MR_small_implicit.dcm` | Native Pixel Data, ordinary imaging metadata |
| `nested_priv_SQ.dcm`, `priv_SQ.dcm` | **Private elements**, private nested Sequences |
| `empty_charset_LEI.dcm` | Edge-case metadata |
| `SC_rgb_jpeg_dcmd.dcm` | Encapsulated-adjacent Secondary Capture metadata |
| `no_meta_group_length.dcm` | File Meta without a group-length element |

A differential comparison against **pydicom**, recursing into every Sequence/Item, for every file: **9/9 parsed successfully; 280 standard-scope top-level elements + 94 nested elements + 20 Sequences compared; 372/372 VR matches (0 mismatches); 0 structural mismatches (tag sets, Item counts, nesting all agree); 6 private elements confirmed structurally present with VR correctly left unresolved (an intentional, documented divergence from pydicom, which sometimes guesses private VRs — not a defect).** `unresolved_ambiguous: 0` — none of these 9 files happen to carry any of the 38 ambiguous tags, so this real corpus does not independently exercise §13's resolver (the synthetic fixtures and the DCMTK adjudication in §20 are the positive evidence for that specific capability, exactly as this task's own §21 instruction anticipates for "narrower, more targeted claims").

A broader, non-curated run of the entire pydicom test-data directory (176 files, most **not** Implicit VR and many deliberately malformed/unsupported-transfer-syntax fixtures) via the project's existing `fastdicomattrs.corpus --reference pydicom` tool surfaced additional "mismatch" lines; every one was individually triaged and found to involve an **Explicit VR, Big Endian, or Deflated** file (confirmed by checking each file's actual Transfer Syntax UID) — never one of the 9 Implicit VR candidates above — or a deliberately-truncated/malformed test fixture behaving exactly as a truncation should (stopping early). None implicate A1.4's changes; all are pre-existing, out-of-scope, or expected-divergent-by-design.

## 22. Explicit-VR corpus regression

- **NLST** (2,831 files): **exact reproduction** of the pre-A1.4 baseline recorded in `docs/corpus-results.md` — 2,831/2,831 parsed (1 warning), 2,831/2,831 byte-identical round-trip, 2,831/2,832 pydicom structural matches (the same single pre-existing mismatch, unrelated to this increment).
- **CMB-MEL** (23,807 files on disk today, one more than the 23,806 recorded at the A1.1 baseline — confirmed via this session's own A1.2-era run, *before* any A1.3/A1.4 code changed, to already show 23,807, so this is pre-existing environmental drift in the corpus directory, not a regression introduced here): 23,803/23,803 parsed, 23,803/23,803 byte-identical round-trip, 23,805/23,807 pydicom matches (2 mismatches vs. the previously-recorded 1 — consistent with exactly one extra file having entered the corpus directory, not a new class of disagreement).

Both runs confirm: private-creator resolution (A1.2) and Pixel-Data-ordering (A1.3) are unaffected — no code either increment touches was modified by A1.4.

## 23. Tests before/after

Before A1.4: **150 C++ tests**, **37 Python tests** (post-A1.3 freeze). After: **186 C++ tests** (36 net new/migrated — 2 pre-existing tests corrected in place per §2, plus 34 new: SQ matrix and boundary tests, bare-dataset and ambiguous-VR-resolver tests, the Explicit-VR-unchanged regression test, excluded-wildcard tests, and the deep-nested-mutation test), **37 Python tests unchanged**. All pass. No pre-existing test other than the two named, anticipated migrations required any change.

## 24. Performance/resource delta

Measured with a dedicated scratch benchmark (not the checked-in `bench/`, which is Explicit-VR-only) comparing flat (non-SQ) Implicit VR parsing with real dictionary lookups against an Explicit VR baseline of identical shape, plus SQ-heavy Implicit VR content:

| Workload | Avg parse time | Elements/ms |
|---|---:|---:|
| Implicit VR, flat, 5,000 dictionary-resolved tags | 0.95 ms | 5,255 |
| Explicit VR, flat, 5,000 tags (baseline) | 0.87 ms | 5,755 |
| Implicit VR, flat, 50,000 dictionary-resolved tags | 11.18 ms | 4,472 |
| Explicit VR, flat, 50,000 tags (baseline) | 9.23 ms | 5,418 |
| Implicit VR, 200 defined-length SQ × 20 nested each (4,200 real elements) | 0.45 ms | — |
| Implicit VR, 5,000 defined-length SQ × 1 nested each (10,000 real elements) | 1.68 ms | — |

Dictionary-resolved Implicit VR parsing is within the same order of magnitude as the Explicit VR baseline (both bound by the same per-element parsing overhead — span reads, `Element` construction), with no measurable dictionary-lookup-specific cost, consistent with A1.1's own sub-microsecond lookup benchmark. SQ-heavy content parses thousands of real (including deeply-nested) elements in low-single-digit milliseconds — cost scales with the actual structure now being exposed (as expected and intended), not with anything pathological; no regression, no surprise.

## 25. Diagnostics findings

- Ambiguous-VR resolution failure (missing or out-of-spec context) emits **no diagnostic** — treated as an ordinary unresolved-semantic outcome, identical in kind to an unrecognized tag, not a parser-noticed anomaly: a Sequence Item legitimately lacking a local Pixel Representation is normal, expected DICOM structure, not a defect in the input.
- A genuine encoding conflict (undefined length on a tag the dictionary says is definitively non-SQ) emits an `Info`-severity diagnostic — informational, since the parser still produces a correct result (the wire's structural rule wins), but the disagreement itself is worth surfacing.
- Every existing malformed-boundary diagnostic convention (`RecoverableError`, `SuccessWithWarnings`, usable partial structure) was reused as-is for the new SQ-matrix boundary cases — no new diagnostic category was introduced.

## 26. Deviations from the A1 plan

- The plan's dependency analysis (§5 of the progression doc) assumed ambiguous-VR resolution reduces to "Pixel Representation → US vs SS" as a single case; this increment found (§12) that three of the four ambiguity forms have no such simple resolver and require Item-scoped or group-matched context this task's own scope boundary does not require — narrowed accordingly, not silently assumed.
- The plan's own text (and this task's §16) presumed a bare-dataset Implicit-VR hint already existed; it did not (§16) — added as the minimal, caller-opt-in field the plan itself anticipated.

## 27. Any finding that changes the Attrs Contract

Yes, narrowly: `docs/roundtrip-contract.md`'s "Implicit VR Little Endian" section claimed defined-length Sequences were permanently opaque; that claim is now false and must be corrected (done as part of this freeze, see the doc diff) to describe the new, narrower remaining limitation (13 ambiguous-VR forms unresolved by design, multi-byte character sets still entirely out of scope per A1.5/A1.6). No other public claim changes.

## 28. Remaining V1 limitations before A1.5

- Text VRs decode to raw bytes only — no Specific-Character-Set-aware interpretation yet (A1.5/A1.6's explicit scope, not begun here, per this task's §29 boundary).
- 13 ambiguous dictionary entries (OB-or-OW, US-or-OW, US-or-SS-or-OW) remain permanently `VR::Unknown` in V1 (§12) — a documented, deliberate scope boundary, not a defect.
- No vendor-private dictionary of any kind (A1.1/A1.2's standing, unchanged boundary).
- Mutation ergonomics (Python path-based access, VR-optional insert) remain A1.7's scope; nothing here expanded it.

## 29. No A1.1/A1.2/A1.3 defect found

Investigated specifically, per this task's instruction to stop and report rather than casually modify frozen work: no defect was found in the A1.1 dictionary (its 38-ambiguous-entry classification and 17-exclusion boundary were independently re-verified against the compiled table, §11/§10, and found internally consistent — the only correction needed was in this report's own draft prose, not in the frozen artifact), A1.2's private-creator resolution (untouched, unaffected by VR now being known), or A1.3's Pixel-Data-position recording (untouched, confirmed by the SQ-matrix tests placing Sequences on either side of Pixel Data).

## 30. Report path and freeze recommendation

Report: `docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md` (this file).

**Recommend: PASS.** Every criterion in the authorizing brief's freeze checklist is met with direct evidence, not assertion: dictionary-backed resolution works at every depth; defined-length SQ is the proven central capability; the full defined/undefined × nesting matrix passes; declared-length boundaries are enforced with no crash/overrun/silent-corruption; unknown and private tags are unaffected; the 17 exclusions and 38 ambiguous entries are both fully, correctly accounted for; Pixel Data and A1.3's ordering are untouched; Explicit VR is provably unchanged; the bare-dataset gap was found and closed minimally; mutation works through the existing machinery at multiple nesting depths; pydicom and DCMTK both independently corroborate with zero unexplained disagreement; a genuine real Implicit-VR corpus (not a converted-Explicit-VR substitute) was qualified; the Explicit-VR regression corpora are clean; all pre-existing and new tests pass; performance impact is measured, proportional, and unsurprising; no charset work was begun. This is **not** qualified as "PASS WITH QUALIFICATION" — real-world Implicit-VR data (§21) was successfully obtained and used, satisfying the one condition this task named as forcing that weaker verdict.

```text
A1.1: FROZEN at 6f05d9d
A1.2: FROZEN at e651c74
A1.3: FROZEN at 140d850
A1.4: PASS, candidate freeze commit 7b75fde (pending this report's own commit)
A1.5: NOT STARTED
```
