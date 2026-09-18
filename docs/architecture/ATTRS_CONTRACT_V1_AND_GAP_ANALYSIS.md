# Attrs Contract v1 and Implementation Gap Analysis

Status: assessment only. No production code was changed to produce this document. Scope: the
`fastDICOMattrs` repository, evaluated against a new target role, plus a cross-repository finding
about where the code that actually fits that role already lives.

## 1. Executive conclusion

The repository presently named `fastDICOMattrs` is **not** a viable starting point for the
`fastDICOMattrs` described in the new architecture. It is a small, deliberately narrow DCMTK
wrapper built for a different problem — fast, read-only, top-level, single-file tag lookup across
large collections — and it explicitly declines the responsibilities the new contract requires:
sequence traversal, mutation, serialization, and typed value access.

However, a second, much more capable engine already exists one directory over, in
`fastDICOMstructure`. Its from-scratch C++ parser/writer (order-preserving structure, zero-copy
`SourceSpan` values, fidelity levels, structured diagnostics, pixel-data-by-reference, and a
proven byte-identical round-trip for unmodified Explicit VR input) is architecturally very close
to what the new contract asks `attrs` to guarantee. The problem is organizational, not technical:
that engine is currently badged as `fastDICOMstructure`, and a policy-evaluation layer
(`python/fastdicomstructure/policy.py`) has already started growing directly on top of it — which
is exactly the layering violation the new architecture exists to prevent. There is today no
separate `attrs` layer beneath `structure`; `structure` *is* the attrs layer, plus policy, fused
together.

**Recommended path: B — substantial restructuring**, specifically an extraction/re-layering, not
a rewrite of the underlying engine and not a simple incremental patch to either existing repository
in place. Promote `fastDICOMstructure`'s C++ core (parser, writer, `Element`/`Value`/`Tag`/`VR`/
`TransferSyntax`/`PixelDataReference`/`Source`) to be the new `fastDICOMattrs`. Rebuild
`fastDICOMstructure` as a thin consumer of that promoted library, carrying traversal, policy, and
orchestration only. Retire or clearly re-scope the current `fastDICOMattrs` (DCMTK wrapper) so the
name is not claimed by two incompatible things at once — see §9 for why this collision is real and
already committed to three repositories' documentation, not a hypothetical.

## 2. A naming collision that must be resolved before anything else

Three repositories on disk already use the name `fastDICOMattrs` to mean two different things:

- `fastDICOMattrs` (this repository, published as `fastDICOM`): a DCMTK-backed, read-only,
  top-level-only, file-path-only tag scanner. Its own README calls it "fast shallow/top-level
  attribute inspection" and states plainly that "sequence traversal is deliberately not implicit."
- `fastDICOMstructure/docs/architecture.md` §1 and `fastDICOMarchive/README.md` both describe
  `fastDICOMattrs` as "independent, cheap, DCMTK-backed attribute probe tier" — consistent with
  the above — and state as a **hard constraint**: "`fastDICOMstructure` MUST NOT depend on
  `fastDICOMattrs`." The two are documented today as parallel, non-dependent tiers, not a layered
  stack.
- The new architecture in this assignment describes `fastDICOMattrs` as the complete-semantics
  foundation that `fastDICOMstructure` depends on — the opposite relationship.

This is not a matter of interpretation; it is a direct contradiction between what is committed to
version control today and what the new architecture requires. Any implementation plan has to
either (a) repurpose the current `fastDICOMattrs` name for the promoted engine and rename today's
DCMTK wrapper to something else (e.g. a `fastDICOMscan` probe tool, keeping its niche use in
`fastDICOMarchive`), or (b) pick a different name for the promoted engine. Given `fastDICOMarchive`
already has its own `docs/fastdicomattrs-capability-assessment.md` built around today's DCMTK
wrapper's actual behavior, (a) is the lower-disruption choice, but it is a deliberate rename this
document flags rather than performs.

## 3. Proposed role of `fastDICOMattrs` (restated precisely)

`fastDICOMattrs` owns: parsing raw DICOM bytes (file, buffer, and eventually stream) into a
complete, order-preserving, mutable structural representation; resolving tag/VR/value semantics
to the extent a dictionary-free or dictionary-backed library can; exposing raw and typed value
access without gratuitous copying; supporting in-place mutation (insert/replace/remove, including
inside nested sequences); and serializing a valid DICOM object back out, preserving what wasn't
touched. It owns nothing about *why* an attribute should be removed or replaced — that is policy,
which lives above it.

## 4. Non-responsibilities / architectural boundary

| Capability | Classification |
|---|---|
| File-routing policy, workflow, orchestration | MUST remain outside `attrs` |
| Input/output adapters (HTTP, C-STORE, STOW-RS, object storage) | MUST remain outside `attrs` |
| Cloud interfaces, deployment architecture | MUST remain outside `attrs` |
| De-identification / anonymization rule content | MUST remain outside `attrs` |
| Archive/cohort/reconstruction behavior | MUST remain outside `attrs` |
| Accept/reject/transform policy evaluation (`Require`, `AllowListPrune`, etc.) | MUST remain outside `attrs` — belongs in `structure` |
| Deciding *which* tags to remove for a use case | MUST remain outside `attrs` |
| Parsing, VR/tag semantics, mutation primitives, serialization | MUST be inside `attrs` |
| Pixel decoding (JPEG, JPEG2000, RLE, etc.) | MUST remain outside `attrs` (pixel data is referenced, never decoded, at any layer in this family) |

`fastDICOMstructure`'s own architecture doc already states this instinct correctly for a different
boundary ("no pseudonymization, UID remapping, date shifting... in policy.py") — the same discipline
now needs to apply one level down, separating "how to touch a DICOM object" (attrs) from "what to
touch and why" (structure).

## 5. Attrs Contract v1

### A. Dataset enumeration
MUST: identify every element at every nesting depth, including sequence items, private and
unknown elements, and group-length/legacy elements, while excluding structural pseudo-tags
(item/sequence delimiters, `(FFFE,E000)`/`(FFFE,E00D)`/`(FFFE,E0DD)`) from the logical attribute
list — they are encoding artifacts, not attributes, and must be modeled as container framing, not
as elements a caller iterates over.

### B. Tag identification
MUST: numeric `(group, element)` identification, always. MUST: correct private/non-private
classification. SHOULD (V1 desirable, not required): canonical keyword resolution via a data
dictionary, and private-tag identity via the (group, private-creator, block) convention (PS3.5
7.8.1). Unknown tags MUST be preserved and readable by numeric tag even with no keyword.

### C. VR resolution
MUST: Explicit VR Little Endian (already solid). MUST: Implicit VR Little Endian at least at
today's structural-heuristic level (sequence vs. non-sequence by undefined-length), with a
documented path to real dictionary-backed VR inference as a V1-desirable upgrade. MAY: Explicit VR
Big Endian (retired; low real-world incidence). MUST: expose whether a VR was dictionary-inferred,
heuristically inferred, or read explicitly from the header, so callers relying on typed access know
how much to trust it.

### D. Value access
MUST: raw bytes always available, zero-copy where the element is unmodified. MUST: typed numeric
accessors (US/SS/UL/SL/FL/FD). MUST: multi-valued string splitting. SHOULD (V1 desirable): typed
accessors/parsers for DA/TM/DT, PN component parsing, UI trimming (partially present today), OB/OW
binary access (present as raw bytes already). MUST: empty-value and malformed-value handling that
does not throw across a stable API boundary.

### E. Character sets
MUST (gating, correctness-critical): read and honor Specific Character Set (0008,0005), decode
text VRs accordingly (including multi-valued Specific Character Set for ISO 2022 escape-sequence
switching), and re-encode correctly when a text value is mutated. This is currently **entirely
absent** from every codebase inspected and is the single largest correctness gap found.

### F. Mutation
MUST: replace value, insert new top-level element, remove element, and do all three inside nested
sequence items — not just at the top level. MUST: caller supplies VR only when inserting a
brand-new element attrs cannot infer (consistent with "no dictionary to infer from" today; once a
dictionary exists, VR inference from tag becomes V1-desirable, not required). MUST NOT: silently
truncate or misencode a value that doesn't fit the element's length form — reject instead (already
done). SHOULD: automatic even-length padding on the caller's behalf (currently pushed onto the
caller, which is an ergonomics/correctness risk, not a fundamental limit).

### G. Structural preservation
Distinguish, and test, three separate guarantees:
1. **Semantic round-trip equivalence** — same attributes, same values, after parse→mutate→write→
   reparse. MUST.
2. **Structural equivalence** — same element ordering, nesting, and undefined/defined-length
   framing preserved where not explicitly changed. MUST.
3. **Byte-for-byte preservation** — identical output bytes for an *unmodified* structure. MUST for
   Explicit VR Little Endian input (already proven by test in `fastDICOMstructure`); MAY for other
   transfer syntaxes.
Untouched pixel data, encapsulated fragments, large private payloads, and undefined-length
constructs MUST be preserved without materialization or reinterpretation.

### H. Serialization/write
MUST live inside `attrs`: length recomputation, VR/endian encoding, sequence/item length and
delimiter handling, file-meta group-length recomputation, and transfer-syntax-consistency fixups
(e.g. not claiming Implicit VR while emitting Explicit VR bytes). A lower-level byte-writer
component underneath `attrs` is an acceptable internal detail; attribute semantics MUST NOT leak
upward into `structure` merely because a value needs re-encoding.

### I. Bulk data behavior
MUST: recognize Pixel Data specifically and expose location/extent/encapsulation metadata without
decoding (already implemented and well-designed in `fastDICOMstructure`). SHOULD: generalize the
same "reference, don't materialize" treatment as an explicit, named property of *any* large value,
not only Pixel Data — today this is only an implicit side effect of the general zero-copy `Value`
design, with no API to ask "is this bulk data" for an arbitrary tag.

### J. Error behavior
MUST: a structured diagnostic severity taxonomy separating fatal parse failure, recoverable
anomaly, warning, and unsupported-but-recognized construct (already implemented well in
`fastDICOMstructure`'s `DiagnosticSeverity`). MUST NOT: throw exceptions across the stable public
API boundary (already enforced at the C ABI layer).

### K. File versus dataset semantics
MUST: accept Part 10 files (preamble + `DICM` + file meta + dataset) and bare datasets (no
preamble, transfer syntax assumed/supplied externally) — already implemented. SHOULD: represent
file meta information as conceptually distinct from the dataset even though both may share one
underlying ordered-list representation internally — today they are fully conflated by design,
which the existing architecture doc itself flags as an open question.

### L. API ergonomics
MUST: a small public surface (read/get/set/remove/iterate, recursive iteration with path
information) that does not require the caller to understand header encoding. SHOULD: keyword-based
convenience lookup (`ds.get("PatientID")`) once a dictionary exists; until then, numeric-tag-only
access is an acceptable interim MUST. MUST NOT: sacrifice the zero-copy design for prettier syntax.

## 6. Proposed public API model (illustrative)

```python
import fastdicomattrs as attrs

ds = attrs.read(source, fidelity="lossless")     # file path, bytes, or (later) stream

element = ds.find((0x0010, 0x0020))               # numeric tag, always available
patient_id = ds.get("PatientID")                  # keyword, once a dictionary exists

for element, path in ds.iter_elements(recursive=True):
    ...                                            # path carries item indices for nested elements

ds.set_value((0x0010, 0x0020), b"DEMO")
ds.set((0x0009, 0x0010), vr="LO", value=b"...")   # VR required only for brand-new elements
ds.remove((0x0010, 0x0030))

pixel_ref = ds.pixel_data()                       # metadata only, never materializes bytes

output = attrs.write(ds)                          # or ds.write_bytes()
```

This is close to `fastDICOMstructure`'s existing Python surface already; the gap is keyword
lookup, recursive path-aware iteration exposed to Python (currently C++-only for anything beyond
the specific `*_recursive` mutation calls), and character-set-aware string decoding.

## 7. Current implementation inventory

### 7a. `fastDICOMattrs` (this repository) — the DCMTK wrapper

- `include/fastdicom/tags.hpp` + `src/tags.cpp` (~120 lines total): `getTag`/`getTags` open a
  `DcmFileFormat` via DCMTK, look up each requested tag with
  `findAndGetOFStringArray`, and return DCMTK's backslash-joined string form. Group `0002` is
  hard-routed to `getMetaInfo()`, everything else to `getDataset()` — no recursive descent into
  sequences at all ("Sequence traversal is deliberately not implicit").
- `ReadOptions::max_value_bytes` (default 4 KiB) relies on DCMTK's own deferred-loading behavior to
  avoid reading large values (chiefly Pixel Data) into memory — a real, working idea, but it is a
  DCMTK feature being exposed, not something this library implements.
- No mutation. No serialization/write path at all. No typed value access — everything is a string.
  No VR exposed to the caller. No character-set handling of its own (whatever DCMTK does
  internally is not surfaced or controlled). No private-tag identity. No dictionary/keyword
  resolution (results are addressed and returned by numeric tag only).
- `python/fastdicom/bindings.cpp`: a pybind11 wrapper exposing exactly `get_tag`/`get_tags`,
  filename-based only; the handle-based (already-loaded-file) overload is C++-only.
- Tests: `tests/dicom_file_test.cpp`, `tests/tags_test.cpp`, and
  `tests/python/test_fastdicom.py` (9 Python tests) check fastdicom's results against pydicom for
  the same tags — appropriate for what the library claims, but the claim itself is narrow.
- `fastDICOMarchive`'s own `docs/fastdicomattrs-capability-assessment.md` independently reaches the
  same conclusion from the consumer side: it uses this library only "as a fast readability probe, a
  root-level tag extractor, and a parser capability telemetry source," explicitly "not currently
  the full transform engine for archive ingest," and falls back to `pydicom` for anything else.

This is a complete, correct implementation of a narrow, different problem. Nothing here is
scaffolding or dead code — it is simply not the right shape for the target contract.

### 7b. `fastDICOMstructure` — the engine that actually fits

- Self-contained, from-scratch C++20 parser and writer (`src/parser/explicit_vr_le_parser.cpp`,
  583 lines; `src/parser/implicit_vr_le_parser.cpp`, 428 lines; `src/writer/lossless_writer.cpp`,
  334 lines). No DCMTK dependency of any kind.
- Object model (`include/fastdicomstructure/*.hpp`): `Tag`, `VR` (33 standard VRs + `Unknown`),
  `ValueLength` (short/long form, defined/undefined), `SourceSpan` (16-byte, zero-copy),
  `Source`/`MemorySource`/`FileSource` (mmap-backed), `TransferSyntax`, `Value` (source-backed or
  owned, with typed numeric accessors), `Element`, `Item`, `Sequence`, `ElementPath` (tag +
  optional item-index steps, for naming a nested element uniquely), `PixelDataReference`
  (native/encapsulated, fragment-aware, never decodes), `ParseDiagnostic`/`ParseResult`/
  `WriteResult` with a six-level severity taxonomy.
- `DICOMStructure`: order-preserving `std::vector<Element>` (never a map), flat and path-aware
  lookup, `visit()` for recursive traversal, `set`/`set_value`/`erase`/`erase_if`/
  `erase_recursive`/`set_value_recursive`/`erase_private_elements`.
- Proven byte-identical round-trip for unmodified `Fidelity::Lossless`, Explicit-VR-sourced input
  (`tests/integration/test_roundtrip_lossless.cpp`), with one documented, tested exception (an
  element positioned after Pixel Data). Proven semantic-reconstruction round-trip for modified
  structures (`tests/integration/test_mutation_roundtrip.cpp`), including a real-world 26,636-file
  corpus cross-checked against pydicom (`docs/corpus-results.md`), reporting 99.99% byte-level
  preservation of untouched value content via `WriteResult::source_backed_value_bytes` /
  `regenerated_value_bytes`.
- Bare-dataset parsing (no preamble/`DICM`) is already supported, assumed Explicit VR
  (`src/parser/explicit_vr_le_parser.hpp` line 20).
- Test suite: 106 C++ test cases across unit + integration, plus 46 Python tests — substantially
  larger and more targeted than the current `fastDICOMattrs` suite.
- C ABI (`abi/`) with generation-counter-based stale-pointer detection on the Python side
  (`StaleElementError`), and a `ctypes` wrapper deliberately kept thin.
- **The liability**: `python/fastdicomstructure/policy.py` (357 lines) already implements a
  `Require`/`Remove`/`Replace`/`AllowListPrune`/`PrivateTagPolicy` policy-evaluation layer directly
  on top of `Structure`'s mutation primitives, inside the same package, with its own module
  docstring calling it "a prototype answering one question: can structure/inspect/mutate already
  support a clean, small accept/transform/reject layer." That is `structure`'s correct future job —
  but it is being built with no `attrs` dependency underneath it to separate from, because none
  exists yet. `docs/architecture.md` §1 states as a hard constraint that `fastDICOMstructure` must
  never depend on `fastDICOMattrs` — which was a reasonable rule when the only `fastDICOMattrs` was
  the DCMTK wrapper, but blocks exactly the dependency the new architecture wants once a real
  `attrs` layer exists.
- Deliberate, stated non-goals that conflict with the new contract: no data dictionary, therefore
  no keyword resolution and no VR inference for Implicit VR (`docs/architecture.md` §9, §11;
  `tag.hpp` line 11's comment: "Deliberately not associated with any name/keyword/VR lookup — that
  is dictionary knowledge this library does not own"). Zero character-set handling anywhere in the
  codebase (confirmed by repository-wide search — the only `utf-8`/`decode` hits are for
  diagnostic-message text and a benchmark script's own PatientID hashing, never DICOM
  Specific-Character-Set-aware text decoding). No private-tag creator-block identity — only the
  odd/even group-number check.

## 8. Capability / gap matrix

Evaluated against the promoted `fastDICOMstructure` engine, since it is the credible baseline —
not against the current `fastDICOMattrs` repository, which fails nearly every row outright.

| Capability | Target | Current support | Evidence | Gap severity |
|---|---|---|---|---|
| Explicit VR LE read | MUST | Complete | `test_parse_explicit_vr_le.cpp` (11 cases); corpus run, 26,636 real files | None |
| Implicit VR LE read | MUST | Partial (structural only, no VR) | `roundtrip-contract.md` "Implicit VR Little Endian"; `test_parse_implicit_vr_le.cpp` (18 cases) incl. documented defined-length-nested-sequence miss | Moderate — foundational, not incidental |
| Explicit VR Big Endian | MAY | None (detected, rejected) | `transfer_syntax.hpp` `Unsupported` path | Low |
| Recursive sequence enumeration | MUST | Complete (C++), partial (Python) | `dicom_structure.hpp` `visit()`; Python `__init__.py` docstring: "Nested (sequence/item) mutation is C++-only in this increment" | Moderate (binding gap, not engine gap) |
| Tag → keyword / dictionary | MUST (new) / explicitly non-goal (current charter) | None | `tag.hpp:11`; `architecture.md` §9, §11 | Foundational — direct scope conflict |
| VR-from-dictionary for Implicit VR | SHOULD | None | Same as above | Foundational |
| Private-tag creator identity | SHOULD | None (odd/even only) | `Tag::is_private()`; no creator-block logic found anywhere | Moderate |
| Character set handling | MUST, correctness-critical | None | Repo-wide search, zero hits for Specific-Character-Set-aware decoding | Foundational, gating |
| Typed numeric value access | MUST | Complete | `value.hpp` `as_uint16/int16/uint32/int32/float32/float64` | None |
| Date/time/PN typed access | SHOULD | None (strings only) | `value.hpp` has no DA/TM/PN-specific accessor | Low-moderate |
| Attribute mutation (top-level) | MUST | Complete | `dicom_structure.hpp` `set`/`set_value`/`erase` | None |
| Attribute mutation (nested, Python) | MUST | Partial | `__init__.py` docstring, as above | Moderate |
| Even-length auto-padding | SHOULD | None (caller's burden) | `set_value` docstring: "does not auto-pad... pad it yourself" | Low-moderate |
| Serialization/write | MUST | Strong | `lossless_writer.cpp`; two documented write contracts in `roundtrip-contract.md` | Low (documented gaps only: no Implicit-VR output, Pixel-Data-always-last) |
| Byte-identical round-trip (unmodified) | MUST for Explicit VR | Proven | `test_roundtrip_lossless.cpp` | None (one documented edge case) |
| Semantic round-trip (modified) | MUST | Proven | `test_mutation_roundtrip.cpp`; corpus policy-transform run | None |
| Pixel data reference (native) | MUST | Complete | `pixel_data_reference.hpp` | None |
| Pixel data reference (encapsulated) | MUST | Complete | Same file, fragment/offset-table walk without decode | None |
| Bulk-data generalization beyond Pixel Data | SHOULD | Implicit only (general zero-copy `Value`), no named API | `pixel_data_reference.hpp` is Pixel-Data-specific | Low |
| Diagnostics taxonomy | MUST | Complete | `parse_diagnostic.hpp`, 6 severities | None |
| No exceptions across public boundary | MUST | Complete (C ABI layer) | `docs/abi-design.md`; `architecture.md` §7 | None |
| Part 10 file + bare dataset input | MUST | Complete | `explicit_vr_le_parser.hpp:20` | None |
| File-meta vs. dataset conceptual split | SHOULD | None (conflated by design) | `architecture.md` §5, self-flagged as revisit-worthy | Low-moderate |
| Streaming source | SHOULD (future) | Declared, not implemented | `parse.hpp` `parse_stream` always returns `Unsupported` | Low (explicitly deferred already) |
| Policy/orchestration kept out of attrs | MUST (architectural) | Violated today | `policy.py` lives inside `fastdicomstructure`, no `attrs` beneath it | Foundational — the core problem this assessment exists to name |

## 9. Foundational architectural mismatches

Three findings rise above "missing feature" to "would fight the design if patched in place":

1. **No layering boundary exists at all today.** `fastDICOMstructure` is simultaneously the parser,
   the mutation engine, the writer, *and* — as of the most recent commit
   (`348663f Add minimal policy prototype`) — the first policy layer. There is no seam where an
   `attrs` dependency could be inserted without either (a) breaking the documented "must not depend
   on `fastDICOMattrs`" rule, which was written before this new architecture existed, or (b)
   duplicating the engine. This is not a bug; it is the absence of a boundary the new architecture
   requires. Fixing it means moving code across a repository line, not adding a feature.

2. **Dictionary-free design is a stated philosophy, not an oversight.** `tag.hpp`'s own comment —
   "Deliberately not associated with any name/keyword/VR lookup... that is dictionary knowledge
   this library does not own" — means every place a keyword, private-creator identity, or
   dictionary-inferred VR would be needed was designed around, not merely left undone. Adding a
   dictionary is therefore a real scope expansion of the existing charter, not a gap-fill; it
   should be planned and resourced as such rather than treated as an easy addition.

3. **Character-set handling was never in the requirements this engine was built against.** Given
   the corpus-results.md real-world validation focused on structural round-trip fidelity, not text
   semantics, there is no existing partial implementation to build on — this is a from-zero build,
   and per this assignment's own instruction, it is correctness-critical and gating, not a V1
   deferral candidate.

Everything else in the matrix above — Implicit VR VR-inference, nested Python mutation ergonomics,
auto-padding, bulk-data API generalization, file-meta conceptual separation — is an ordinary
missing feature that can be added inside the existing representation without fighting it. The
`SourceSpan`/zero-copy design, the diagnostics taxonomy, the fidelity levels, and the
"reconstruct headers deterministically, copy value bytes verbatim" writer strategy are all sound
and should be preserved, not redesigned.

## 10. Refactor / restructure / rewrite recommendation

**Path B — substantial internal restructuring**, specifically:

- Do **not** build forward from the current `fastDICOMattrs` repository. Its representation
  (string-only values, no structure beyond top-level, DCMTK-owned parsing) cannot reach the target
  contract without becoming a different program; every one of sections A, C (Implicit VR), D
  (typed access), F, G, H, I of the contract would need to be built from nothing inside it anyway,
  at which point the DCMTK dependency and file-only API are pure liability, not head start.
- Do **not** rewrite `fastDICOMstructure`'s engine. Its object model, zero-copy design, diagnostics
  taxonomy, and writer strategy already satisfy most of the MUST rows in §8 and are proven against
  a large real-world corpus. Rewriting it would discard tested, working code to solve a problem
  (missing dictionary, missing character sets, missing layering) that doesn't require touching the
  parser or writer's core algorithms at all.
- **Do** extract/promote: move (or re-badge, with git history preserved via a repository split)
  `fastDICOMstructure`'s `include/`, `src/`, `abi/`, and the read/inspect/mutate/write portions of
  `python/fastdicomstructure/__init__.py` into the new `fastDICOMattrs`. Leave `policy.py` and the
  as-yet-unbuilt traversal/orchestration layer behind as the new, genuinely thin
  `fastDICOMstructure`, which becomes a declared dependent of the promoted `attrs` package instead
  of its fused implementation.
- **Do** retire the "MUST NOT depend on fastDICOMattrs" rule as written — it was correct for the
  old, unrelated DCMTK wrapper and is the opposite of what the new architecture needs once `attrs`
  means the promoted engine.
- **Do** resolve the naming collision from §2 explicitly, as a decision, before any code moves.

## 11. Components worth preserving

From `fastDICOMstructure` (the primary asset):
- The entire object model (`Tag`, `VR`, `ValueLength`, `SourceSpan`, `Source`/`MemorySource`/
  `FileSource`, `TransferSyntax`, `Value`, `Element`, `Item`, `Sequence`, `ElementPath`,
  `PixelDataReference`).
- Both parsers (`explicit_vr_le_parser.cpp`, `implicit_vr_le_parser.cpp`) and the writer
  (`lossless_writer.cpp`), including the "reconstruct headers, copy values verbatim" strategy.
- The full test suite (106 C++ cases, 46 Python cases), especially `test_roundtrip_lossless.cpp`
  and the corpus-validation methodology in `docs/corpus-results.md` — this is exactly the Level 3/4
  evidence the new qualification strategy (§13) needs, already built.
- The diagnostics taxonomy and the C ABI's no-exceptions-across-the-boundary discipline.
- `docs/roundtrip-contract.md`'s falsifiability discipline ("every bullet either has a passing
  byte-comparison test or is marked not yet implemented") — carry this norm forward verbatim into
  the new `attrs` documentation.

From the current `fastDICOMattrs` (secondary, narrower value):
- The `ReadOptions::max_value_bytes` / deferred-loading idea, as a design reference for the
  bulk-data-awareness generalization in contract §I, even though the DCMTK-specific mechanism
  itself isn't reusable.
- The pytest-against-pydicom differential pattern in `tests/python/test_fastdicom.py`, as a
  template for Level 4 differential testing in the new suite.
- Nothing in this repository's C++ core should be retained as implementation; it solves a
  different, still-legitimate problem (fast shallow probing) that can continue to exist as a
  distinct, honestly-named tool.

## 12. V1 MVP scope

| Requirement | Classification |
|---|---|
| Explicit VR LE parse/mutate/write, recursive, with nested Python mutation | V1 required |
| Implicit VR LE structural parse (current heuristic level) | V1 required |
| Diagnostics taxonomy, no exceptions across public API | V1 required |
| Pixel data reference (native + encapsulated), zero materialization | V1 required |
| Character-set-aware text decode/encode for Specific Character Set | V1 required (gating) |
| Byte-identical round-trip for unmodified Explicit VR input | V1 required |
| Semantic round-trip for modified structures | V1 required |
| Data dictionary: keyword resolution for well-known tags | V1 desirable |
| Data dictionary: VR inference for Implicit VR | V1 desirable |
| Private-tag creator-block identity | V1 desirable |
| Date/time/PN typed accessors | V1 desirable |
| Automatic even-length value padding | V1 desirable |
| Explicit VR Big Endian | Deferred |
| Streaming `Source` | Deferred |
| File-meta/dataset conceptual split | Deferred |
| Generalized named "bulk data" API beyond Pixel Data | Deferred |

Nothing in the "deferred" row forces `structure` to implement DICOM semantics itself — a caller
without dictionary support still gets correct numeric-tag-addressed access, correct mutation, and
correct serialization; it only loses convenience (keywords) and one Implicit VR refinement, neither
of which `structure` would need to work around by reaching into encoding details itself.

## 13. Qualification / test strategy

- **Level 1 (deterministic unit tests):** already strong (46 unit/integration-adjacent C++ cases
  for `Tag`/`VR`/`ValueLength`/`SourceSpan`/`ElementPath`). Extend with character-set-specific
  cases once built.
- **Level 2 (generated combinatorial fixtures):** `tests/integration/fixture_builder.cpp` already
  provides in-code fixture construction; extend its matrix to cover VR × length-form ×
  defined/undefined-length × nesting-depth × character-set combinations.
- **Level 3 (real corpus):** `docs/corpus-results.md`'s 26,636-file TCIA CT corpus run is a
  functioning template — reuse the harness, extend it to include non-CT modalities and any
  available non-Explicit-VR real-world samples once dictionary/Implicit-VR work lands.
- **Level 4 (differential testing):** the pydicom cross-check pattern already exists in both
  repositories (`tests/python/test_fastdicom.py` here; the corpus run in `fastDICOMstructure`) —
  formalize it as a standing CI gate rather than a one-off validation run, and add DCMTK as a
  second oracle where pydicom and the new dictionary disagree.
- **Level 5 (round-trip):** `test_roundtrip_lossless.cpp` and `test_mutation_roundtrip.cpp` already
  separate structural/semantic/byte-identical claims correctly — keep that separation explicit in
  any new test as the codebase grows a dictionary and character-set support, since those features
  are exactly the kind of thing that could quietly break the byte-identical guarantee if not
  re-verified.
- **Level 6 (resource behavior):** `docs/benchmarks.md`/`bench/` exist but are described as
  "engineering characterization, not the organizing objective" — appropriate; add an explicit
  memory-vs-pixel-data-size benchmark (large file, policy that never touches Pixel Data, assert
  peak RSS stays roughly constant as file size grows) since that is the specific claim §I of the
  contract needs proof for, and no existing benchmark isolates it.

## 14. Major technical risks

- **Character-set implementation is genuinely hard and untested territory here.** ISO 2022
  escape-sequence switching for multi-valued Specific Character Set is a known source of subtle
  bugs in every DICOM toolkit; budget real time for it, and lean on Level 4 differential testing
  against pydicom specifically for this feature before trusting it.
- **Adding a dictionary changes the mutation contract.** Once `set()` can infer VR from tag, the
  "caller must supply VR" rule in §F needs a compatibility decision (still require it explicitly,
  or make it optional) that affects every existing consumer, including `fastDICOMgateway`'s
  `transform.py` and `fastdicomstructure/policy.py`'s `Replace`/`AllowListPrune` operations.
- **The extraction itself is a coordination risk, not a technical one.** Three consumer
  repositories (`fastDICOMgateway`, `fastDICOMarchive`, and `fastDICOMstructure`'s own Python
  examples) currently import against today's names and either DCMTK-`fastDICOMattrs` or
  fused-`fastDICOMstructure`. A rename/promotion must update every `sys.path`/import assumption
  (see `fastDICOMgateway/src/fastdicom_gateway/transform.py`'s `_add_fastdicomstructure_to_path`)
  or provide a compatibility shim during transition.
- **The Pixel-Data-always-last writer limitation** (documented, tested, real) will surface more
  often once real-world Implicit VR and non-CT corpora are exercised at Level 3/4 — track it as a
  known pre-existing defect to fix during this restructuring, not a new one introduced by it.

## 15. Recommended implementation sequence

1. Resolve the naming decision (§2) and get it agreed in writing before moving any code.
2. Extract `fastDICOMstructure`'s core (parser/writer/object-model/C-ABI, excluding `policy.py`)
   into the new `fastDICOMattrs` repository, preserving git history where practical; carry all 106
   C++ and 46 Python tests with it unchanged as the regression baseline.
3. Rebuild `fastDICOMstructure` as a declared dependent of the promoted `attrs` package; port
   `policy.py` over unchanged first, to prove the dependency direction works before adding anything
   new to it.
4. Implement Specific Character Set handling inside `attrs` (V1 required, gating) with Level 1/2/4
   tests before anything else new, since every text-value consumer downstream depends on it being
   correct.
5. Add the data dictionary (keyword resolution, then VR inference for Implicit VR, then
   private-creator identity), each as its own increment with its own differential-test pass against
   pydicom.
6. Close the nested-mutation Python-binding gap and the auto-padding ergonomics gap.
7. Re-run the real-world corpus validation (Level 3) against the reorganized `attrs` to confirm
   nothing regressed in the move, before declaring the extraction complete.

## 16. Go/no-go recommendation

**Go**, on the extraction/restructuring path (Path B) described above, contingent on resolving the
naming collision in §2 first and treating character-set support as a blocking, not deferrable,
piece of V1. Do not begin by writing new parser/writer code — the engine to build on already
exists and is well-tested; the actual work is moving a boundary, retiring a conflicting name, and
filling three specific, previously-out-of-scope gaps (dictionary, character sets, private-creator
identity) on top of a foundation that does not need to be re-architected to receive them.
