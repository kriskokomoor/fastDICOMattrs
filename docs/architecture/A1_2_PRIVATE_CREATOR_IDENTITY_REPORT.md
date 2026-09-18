# A1.2 — Private Creator / Block Identity: Freeze Report

## 1. Implementation status: PASS

`fastDICOMattrs` can now answer, structurally and without guessing, "which registered private
creator owns this private data element?" — via a new free function,
`fds::resolve_private_creator(const DICOMStructure&, const ElementPath&)`, additive to
`dicom_structure.hpp`/`.cpp` only. Nothing in the parser, writer, mutation primitives, or the A1.1
dictionary was touched. This is identity, not interpretation: the function never inspects or
claims to understand what a resolved creator's private data means.

## 2. Standard rule revalidated before implementation

**PS3.5 section 7.8.1** ("Private Data Element Tags"), confirmed against the standard text rather
than implemented from the plan's shorthand:

- Private (odd-group) elements with element number in `[0x0010, 0x00FF]` are **Private Creator
  Data Elements** — an identification value (conventionally `LO`) that reserves one block for the
  registering creator. The element number's own value (16–255, written `cc`) *is* the block
  number.
- Private data elements belonging to block `cc` sit at element numbers `[cc00, ccFF]` — i.e. the
  16-bit range `[0x1000, 0xFFFF]` once every valid `cc` (`0x10`–`0xFF`) is enumerated. The **high**
  byte of the data element's number equals the creator element's own (low-byte) number.
- `(gggg,0000)` is the group-length element, unrelated to the creator scheme (present regardless of
  group parity historically, still round-tripped structurally today via `Tag::is_group_length()`).
- Element numbers `[0x0001, 0x000F]` and `[0x0100, 0x0FFF]` are outside the `[0x10, 0xFF]` block
  range the standard defines for creators. These addresses correspond to no valid block number
  (`element >> 8` would be `< 0x10`). They are legacy/reserved, not a case the creator scheme
  covers, and the standard gives no rule for inferring an owner for them.
- Creator registration is scoped to the dataset it appears in. The standard defines this
  implicitly by never distinguishing "the encoding" from "the dataset containing that element" —
  a Sequence Item is its own dataset (PS3.5 7.5), so a creator declared in one Item has no bearing
  on a different Item, a sibling, or the parent.
- The standard does not forbid a zero-length (blank) creator value; nothing here treats "found but
  empty" as an error.
- No discrepancy was found between the plan's shorthand and the actual standard text — no
  discrepancy report was needed.

**Toolkit cross-check (pydicom):** pydicom's own private-creator resolution (manually replicated:
`ds.get(Tag(group, tag.element >> 8))`) was checked against a real corpus file's private elements
(§9) and agreed exactly. No ambiguity was discovered between the standard, pydicom's behavior, and
this implementation.

## 3. API chosen

```cpp
enum class PrivateElementKind { NotPrivate, GroupLength, Creator, Reserved, Data };
enum class PrivateCreatorStatus { NotApplicable, Resolved, NoCreator, Malformed };

struct PrivateCreatorResolution {
  PrivateElementKind kind = PrivateElementKind::NotPrivate;
  PrivateCreatorStatus status = PrivateCreatorStatus::NotApplicable;
  std::optional<std::uint8_t> block;      // set for Creator and Data
  std::optional<std::string> creator;     // set only when status == Resolved
};

PrivateCreatorResolution resolve_private_creator(const DICOMStructure& structure,
                                                  const ElementPath& path);
```

A free function, per the plan's preferred shape — not a property of `Element` (which lacks the
sibling/container context needed to find the creator) and not eagerly computed. It is implemented
using only the *public* `DICOMStructure::elements()` / `Sequence::items()` / `Item::elements()`
accessors, so it needed no access to `DICOMStructure`'s private internals and no friend
declaration — a genuinely additive surface, not a privileged one.

The five-way `PrivateElementKind` split (rather than collapsing "not private," "group length,"
"this is a creator," and "reserved/no valid block" into one bucket) exists specifically so an
empty creator string is never confused with "not applicable," and so a caller can tell "this tag
declares a block" apart from "this tag is data in a block" without inspecting the raw tag itself.

## 4. Exact rule implemented

For an element at `(gggg, eeee)`:

| `eeee` | Kind | Rule |
|---|---|---|
| even `gggg` | `NotPrivate` | not a private element at all |
| `0x0000` | `GroupLength` | outside the creator scheme |
| `[0x0010, 0x00FF]` | `Creator` | this element *is* the creator declaration for block `eeee & 0xFF` |
| `[0x0001, 0x000F]` ∪ `[0x0100, 0x0FFF]` | `Reserved` | no valid block number is derivable (`eeee >> 8 < 0x10`); never guessed |
| `[0x1000, 0xFFFF]` | `Data` | belongs to block `eeee >> 8`; look up `(gggg, eeee >> 8)` in the *same container* |

For `Data`, the creator lookup scans only the sibling list that directly contains the element
(top-level `elements()`, or the one specific `Item::elements()` the path descended into) — never a
parent, a sibling Item, or an unrelated nested dataset.

## 5. Nested dataset scope

Implemented by walking `ElementPath`'s existing `{Tag, item_index}` step list from the root,
exactly as the A1 plan's architectural note anticipated (no parent back-pointer needed or added).
The walk returns both the target element and the `std::vector<Element>&` that is its *direct*
container; creator lookup then searches only that container. This is the same
top-down/no-back-pointer pattern `find_mutable`/`locate()` already use for mutation, applied
read-only. Four dedicated tests (`test_private_creator.cpp`) prove root-vs-Item, Item-0-vs-Item-1,
and outer-Item-vs-nested-child-Item isolation — a creator declared in one scope is confirmed
never visible in a structurally different one.

## 6. Malformed / missing-creator behavior

- **No creator element present:** `status = NoCreator`, `creator` unset. Never guessed.
- **Creator element present but declared as a Sequence** (structurally impossible for a valid `LO`
  creator identifier): `status = Malformed`, `creator` unset, no crash.
- **Creator element's source-backed bytes unreadable** (e.g. a truncated `Source`): the
  `ValueTypeError` `Value::bytes()` would throw is caught; `status = Malformed`, never a crash,
  never a fabricated string.
- **Legitimate blank creator** (a zero-length `LO` value genuinely present): `status = Resolved`,
  `creator = ""`  — deliberately distinct from `NoCreator`, per the plan's instruction not to
  collapse "found but blank" into "not found."
- **Querying the creator element itself:** `kind = Creator`, `status = NotApplicable`, `block` set
  to the block it declares, `creator` left unset. Documented, intentional: "who is this creator's
  creator" is not a question PS3.5 defines an answer to, and this function never fabricates one.
- **Path that does not resolve to any element:** returns the same shape as `NotPrivate` — there is
  no element to classify. Documented as the deliberate default rather than a distinct error case.

## 7. Corpus characterization (descriptive, not an acceptance threshold)

Ad hoc characterization tool (not part of the library; scratch-built for this report), run over
two real, license-cleared TCIA corpora already present on this machine:

| | NLST (2,831 files) | CMB-MEL (23,803 files) |
|---|---:|---:|
| Files with ≥1 private element | 2,831 | 23,803 |
| Private Creator elements seen | 2,831 | 83,972 |
| Reserved-range elements seen | 0 | 0 |
| Private data elements | 11,324 | 366,552 |
| Resolved | 11,324 (100%) | 366,552 (100%) |
| No creator | 0 | 0 |
| Malformed | 0 | 0 |
| Distinct creator strings | 1 | 18 |

Distinct creators recovered from CMB-MEL: `CTP`, `A.L.I. Technologies, Inc.`, `GEHC_CT_ADVAPP_001`,
`GEIIS`, `GEIIS PACS`, `GEMS_ACQU_01`, `GEMS_HELIOS_01`, `GEMS_IDEN_01`, `GEMS_IMAG_01`,
`GEMS_PARM_01`, `GEMS_PETD_01`, `GEMS_RELA_01`, `GEMS_STDY_01`, `MMCPrivate`,
`SIEMENS CT VA0  COAD`, `SIEMENS MED`, `SIEMENS MED PT`, `TOSHIBA_MEC_CT3` — all real,
recognizable manufacturer/tool identifiers (GE, Siemens, Toshiba, the RSNA Clinical Trial
Processor, A.L.I. Technologies), consistent with genuine scanner/anonymization-tool output, not
noise. **These are characterization numbers, not a pass/fail bar** — a corpus with zero
`NoCreator`/`Malformed` results says this corpus happens to be well-formed, not that every
possible input would resolve; the dedicated fixtures in §6/`test_private_creator.cpp` are what
prove the unresolved/malformed paths behave correctly.

**Spot check:** one CMB-MEL file's two private data elements (`(0013,1010)`, `(0013,1013)`, block
`0x10`) were independently resolved with pydicom (`ds.get(Tag(group, tag.element >> 8))`) and this
library's `resolve_private_creator` side by side — both returned creator `CTP` for both elements,
exact agreement.

## 8. Tests

Before this increment: 125 C++ tests (`ctest`), 37 Python tests (`pytest`). After: **139 C++
tests** (14 new, in `tests/integration/test_private_creator.cpp`, all Explicit-VR-LE fixtures
built with the existing `FixtureBuilder` and parsed through the real `fds::parse_buffer` path —
not hand-constructed `DICOMStructure` objects), **37 Python tests unchanged** (A1.2 added no
Python/ABI surface, per the plan's stated scope). All 139 + 37 pass. New coverage: basic
resolution; multiple creators/multiple blocks in one group; missing creator; legitimate blank
creator; malformed (Sequence-typed) creator; nested-Item resolution; three distinct scope-isolation
cases (root-vs-Item, Item0-vs-Item1, outer-Item-vs-nested-child); ordinary public tag; nonexistent
path; the creator element queried directly; group length; and the `Reserved` range on both its
sub-ranges.

## 9. Standard/toolkit ambiguity discovered

None. The standard text, pydicom's behavior, and the real corpus data were all mutually
consistent; no triage decision was required.

## 10. Scope discipline confirmed

- `git diff --stat`: `dicom_structure.hpp` (+61), `dicom_structure.cpp` (+124),
  `tests/CMakeLists.txt` (+1), plus the new test file — nothing else in the tree changed.
- No file under A1.1's frozen surface (`include/fastdicomattrs/dictionary.hpp`, `src/dictionary*`,
  `thirdparty/dicom_standard/**`, `tools/generate_dictionary.py`) was touched or read from by the
  new code. `fds::dictionary::lookup` is not called anywhere in this increment.
- No parser or writer file was touched. Parsing and writing behavior is unchanged (confirmed by
  the full pre-existing suite passing unchanged, and by the corpus round-trip/characterization run
  producing the same private-tag-presence counts `docs/corpus-results.md` already recorded).
- No vendor-private dictionary, VR inference, or semantic interpretation of private data was
  added — `resolve_private_creator` never inspects a data element's *value*, only tag arithmetic
  and the creator element's identification string.

## 11. Performance

`resolve_private_creator` is caller-invoked only; nothing on the parse or write hot path changed.
No caching was added (not evidenced as necessary — the corpus run above resolved 366,552 elements
across 23,803 files, each requiring at most one short linear scan of its own container, in about
20 seconds wall time dominated by parsing itself, not resolution).

## 12. Report path and freeze recommendation

Report: `docs/architecture/A1_2_PRIVATE_CREATOR_IDENTITY_REPORT.md` (this file).

**Recommend: freeze A1.2.** All acceptance criteria in the authorizing brief are met: correct
creator/block arithmetic per PS3.5 7.8.1; correct dataset-relative scope with proven isolation;
missing creator explicitly unresolved; malformed input produces no fabricated creator and no
crash; public vs. private elements are distinguishable; no vendor-private semantics were added; no
parsing/writing/mutation behavior changed; all 125 pre-existing C++ tests plus 37 pre-existing
Python tests still pass; all 14 new tests pass; corpus regression is clean.
