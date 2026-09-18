# A1.7 — Mutation Ergonomics and Public API Completion: Freeze Report

## 1. Starting frozen state

A1.1 `6f05d9d`, A1.2 `e651c74`, A1.3 `140d850`, A1.4 `87e79fe`, A1.5 `656edf4`, A1.6 `50369c0`.
Baseline at A1.7 authorization: 244 C++ test cases, 39 Python tests, all passing, clean build.

## 2. Scope

Central requirement (verbatim from the authorization): "Expose the semantic capabilities already
proven in A1.1–A1.6 through a small, coherent, path-aware mutation API that works for root and
nested elements, supports safe insertion/removal/replacement, uses dictionary and charset
semantics rather than bypassing them, and provides usable C++ and Python surfaces without
duplicating core logic." Explicitly an API/composition increment, not a new DICOM-semantics
increment. In scope: path-oriented CRUD completion, nested insertion, VR-optional insertion,
charset-aware nested insertion, Python bindings, limited C ABI, atomicity, modification
propagation, stale-reference safety, recursive iteration, auto-padding review, documentation. Out
of scope: new charset repertoires, new transfer syntaxes, dataset-wide transcoding, vendor-private
dictionaries, a policy engine, de-identification, streaming, archive/cloud interfaces,
`fastDICOMstructure` redesign, new parser semantics.

## 3. Design checkpoint

Mid-increment, after implementing the first structural substrate (an early `insert()`/`upsert()`
pair taking a full leaf `ElementPath`), work was paused for a design checkpoint per the
authorization's own instruction. The checkpoint delivered: (a) 21 passing structural-insertion
tests against that early primitive (no defect found), (b) a full API inventory across
`DICOMStructure`/`ElementPath`/`fds::charset`/the C ABI/Python, (c) a reconsideration of the
`insert()` signature and `upsert()`'s place in V1, (d) a proposed minimal C++ surface, (e) a
canonical-semantics recommendation distinguishing "locate an existing object" from "locate an
insertion container," (f) a result-model proposal, (g) a VR-inference policy proposal, (h) a
charset-composition proposal, (i) Python/ABI surface proposals, and (j) an explicit list of APIs
recommended against. The checkpoint was reviewed and accepted with corrections; sections 4–19
below describe the accepted design and its implementation, explicitly noting where it reverses the
pre-checkpoint approach.

## 4. Final insertion signature and canonical path semantics

`DICOMStructure::insert(const ElementPath& parent, Tag tag, VR vr, Value value)` — reversing the
pre-checkpoint design. `parent` is a *container locator*: every step, with no exception for the
last, must be a descent step (Sequence tag + Item index); an empty `parent` names the root
dataset. This is a second, narrower use of `ElementPath` than `find`/`set_value`/`erase` make of
it (an *element locator*, where the last step is bare and names the leaf itself) — the same type,
used in two documented, non-interchangeable shapes, rather than a new `ContainerPath` type (a
dedicated type was considered and rejected in the checkpoint as more surface than the ergonomic
gain justifies for V1). `Tag` is a separate parameter, not folded into the path, precisely because
a not-yet-existing element cannot itself be the locator. Both contracts are documented at length in
`dicom_structure.hpp`'s doc comments and in `element_path.hpp`. A dedicated test
(`test_nested_insert.cpp`, "insert rejects a parent path whose last step is bare") proves a
caller who mistakenly passes an element-locator-shaped path as `parent` fails cleanly
(`ContainerNotFound`), not silently or by crashing.

## 5. `upsert()` removal

The short-lived checkpoint `DICOMStructure::upsert()` (and its two tests) was deleted in its
entirety, per the reviewed design decision: it ambiguously combined insertion and replacement, and
silently discarded a caller-supplied VR on the replace branch — a footgun not worth enshrining in
a second primitive. No path-based upsert exists anywhere in the final surface (C++, Python, ABI).
The documented replacement is explicit two-call composition: `find` then `set_value` (existing) or
`insert` (absent) — demonstrated in `dicom_structure.hpp`'s `insert()` doc comment and in the
Python module docstring. The pre-existing, frozen, root-only `DICOMStructure::set()` (A0-era) is
untouched — its own upsert behavior (silently ignoring `vr` on replace) is a known, accepted,
*separate* piece of compatibility surface, left alone per the explicit instruction not to "fix"
it during A1.7.

## 6. Structural/dictionary layering

`DICOMStructure` remains fully dictionary-agnostic — it never includes `dictionary.hpp`, exactly
as before A1.7. The new `fds::mutation` namespace (`include/fastdicomattrs/mutation.hpp`,
`src/mutation.cpp`) is where the PS3.6 dictionary dependency is allowed to exist:
`mutation::insert_inferred()` infers a VR and calls `DICOMStructure::insert()` underneath;
`mutation::insert()` is a richer-status, explicit-VR twin of `DICOMStructure::insert()` (added
specifically so the C ABI's single `fds_structure_insert_path` entry point can report the same
diagnostic richness — `FDS_STATUS_ALREADY_EXISTS` in particular — for both its explicit- and
inferred-VR modes, without a fourth reimplementation of container-location logic). Neither
function is a "hybrid": `insert()` never touches the dictionary; `insert_inferred()` has no
explicit-VR parameter at all, so there is never a caller-supplied-vs-inferred conflict to
arbitrate.

## 7. Centralized container-location logic

One authoritative traversal implementation, `fds::internal::locate_container`/
`locate_container_const` (`src/internal/container_locate.{hpp,cpp}`, unexported), walks a
container-locator path and returns `Success`/`ContainerNotFound`/`NotASequence`/
`ItemIndexOutOfRange`. `DICOMStructure::insert()`, `fds::mutation::insert()`/`insert_inferred()`,
and `fds::charset::insert_text()`/`insert_text_inferred()` all call this one function — none
reimplements the walk. `DICOMStructure::insert()` translates the result to `bool` (matching every
other structural primitive's convention); the higher layers translate it to their own richer
status enums. `fds::internal::infer_vr` (`src/internal/vr_inference.{hpp,cpp}`) is the identical
pattern for VR-inference policy: one implementation, used by both `fds::mutation` and
`fds::charset`.

## 8. VR-inference policy (final)

Implemented exactly as approved: unambiguous dictionary VR → inferred; ambiguous VR (PS3.6
`VRAmbiguity != None`, e.g. "US or SS") → `VRRequired`, with A1.4's sibling-context resolution
deliberately *not* reused (that reads context that may not exist yet at insertion time, and reuse
would reintroduce a "guess instead of ask" pattern); a tag absent from the dictionary →
`VRRequired`; a private (odd-group) data element → `VRRequired` always (the dictionary has no
entries for private data by design); a Private Creator declaration (odd group, element
0x0010–0x00FF) → inferred `VR::LO`, a normative PS3.5 7.8.1 fact independently confirmed by A1.2's
own `PrivateElementKind::Creator` classification, not a dictionary guess. Explicit caller VRs are
never overwritten — there is structurally no mechanism by which they could be, since
`insert_inferred()` takes no VR parameter at all. Tested exhaustively in `test_mutation_inferred.cpp`
(unambiguous standard tag at root/nested, ambiguous rejection, dictionary-miss rejection, private
data rejection, Private Creator inference at root/nested, container-location-failure propagation,
duplicate rejection).

## 9. Charset-aware nested insertion composition

`fds::charset::insert_text()`/`insert_text_inferred()` (`charset.hpp`/`charset.cpp`) implement
exactly the approved flow: locate parent container → confirm target tag absent → resolve/infer VR
→ resolve the effective Specific Character Set context **at the target container** → `encode_text()`
(A1.6, unchanged) → only on total success, `DICOMStructure::insert()`. No second charset
implementation exists: `insert_text_inferred()` calls `fds::internal::infer_vr` (the same function
`mutation::insert_inferred` uses) then delegates to `insert_text()`.

## 10. Parent-scope charset resolution — the freeze-critical proof

`resolve_character_set_context()` (A1.5) was built for an *element-locator* path: its internal
walk stops at the last step without descending into it, which is correct for that use (the last
step names the leaf itself) but would be *wrong* if handed a container-locator path directly — it
would never check the target container's own local `(0008,0005)` override, silently falling back
to whatever ancestor scope was found first. This was inspected, not assumed. The fix: the shared
inheritance walk was extracted into `find_effective_charset_declaration()` (a pure descent-steps
walker, no `is_last` special case), with `find_effective_charset_element()` (element-locator, feeds
it `steps[0..N-2]`) and a new internal-only `find_effective_charset_for_container()`
(container-locator, feeds it every step) as thin wrappers — one inheritance algorithm, two callers,
exactly as required. `resolve_character_set_context_for_container()` (internal, not in the public
header — reached only through `insert_text`/`insert_text_inferred`) composes this with the existing
classification logic (also extracted into a shared `build_context_from_search()`, so no
duplication there either).

This was proven, not just argued: `tests/integration/test_charset_insert.cpp` has six tests
matching the required scenarios exactly (root declaration → root insertion; root declaration →
nested inheritance; nested local override; a three-level-deep inheritance case skipping a
no-declaration intermediate level; sibling-isolation; an unsupported nested declaration failing
atomically) plus five more atomicity/failure-class tests. **Differential verification**: the
container-context resolver was temporarily reverted to call the (incorrect) element-locator walker
directly, the suite rebuilt, and rerun — 3 of the 6 scenario tests failed exactly as predicted
(local-override, sibling-isolation's override assertion, and unsupported-declaration atomicity),
confirming these tests actually catch this specific class of bug rather than passing vacuously.
The fix was then restored and the full suite reconfirmed green (280 test cases at that point).

## 11. Status/result model

`DICOMStructure::insert()` stays `bool`, matching every existing structural primitive's convention
(no new enum at the lowest tier). `fds::mutation::InsertStatus` (8 values: `Success`,
`ContainerNotFound`, `NotASequence`, `ItemIndexOutOfRange`, `AlreadyExists`, `VRRequired`,
`InvalidVR`, `ValueTooLong`) is the one new enum for the raw insertion layer.
`fds::charset::SetTextStatus` was extended additively (6 new values reusing the same names) rather
than replaced — `set_text()`'s existing enumerators and behavior are byte-for-byte unchanged, so
this is not a rewrite of frozen A1.6. No universal cross-cutting status type was created; charset
concerns (`UnrepresentableCharacter`, `InvalidUnicodeInput`) stay in `SetTextStatus`, raw concerns
stay in `InsertStatus`.

## 12. Atomicity

Every insertion/replacement path (`DICOMStructure::insert`, `mutation::insert(_inferred)`,
`charset::insert_text(_inferred)`, `charset::set_text`) validates everything — container
resolution, duplicate check, VR resolution, charset resolution, Unicode validation,
representability, padding, length — before touching `structure` at all; a single mutation call
happens only on total success. Tested directly, not assumed, for every failure class: duplicate
tag (root and nested), nonexistent/non-sequence/out-of-range container, malformed
container-locator shape, SQ/Unknown VR, odd-length-then-oversized value, unsupported/malformed
charset declaration, unrepresentable character, invalid UTF-8, VR-required. Every such test
asserts both the specific rejection and `structure.is_modified() == false` / no partial insertion
afterward.

## 13. Existing nested replacement/removal — coherence, not new capability

`find(path)`, `set_value(path, ...)`, `erase(path)`, `set_text(path, ...)` were already
nesting-capable before A1.7 (A1.2–A1.6); this increment did not rename `erase` to `remove` or add
aliases for naming symmetry, per instruction. `erase()`'s existing behavior — a Sequence element's
entire subtree is destroyed via ordinary C++ RAII when erased — was confirmed (not re-implemented)
to already fully support root, nested-scalar, and nested-Sequence removal; item-level deletion
(removing one Item without removing the whole Sequence) remains deliberately unimplemented, per
the authorization's explicit permission to skip an "awkward pseudo-tag" mechanism for it.

## 14. Recursive iteration

C++ already had `DICOMStructure::visit()` (pre-A1.7); no change was needed there. Python gained
`Structure.iter_elements(recursive: bool = True)`, yielding `(element, path)` in deterministic
depth-first, declaration order, composed *entirely* from existing indexed accessors
(`fds_structure_element_at`/`fds_element_is_sequence`/`fds_element_sequence_item_element_at`) —
no new ABI function was added or needed for this, per the checkpoint's own recommendation. Tested
for: agreement with plain top-level iteration when `recursive=False`, reaching nested elements
with paths directly reusable by `find()`, and deterministic document order (`test_nested_mutation.py`,
`IterElementsTest`).

## 15. Auto-padding audit

Audited where padding already occurred: `charset::encode_text()` (A1.6) already pads its seven
text VRs with SPACE before returning encoded bytes — a pre-existing, correct behavior, unchanged.
The raw structural primitives (`DICOMStructure::insert`/`set`/`set_value`) never auto-pad and still
don't — that contract (a caller must pre-pad an odd-length raw value) predates A1.7 and is
unchanged, since these primitives may not always have a stable, safe VR to pad by. The gap found:
`fds::mutation::insert()`/`insert_inferred()` — the raw *convenience* layer — did not auto-pad,
inconsistent with its charset sibling despite always knowing the final VR by the time padding would
matter. Fixed: both now auto-pad an odd-length value with PS3.5 6.4's correct trailing byte (SPACE
for text-natured VRs, NUL for `VR::UI` and every binary VR) before the length-ceiling check, so a
caller using the ergonomic insertion API never has to manually pad where the library already knows
enough to do it safely — while the low-level primitive's contract is untouched. Tested in both C++
(`test_mutation_inferred.cpp`, SPACE- and NUL-pad cases, plus a still-too-long-after-padding case)
and Python (`test_insert_auto_pads_an_odd_length_value`).

## 16. Stale-reference safety

Structural insertion/removal can reallocate or shift the backing `std::vector<Element>` at any
level of the tree, exactly like ordinary `std::vector` invalidation. C++ and the C ABI offer no
runtime protection by design (a stale `Element*`/`fds_element_t*` is a plain dangling pointer,
undefined behavior to dereference — unsafe to "test" directly) — this is now explicitly documented
at both layers: `dicom_structure.hpp`'s class-level comment (new), and `docs/abi-design.md`'s
"Pointer validity" section (extended to the seven new `*_path` functions, and to the "a
no-op/rejected call invalidates nothing" clarification). Python is the one layer with a real
runtime check — a generation counter stamped on every `Element`/`Item` at creation, checked before
use, raising `StaleElementError` deterministically. This is comprehensively tested
(`test_nested_mutation.py`, `StaleReferenceTest`): `insert()`, `insert_text()`, and `set_text()`
each invalidate a previously-obtained handle; a *failed* (exception-raising) `insert()` call does
**not** bump the generation counter, so an unrelated live handle survives it untouched; a
write-after-nested-insert round-trips correctly. No regression to the pre-A1.7 generation-counter
contract for the already-existing `set_value`/`erase`/`set`/`erase_private` paths.

## 17. Private-creator qualification

Beyond the checkpoint's non-interference test, `test_mutation_inferred.cpp` proves the full
insertion flow: an inferred-LO Private Creator declaration plus an explicit-VR private data
element, then `resolve_private_creator()`, at root, nested inside one Item, and — the decisive
case — across two sibling Items each declaring a *different* creator at the *same* block number
(0x10), proving A1.2's non-cascading, container-scoped resolution rule (deliberately distinct from
A1.5/A1.6's cascading charset inheritance) is completely unaffected by insertion: each sibling
resolves to its own creator, never the other's. A dedicated test also confirms private DATA VR is
never inferred from its creator's identity — always `VRRequired`, by policy, with a resolvable
creator present or not.

## 18. Implicit-VR-origin qualification

`tests/integration/test_implicit_vr_qualification.cpp` builds a genuine Implicit VR Little Endian
fixture (dictionary-resolved `VRProvenance::Dictionary` throughout, confirmed on both the Sequence
element and its nested scalar) and exercises the complete surface against it: nested lookup, nested
`set_value`, nested insertion with an inferred unambiguous standard VR, nested insertion with an
explicit (private) VR, nested Unicode text insertion (proving charset inheritance works
identically for an Implicit-VR-origin structure), nested erase, and a full combined
write/reparse round-trip verifying every edit survived. No Implicit VR *writer* was added — the
writer re-emits whatever transfer syntax the structure already carries, exactly as A1.4 already
established; this qualification confirms A1.7's mutation surface is transfer-syntax-origin-agnostic,
nothing more.

## 19. C++ test suite summary

312 test cases, 1,519 assertions, all passing — 68 new test cases since the 244-test A1.6 baseline
(21 checkpoint tests, minus 2 removed `upsert()` tests, plus 1 malformed-parent test, plus
`test_mutation_inferred.cpp` [18], `test_charset_insert.cpp` [11], `test_implicit_vr_qualification.cpp`
[7], and incremental additions across the increment). Clean rebuild from the committed state, zero
compiler warnings, `-Wall -Wextra` throughout.

## 20. Python API (final)

`find(path)`, `get(tag)` (unchanged root convenience), `set_value(path, bytes)`, `erase(path)`
(both now path-capable, a bare `(group, element)` tuple still accepted as the single-step root
case — every pre-A1.7 call site keeps working unchanged), `insert(tag, value, vr=None, parent=None)`,
`insert_text(tag, values, vr=None, parent=None)`, `set_text(path, values)`, `decode_text(path)`,
`iter_elements(recursive=True)`. Path representation: a bare tag tuple for root, or a `list` of
`(tag, item_index_or_None)` steps for nested — the same distinction between element-locator
(bare-last-step) and container-locator (`parent=`, no exception for the last step) the C++ layer
makes, enforced Python-side (`_normalize_parent` raises `TypeError` for a `None`-item-index parent
step). `vr=None` on `insert`/`insert_text` means "infer"; the `str`-vs-`bytes` argument to
`insert_text` vs `insert` is the only signal used to choose semantics — never type-guessed from
content.

## 21. Python error model

Extends the established `FdsError`/`StaleElementError` convention. Four new subclasses
(`VRRequiredError`, `AlreadyExistsError`, `UnrepresentableCharacterError`,
`InvalidUnicodeInputError`) map to the handful of `fds_status_t` values a well-written caller
plausibly branches on; every other new failure (bad container path, invalid VR) stays a plain
`FdsError`. A deliberate, documented convention split: `set_value`/`erase`/`set`/`erase_private`/
`erase_recursive`/`set_value_recursive` keep their pre-A1.7 bool-return convention (`False` =
not-found/no-op, not an error) unchanged; `insert`/`insert_text`/`set_text` raise instead, because
"insert failed" has enough distinguishable reasons that collapsing them into one bit would be a
materially worse interface than the single-reason case `set_value`/`erase` already handle well.
`InvalidUnicodeInputError` is defined for symmetry with the C++/ABI layers and is exercised there,
but is not independently unit-tested at the Python layer — a Python `str` passed through
`.encode("utf-8")` cannot itself produce malformed UTF-8 bytes, so this specific status is not
reachable via the `str`-based Python surface as built; noted here rather than papered over with an
artificial test.

## 22. Python test suite summary

65 tests (39 baseline + 25 new in `test_nested_mutation.py`, +1 padding test), all passing. Covers
find/set_value/erase at nesting depth, insert (root/nested, explicit/inferred VR, every documented
failure mode), insert_text/set_text/decode_text (round-trip, multi-valued, unrepresentable-character
rejection, nested local-declaration-not-root proof mirroring the C++ freeze-critical test),
iter_elements (both modes, path reusability, deterministic order), the full stale-reference suite
described in §16, and a write/reparse round-trip through a nested insert.

## 23. C ABI final surface

`fds_path_step_t{tag, has_item_index, item_index}` plus seven new functions:
`fds_structure_find_path`, `_set_value_path`, `_erase_path`, `_insert_path`, `_decode_text_path`,
`_set_text_path`, `_insert_text_path` (41 `FDS_API` functions total, up from 34). Four new
`fds_status_t` values (`FDS_STATUS_ALREADY_EXISTS`, `_VR_REQUIRED`, `_UNREPRESENTABLE_CHARACTER`,
`_INVALID_UNICODE_INPUT`) were added — not a mechanical mirror of every C++ status, only the
handful needed for the Python exception model in §21. `FDS_VR_UNKNOWN` doubles as an "infer the
VR" sentinel in `fds_structure_insert_path`/`_insert_text_path` only (documented explicitly as
scoped to those two functions; its ordinary meaning elsewhere is unchanged), rather than doubling
the function count — a deliberate C-idiomatic choice distinct from C++'s two-named-function
approach. No ABI upsert, no standalone charset-context/encode-text ABI functions, no recursive
callback (Python composes traversal itself, §14) — all as approved. ABI version bumped `0.5.0` →
`0.6.0` (additive per `docs/abi-design.md`'s own rule).

## 24. C ABI test summary

`tests/integration/test_c_abi_path.cpp`, 9 test cases / 44 assertions: find/set_value/erase at
depth, explicit- and inferred-VR insertion (root and nested), `FDS_STATUS_ALREADY_EXISTS`,
`FDS_STATUS_VR_REQUIRED`, SQ/Unknown rejection, decode/set-text round-trip, insert_text
(explicit and inferred, root and nested), and `FDS_STATUS_UNREPRESENTABLE_CHARACTER`/
`FDS_STATUS_INVALID_UNICODE_INPUT` reported distinctly. Included in the 312/1,519 total in §19.

## 25. Real end-to-end qualification

Ran against a genuine, unmodified real-world file with real nested Sequence content — pydicom's
bundled `rtplan.dcm` (Implicit VR Little Endian, 36 top-level elements, `BeamSequence`/
`DoseReferenceSequence`/`FractionGroupSequence`/etc., no pre-existing Specific Character Set).
Sequence performed: recursive iteration (132 total elements, 42 top-level), find a nested text
element (`BeamSequence[0]`→`BeamName`), decode it, insert a root `(0008,0005)` = `ISO_IR 100`
declaration via raw `insert()`, replace `BeamName` with genuinely non-ASCII text ("Field 1 Révisé")
via `set_text`, insert a nested standard tag with an *inferred* VR (`ReferringPhysicianName` → PN),
insert a nested Private Creator (inferred LO) plus private data (explicit LO), erase an unrelated
nested tag (`NumberOfBoli`), write, and reparse. **Three independent implementations agree
completely on the result**: fastdicomattrs's own reparse; pydicom (`BeamName` == `'Field 1 Révisé'`,
`ReferringPhysicianName` == `'Dr. Smith'`, `(0043,0010)`/`(0043,1001)` both present with correct
values, `NumberOfBoli` absent); and DCMTK's `dcmdump` (exit code 0 — structurally valid — with
every edited/inserted/removed tag visible at the correct byte content; the private tag correctly
reported as `Unknown Tag & Data` by DCMTK's own dictionary, exactly the expected behavior for a
tag it doesn't register). No Pixel Data in this object type (RT Plan); its absence is unaffected,
trivially confirmed. Top-level element count moved 36→37 (exactly the one new root
`SpecificCharacterSet`), confirming every other edit stayed correctly nested and didn't leak into
the top level.

## 26. Corpus regression

Re-ran the established corpus-regression methodology (`python/fastdicomattrs/corpus.py`) against
both established real-world corpora to confirm A1.7's new opt-in mutation code did not affect
ordinary read/write behavior for untouched datasets.

**NLST** (2,832 discovered: 2,831 `.dcm` + 1 `LICENSE`): parsed clean 2,831/2,831 (0% failure,
matching baseline); Lossless-eligible and byte-identical round-trip 2,831/2,831 (100%); pydicom
cross-check found 0 mismatches on a 50-file sample (element count, Modality/PatientName/
SpecificCharacterSet); private-creator sample 50/50 files had private elements, creator `CTP`
(TCIA's anonymizer, as expected); Pixel Data 2,831/2,831 native, 0 encapsulated, matching Explicit
VR LE throughout, 0 mismatches vs. pydicom.

**CMB-MEL** (23,807 discovered — one more than the documented 23,806 baseline, attributable to a
`metadata.csv` file newly present in the corpus directory since the last freeze report, not to any
code change; every downstream count shifts consistently with exactly that one extra non-DICOM
input): parsed clean 23,803/23,803 real DICOM files (0% failure); 4 warnings = the same 2
genuinely truncated CT slices already documented in `docs/corpus-results.md`, plus `LICENSE` and
the new `metadata.csv`, both correctly flagged non-DICOM rather than hard failures;
Lossless-eligible and byte-identical round-trip 23,803/23,803 (100%), matching baseline exactly;
pydicom cross-check 0 mismatches on a 50-file sample; private-creator sample 50/50 files, 21
distinct real-world creator IDs (`CTP`, `SIEMENS CSA HEADER`, `SIEMENS MEDCOM HEADER`,
`GEMS_HELIOS_01`, `GEIIS`, `FujiFILM TM`, etc.); Pixel Data 23,803/23,803 native, 0 encapsulated, 0
mismatches vs. pydicom.

**No regressions found.** Every number matches the documented pre-A1.7 baseline exactly, modulo
the one explained extra file in CMB-MEL's directory. A1.7's new code is confirmed to be
purely additive to the read/write path — it changed nothing about how any existing, untouched file
is parsed or written.

## 27. Performance

Representative C++ timings (scratch benchmark, 2,000-Item nested Sequence, 20 elements/Item,
release build; not a formal benchmark-suite commitment — see §33 Path A): root `find(Tag)`
~137,000/ms; deep `find(ElementPath)` (into Item 1999) ~27,000/ms; root `insert()` ~390/ms
(inherent `O(n)` vector-based ascending-order insertion — unchanged design, not new to A1.7);
deep `insert()` into a small per-Item vector ~890/ms; `visit()` full recursive traversal of ~46,000
elements ~23,000 elements/ms; `set_text()` (root, existing element) ~1,900/ms; `insert_text()`
(deep) ~1,080/ms. Deep `erase()` measured slower (~66/ms) than deep `insert()` only because, by
benchmark construction, it ran against an Item that the preceding insert step had already grown to
~2,020 elements — `O(n)` linear-scan-plus-shift against that larger vector, not a new
pathological path. Python-layer timings (root `get()` ~380/ms, deep `find()` ~57/ms, root
`insert()` ~92/ms, deep `insert()` ~61/ms, full `iter_elements(recursive=True)` over ~40,000
elements ~330 ms total) are dominated by ctypes marshaling overhead (2–3 orders of magnitude
versus the underlying C++ call), consistent with this binding's stated "thin ctypes wrapper, not
a performance-optimized binding" design, unchanged by A1.7. Conclusion: the ergonomics layer adds
no new algorithmic complexity class anywhere; every "deep" operation costs what the analogous
"root" operation already cost, plus the (cheap, `O(depth)`) container-locator walk.

## 28. API-surface audit

Full inventory pass across `DICOMStructure`/`fds::mutation`/`fds::charset`/the C ABI/Python before
freeze, specifically checking for: **upsert accidentally remaining** — none (`grep -rn upsert`
across `include/ src/ abi/ python/` returns only doc-comment mentions of its removal). **Duplicate
root/nested methods** — none found beyond the deliberate, documented `get()`/`find()` pair (root
convenience vs. general, both kept — `get` predates A1.7 and removing it would break existing
callers). **Raw/text ambiguity** — none; `bytes` vs. `str`/`Sequence[str]` is the only signal, never
content-sniffed. **Old top-level-only helpers now redundant** — `charset::set_new_top_level_text`
removed (§29); `DICOMStructure::set()` and the ABI's `fds_structure_set` kept deliberately (frozen,
pre-A1.7 compatibility surface, not touched per instruction). **C++ features not reachable from
Python** — none among A1.7's new surface; `fds::mutation::insert`/`insert_inferred` and
`fds::charset::insert_text`/`insert_text_inferred` are all reachable through the ABI's
`FDS_VR_UNKNOWN`-sentinel convention. **Python-only semantic inventions** — `iter_elements()`'s
name and signature are Python-only (C++ has `visit()`; the ABI has neither, by design, §14) —
deliberate, documented, not an accidental divergence. **C ABI overgrowth** — 7 new functions,
matching the approved plan exactly. Two pre-existing (not A1.7-introduced) gaps were noted, not
fixed, as out of the approved scope: `resolve_private_creator` (A1.2) and predicate-based
`erase_if` (A1.1-era) have no ABI/Python exposure — both are Path A candidates (§34), not new A1.7
debt.

## 29. Removal of `charset::set_new_top_level_text`

Evaluated against the new general `insert_text(parent=<empty>, ...)`, which covers the identical
top-level-only case exactly. It was never exposed through the ABI or Python (confirmed by
inspection before this increment's checkpoint), so no external compatibility surface depended on
it; it was itself only a few commits old (introduced within A1.6, in this same working session).
Removed, and its two A1.6-era tests ported to call `insert_text(ElementPath(), ...)` instead
(`test_charset_encode.cpp`) rather than simply deleted, preserving the coverage. No two long-term
APIs now exist for the same capability.

## 30. Documentation delivered

`python/examples/nested_mutation.py` (new): a runnable, verified walkthrough covering every item
the authorization listed — recursive/root/nested read, raw replacement, Unicode replacement
(insertion then replacement of the same element), nested explicit-VR insertion, nested inferred-VR
insertion, nested text insertion, private creator/data insertion, erase, write/reparse.
`docs/abi-design.md`: pointer-validity rule extended to the seven new functions, surface listing
updated, "deliberately absent" section rewritten to reflect what A1.7 actually closed versus what
remains deferred, version-bump note added. `docs/architecture.md` section 9: a new addendum bullet
describing the `fds::mutation` layering decision without rewriting the pre-A1.7 rules it extends.
`dicom_structure.hpp`: new pointer-invalidation doc comment (§16); `insert()`'s full contract
documented in place. `mutation.hpp`/`charset.hpp`: every new function and enum extensively
doc-commented in place, including explicit cross-references to this report's sections.

## 31. Public support envelope

**SUPPORTED**: Part 10 file format; bare dataset input; Explicit VR Little Endian; Implicit VR
Little Endian input; recursive Sequence/Item semantics; the standard PS3.6 dictionary; Private
Creator/block identity; raw mutation (root and nested); Unicode mutation within the V1 charset
envelope (root and nested, insertion and replacement); nested insertion and removal at any depth;
Pixel Data by reference (never decoded, never disturbed by unrelated mutation); serialization
(Lossless for unmodified input, Lossless or Standard for modified input).

**NOT CLAIMED**: Explicit VR Big Endian; Deflated Explicit VR Little Endian; multi-byte
Japanese/Korean/Chinese charset repertoires (ISO 2022 IR 87/159/149/58, GB18030, GBK — recognized
as declared terms, never decoded or encoded); vendor-private dictionaries (private tag *meaning*,
as opposed to private tag *identity*, which A1.2 does support); Pixel Data decoding; dataset-wide
charset transcoding; streaming input; a policy/de-identification engine (explicitly
`fastDICOMstructure`'s domain, §36); item-level Sequence-Item deletion (whole-Sequence and
whole-subtree removal only); keyword-string addressing (numeric tags only in V1).

## 32. Any defect discovered

None in any previously-frozen increment (A1.1–A1.6). One design flaw was found and fixed *within*
this same increment, before being proposed for freeze: naively reusing
`resolve_character_set_context()` (built for an element-locator path) against a container-locator
path would have silently skipped a target container's own local charset declaration (§10) — caught
by design review before any code shipped, fixed by extracting the shared walk, and the fix was
independently verified by differential testing against the naive version (§10). No structural
defect was found in the reviewed checkpoint primitive itself (§3).

## 33. Attrs Contract change

None required. `docs/roundtrip-contract.md`'s existing framing (modified-structure write support,
Lossless/Standard fidelity rules) already covers nested insertion/removal without amendment — A1.7
adds new ways to reach a modified state, not a new write contract.

## 34. Remaining limitations before Post-A1 work

- `resolve_private_creator` and predicate-based `erase_if` have no ABI/Python exposure (pre-existing
  gaps, not introduced by A1.7 — §28).
- Keyword-string addressing (`"PatientID"` instead of `(0x0010,0x0020)`) remains deliberately
  deferred, per the authorization's explicit instruction — no reverse dictionary lookup exists at
  any layer.
- No formal, CI-integrated benchmark suite exists for the new mutation surface — §27's numbers are
  a one-off scratch measurement, not committed benchmark infrastructure.
- `fds::mutation::insert`/`insert_text` auto-padding (§15) is new, ergonomics-layer-only behavior;
  a caller who mixed raw `DICOMStructure::insert()` calls with the convenience layer in the same
  codebase must remember which tier they're calling — documented, not eliminated.

## 35. Post-A1 Decision Analysis — Path A: tighten fastDICOMattrs

Analysis only, per explicit instruction; nothing below was implemented in A1.7.

**Must before V1 (if "V1" means public-supportable)**: (1) close the `resolve_private_creator`/
`erase_if` ABI-Python gap (§28/34) or explicitly document them as C++-only forever; (2) a security
review of the parser against malformed/adversarial input (fuzzing) — this codebase has adversarial
*structural* tests (A1.4's matrix) but no fuzz-harness coverage; (3) package/release engineering —
there is no versioned release process, no packaging (wheel/conda/vcpkg), no documented
installation path beyond "build from source."

**Should soon**: (4) a formal benchmark suite (§34) integrated into CI, replacing scratch
measurements with tracked regression detection; (5) API simplification pass now that V1's full
surface exists — some of the audit's "kept for compatibility" items (`DICOMStructure::set()`'s
VR-discarding upsert behavior in particular) are candidates for a documented, opt-in v2 behavior
change once real external callers exist to consult; (6) expand the keyword-lookup question from
"deferred" to "decided" — either commit to numeric-only addressing permanently (documented as a
design principle) or scope a reverse-dictionary lookup properly.

**Nice to have**: (7) richer C ABI diagnostics matching the C++ status enums more closely, if a
real binding consumer other than this repo's own Python wrapper ever needs them; (8) a
`fds_structure_visit_path`-style callback if a non-Python FFI consumer (Rust/C#/Java, per
`docs/abi-design.md`'s own stated audience) can't compose recursion as cheaply as Python does.

**Out of scope for Path A**: any new DICOM semantics (charset repertoires, transfer syntaxes,
Pixel Data decoding) — those are new *capability* increments, not hardening, and belong to a
different kind of decision than "harden what exists."

## 36. Post-A1 Decision Analysis — Path B: strategize fastDICOMstructure

Analysis only; nothing below was implemented in A1.7. `fastDICOMstructure` is documented
(`README.md`'s family diagram, `docs/architecture.md` §9a) as the separate, higher-layer project
responsible for policy/orchestration — attrs "owns no policy content and depends on nothing else in
this family." With A1.7 complete, attrs now owns a *complete* V1 semantic mutation surface
(read, find, replace, insert, remove, at any depth, raw or Unicode, dictionary- or explicitly-VR'd,
charset-aware) for the first time — this is the precondition Path B analysis needs, not a reason to
begin implementing it.

**Policy model**: fastDICOMstructure's job is to decide *what* to change (match a condition,
choose an action) using attrs' primitives to *apply* the change. A policy engine needs, at minimum:
a matcher (by tag, by nested path shape, by VR, by private-creator identity — all now queryable via
attrs' V1 surface), a small action vocabulary (Require/Remove/Replace/AllowListPrune/
PrivateTagPolicy, per the pre-A0 sketch already in `docs/architecture.md` §9a), and a way to express
"apply this policy across every matching element, however deeply nested" — which is exactly what
attrs' `erase_if`/`set_value_recursive`/new `iter_elements(recursive=True)` already give it, without
fastDICOMstructure needing its own traversal code.

**JSON configuration**: a policy document needs to name tags/paths declaratively; attrs' path
representation (list-of-(tag,item_index) steps) is a reasonable serialization target as-is, or a
JSON-friendlier tag-path DSL could be layered on top *inside* fastDICOMstructure without attrs
needing to know about it.

**Match/condition/action**: conditions should be expressible over anything attrs' V1 surface can
already answer — tag, VR, nesting depth/shape, private-creator identity/resolution status, charset
mode — without fastDICOMstructure needing new attrs capability to get started.

**Input/output adapters**: attrs already gives file/buffer read and file/buffer write; a streaming
adapter is explicitly out of attrs' V1 scope (§31) and would need to be fastDICOMstructure's own
concern if ever needed, or a later, separate attrs increment.

**De-identification/anonymization**: this is the paradigm case Path B exists for — a policy that
walks every element (attrs' recursive primitives), matches on tag/private-creator identity (attrs'
V1 query surface), and replaces/removes/inserts-a-replacement value (attrs' V1 mutation surface,
including the new nested and charset-aware insertion this increment adds) — attrs now has every
primitive de-identification needs; fastDICOMstructure's job is the policy language and execution
model on top, not new attrs semantics.

**Local/stream/container/serverless execution; audit/error model; bulk-data preservation**: all
orchestration/deployment concerns genuinely outside attrs' stated scope (`README.md` "Explicitly
out of scope") — Path B's job, not Path A's, and not attrs'.

**What existing structure code should remain/simplify/delete/rewrite against attrs**: unable to
assess concretely without reading fastDICOMstructure's current source, which is out of scope for
this attrs-only report to inspect or judge — this question should be the *first* task of an
actual Path B planning session, not answered speculatively here.

## 37. Final validation

Clean rebuild from the committed state (`cmake --build build -j$(nproc)`): zero warnings. Full
C++ suite: 312 test cases, 1,519 assertions, all passing. Full Python suite: 65 tests, all passing.
Deterministic generated-table verification: unaffected by A1.7 (no changes to
`tools/generate_dictionary.py`/`generate_charset_tables.py` or their generated output). NLST/CMB-MEL
regression: §26. pydicom/DCMTK qualification: §25 (real end-to-end) and §26 (corpus sample
cross-check). `git status`: clean tree after this report's commit.

## 38. Freeze recommendation

**PASS.** Every criterion from the original authorization and the accepted checkpoint is met with
direct evidence, not assertion: `upsert()` is fully removed (§5, confirmed by full-tree grep, §28);
the parent-path/tag insertion contract is applied consistently in C++, Python, and the ABI (§4,
§20, §23); container-location and VR-inference logic are each centralized once (§7, §8); the
freeze-critical parent-scope charset claim is proven, including a differential test against the
naive/incorrect implementation to confirm the tests actually catch the bug (§10); stale references
are explicitly qualified at every layer, with the two layers that can offer a runtime guarantee
(Python) tested to do so and the two that structurally cannot (C++, ABI) documented instead of
silently assumed safe (§16); private-creator insertion is proven non-cascading across sibling Items
sharing a block number (§17); Python and ABI path operations are both qualified with dedicated,
passing test suites (§22, §24); a real end-to-end scenario on genuine nested-Sequence content
agrees across three independent DICOM implementations (§25).

```text
A1.1: FROZEN at 6f05d9d
A1.2: FROZEN at e651c74
A1.3: FROZEN at 140d850
A1.4: FROZEN at 87e79fe
A1.5: FROZEN at 656edf4
A1.6: FROZEN at 50369c0
A1.7: PASS -- freeze commit recorded once this report is committed

A1 capability progression: COMPLETE
```
