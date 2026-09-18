# A1.3 — Pixel-Data-Relative Ordering: Freeze Report

## 1. Implementation status: PASS

The writer no longer always emits Pixel Data last. `DICOMStructure` now records where Pixel Data
actually sat among the top-level elements (`pixel_data_position()`), and `write_lossless` honors
it — for both the unmodified (byte-identical) and modified (semantic-reconstruction) write
contracts, each with its own correct, non-stale interleaving rule. Pixel Data remains a distinct,
source-backed `PixelDataReference` throughout — never materialized, never copied, never decoded.

## 2. Starting commit

`e651c74` (A1.2 freeze).

## 3. Commits created

1. Implementation + tests + doc corrections (this increment's main commit).
2. `docs: A1.3 pixel-data-relative ordering freeze report -- PASS` (this file).

## 4. Pre-fix defect reproduced

Before any code change, the pre-existing test
`tests/integration/test_roundtrip_lossless.cpp`'s *"An element after Pixel Data breaks the
unmodified byte-identical guarantee"* already pinned the defect as a **documented, expected
failure of byte-identical round-tripping** — it asserted `written_bytes != b.bytes()` for a fixture
with native Pixel Data followed by a Data Set Trailing Padding `(FFFC,FFFC)` element. Running the
full suite at the A1.2 freeze commit confirmed 139/139 tests passing, this one included, proving
the defect was real and exactly as documented: the writer produced valid, same-size, but
differently-ordered output (Pixel Data emitted first, then the trailing element — instead of the
source's trailing-element-then-nothing-more-after-Pixel-Data order). After implementing the fix,
this same test's old assertion (`written_bytes != b.bytes()`) started **failing**, i.e. output
became byte-identical — the exact converse of the defect, confirmed by intentionally running the
suite mid-fix and observing that specific, expected failure before correcting the assertion (§8).

## 5. Representation change chosen

`DICOMStructure` gained one new field, exactly the plan's preferred minimal design:

```cpp
std::optional<std::size_t> pixel_data_position_;
```

exposed via a new accessor, `pixel_data_position()`, and a new (defaulted, so no existing call site
broke) constructor parameter. Pixel Data itself is unchanged: still a separate
`std::optional<PixelDataReference>`, never folded into `elements_`. Both parser call sites that
feed one shared `elements`/`pixel_data` pair before constructing a `DICOMStructure` — the Explicit
VR LE top-level loop and the Implicit VR LE dataset loop in `parse_dataset_implicit_vr`
(confirmed by inspection to be the *only* two places a Pixel Data element is recognized during
parsing; there is exactly one `DICOMStructure` construction site,
`explicit_vr_le_parser.cpp:577`, matching the A1 plan's own claim) — were updated to record
`elements.size()` at the exact moment Pixel Data is consumed.

## 6. Positional invariant

`pixel_data_position()` is **the number of ordinary top-level elements that preceded Pixel Data in
the source** (an index into `elements()`; `element_count()` means "last"; `nullopt` means either
no Pixel Data, or an unknown position). Precisely:

- Pixel Data absent → the field is irrelevant (never consulted).
- Pixel Data first → `0`.
- Pixel Data in the middle → the count of elements before it.
- Pixel Data last → `element_count()` (nothing follows).
- No ordinary elements at all → `0`.
- Programmatically constructed (not parsed), position omitted → `nullopt`, which the writer
  documents and treats as "after every ordinary element" (the conventional layout, and this
  writer's own pre-A1.3 behavior) — proven by a dedicated test (§8).

Note this position counts **every** top-level element, File Meta Information (`group 0002`)
included — `DICOMStructure` has always represented File Meta and dataset content as one flat
ordered list, by design (`docs/architecture.md` §5's "one concept" decision); A1.3 introduces no
new File-Meta/dataset distinction, and the position is defined relative to the same single list
`elements()` already exposes.

## 7. Mutation interaction

**The recorded position is never updated after a mutation, and is never consulted once one has
happened — this is a deliberate invariant, not a gap.** `pixel_data_position()`'s value is only
ever read by the **unmodified** write path (`!structure.is_modified()`), which by construction can
only run before any `set`/`set_value`/`erase`/`erase_if` call — those all set `modified_ = true`
first. So the recorded position can never go stale, because it is structurally unreachable once
mutation could have invalidated it.

On the **modified** (semantic-reconstruction) write path, Pixel Data's placement is instead
**recomputed fresh from its own tag**, `(7FE0,0010)`, against the current `elements_` list: it is
written immediately before the first element whose tag is not less than `kPixelDataTag`. This is
exactly the same ascending-tag-order rule `DICOMStructure::set()` already applies to every
newly-inserted top-level element (see its existing `std::lower_bound` call) — Pixel Data simply
participates in that same, pre-existing contract instead of getting a special case. Concretely:

- Removing an element before or after Pixel Data: the remaining elements' tags are unchanged, so
  the recomputed boundary is automatically correct — no bookkeeping needed.
- Inserting a new element with a tag numerically before `(7FE0,0010)`: it lands before Pixel Data.
- Inserting a new element with a tag numerically after `(7FE0,0010)`: it lands after.
- Mutating a value in place (`set_value`) never changes any tag, so it can never change Pixel
  Data's computed position — proven by a dedicated test using a tag that numerically precedes
  Pixel Data and one that numerically follows it (see §8; a low-tag'd element that happened to sit
  *after* Pixel Data in the source is legitimately re-sorted *before* it once mutated — that is the
  existing ascending-tag-order reconstruction contract working as designed, not a defect).

This is exactly the distinction the authorizing brief asked to keep separate, not blur: **source-
order preservation for the untouched structure** (§6's recorded position, honored verbatim) versus
**semantic reconstruction ordering after mutation** (§7's fresh tag-order computation, the same
rule every other element already follows). No third contract was invented.

## 8. Native and encapsulated Pixel Data results

Both forms are handled by the same position mechanism (recorded at the same two parser call
sites, consumed by the same writer logic) — encapsulation only affects how `write_pixel_data`
serializes the payload itself (Basic Offset Table + fragment Items vs. one flat span), never where
it is placed among the other top-level elements. Tests prove both:
`tests/integration/test_pixel_data_ordering.cpp`'s "native Pixel Data in the middle..." and
"encapsulated Pixel Data followed by a trailing element remains trailing" cases.

## 9. Independent reparse result

The exact regression fixture (native Pixel Data + `(FFFC,FFFC)` trailing padding), built and
written through this library, was reparsed independently with **pydicom**:

```
has PixelData tag: True
has trailing padding tag: True
PixelData length: 16
trailing padding value: b'\x00\x00\x00\x00'
tag order: ['(7FE0,0010)', '(FFFC,FFFC)']
```

pydicom independently confirms Pixel Data is followed by the trailing element — the correct,
source-matching order — not the other way around. Output is structurally valid Part-10 content to
a second, mature, independent implementation; no unrelated attribute changed.

## 10. Tests

Before this increment: 139 C++ tests, 37 Python tests (post-A1.2). After: **150 C++ tests** (one
pre-existing test corrected from asserting the defect to asserting the fix, per §4; 11 new tests
in `tests/integration/test_pixel_data_ordering.cpp`), **37 Python tests unchanged**. All 150 + 37
pass. New/corrected coverage, matching the plan's matrix: Pixel Data last (unchanged,
byte-identical — proven by every pre-existing Pixel Data round-trip test continuing to pass),
Data Set Trailing Padding after Pixel Data (the corrected regression test), a non-`(FFFC,FFFC)`
ordinary tag after Pixel Data (proving the fix isn't special-cased to that one tag), Pixel Data in
the middle, encapsulated Pixel Data with a trailing element, mutation before/after Pixel Data,
insertion before/after Pixel Data's tag, removal before/after Pixel Data, no Pixel Data present,
and a programmatically-constructed structure exercising the documented `nullopt` → "last" default.

## 11. Corpus result

Re-ran the NLST corpus (2,831 real files) as a pure regression check, exactly reproducing the
pre-A1.3 baseline in `docs/corpus-results.md`: 2,831/2,831 parsed successfully-with-at-most-one-
warning, **2,831/2,831 byte-identical round-trip**, 2,831/2,832 pydicom structural matches (the one
known pre-existing mismatch, unrelated to Pixel Data ordering, unchanged). As the plan anticipated,
this corpus (100% Pixel-Data-last) cannot exercise the new capability positively — the synthetic
fixtures in §8/§10 are the positive evidence; this corpus run is the regression evidence that
nothing broke for the overwhelmingly common case.

## 12. Evidence Pixel Data remains source-backed

- `write_pixel_data` (`lossless_writer.cpp`) is unchanged by this increment except for *when* it is
  called, never *how*: it still reads directly from `structure.source()` via `SourceSpan`s and
  writes those bytes straight through — no intermediate buffer, no decode.
- `grep`-confirmed: no new `std::vector<std::byte>` copy or allocation was introduced on the Pixel
  Data write path; the only new state is one `std::size_t` index and a `bool` flag in the writer's
  local interleaving logic.
- The new representation field, `pixel_data_position_`, is a single `std::optional<std::size_t>` —
  negligible, fixed-size, independent of Pixel Data's actual byte length.

## 13. Report path and freeze recommendation

Report: `docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md` (this file). Related doc
corrections: `docs/roundtrip-contract.md` "Two write contracts" and "Known gaps" (the obsolete
"writer always emits Pixel Data last" gap is corrected, not left standing), `docs/architecture.md`
§8 (Pixel data).

**Recommend: freeze A1.3.** All acceptance criteria in the authorizing brief are met: the pre-fix
regression fixture's old assertion demonstrably failed once the fix landed (proving the defect was
real and is now closed); Pixel Data's original relative position is represented explicitly and
precisely defined; native and encapsulated forms both work; mutation cannot leave the position
metadata incoherent (it is simply never consulted post-mutation, by design); Data Set Trailing
Padding and an ordinary non-special-cased trailing tag are both preserved correctly; Pixel Data
remains referenced, never materialized; all 139 pre-existing C++ tests plus 37 Python tests still
pass; all 11 new tests pass; the one pre-existing test whose assertion the fix inverted was
corrected, not deleted or weakened; corpus regression is clean; no dictionary or parser VR-
resolution behavior was touched.
