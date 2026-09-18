# A1 — Attrs v1 Implementation Progression

Status: planning only. No production code was changed to produce this document. All claims about
current behavior are grounded in the post-A0 repository at commit `088999d` (clean, 115/115 C++
tests, 34/34 Python tests passing) and cited by file/line where load-bearing.

## 1. Executive recommendation

Do the dictionary substrate first, then bank two small independent wins (private-creator identity,
the writer's Pixel-Data-ordering defect) that need nothing from it, then spend the two largest
increments — dictionary-backed VR resolution (which is inseparable from Implicit VR structural
completeness) and character-set decoding — in that order, because character-set correctness for
Implicit VR data structurally requires the VR-resolution work to exist first, and building the
character-set subsystem against an already-uniform VR substrate avoids a retrofit. Character-set
re-encoding follows decoding as its own increment. Mutation ergonomics (path-based Python access,
VR-optional insert, auto-padding) comes after dictionary work because one of its concrete payoffs —
inferring VR instead of requiring the caller to supply it — literally depends on the dictionary
existing. Full multi-byte (Kanji/Korean/GB18030) character-set support is real, valuable work that
is deliberately **not** part of the V1 freeze — it is sized and scheduled as an explicit fast-follow
so schedule pressure on it never quietly weakens the V1 claim for everything else.

Seven implementation increments, one qualification-and-freeze phase. No increment is later than
`M` effort except the two largest (dictionary+Implicit-VR, character-set decoding, both `L`) and
the explicitly-deferred multi-byte character-set work (`XL`, outside the freeze).

## 2. Exact Attrs v1 support claim

Falsifiable, so each bullet can be checked true or false against a test:

**Input.** DICOM Part 10 files (128-byte preamble + `DICM` + File Meta + dataset) and bare datasets
(no preamble; Explicit VR LE assumed by default, Implicit VR LE selectable via an explicit caller
hint — new in A1, see §7). File-path, in-memory buffer. (Streaming input: architecturally
unforeclosed, not implemented — see §14.)

**Transfer syntaxes.** Explicit VR Little Endian (native and encapsulated Pixel Data) and Implicit
VR Little Endian — both **fully**, meaning both get correct dictionary-resolved VRs, full recursive
sequence/item visibility, and full mutation/write support. Explicit VR Big Endian remains detected
and rejected. Deflated Explicit VR LE remains detected and rejected.

**VRs.** All 33 standard PS3.5 VRs, correctly resolved for both supported transfer syntaxes.
Explicit VR: from the wire, as today. Implicit VR: from the dictionary, including retired tags,
repeating-group tags (Overlay Data and friends), and the small closed set of context-dependent
tags whose dictionary entry is itself ambiguous (`US or SS` and similar) — resolved via the
standard's own documented disambiguation rule (checking Pixel Representation), not guessed.

**Structure.** Sequences and items at arbitrary nesting depth (bounded by the existing
configurable safety limit), defined and undefined length, for both transfer syntaxes. Private
elements preserved, with private-creator/block identity resolvable (which creator registered
which block; never vendor-specific interpretation of the data itself). Unknown/unrecognized tags
preserved with `VR::Unknown`. Retired non-repeating tags resolved like any other dictionary entry.
Group-length elements preserved structurally, as today.

**Value access.** Raw bytes always available, zero-copy where unmodified (unchanged from today).
Existing typed numeric accessors, unchanged. Text access with Specific Character Set-aware
decoding for: the default repertoire, every single-byte repertoire in PS3.5 (ISO_IR 100/101/109/
110/144/127/126/138/148/166), their ISO 2022 code-extension forms, and ISO_IR 192 (UTF-8).
Multi-byte code-extension repertoires (Japanese Kanji/Katakana via ISO 2022 IR 87/159, Korean via
ISO 2022 IR 149) and GB18030/GBK are **explicitly out of the V1 claim** — raw bytes remain fully
accessible for elements in those repertoires; decoded/typed text access is not guaranteed correct
and callers are told so via a diagnostic, not silently given wrong text.

**Mutation.** Insert/replace/remove at the top level and at any nested path (Python gains
`ElementPath`-based access, not just tag-based recursive calls — see §7). VR is caller-supplied or
dictionary-inferred for a newly-inserted element (caller's choice; inference only when the caller
omits it and the dictionary has an unambiguous answer). Values requiring even-length padding are
padded automatically unless the caller opts out. Text values are encoded per the resolved Specific
Character Set for their position, for the same repertoire set text decoding covers; an attempt to
encode a value the current Specific Character Set cannot represent is refused with a diagnostic,
not silently mis-encoded.

**Serialization.** Unchanged two-contract model (byte-identical for unmodified Explicit-VR-Lossless
input; valid semantic reconstruction otherwise), **plus** correct handling of any element
positioned after Pixel Data in the source — today's documented writer defect is closed as part of
V1, not deferred (see §6, A1.3, and §12 for why).

**Explicitly out of scope for V1** (unchanged from A0's assessment unless noted): Explicit VR Big
Endian, pixel decoding of any kind, a general vendor-private-tag dictionary, streaming `Source`,
conceptual file-meta/dataset separation (remains one ordered element list by design), and — new —
multi-byte code-extension character sets.

## 3. Current post-A0 baseline (revalidated)

Commit `088999d`, working tree clean. 115/115 C++ tests (`ctest --test-dir build`), 34/34 Python
tests (`tests/python`, engine scope only — the other 12 moved to `fastDICOMstructure` at A0).
Public Python API unchanged since A0: `read, read_buffer, Structure, Element, Item, Diagnostic,
FdsError, StaleElementError, WriteStats` (`python/fastdicomattrs/__init__.py:26`). VR enum has all
33 standard VRs plus `Unknown` (`include/fastdicomattrs/vr.hpp:12-16`). `TransferSyntaxKind`
distinguishes Implicit VR LE, Explicit VR LE, Explicit VR BE (rejected), Deflated Explicit VR LE
(rejected), and generic encapsulated Explicit VR (`include/fastdicomattrs/transfer_syntax.hpp:8-26`).

Three facts specifically load-bearing for this plan, checked directly rather than assumed:

- **No parent back-pointer exists anywhere in the object model** (`Item`, `Sequence`, `Element`
  hold no reference to their containing structure). Confirmed by inspection of
  `include/fastdicomattrs/item.hpp`, `sequence.hpp`, `element.hpp` — none declare one.
  `ElementPath` (`include/fastdicomattrs/element_path.hpp`) already carries exactly the
  information a top-down resolver needs (a list of `{Tag, optional item_index}` steps from the
  root), which is the vehicle this plan uses for Specific-Character-Set context resolution (§10) —
  no upward-pointer retrofit needed.
- **Pixel Data is not a member of the ordinary `elements()` vector.** `DICOMStructure` holds
  `std::vector<Element> elements_` and a separate `std::optional<PixelDataReference> pixel_data_`
  (`include/fastdicomattrs/dicom_structure.hpp:122,124`). There is currently no data recording
  where, among the other top-level elements, Pixel Data originally sat — confirming the writer's
  "always last" behavior is a direct, structural consequence of the representation, not a loose
  end in the writer loop alone (§12).
- **The exact code decision point for Implicit VR's structure-only inference** is
  `parse_one()` in `src/parser/implicit_vr_le_parser.cpp` (around line 176 onward): undefined
  length → `Sequence`; every other (defined-length) element → `VR::Unknown`, opaque. Two existing
  tests assert this directly — `tests/integration/test_parse_implicit_vr_le.cpp:45` and `:94`
  (`REQUIRE(e->vr() == VR::Unknown)`) — both against ordinary, non-private, dictionary-resolvable
  tags in their fixtures. These two assertions are the concrete migration cost A1.4 (§6) will
  incur: once dictionary resolution lands, either the fixture's tag needs to change to a
  genuinely-unknown one to keep testing "unknown tag stays Unknown," or the assertion needs to
  change to the tag's real VR. Either way, this is now a known, bounded, named risk instead of a
  hypothetical one.

## 4. Remaining V1 gaps, reclassified against the corrected requirement

| Gap (from A0's list) | Classification for V1 |
|---|---|
| No data dictionary | **MUST** — foundational, blocks two other MUSTs below |
| Incomplete Implicit VR semantic interpretation | **MUST** — corrected per this task's instruction: VR correctness, not a convenience |
| No Specific Character Set handling | **MUST** — corrected per this task's instruction, for the repertoire subset in §2 |
| No private-creator/block identity | **MUST**, but small — a real, bounded, structural (non-dictionary) fact attrs should expose |
| Incomplete nested path-based mutation via Python | **MUST** for ergonomics parity (C++ already has it; the API promise "callers need not understand DICOM encoding" is not yet true from Python) |
| Caller-managed even-length padding | SHOULD → promoted to MUST-adjacent, bundled into the same increment as path-based mutation (§6, A1.7) since it is nearly free once that code is being touched anyway |
| Pixel-data-ordering writer defect | **MUST** — see explicit recommendation in §12, overriding this task's own stated bias toward not automatically deferring it |
| No Explicit VR Big Endian | Deferred beyond V1 (unchanged from A0) |
| No streaming `Source` | Deferred beyond V1, architecture must remain compatible (§14) |
| File-meta/dataset conflation | Deferred beyond V1 — no concrete consumer need identified (§13) |
| No generalized named bulk-data API | Deferred beyond V1 — the general zero-copy `Value` design already covers the substance of this for any large value; no consumer has asked for the naming convenience |
| Multi-byte character sets (new finding, see §9) | **Split out of the MUST charset scope** — SHOULD, explicit fast-follow, not part of the V1 freeze |

## 5. Dependency analysis among the MUST gaps

This is the load-bearing technical reasoning; the sequence in §6 follows directly from it.

**Dictionary vs. Implicit VR structural completeness: inseparable, not merely ordered.** Under
Implicit VR, an element with a *defined* length is structurally indistinguishable from an ordinary
value unless something tells the parser its tag is `SQ` — and the only source for that (there is no
wire signal) is the dictionary. This is not "dictionary should come before Implicit VR completeness
for tidiness" — it is the same fact expressed twice: dictionary-backed VR resolution *is* what
closes the Implicit-VR structural-invisibility gap, for `SQ` and every other VR simultaneously.
There is no way to sequence these as independent increments; they are one increment (§6, A1.4).

**Dictionary vs. character sets: a real but partial dependency.** Specific Character Set only
governs decoding of a fixed set of *text* VRs (`SH LO ST LT PN UC UT`, per PS3.5 §6.1.2.3 — `AE AS
CS DA DS DT IS TM UI UL` and the rest are restricted to the default repertoire regardless, and
every other VR is untouched by it). Under **Explicit VR**, which of those VRs an element has is
already known today, directly from the wire, with zero dictionary involvement — so a character-set
decoder that only ever needs to ask "is this element's *already-known* VR one of the text VRs"
could be built and correctly exercised against Explicit VR data *before* the dictionary lands.
Under **Implicit VR**, that same question cannot be answered correctly without dictionary
resolution first. Since this task's instruction is explicit that the V1 claim must cover Implicit
VR character-set correctness too (not just Explicit VR's), the dictionary dependency is real for
the *complete* V1 claim, even though it is not real for an Explicit-VR-only slice of it.
**Recommendation: sequence dictionary work first anyway**, not because the data dependency forces
it for every case, but because building the character-set subsystem against a substrate where "VR
is always correctly known regardless of transfer syntax" is already a true invariant avoids
designing (and then having to revisit) a codec layer that special-cases "well, for Implicit VR I
also need to check the dictionary myself here." The total engineering cost of dictionary work does
not change based on this choice — it is required either way — so there is no real cost to placing
it first, and a real design-cleanliness benefit to doing so. If a future team ever wants to
parallelize this work across two engineers, the character-set core subsystem (§10) genuinely does
not need to *wait* on dictionary work landing in a merged, tested state — it only needs the
*concept* "ask this element's VR" to exist, which it already does. That parallelization option is
noted here and not exercised in this single-stream plan.

**Dictionary vs. mutation ergonomics: real, one-directional.** VR-optional insertion
(`set(tag, value)` inferring VR instead of requiring the caller supply it) is meaningless without a
dictionary to infer from. This increment (§6, A1.7) must follow dictionary work; it cannot precede
or parallel it.

**Private-creator identity vs. everything else: no dependency in either direction.** Identifying
which registered creator owns a private block is pure structural logic — read the `LO` value at
`(gggg,00cc)` for `cc` in `0x10`–`0xFF`, and elements at `(gggg,ccXX)` belong to that creator. This
needs zero dictionary data and touches no parser/writer code path the other increments touch. It
can be sequenced anywhere; this plan pulls it early (§6, A1.2) purely for early risk retirement and
incremental value, not because anything requires it there.

**Writer Pixel-Data-ordering fix vs. everything else: no dependency in either direction**, but
larger than it looks (§12) — it is a small object-model addition (recording Pixel Data's original
position), not a writer-only patch, because that position is not tracked anywhere today. Pulled
early (§6, A1.3) for the same reason as private-creator identity: it is fully independent, and
retiring it early means later increments are never the ones that "accidentally" have to deal with
it landing on top of unrelated in-flight changes.

## 6. Proposed A1.x sequence

```text
A1.1  Dictionary substrate (data, generation, lookup API — no parser wiring)
A1.2  Private-creator / block identity
A1.3  Writer: correct Pixel-Data-relative-ordering on write
A1.4  Dictionary-backed VR resolution + Implicit VR structural completeness
A1.5  Character-set core: context resolution + single-byte repertoires + UTF-8 (decode)
A1.6  Character-set re-encoding (mutation direction, same repertoire scope as A1.5)
A1.7  Mutation ergonomics: path-based Python/ABI access, VR-optional insert, auto-padding
──────────────────────────── V1 qualification & freeze (§15) ────────────────────────────
A1.8  [fast-follow, outside the V1 freeze] Multi-byte character sets (Kanji/Korean/GB18030)
```

A1.1 must be first. A1.2 and A1.3 have no ordering constraint relative to each other or relative to
A1.1 being *in progress* (they could even land before A1.1 completes), but are sequenced after it
here simply as a matter of narrative and commit hygiene, not necessity. A1.4 strictly requires
A1.1. A1.5 strictly requires nothing new from A1.2–A1.4 to *begin*, but is sequenced after A1.4 for
the design-cleanliness reason in §5, not a hard dependency. A1.6 strictly requires A1.5. A1.7
strictly requires A1.4 (for VR-optional insert specifically; the path-based-access and
auto-padding parts of A1.7 have no dictionary dependency and could move earlier if there were
reason to).

## 7. Detailed specification per increment

### A1.1 — Dictionary substrate

**Purpose.** Give the engine a correct, provenance-tracked, reproducibly-generated tag→VR (and
tag→keyword) table, with no behavior change yet to parsing, mutation, or writing.

**Capability gained.** A new lookup API (`fds::dictionary::lookup(Tag) -> optional<DictionaryEntry>`
in C++; nothing new exposed through the ABI yet except an additive, optional keyword-lookup
convenience — see below) that nothing in the parser or writer calls yet. Purely additive.

**Explicit non-goals.** No parser change. No writer change. No VM-based validation logic (VM is
recorded if free to include, never enforced). No private-tag dictionary (impossible in principle;
private semantics are vendor-specific). No dynamic/runtime dictionary loading or update mechanism.

**Implementation scope.** New `include/fastdicomattrs/dictionary.hpp` + `src/dictionary.cpp`. A
new, checked-in, generated data file (see §8 for the generation pipeline and representation
recommendation) plus the small generator script itself (not part of the runtime build). No changes
to any existing header or source file.

**Public API impact.** Additive only. New C++ namespace/functions; one new, small, optional
Python/ABI convenience (`fds_dictionary_vr(tag) -> vr_or_not_found`,
`fds_dictionary_keyword(tag) -> const char* or NULL`) satisfying the "SHOULD" keyword-lookup
requirement cheaply, without yet wiring dictionary results into parsing.

**Tests added.** Deterministic unit tests for the lookup API itself (exact match, repeating-group
range match — see §8 — retired-tag match, not-found case, keyword lookup, VR lookup). A
differential test comparing the *entire* generated table against pydicom's own dictionary
(`pydicom.datadict`) tag-by-tag for VR agreement, with any disagreement triaged and either fixed
(if fastDICOMattrs' generation pipeline has a bug) or explicitly recorded (if pydicom itself
diverges from the cited standard edition — this does happen for a handful of tags across DICOM
editions, and disagreements must be *decided*, not silently adopted from either side; see §8's
provenance requirement for why this is answerable).

**Differential validation.** pydicom's dictionary, full table, as above. DCMTK's dictionary
(`dcdict.h`/the DCMTK `.dic` text format) as a second oracle specifically for the handful of
disagreements pydicom's table might have, since a two-out-of-three majority is more actionable
than a single oracle's unexplained difference.

**Corpus validation.** Not needed at this stage — nothing consumes the dictionary yet.

**Performance/resource validation.** Binary size delta (expect on the order of 100–150 KB, see
§8's estimate) and a microbenchmark of `lookup()` alone (expect sub-microsecond via binary search
over ~5,000 entries; no parsing-throughput impact possible yet since nothing calls it from the hot
path).

**Acceptance criteria.** (1) Every entry in the generated table matches the cited DICOM standard
edition's PS3.6 data, verified by an independent regeneration cross-check. (2) Zero VR
disagreements against pydicom's table, or every disagreement explicitly triaged and documented with
a decided-correct answer. (3) All existing 115+34 tests still pass unchanged (nothing they exercise
should be able to change, since this increment is purely additive and unwired).

**Commit/freeze point.** Freeze the dictionary's public lookup API signature and the generated
table's content (pinned to a named standard edition) before A1.4 begins consuming it. Changing the
table after downstream code depends on specific lookups becomes a much more expensive class of
change (silent behavior drift in Implicit VR parsing), so this freeze is deliberate and named, not
incidental.

### A1.2 — Private-creator / block identity

**Purpose.** Let a caller determine which registered private creator owns a given private data
element, without inventing or claiming interpretation of vendor-specific semantics.

**Capability gained.** Given any private (odd-group) element, resolve the creator string recorded
at that group's `(gggg,00cc)` creator element for the block `cc` the element's own low byte falls
into, and expose that creator identity alongside the element. This makes "reliably read all
attributes" honestly stronger for the private-element case without overclaiming.

**Explicit non-goals.** No interpretation of what a given creator's private data *means*. No
built-in table of known vendor private dictionaries (a legitimate future extension, explicitly not
V1). Does not change how `erase_private_elements()` behaves today (still all-or-nothing by odd
group number) — this increment is additive identity information, not a new mutation primitive.

**Implementation scope.** A pure-function addition, most naturally on `DICOMStructure` or as a
free function taking an `Element`/`Tag` plus the owning structure (needs sibling elements — the
creator element — so cannot be a property of `Element` alone). Touches `dicom_structure.hpp/.cpp`
only. No parser or writer change.

**Public API impact.** Additive only.

**Tests added.** Deterministic: single creator/single block, multiple creators/multiple blocks in
one group, a private element whose group has no creator element at all (malformed/legacy input —
must not crash, must report "creator unknown" rather than guessing), creator elements nested inside
a sequence item (must resolve relative to the enclosing item, not the top level).

**Differential validation.** Not needed — this is structural logic with a directly-readable
correct answer from the PS3.5 §7.8.1 text, not something with room for competing interpretations
to differ against pydicom/DCMTK. A spot check against pydicom's own `private_creators()` helper is
cheap and worth doing regardless.

**Corpus validation.** Useful, not required: run against the existing NLST corpus (which the
baseline already shows has private tags present in all 2,831 files) and manually spot-check a
sample of resolved creator identities look sane (real, recognizable manufacturer strings).

**Performance/resource validation.** Negligible; this is opt-in (called only when a caller asks),
never on the hot parse path.

**Acceptance criteria.** Every private element in a fixture with well-formed creator declarations
resolves to the correct creator string and block number; malformed/missing-creator cases report
"unresolved," never a wrong answer or a crash.

**Commit/freeze point.** None beyond the increment's own tests passing — nothing downstream depends
on this API's shape.

### A1.3 — Writer: correct Pixel-Data-relative ordering

**Purpose.** Close the documented defect where an element positioned after Pixel Data in the source
is written before it instead, breaking both the byte-identical guarantee for unmodified input and
(more importantly for the V1 claim) correctness of principle: a library claiming reliable
read/write of *all* attributes should not silently reorder valid content.

**Capability gained.** `write()`/`write_bytes()` reproduce the source's true top-level element
order, Pixel Data included, for both the unmodified byte-identical path and the modified
semantic-reconstruction path.

**Explicit non-goals.** No change to how Pixel Data itself is represented (still referenced, never
materialized). No change to the two-write-contracts model. Does not attempt to reorder anything on
mutation beyond maintaining ascending tag order for newly-inserted top-level elements, exactly as
today.

**Implementation scope.** `DICOMStructure` gains a way to record Pixel Data's original position
among the top-level elements — a new `std::optional<std::size_t> pixel_data_position_` field is
the minimal change (rather than folding Pixel Data into the `elements_` vector itself, which would
be a much larger representational change with no other benefit). Every parser call site that
constructs a `DICOMStructure` (currently `explicit_vr_le_parser.cpp` only, since Pixel Data is
Explicit-VR-only structurally — Implicit VR's own Pixel Data handling is a separate, already-
generic code path per `docs/architecture.md` §8) must be updated to supply this value. The writer
(`lossless_writer.cpp`) consults it to interleave Pixel Data's write at the correct point instead
of unconditionally last.

**Public API impact.** Additive (a new accessor); no change to any existing signature.

**Tests added.** The exact case `test_roundtrip_lossless.cpp:185` already documents as a known,
currently-failing-by-design limitation ("An element after Pixel Data breaks the unmodified
byte-identical guarantee") becomes a **passing** test instead of a documented gap. New cases: an
element after Pixel Data in an otherwise-unmodified structure (byte-identical write), the same
input after a mutation elsewhere (semantic-reconstruction write, still correctly ordered),
encapsulated Pixel Data with a trailing element (Data Set Trailing Padding `(FFFC,FFFC)` is the
standard's own named example and should be the literal fixture).

**Differential validation.** pydicom round-trip of the same fixture, confirming both libraries
agree on final element order.

**Corpus validation.** Re-run the existing NLST corpus purely as a regression check — the baseline
already establishes 100% of that corpus has Pixel Data last, so this increment cannot show a
*positive* result there, only confirm no regression.

**Performance/resource validation.** None expected; one extra field, one extra branch in the write
loop.

**Acceptance criteria.** The named `test_roundtrip_lossless.cpp` case passes. No existing test
regresses.

**Commit/freeze point.** None beyond its own tests — this is a self-contained defect closure.

### A1.4 — Dictionary-backed VR resolution + Implicit VR structural completeness

**Purpose.** Make every Implicit-VR-sourced element carry its correct VR (not `VR::Unknown`), and
make every Implicit-VR-encoded, dictionary-recognized sequence — defined-length or not — visible to
recursive traversal and mutation, exactly matching this task's acceptance criterion in its §8.

**Capability gained.** For supported Implicit VR Little Endian objects: every standard sequence
recognized by the dictionary is recursively parsed, and every nested attribute inside it is visible
to `visit()`, `find(ElementPath)`, and mutation, exactly as Explicit VR input already is. Every
other Implicit-VR element carries its real, dictionary-resolved VR (or `VR::Unknown` only when the
tag is genuinely unrecognized — private or truly novel), enabling typed value access
(`as_uint16()`, future date/PN/charset access) for Implicit VR data for the first time.

**Explicit non-goals.** No change to Explicit VR parsing at all (already correct). No attempt at
Implicit VR Big Endian (does not exist as a standard transfer syntax; not a real gap). Ambiguous-VR
resolution (below) covers only the standard's own documented `US or SS`-class cases — no attempt to
guess VR for a tag the dictionary itself does not disambiguate.

**Implementation scope.** `src/parser/implicit_vr_le_parser.cpp`'s `parse_one()`: for a
defined-length element, look up the tag in the dictionary (A1.1) instead of unconditionally
assigning `VR::Unknown`. If the resolved VR is `SQ`, recurse using the same item/sequence parsing
machinery the undefined-length case already uses (that machinery is transfer-syntax-generic
internally — verify and reuse, do not duplicate). If the resolved VR is one of the standard's small
set of context-dependent ambiguous entries (`US or SS` and the few similarly-shaped cases in PS3.6),
apply the standard's documented disambiguation rule: consult `(0028,0103)` Pixel Representation
where applicable, defaulting per PS3.5 when even that is absent, and record which path was taken as
a diagnostic (`Info` severity) rather than silently. If the tag is not in the dictionary at all,
keep today's `VR::Unknown` behavior exactly. Also: `ParseOptions` gains a way to declare "this bare
dataset (no File Meta) is Implicit VR," since today a bare dataset always defaults to Explicit VR
LE and there is currently no way to correctly parse a bare Implicit VR dataset at all — a small,
clearly-scoped gap that becomes worth closing once Implicit VR is a first-class citizen.

**Public API impact.** Additive at the ABI level (no new functions strictly required — existing
`fds_element_vr()` simply starts returning richer, correct answers for Implicit-VR-sourced
structures). Behavior-changing at the semantic level for Implicit VR input specifically — this is
the point of the increment, not a side effect, but it must be called out precisely: any caller that
depended on "every Implicit-VR element is `VR::Unknown`" as if it were a permanent contract (rather
than a documented, temporary limitation) will see different, more correct results. No such
dependency exists in `fastDICOMstructure` or `fastDICOMgateway` today — neither restricts behavior
by Implicit-VR-sourced `VR::Unknown`-ness specifically (`fastDICOMgateway` currently rejects
Implicit VR input outright at its own acceptance-scope boundary, unaffected either way).

**Tests added.** Exactly the matrix this task's own §8 specifies, all against real (not merely
synthetic) constructions where feasible: defined-length SQ (the previously-opaque case — the
central new capability), undefined-length SQ (already worked, must keep working), defined-length
items, undefined-length items, multiple nesting levels, empty sequences, empty items, unknown
(non-dictionary) tags remaining `VR::Unknown`, private elements remaining `VR::Unknown` (dictionary
has no private entries, by design), and malformed sequence boundaries (must degrade to a
`RecoverableError` diagnostic, never a crash or silent data loss). Existing tests requiring triage,
named precisely: `tests/integration/test_parse_implicit_vr_le.cpp:45` and `:94` — each must be
checked against its fixture's actual tag and either updated to assert the newly-correct VR or (if
the test's intent was specifically "an unrecognized tag stays Unknown") have its fixture tag
changed to a genuinely private/unrecognized one so the test keeps testing what it says it tests.

**Differential validation.** pydicom, parsing the same Implicit VR fixtures, comparing VR
assignment and full recursive element visibility tag-by-tag, nesting-level-by-nesting-level. DCMTK
as a second oracle specifically for the ambiguous-VR disambiguation cases, since this is exactly
the kind of narrow, standard-mandated behavior where a second mature implementation's agreement is
worth having before trusting a single interpretation of the standard text.

**Corpus validation.** Required, and this is the increment that most needs *new* corpus material —
the existing NLST/CMB-MEL corpora are both 100% Explicit VR LE (`docs/corpus-results.md`), so they
cannot exercise this increment's central claim at all. See §16 for specific, named sources of real
Implicit VR sample data to acquire before this increment's freeze point.

**Performance/resource validation.** A dictionary lookup is now on the Implicit-VR hot path (one
per defined-length element). Given the lookup's own A1.1 benchmark (sub-microsecond), and that
Implicit VR parsing already does strictly more work per element than this adds, expect no
measurable throughput regression — but this must be *measured*, not assumed, against the same
micro-benchmark methodology used in the A0 baseline, run specifically on an Implicit-VR-encoded
version of the same representative CT file (synthesize one by re-encoding an existing Explicit-VR
NLST file as Implicit VR, since a real Implicit-VR sample of the same content does not exist
locally).

**Acceptance criteria.** This task's own §8 acceptance criterion, verbatim: for supported Implicit
VR Little Endian objects, every standard sequence recognized by the dictionary is recursively
parsed and every nested attribute is visible to recursive traversal and mutation. Plus: zero
regressions in the 115+34 existing tests beyond the two named, explicitly-triaged assertions above.

**Commit/freeze point.** Freeze Implicit VR's now-complete structural/VR behavior, and re-freeze
`docs/roundtrip-contract.md`'s "Implicit VR Little Endian" section to describe the new, narrower
remaining limitation (ambiguous-VR edge cases only, if any remain undocumented; everything else in
that section's current text about defined-length sequences being opaque becomes obsolete and must
be corrected, not left standing — the same "don't silently leave stale claims" discipline A0 used
for the family-relationship documentation).

### A1.5 — Character-set core (context resolution + single-byte repertoires + UTF-8, decode direction)

**Purpose.** Correctly decode DICOM text for the repertoire subset defined as V1 MUST in §2, with a
context-resolution mechanism that is correct for nested and multi-valued Specific Character Set,
not merely correct for the simple top-level-only case.

**Capability gained.** Given any `Element` reachable via an `ElementPath`, and the owning
`DICOMStructure`, resolve the effective Specific Character Set at that position (root default, or
the nearest enclosing Item's own override, per PS3.5's stated inheritance rule) and decode that
element's bytes into correctly-interpreted text if — and only if — its VR (now reliably known for
both transfer syntaxes, thanks to A1.4) is one of the text VRs Specific Character Set governs
(`SH LO ST LT PN UC UT`). Handles multi-valued Specific Character Set (ISO 2022 escape-sequence
switching) for the single-byte repertoires and UTF-8 named in §2.

**Explicit non-goals.** No encoding/mutation direction yet (A1.6). No multi-byte code-extension
repertoires (A1.8, explicitly deferred). No changes to `Value::as_string()`/`bytes()` — those stay
exactly as they are today (a "default repertoire" / raw view); this increment adds a **new**,
separate accessor rather than changing an existing one's meaning, per this task's explicit
instruction not to bolt charset semantics onto `Value`.

**Implementation scope.** New `include/fastdicomattrs/charset.hpp` + `src/charset.cpp`,
architecturally separate from `Value` and `Element` — a dedicated codec subsystem, as this task's
own framing in its §3/§9 recommends. Two clearly separated internal concerns inside it: (a) **context
resolution** — given a `DICOMStructure` and an `ElementPath`, walk the path's steps from the root,
checking for a `(0008,0005)` override at each `Item` level (this is exactly what `ElementPath`'s
existing `{Tag, item_index}` step list is for — no new object-model field needed, confirmed by the
absence of any parent back-pointer noted in §3, which makes this walk-down-from-root approach the
only sound option anyway), and (b) **codec engine** — a pure function from
`(bytes, resolved Specific Character Set value(s)) -> decoded text`, implementing the ISO 2022
single-byte state machine (G0/G1 designation, shift functions) and UTF-8 validation/passthrough,
parameterized entirely by data, never by structure-tree position. New public accessor, most
naturally `Element::as_text(const CharacterSetContext&)` or an equivalent free function taking the
`Element` plus a caller-obtained context, so a caller who wants many text values from the same
structural region resolves context once and decodes many elements against it (avoiding repeated
path-walks).

**Public API impact.** Additive only. New ABI functions needed since context resolution requires
structure-tree access the current per-`Element` ABI handle does not carry alone:
`fds_structure_resolve_charset_context(structure, path) -> opaque context handle`,
`fds_element_text(element, context) -> owned UTF-8 C string` (UTF-8 chosen as the ABI's canonical
text-interchange encoding since it round-trips through Python's native `str` with zero information
loss and no additional codec needed on the Python side).

**Tests added.** One fixture-and-assertion pair per MUST-scope repertoire (§2's list): the
repertoire's own designated escape/declaration correctly recognized, a representative string in
that repertoire decoded correctly against a known-correct reference decoding (not merely "does not
crash"). Additional: no `(0008,0005)` present (default repertoire applies), `(0008,0005)` present
with a single value, `(0008,0005)` present with multiple values (ISO 2022 switching mid-string,
the textbook Latin+Cyrillic-style case using two of the MUST-scope single-byte repertoires),
`(0008,0005)` overridden inside a nested Item (must apply only within that item and its own nested
content, not leak to sibling items or the parent), Person Name's component-group and component
delimiters interacting correctly with an active multi-byte-*capable* switch state even though the
actual multi-byte repertoires are out of scope (the delimiter-handling logic itself must still be
correct for the in-scope repertoires), empty string, a string of exactly the padding-boundary
length, and a deliberately malformed escape sequence (must produce a diagnostic, not a crash or
silently-wrong text).

**Differential validation.** pydicom, decoding the same fixtures — pydicom's own character-set
handling is mature and widely relied upon, making it the primary oracle here. Any disagreement
triaged against the PS3.5 text directly, not automatically resolved in pydicom's favor.

**Corpus validation.** The existing NLST/CMB-MEL corpora are almost certainly entirely default-
repertoire (US-based trial data); this increment specifically needs corpus material with non-
default Specific Character Set values to be a meaningful real-world check, not just a synthetic-
fixture one — see §16 for named sources.

**Performance/resource validation.** Context resolution must be lazy (computed only when a caller
actually requests text decoding for a specific path), never eager during parsing, to preserve the
"avoid gratuitous decoding" principle this whole family is built on — this is an explicit design
constraint to verify in review, not just a performance nicety. Cost when invoked: proportional to
nesting depth (bounded, small) per resolution, plus the codec's own per-byte cost only for values a
caller actually decodes. A structural/pixel-only consumer that never calls `as_text()` should see
zero measurable overhead from this increment's existence — verify by re-running the A1.4 Implicit-
VR-oriented benchmark and confirming no change, since that benchmark's workload does not call the
new text-decoding path at all.

**Acceptance criteria.** Every MUST-scope repertoire fixture decodes to the correct reference text.
Multi-valued switching and nested-context override both behave per PS3.5. No measurable parse-time
or memory regression for callers who never invoke text decoding.

**Commit/freeze point.** Freeze the `CharacterSetContext` resolution API and the codec engine's
internal table format before A1.6 builds the encode direction against the same tables (encoding
needs the inverse of exactly these tables — building them once, correctly, and freezing their
shape avoids a second independent table-construction effort for A1.6).

### A1.6 — Character-set re-encoding (mutation direction)

**Purpose.** Let a caller set a native (already-decoded) text value into a text-VR element and have
it correctly encoded back to bytes consistent with that position's resolved Specific Character Set,
for the same MUST-scope repertoire set A1.5 decodes.

**Capability gained.** `set_value()`/`set()` (and their new path-based Python counterparts from
A1.7 — sequenced after this, so this increment's own tests exercise it via the existing C++/ABI
mutation primitives) gain a text-aware mode: given a native string and a resolved
`CharacterSetContext`, encode it to conformant bytes, pad to even length, and write it exactly as
any other mutation would be written.

**Explicit non-goals.** No automatic rewriting of `(0008,0005)` to widen the repertoire for a value
that does not fit the current one — refused with a clear diagnostic instead (this task's own
principle: "interpretation is additive; it must not destroy access to original encoded bytes,"
extended here to "mutation must not silently rewrite dataset-level context the caller did not ask
to change"). No re-encoding of *other, untouched* text elements when `(0008,0005)` itself is
directly mutated by the caller — that is the caller's own responsibility if they choose to change
it; attrs neither forbids nor auto-corrects it, consistent with mutation always being scoped to the
element actually named.

**Implementation scope.** `src/charset.cpp` gains the inverse direction of A1.5's per-repertoire
tables (encode instead of decode) and a validation path (can this text be represented in the target
repertoire? if not, which character/position failed?). `dicom_structure.cpp`'s `set_value`/`set`
gain a text-aware overload or a separate `set_text()`-shaped entry point — additive either way.

**Public API impact.** Additive. New ABI function, `fds_structure_set_text(structure, tag_or_path,
context, utf8_text) -> status`, mirroring A1.5's `fds_element_text` on the write side.

**Tests added.** Round-trip (decode-then-encode-then-decode-again yields the original text) for
every MUST-scope repertoire. The refusal path: a value that cannot be represented in the current
repertoire is rejected with a specific, actionable diagnostic, and the structure is left unmodified
(matching the existing `set_value`/`set` contract of "reject rather than silently truncate/corrupt"
established at A0's baseline for length-form violations — this increment extends the same
discipline to encoding failures). Padding interaction: an odd-length encoded result is padded
correctly per the target VR's padding rule (space for most text VRs, NUL for `UI`-adjacent cases —
though `UI` itself is not charset-affected; verify the padding byte choice is VR-driven, not
charset-driven, and stays correct).

**Differential validation.** pydicom encoding the same native strings under the same Specific
Character Set, byte-for-byte comparison of the encoded result.

**Corpus validation.** Not meaningfully corpus-testable (mutation is a synthetic-fixture concern by
nature); skip.

**Performance/resource validation.** None expected beyond A1.5's own; encoding is caller-invoked,
never on any hot path.

**Acceptance criteria.** Round-trip fidelity for every MUST-scope repertoire. Refusal path never
silently corrupts. No regression in existing mutation tests (odd-length rejection, length-form
overflow rejection) from A0's baseline.

**Commit/freeze point.** None beyond its own tests; A1.7 does not depend on anything this increment
freezes beyond what A1.5 already froze.

### A1.7 — Mutation ergonomics: path-based Python/ABI access, VR-optional insert, auto-padding

**Purpose.** Close the last gap in the "callers need not understand DICOM encoding" promise for
Python specifically: today, C++ has `find_mutable(ElementPath)` but Python only has tag-based
`*_recursive` calls (`python/fastdicomattrs/__init__.py`'s existing methods) — a caller cannot
address one *specific* nested occurrence from Python without dropping to C++.

**Capability gained.** Python gains `Structure.find(path)` / `Structure.set_value(path, value)` /
`Structure.erase(path)` accepting the same `{tag, item_index}` step list C++'s `ElementPath` uses,
not just a bare tag. `Structure.set(tag_or_path, value)` (no VR argument) infers VR from the
dictionary (A1.1/A1.4) when the tag is a new top-level insertion and the dictionary has an
unambiguous answer; the existing VR-required form remains available and is still required when the
dictionary cannot supply one. Values needing even-length padding are padded automatically by
default, with an explicit opt-out for a caller that wants today's exact behavor.

**Explicit non-goals.** No change to the *recursive*, tag-only convenience methods
(`erase_recursive`, `set_value_recursive`, `erase_private`) — they remain, unchanged, as the
right tool for "every occurrence of this tag, wherever it is." This increment adds precision
addressing alongside them, not instead of them.

**Implementation scope.** New ABI functions marshaling a variable-length path (array of
`{group, element, has_item_index, item_index}` structs) — the natural extension of the existing
`fds_tag_t`-based ABI surface, not a redesign of it. `python/fastdicomattrs/__init__.py` gains an
`ElementPath`-equivalent Python type and the new method overloads.

**Public API impact.** Additive only.

**Tests added.** Path-based get/set/erase at every nesting depth already covered by the C++-side
`ElementPath` tests, now exercised from Python end to end through the ABI. VR-optional insert: a
well-known tag inserted without specifying VR resolves correctly; an ambiguous or unrecognized tag
inserted without VR is rejected with a clear diagnostic (never silently guesses). Auto-padding: odd-
length values padded correctly by default; the opt-out reproduces today's exact (unpadded, caller-
responsible) behavior byte-for-byte, so no silent behavior change for any caller who does not ask
for the new default.

**Differential validation.** Not applicable — this is an ergonomics/API-surface increment, not a
new semantic interpretation; correctness is defined by matching the already-validated C++ behavior
through the new Python path, not by comparison against another toolkit.

**Corpus validation.** Not needed.

**Performance/resource validation.** None expected; this is Python-binding surface area, not a
change to the parsing or writing hot path.

**Acceptance criteria.** Every C++-only nested-mutation capability documented today as
"C++-only in this increment" (`python/fastdicomattrs/__init__.py`'s own module docstring) is
reachable from Python. VR-optional insert behaves correctly or refuses clearly; never guesses
silently.

**Commit/freeze point.** This is the last increment before the V1 qualification campaign (§15); no
further API surface changes should occur once this freezes, since the campaign exercises the full,
final V1 surface.

### A1.8 — [Fast-follow, outside the V1 freeze] Multi-byte character sets

**Purpose.** Extend A1.5/A1.6's charset subsystem to the multi-byte code-extension repertoires
explicitly excluded from V1's MUST scope: Japanese Kanji (ISO 2022 IR 87) and supplementary Kanji
(ISO 2022 IR 159), Korean (ISO 2022 IR 149), and the non-code-extension multi-byte sets GB18030 and
GBK.

**Why this is not part of the V1 freeze, stated plainly.** The single-byte ISO 2022 state machine
A1.5 builds is a strict, much simpler subset of the general mechanism — multi-byte code-extension
switching involves G0/G1 *and* G2/G3 designation nuances and genuinely larger per-codec table data
(JIS X 0208 alone is on the order of several thousand code points). This is real, substantial,
independently-scoped engineering work, not a small extension, and treating it as part of the same
increment as A1.5 would violate this plan's own "narrow increments, one semantic change per
increment" principle more than any other candidate merge would. Given no multi-byte-charset sample
data exists in the currently-available local corpus, and given the explicit instruction to choose
corpus diversity "based on the claims being tested" rather than for appearance, this work is best
scheduled once real sample data (§16) has actually been acquired, rather than estimated blind.

**Disposition.** Scheduled immediately after the V1 freeze, described in the same document family,
sized and risked honestly (§8's XL estimate) so it is never quietly treated as "nearly done" filler
work squeezed into V1 under schedule pressure. The V1 claim in §2 is written to be true *without*
this work landing, and remains true regardless of when A1.8 actually ships.

## 8. Dictionary architecture recommendation

**Representation: a compile-time-generated, sorted static array of POD entries (packed
`uint32_t` tag, `VR` byte, small flag byte, `uint32_t` offset into a shared string pool for the
keyword), looked up by binary search.** Not a runtime-loaded resource file, not a generated perfect
hash.

**Why not runtime-loaded.** A loaded resource (JSON, binary blob, whatever format) reintroduces
exactly the class of path-discovery problem the shared-library loader already has to solve
(`_find_library`/`_candidate_dirs` in `python/fastdicomattrs/__init__.py`) — now for a second,
different kind of artifact, and one that additionally has to survive Python wheel packaging and
serverless/container filesystem layouts cleanly. It also adds a real, if small, first-use load/parse
cost and a new failure mode ("dictionary file missing or corrupt") that compiling the data directly
into the binary eliminates by construction. None of the traditional reasons to prefer a runtime
resource — updating data without recompiling, sharing one data file across many processes — are
actual goals here.

**Why not a perfect hash / generated index (e.g., gperf).** At roughly 5,000 entries, binary search
costs about 12–13 comparisons — sub-microsecond, and already dwarfed by the per-element work
parsing does regardless (reading bytes, computing spans, allocating `Element` objects). A perfect
hash trades this negligible cost for an external build-tool dependency and a less auditable
generated artifact, for no measurable benefit. Revisit only if a future benchmark ever shows
dictionary lookup as a non-negligible fraction of total parse time — a claim this plan does not
expect to become true, and will verify explicitly in A1.4's performance validation rather than
assume.

**Why not simply copy pydicom's approach (a Python dict).** pydicom is not compiled, has no binary-
size or native-cold-start profile to protect, and runs in an environment where "just import a large
Python module" is cheap and idiomatic. `fastDICOMattrs`'s stated deployment targets — containers,
serverless, cold-start-sensitive execution — are a materially different environment where a
zero-I/O, compiled-in table is the right choice for reasons that do not apply to pydicom at all.
Divergence here is deliberate, not an oversight.

**Minimum information per entry, and why not more.** Tag, VR (or an "ambiguous, resolve via rule
X" marker for the small `US or SS`-class set), and — cheap to include, not gating — keyword. VM is
*not* included as an enforced field: the existing generic backslash-splitting (`as_string_list()`)
already handles multi-valued elements correctly without needing to know or validate against a
declared VM, and building VM-based validation would risk *rejecting* legitimately-tolerant real-
world input a permissive parser should accept. Retired tags are included with their historically-
correct VR (still needed for legacy Implicit VR files) but no separate "is retired" runtime
behavior is built — informational only, if anything, and only if free.

**Repeating groups.** A small number of PS3.6 patterns use a group *range* rather than one fixed
group (Overlay Data and its siblings at `60xx,3000` for `xx` in `00`–`1E`, plus a couple of retired
Curve Data patterns at `50xx,...`). Rather than enumerating every concrete group as a separate table
row, the lookup falls back to a short, explicit list of masked-range rules (fewer than ten) checked
only after an exact-match lookup fails — keeping the primary table's row count and generation logic
simple while still handling every real repeating-group case correctly.

**Generation pipeline and provenance.** A small, checked-in, offline Python generator script
(not part of the runtime build or any consumer's build graph) consumes a specific, pinned,
version-controlled extraction of the official DICOM PS3.6 machine-readable data (the standard is
published with a machine-readable DocBook/XML source; a pinned snapshot of the relevant table,
checked into this repository alongside the generator, is the source of truth — not a live fetch at
build or generation time, for determinism) and emits the generated `.cpp`/`.hpp` pair
deterministically. The generated files are committed to the repository (not regenerated by CMake at
every build) specifically so the answer to "which DICOM dictionary version produced this build?" is
a `git blame`/commit-message fact, not something requiring the generator and its exact input
snapshot to be re-run to discover. Every regeneration commit message must name the standard edition
and publication date the snapshot came from.

**Estimated size impact.** Roughly 5,000 entries × ~10 bytes fixed fields ≈ 50 KB, plus a string
pool of roughly 5,000 keywords averaging ~15 characters ≈ 75 KB. Call it 125–150 KB total —
negligible next to the existing compiled library's size and utterly irrelevant to any realistic
binary-size or cold-start budget.

## 9. Implicit VR strategy

Covered in full technical detail in §5 and §6 (A1.4). Summary of the decision: dictionary-backed VR
resolution and Implicit-VR structural completeness are the same increment, not two — the dictionary
*is* the mechanism that makes `SQ` (and every other VR) recoverable under Implicit VR, so there is
no meaningful way to sequence "resolve VR" separately from "make sequences visible." The one
genuinely separable sub-concern, ambiguous-VR disambiguation (`US or SS` and similar), is folded
into the same increment rather than split out, because it shares the same fixture set, the same
differential-testing partners, and the same risk surface (both are "does this Implicit-VR element
get the right VR" questions), and splitting it would produce two increments that could not be
meaningfully frozen or tested independently of each other.

## 10. Character-set architecture recommendation

Covered in full technical detail in §6 (A1.5/A1.6). Summary of the decision: character-set logic
lives in a **new, dedicated codec subsystem** (`charset.hpp`/`.cpp`), never inside `Value` — `Value`
has no concept of its position in the structural tree, and Specific Character Set is fundamentally
tree-position-dependent context (it can be overridden per-Item, per PS3.5's own inheritance rule),
so baking decoding into `Value` would require giving the lowest-level byte-holding type a concept it
structurally cannot correctly reason about. Context resolution is a top-down walk of an
`ElementPath`'s existing step list (checking for a `(0008,0005)` override at each `Item` level),
computed lazily on demand rather than eagerly during parsing — this is the only sound option given
the object model's confirmed absence of any parent back-pointer (§3), and it happens to also be the
right choice on its own merits (avoids paying any cost for callers who never decode text).

## 11. Private-element strategy

Covered in §6 (A1.2). The realistic, honest V1 guarantee is identity, not interpretation: which
registered creator owns a given private element's block. This is derivable purely structurally
(reading the creator element's own `LO` value, per PS3.5 §7.8.1) and requires no dictionary, no
per-vendor knowledge, and makes no claim whatsoever about what a given creator's private data
actually means. A general vendor-private-tag dictionary (the kind DCMTK partially ships, covering
some well-known manufacturers' well-known private tags) is a legitimate, larger, separate future
project — explicitly not part of V1, and not implied or half-attempted by this increment.

## 12. Writer-gap disposition

**Recommendation: fix it, as part of V1, not defer it — agreeing with this task's own stated
bias.** The counter-argument for deferral would be "this is rare in practice" (true: the entire
26,636-file corpus that grounds A0's evidence has Pixel Data last in every file), but rarity is not
the same as irrelevance to the V1 *claim*. A library whose central promise is "reliable... mutation
of, and serialization of all attributes within its declared supported scope" cannot honestly make
that promise while silently reordering — not merely failing to preserve byte-identity for, which
would be a lesser and more defensible limitation, but actually *moving* — a valid element to a
different position in its output than the source declared. Silent reordering is a correctness
defect, not a documented-limitation-shaped gap like "Explicit VR Big Endian is not supported." Fixed
in A1.3, early and independently, precisely because it is cheap, self-contained, and there is no
principled reason to carry a known correctness defect into a release whose explicit purpose is
establishing a credible, falsifiable completeness claim.

Padding, transfer-syntax consistency, and defined/undefined-length preservation are unaffected by
this fix and remain exactly as today (already correct, per the A0 baseline's evidence) — no
additional writer-gap work is identified beyond the Pixel-Data-ordering case.

## 13. File-meta/dataset separation: deferred

No increment in this plan touches the current single-ordered-list representation of File Meta
Information alongside the dataset. No consumer — not `fastDICOMstructure`'s policy layer, not
`fastDICOMgateway`'s transform pipeline, not any test or fixture in the current suite — has ever
needed to distinguish "this is File Meta" from "this is dataset content" as anything other than "an
element whose group happens to be `0002`," which `Tag`'s existing `group` field already answers
without any object-model change. This is deferred beyond V1 as a documentation/API-clarity
question, not a correctness gap, and this plan does not manufacture a need for it that does not
exist.

## 14. Streaming `Source`: architecture must permit it; V1 does not implement it

None of A1.1–A1.7 introduces any new assumption that would make a future streaming `Source`
harder to add than it already would be. Concretely: dictionary lookups are pure data operations,
entirely independent of how bytes reached the parser. Character-set decoding operates on already-
materialized `Value` bytes, regardless of the `Source` that produced them. The one increment that
touches the object model in a structural way (A1.3's Pixel-Data-position field) adds a single
optional integer, not a new access pattern. **No increment in this plan should be implemented in a
way that assumes full random-access to the entire input up front beyond what the existing
`Source`/`SourceSpan` abstraction already assumes** — this is a design constraint to check in code
review for each increment, not a new feature to build. Streaming input itself remains explicitly
out of scope for A1, matching this task's own framing: the architecture must permit it eventually;
V1 does not need to deliver it now.

## 15. Final V1 qualification campaign

A distinct phase after A1.7, not folded into any single increment's own tests, because its purpose
is to exercise the *combination* of everything A1 built, not any one piece in isolation.

**Unit qualification.** Every semantic subsystem's own test suite (dictionary, Implicit VR parsing,
charset decode, charset encode, path-based mutation) re-run together as one suite, not just
individually, to catch any interaction the individual increments' own narrower tests could not see.

**Generated combinatorial fixtures.** A matrix generator (extending `tests/integration/
fixture_builder.cpp`'s existing in-code fixture-construction approach) producing every combination
of: {Explicit VR, Implicit VR} × {every standard VR, including the ambiguous set} × {short-form,
long-form length} × {defined length, undefined length} × {nesting depth 0, 1, 3} × {default
repertoire, each MUST-scope single-byte repertoire, UTF-8, multi-valued repertoire} × {unmodified,
one value replaced, one value inserted, one value removed}. Not every cell of this matrix is
meaningful (e.g., VR and length-form are not independent), so the generator should skip invalid
combinations rather than force them, but the intent is systematic, not hand-picked, coverage.

**Differential qualification.** pydicom across the full combinatorial matrix above (fast enough to
run in CI at that scale); DCMTK specifically for the ambiguous-VR and multi-valued-charset cases
where a second oracle's agreement is most valuable. Any disagreement is triaged against the PS3.5
standard text directly — an oracle's answer is evidence, not automatic ground truth, consistent
with this task's own instruction.

**Real-world corpus qualification.** The existing NLST/CMB-MEL corpora remain valuable for what
they already prove (structural round-trip fidelity at scale) but contribute nothing new to this
phase's specific claims (Implicit VR, non-default charset), since both are 100% Explicit VR,
default-repertoire data. New, targeted corpus material is needed — see §16 for exactly what and
where, chosen for the specific claims being tested rather than for volume.

**Round-trip qualification.** The existing three-way distinction (semantic equivalence, structural
equivalence, byte preservation) is unchanged in kind, but now must be proven across the newly-
supported Implicit VR and charset dimensions too, not only Explicit VR/default-repertoire as today.

**Mutation qualification.** Targeted value changes leave unrelated attributes untouched (existing
proof, re-run against the new nested-path and charset-aware mutation paths specifically). Output
re-parses independently (pydicom). Character-set mutations serialize to conformant bytes and
decode back to the original text through an independent parser, not just this library's own.

**Resource qualification.** Throughput and peak RSS, re-measured against the same methodology as
the A0 baseline, specifically checking: dictionary lookup overhead (should be unmeasurable per
§6/A1.1's own microbenchmark), charset overhead for callers who invoke it versus callers who never
do (the latter must show zero delta), package/binary size delta (expect the ~125–150 KB dictionary
plus a comparably modest charset-table addition — call it under 500 KB total, to be confirmed, not
assumed), and — since this now matters for the first time — Python import/cold-start time, given
the family's stated interest in serverless deployment.

## 16. Real-world corpus diversity needed, and named sources

Chosen for the specific claims A1 makes, not for volume:

- **Implicit VR Little Endian real-world samples.** Needed to validate A1.4's central claim against
  real scanner output, not only synthetic fixtures — the existing corpus cannot do this at all
  (100% Explicit VR). David Clunie's public DICOM test-image collection
  (`dclunie.com/images.html`) is the standard, widely-cited source for exactly this kind of
  deliberately edge-case-covering sample data and is known to include Implicit VR examples; it is
  named here as a concrete starting point rather than left as "find more data."
- **Non-default Specific Character Set real-world samples.** The same Clunie collection includes a
  dedicated character-set test set (commonly referenced as the "charset" samples in DICOM toolkit
  test suites, covering several of the single-byte repertoires and at least one multi-valued
  case) — the right first place to look before synthesizing everything from scratch.
- **Encapsulated (compressed) Pixel Data with a non-trivial private-tag/charset combination**,
  to exercise A1.2/A1.5 together on real data rather than only in isolation.

None of this requires an enormous corpus. A few dozen well-chosen files covering Implicit VR and
each MUST-scope repertoire at least once is sufficient to move these claims from
"synthetic-fixture-proven" to "real-world-proven," matching how the existing 26,636-file NLST/
CMB-MEL run already proves Explicit VR structural fidelity — this plan does not recommend chasing a
similarly large *count* for these narrower, more targeted claims.

## 17. Publication-claim mapping

| Increment | Claim it enables |
|---|---|
| A1.1 (dictionary substrate) | Foundation only; no standalone claim |
| A1.2 (private-creator identity) | "Reliable identification of private elements, including creator/block identity" |
| A1.3 (writer ordering fix) | "Serialization preserves all attributes' relative order, including Pixel Data" |
| A1.4 (dictionary VR + Implicit VR) | **"Complete recursive attribute visibility within the declared syntax envelope"** and **"deterministic semantic interpretation"** — the two central claims this task's framing repeatedly returns to |
| A1.5/A1.6 (charset) | **"Deterministic semantic interpretation"** extended to text content specifically — without this, "interpretation" would silently mean "interpretation only for ASCII-compatible text," which is not a claim worth publishing as complete |
| A1.7 (mutation ergonomics) | **"Mutation without requiring callers to understand DICOM encoding"** — the one claim that was still only true for C++ before this increment |
| Charset core's lazy-resolution design (A1.5) + A1.3's zero-copy discipline throughout | "Preservation of uninterpreted bulk data" and "bounded resource behavior for metadata-oriented processing" — proven, not merely designed-for, by §15's resource qualification |
| Explicit non-touching of `Source`/streaming (§14) | "Deployment-neutral semantic core" — the claim that the *architecture* (not yet the implementation) does not foreclose portable, streaming-capable execution |

## 18. Major technical risks

- **A1.4 is the highest technical risk in this plan.** It is the most architecturally entangled
  change: it touches the parser, changes the meaning of an existing, load-bearing accessor
  (`Element::vr()`) for a whole class of previously-uniform input, requires migrating two named,
  currently-passing tests rather than merely adding new ones, and must simultaneously update
  `docs/roundtrip-contract.md`'s own claims about Implicit VR without leaving stale text standing.
  Any subtle bug here (an incorrect ambiguous-VR disambiguation, an off-by-one in the new
  bare-Implicit-VR-dataset path, a dictionary entry that disagrees with real-world scanner output
  despite matching the standard text) would silently produce wrong *interpretation* rather than an
  obvious crash — the class of bug hardest to catch without the differential and adversarial testing
  this plan insists on for exactly this increment.
- **Character-set correctness is a close second**, for a related reason: a wrong decode is a wrong
  *answer*, not a crash, and multi-valued/nested-context resolution is exactly the kind of logic
  that looks correct against a simple fixture and fails against a real, more complex one. This is
  why §16 insists on acquiring real non-default-repertoire sample data rather than trusting
  synthetic fixtures alone.
- **Dictionary provenance/currency risk.** DICOM's PS3.6 table is periodically updated; a dictionary
  frozen at one edition will, by construction, mis-resolve any tag introduced by a later edition as
  "not found" (safe, `VR::Unknown` fallback — not a wrong-answer risk, but a completeness gap that
  will grow slowly over time unless the generation pipeline is revisited periodically). This is a
  known, bounded, and honestly-disclosable limitation as long as the generated artifact's provenance
  comment (§8) always states which edition it reflects — the risk is real but well-contained by the
  generation pipeline's own design.
- **Scope-creep risk on A1.8.** The single largest temptation in this whole plan is finishing V1
  qualification, seeing that multi-byte character-set support is "most of the way there" because
  the single-byte scaffolding already exists, and pulling it into the freeze under schedule
  pressure. §6 and §7 (A1.8) are written specifically to make that an explicit, visible decision
  rather than something that happens by drift.

## 19. Estimated relative effort by increment

| Increment | Effort |
|---|---|
| A1.1 — Dictionary substrate | M |
| A1.2 — Private-creator identity | S |
| A1.3 — Writer ordering fix | M (object-model touch, not writer-only, per §3's evidence) |
| A1.4 — Dictionary VR + Implicit VR completeness | L |
| A1.5 — Character-set core (decode) | L |
| A1.6 — Character-set re-encoding | M |
| A1.7 — Mutation ergonomics | M |
| V1 qualification campaign (§15) | L (its own phase, not sized as a single increment) |
| A1.8 — Multi-byte character sets (fast-follow, outside freeze) | XL |

## 20. Explicit recommendation for the first implementation increment

**A1.1 — Dictionary substrate.** Three reasons, not one: it is the single highest-leverage
foundational piece (A1.4 cannot begin without it, A1.7's VR-optional insert cannot exist without
it, and A1.5/A1.6 are cleaner for having it land first even though not strictly blocked by it); it
carries real, worth-discovering-early uncertainty of its own (generation pipeline correctness,
provenance tracking, cross-checking against pydicom/DCMTK) that is better surfaced now than
discovered mid-way through A1.4; and it is itself a clean, fully independently testable and
freezable increment with a binary pass/fail bar (does the generated table agree with the cited
standard edition and with pydicom's own table), matching this plan's own "smallest independently
testable increment" standard exactly. A1.2 and A1.3 may proceed before, after, or alongside it
without conflict, per §5's explicit note that they carry no dependency relationship with A1.1 or
each other.
