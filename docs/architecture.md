# fastDICOMattrs — Architecture

> **Historical note (added at the A0 semantic-engine extraction):** this document was written
> while this engine still lived inside a repository called `fastDICOMstructure`, alongside a
> policy-evaluation layer. It has been carried into the newly-promoted `fastDICOMattrs` repository
> with its identifiers mechanically updated, but sections §1 and §9a below have been rewritten (not
> just find-and-replaced) to describe the post-extraction reality accurately — see
> `docs/architecture/ADR-001-ATTRS-NAMING-AND-LAYERING.md` and
> `docs/architecture/A0_SEMANTIC_ENGINE_EXTRACTION_REPORT.md` for the decision and evidence. §2–§8,
> §10, and §11 describe the parser/object-model/writer engine itself, which moved essentially
> unchanged, and remain accurate as originally written.

Status: the pre-publishing plan described in the original `README.md` (now this repository's own
`README.md`) is complete (see "Success criteria"). This document remains authoritative for the
current implementation; where the implementation is narrower than the long-term charter (see
`README.md` and `initial_requirements.txt`, the latter retained in `fastDICOMstructure`), that is
called out explicitly as a **scope boundary**, not a silent limitation or unfinished work.

## 1. Position in the fastDICOM family

```
fastDICOM
|
├── fastDICOMattrs        this repository: structural DICOM attribute semantics within
|                         documented scope boundaries (see docs/SUPPORTED_SCOPE.md) --
|                         parse, mutate, serialize, no policy content
├── fastDICOMstructure    thin consumer of fastDICOMattrs -- traversal, policy
|                         evaluation (policy.py), orchestration
├── fastDICOMscan         independent, cheap, DCMTK-backed shallow attribute probe
|                         (formerly published under the name fastDICOMattrs; renamed
|                         at the A0 extraction -- see ADR-001. Not a dependency of
|                         either library above.)
└── applications (reference implementations, not peer libraries)
      ├── fastDICOMgateway   demonstrates the parse -> policy -> write -> verify pipeline
      └── fastDICOMarchive   intended to become a thin persistence/transport layer
                              (filesystem, C-STORE, STOW-RS) that carries out what
                              structure already decided, rather than deciding itself;
                              its present implementation predates that direction and
                              is not built on this library — see its own docs
```

**Post-extraction dependency rule: `fastDICOMstructure` depends on `fastDICOMattrs`.** This
inverts the pre-A0 rule this document originally stated here ("fastDICOMstructure MUST NOT depend
on fastDICOMattrs"), which was correct only while `fastDICOMattrs` meant the unrelated DCMTK
scanner (now `fastDICOMscan`) — see ADR-001 for the full reasoning. `fastDICOMattrs` itself
depends on nothing in this family: it is the foundation, and it owns no policy content — no
`Require`/`Remove`/`Replace`/`AllowListPrune`-style decision framework lives in this repository
(that is `fastDICOMstructure`'s `policy.py` — see §9a below for what stayed and why).

`fastDICOMattrs` MUST NOT depend on `fastDICOMscan`, and MUST NOT depend on `fastDICOMstructure`
(that would be circular, since structure now depends on attrs). `fastDICOMscan` remains a
completely independent, non-dependency tier that an application may use for cheap file triage
before committing to a full parse through this library — unrelated to this repository's own
parser, which is self-contained (buffer input, mutation, no DCMTK dependency) and was never built
on top of `fastDICOMscan`'s engine.

No shared `fastDICOMcore` library exists. `fastDICOMattrs` and `fastDICOMscan` share no code by
design, not merely by omission.

This family is intentionally pursuing a small, composable technical core — a few strong,
orthogonal mechanisms applications compose — rather than a general DICOM platform, PACS, or
routing product. Its case for being useful rests on clarity, composability, and deterministic
policy enforcement before persistence, not on competitive performance claims against other DICOM
toolkits; benchmarking (`docs/benchmarks.md`) is engineering characterization, not the organizing
objective.

## 2. Pipeline

```
DICOM source (file / buffer / [future] stream)
        |
        v
     Parser  ──────────────────────────► ParseDiagnostic*  (warnings/errors, never exceptions)
        |
        v
  DICOMStructure
        |
        +-- Element        (top-level, in original order)
        +-- Element
        +-- Sequence
        |     +-- Item
        |           +-- Element
        |           +-- ...
        +-- PixelDataReference   (referenced, never materialized)
```

`DICOMStructure` is the single canonical product of parsing. Everything downstream — the C++
API, the C ABI, the Python wrapper, a future writer/anonymizer/archive — operates on this one
representation. `DICOMStructure` is not a `map<Tag, bytes>`; it preserves the original element
order, nested sequence/item structure, and (depending on fidelity) enough encoding detail to
reproduce the source bytes exactly.

## 3. Core types

| Concept | Header | Notes |
|---|---|---|
| `Tag` | `tag.hpp` | `{group, element}` pair, `is_private()`, `is_group_length()`. |
| `VR` | `vr.hpp` | Closed enum of the 33 standard VRs + `Unknown`. Short-form vs. long-form header shape is a pure function of `VR`. |
| `ValueLength` | `value_length.hpp` | Distinguishes 16-bit ("short form") vs 32-bit ("long form") length fields, and defined vs. `0xFFFFFFFF` undefined length. |
| `SourceSpan` | `source_span.hpp` | `{offset, length}` into a `Source`. Carries no pointer — resolved lazily against whichever `Source` owns it. |
| `Source` / `MemorySource` / `FileSource` | `source.hpp` | Byte-providers. See §4. |
| `TransferSyntax` | `transfer_syntax.hpp` | Classifies a Transfer Syntax UID into encoding rules (explicit/implicit VR, endianness, encapsulated pixel data). |
| `Value` | `value.hpp` | The bytes of one non-sequence element: either a `SourceSpan` into a `Source` (zero-copy) or an owned buffer (post-mutation). Typed accessors are thin views over the same bytes. |
| `Element` | `element.hpp` | `Tag` + `VR` + length representation + either a `Value` or a `Sequence`. |
| `Item` | `item.hpp` | An ordered list of `Element`s plus its own defined/undefined-length bookkeeping. |
| `Sequence` | `sequence.hpp` | An ordered list of `Item`s plus its own defined/undefined-length bookkeeping. |
| `ElementPath` | `element_path.hpp` | A path of `(Tag, optional item index)` steps that uniquely names an element nested inside sequences, where `Tag` alone is ambiguous. |
| `PixelDataReference` | `pixel_data_reference.hpp` | Location/extent/encoding of Pixel Data, native or encapsulated, without decoding. |
| `ParseOptions` / `Fidelity` | `parse_options.hpp` | Input to the parser (see §6). |
| `ParseDiagnostic` | `parse_diagnostic.hpp` | One structured diagnostic (severity + message + offset + optional tag). Reused for write-time diagnostics too. |
| `ParseResult` | `parse_result.hpp` | `DICOMStructure` (maybe partial) + diagnostics + overall status. |
| `WriteResult` | `write_result.hpp` | Status + bytes written + diagnostics. |
| `DICOMStructure` | `dicom_structure.hpp` | The canonical structural representation; see §5. |

Mutual recursion between `Element`, `Item`, and `Sequence` is broken with a forward declaration
(`element.hpp` forward-declares `Sequence` and stores it behind `std::unique_ptr<Sequence>`), so
each header only depends downward: `sequence.hpp` includes `item.hpp` includes `element.hpp`.

## 4. Source abstraction and lifetime

`Source` is a small abstract interface:

```cpp
class Source {
  virtual std::uint64_t size() const = 0;
  virtual bool try_get(SourceSpan, const std::byte** out) const noexcept = 0;
  virtual std::string_view description() const = 0;
};
```

Two concrete implementations ship in this increment:

* **`MemorySource`** — either a non-owning view over a caller-supplied buffer (`::view`), as used
  by the C++ `parse_buffer`, or an owned `std::vector<std::byte>` (`::own`). The C ABI keeps its
  own input copy in the opaque structure handle before calling the borrowed C++ API.
* **`FileSource`** — memory-maps the file (POSIX `mmap`) for true zero-copy access to file
  contents; if `mmap` is unavailable (non-POSIX target) it falls back to reading the whole file
  into an owned buffer through the same `Source` interface. Callers do not need to know which
  strategy was used.

**Lifetime rule (load-bearing, must not be violated):** `DICOMStructure` holds a
`std::shared_ptr<const Source>`. Every `SourceSpan`-backed `Value` inside that structure's
`Element`s is only valid for as long as that `shared_ptr` (or a copy of it) is alive. `Element`
and `Value` themselves hold a raw, non-owning `const Source*` — this is safe *only* because they
never exist independently of the owning `DICOMStructure`, which is the sole place `shared_ptr`
ownership lives. Copying a `DICOMStructure` (not implemented yet — see §9) would need to copy or
share that `shared_ptr`, never re-derive a new `Source`.

This is intentionally not "copy every value out of the file up front." A `SourceSpan` is 16
bytes; resolving it costs one bounds-checked pointer computation. Values are only ever copied
into an owned buffer when (a) the caller explicitly mutates an element, or (b) `Value` is
constructed directly from caller-owned memory that has no backing `Source`.

## 5. `DICOMStructure`

* Holds the top-level `Element`s **in original source order** (a `std::vector<Element>`, not a
  map — order is part of the structure, especially for `LOSSLESS` fidelity).
* Holds `shared_ptr<const Source>`, the resolved `TransferSyntax`, the `Fidelity` it was parsed
  at, and whether/what an original 128-byte file preamble was (needed for byte-identical
  reproduction — see `docs/roundtrip-contract.md`).
* File Meta Information (group `0002`) elements are represented as ordinary top-level `Element`s
  alongside the main dataset, not as a separate sub-object. This keeps the object model to one
  concept ("ordered list of elements") instead of two, at the cost of dataset consumers needing
  to know group `0002` is metadata-about-the-file rather than metadata-about-the-image if that
  distinction matters to them. Revisit if a concrete use case needs a split view.
* Provides flat access (`elements()`, `find(Tag)`, `contains(Tag)`), path-aware access
  (`find(ElementPath)`) that walks into sequences/items, and `visit(Visitor)` for recursive
  traversal (see `docs/api-design.md`).
* `pixel_data()` returns the `PixelDataReference` for the dataset's PixelData element, if any,
  without exposing pixel bytes as an allocated `Value`.

## 6. Fidelity levels

`Fidelity` (`FAST`, `STANDARD`, `LOSSLESS`) controls how much encoding detail the parser retains,
not which tags it visits — all three levels walk the entire dataset structure (sequences,
private/unknown elements included). The difference is what is *kept*:

* **FAST** — retains `Tag`, `VR`, length, and value bytes (as `SourceSpan`s, never copied) needed
  for navigation and inspection. In this increment FAST and STANDARD parsing follow the same code
  path; FAST does not yet skip any work. The charter is explicit that FAST should not be
  over-optimized before benchmarks justify it (see `docs/roundtrip-contract.md` §"FAST" and
  `bench/`), so the only thing FAST currently buys is a documented boundary for future
  short-circuiting (e.g., stopping at PixelData, not resolving VR text for skipped elements).
* **STANDARD** — same as FAST in this increment, plus the explicit guarantee that private and
  unknown elements are preserved with their raw values (never dropped), which is enough for
  semantic reconstruction of the dataset. Exact byte reproduction is not guaranteed at this level
  because header representation detail (e.g. reserved-byte values, distinguishing an
  implicit-VR-inferred VR from a source-encoded one) is not required to be retained.
* **LOSSLESS** — additionally retains every detail enumerated in
  `docs/roundtrip-contract.md` (VR encoding mode, length-field width, undefined-length markers,
  reserved bytes, item/sequence delimiters, file preamble bytes) so that an **unmodified** parsed
  object can be serialized back to the exact original bytes. This is proven by test, not just
  claimed — see `tests/integration/test_roundtrip_lossless.cpp`.

## 7. Diagnostics, not booleans

Parsing (and later, writing) never returns a bare success/fail bit and never throws a C++
exception out of `parse_*`. It returns a `ParseResult` carrying zero or more `ParseDiagnostic`s,
each with a `DiagnosticSeverity`:

`Info`, `Warning`, `RecoverableError`, `FatalError`, `Unsupported`, `IOError`.

A parse that hits a recoverable abnormality (e.g. a truncated trailing element, a length that
overruns the source) stops cleanly, keeps everything parsed so far, records a diagnostic, and
still returns a usable `DICOMStructure` with `ParseStatus::SuccessWithWarnings`. A parse of a
Transfer Syntax this increment does not implement (the retired Explicit VR Big Endian) returns
`Unsupported` with no partial structure, rather than guessing. Implicit VR Little Endian is
implemented, via a structural heuristic rather than a data dictionary — see
`docs/roundtrip-contract.md` "Implicit VR Little Endian" for exactly what that does and does not
recover.

C++ exceptions are used internally for programmer-error conditions (e.g. calling `.value()` on a
sequence `Element`) but never escape `parse_file`/`parse_buffer`/`parse_stream`, and are
categorically forbidden from crossing the C ABI (see `docs/abi-design.md`).

## 8. Pixel data

Pixel Data (`(7FE0,0010)`) is never turned into an allocated `Value` inside `DICOMStructure`.
Instead, `PixelDataReference` records:

* native (non-encapsulated) pixel data: one `SourceSpan` covering the raw pixel bytes in place;
* encapsulated (compressed) pixel data: the Basic Offset Table's `SourceSpan` (if present) and a
  `SourceSpan` per fragment Item, obtained by walking the Item headers of the undefined-length
  Item stream **without interpreting fragment contents**;
* the resolved `TransferSyntax`, from which encapsulation and (eventually) compression codec can
  be inferred.

Decoding pixel bytes is explicitly out of scope, permanently, not just for this increment.

`DICOMStructure::pixel_data_position()` (A1.3) separately records how many ordinary top-level
elements preceded Pixel Data in the source, so the writer can reproduce its true relative position
(including any element that followed it, e.g. Data Set Trailing Padding `(FFFC,FFFC)`) instead of
always emitting it last — see `docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md` and
`docs/roundtrip-contract.md` "Known gaps".

## 9. Mutation semantics (design now, minimal code now)

Rules, so future increments extend rather than redesign:

1. An `Element` sourced from `SourceSpan` is untouched on parse; its bytes are never copied
   unless mutated.
2. `DICOMStructure::set(...)`/`set_value(...)` replace an `Element`'s `Value` with an owned
   buffer and mark that `Element` (only that element, not its siblings) as modified. The
   element's `VR` is preserved unless the caller is inserting a brand-new top-level element
   (which must supply a `VR` explicitly — there is no data dictionary in this library to infer
   one from, by design, since VR-from-tag inference is exactly the kind of clinical/dictionary
   knowledge this library deliberately does not own). A new top-level element is inserted at its
   sorted position, not appended, since DICOM data sets require ascending tag order (PS3.5
   section 7.1). Both calls reject a value that would overflow the element's `LengthForm` (a
   short-form header cannot encode a value past `ValueLength::kMaxShortFormLength`) rather than
   truncate it — the mutation does not apply and the structure is left unmodified.
3. Modifying a value nested inside a `Sequence`/`Item` changes only that `Element`; it does not
   eagerly renumber or resize anything. `Item`/`Sequence` length fields only become meaningful
   again once the writer recomputes them — which it now does, at write time, for every
   defined-length `Item`/`Sequence`: rather than trusting a stored length, it serializes the
   container's content to a temporary buffer, measures the actual byte count, and writes that
   (see `write_item` and the `Sequence` branch of `write_element` in
   `src/writer/lossless_writer.cpp`). This was already required for *unmodified* lossless output
   to work — reconstruction, not verbatim copy, is the round-trip contract's stated design (see
   `docs/roundtrip-contract.md` "Reconstruction, not verbatim copy") — so no separate
   recompute-lengths pass was needed to also make it correct for modified structures. `write()`
   on a modified structure is therefore supported today (`Fidelity::Lossless` or
   `Fidelity::Standard` — see `docs/roundtrip-contract.md` "Two write contracts"), producing a
   valid, semantically-correct reconstruction rather than the byte-identical guarantee reserved
   for unmodified `Fidelity::Lossless` input.
4. `erase()` removes an `Element` from its containing list (top-level vector or parent `Item`'s
   element vector). Like `set`, it marks the containing structure modified; no separate
   length-recompute step is needed on erase either, for the same reason as (3).
5. There is no in-place byte patching of source memory, ever — `Source` is treated as read-only
   for the lifetime of the `DICOMStructure` that references it.
6. **A1.7 addendum.** `DICOMStructure::insert(parent, tag, vr, value)` generalizes rule 2's
   top-level-only insertion to any nesting depth: `parent` is a *container-locator* ElementPath
   (every step a Sequence-tag + Item-index descent; empty means root — a different shape from the
   *element-locator* ElementPath `find`/`set_value`/`erase` use, where the last step is bare and
   names the leaf itself). It is still explicit-VR-only and still dictionary-agnostic, exactly as
   rule 2 says — the "no data dictionary in this library" claim remains true of `DICOMStructure`
   itself. What A1.7 adds is a namespace *above* it, `fds::mutation`, that is allowed to depend on
   the PS3.6 dictionary (`fds::dictionary`, frozen at A1.1) and infers a VR for the common
   unambiguous case, calling `DICOMStructure::insert()` underneath once it has one — the
   dictionary dependency lives in that convenience layer, never in `DICOMStructure` itself.
   `fds::charset::insert_text`/`insert_text_inferred` compose the same structural insert()
   with A1.5/A1.6's charset resolution/encoding, for Unicode-aware nested insertion. See
   `docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md` for the full design.

## 9a. Policy layer — moved to `fastDICOMstructure` at the A0 extraction

A minimal, deterministic accept/transform/reject layer (`Require`, `Remove`, `Replace`,
`AllowListPrune`, `PrivateTagPolicy` operations, executed by a `Policy`/`apply()` pair returning a
`PolicyResult`) was originally prototyped in this codebase, built entirely from the mutation and
inspection primitives in §9 above. At the A0 semantic-engine extraction it was kept in
`fastDICOMstructure` (`python/fastdicomstructure/policy.py`) rather than moved here, per the
architectural rule that `fastDICOMattrs` owns DICOM semantics and never policy content — see
ADR-001 and `docs/architecture/A0_EXTRACTION_MANIFEST.md`.

Two things worth recording about that move:

* `policy.py` required **no code change at all** to move cleanly: it already imported `Structure`/
  `Element` only under `TYPE_CHECKING`, never at runtime, and operates purely by calling methods
  (`get`, `erase`, `erase_recursive`, `set_value`, `set_value_recursive`, `erase_private`, and
  read-only iteration) on whatever object it's given. The boundary this section now describes was
  already latent in the implementation before the repository split existed.
* One real coupling did exist and was removed from this repository's `Structure` class as part of
  the extraction: a convenience method, `Structure.apply(policy)`, that lazily imported `policy`
  and delegated to it. It carried no policy logic itself, but its presence on this repository's
  public class was a policy-shaped hook `fastDICOMattrs` should not expose. Callers now write
  `policy.apply(structure, policy)` explicitly (a one-argument-order change, no behavior change) —
  see `fastDICOMstructure`'s own `docs/architecture.md` for the current API and its non-goals
  (no pseudonymization, UID remapping, date shifting, or data dictionary; no transactionality
  across a policy's operations).

## 10. Directory layout

```
fastDICOMattrs/
  docs/                          this document + api/abi/roundtrip design docs
  include/fastdicomattrs/    public C++ headers (the only headers consumers should include)
  src/                           implementation, incl. src/parser, src/writer
  abi/
    include/fastdicomattrs_c/fds.h   public C ABI header
    src/                                  ABI implementation (thin wrapper over src/)
  python/fastdicomattrs/     ctypes-based Python wrapper over the C ABI (see below)
  tests/
    unit/                        pure data-type tests (Tag, VR, ValueLength, SourceSpan, ElementPath)
    integration/                 parser + structure + round-trip tests, with fixtures built in-code
    fixtures/                    fixture provenance notes (see docs/roundtrip-contract.md)
  bench/                         benchmark scaffold
  CMakeLists.txt
```

**Python binding choice:** the initial wrapper is built with `ctypes` against the compiled C ABI
shared library, not `pybind11`. This is a direct reading of the charter's own ordering ("first
prove the C++ model and the ABI are correct... do not build a large Python convenience layer
yet") — `ctypes` requires no extra build dependency, forces the wrapper to go through the exact
same stable ABI any other language binding will use, and needs no compiled extension module of
its own. Revisit only if the ABI surface grows enough that hand-written `ctypes.Structure`
declarations become the bottleneck.

## 11. What this increment deliberately does not do

* No Explicit VR Big Endian dataset parsing. Detected but rejected with `Unsupported`. (Implicit
  VR Little Endian *is* parsed, structurally — see `docs/roundtrip-contract.md` "Implicit VR
  Little Endian" for what that does and does not mean.)
* No pixel decoding of any kind, encapsulated or native.
* No data dictionary, and therefore no VR inference for Implicit VR datasets (elements come back
  as `VR::Unknown` instead) and no clinical convenience accessors. **A1.1/A1.4/A1.7 addendum**:
  this was true of the increment this section originally described. A generated PS3.6 dictionary
  (`fds::dictionary`, frozen at A1.1) now exists, drives Implicit VR sequence/element VR resolution
  and recursive expansion (A1.4), and is consumed by an explicit convenience layer above
  `DICOMStructure`'s own dictionary-agnostic core (A1.7, §9 rule 6). "No data dictionary" remains
  true only of `DICOMStructure` itself, never of this repository as a whole — see
  `docs/SUPPORTED_SCOPE.md` for exactly what is and is not covered.
* No byte-identical write path for anything other than an unmodified `Fidelity::Lossless`,
  Explicit-VR-sourced structure. Mutation-aware serialization (writing after `set`/`erase`) is
  implemented, at `Fidelity::Lossless` or `Fidelity::Standard`, but only as a valid semantic
  reconstruction, not a byte-identical guarantee — see `docs/roundtrip-contract.md` "Two write
  contracts".
* No stream-backed `Source` implementation yet; `parse_stream` exists in the API for interface
  stability but returns `Unsupported`.
* No `fastDICOMcore` shared library.

The README's "Success criteria" are all met as of this writing (see `README.md` "Success
criteria" and `docs/corpus-results.md`/`docs/benchmarks.md` for the evidence), which was this
project's stated finish line. The items in this list are genuine remaining gaps, not blockers to
that finish line: Explicit VR Big Endian support, real-world (not just synthetic-fixture) coverage
of the Implicit VR Little Endian parser, FAST-mode optimization, and a stream-backed `Source` are
the natural candidates for a future increment, in roughly that priority order given what the
real-world corpus run in `docs/corpus-results.md` actually exercised.
