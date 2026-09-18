# fastDICOMattrs

**High-performance structural DICOM transformation for pre-persistence screening.**

`fastDICOMattrs` is a C++ library for inspecting and transforming DICOM objects at an ingestion boundary without unnecessarily materializing large bulk data such as Pixel Data.

> [!IMPORTANT]
> This project is experimental and not yet a complete de-identification or validation solution. Explicit VR Little Endian and Implicit VR Little Endian datasets are both parsed (Explicit VR Big Endian is not). Implicit VR resolution now uses a generated PS3.6 data dictionary (see [A1.1](docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md)), so a defined-length nested sequence recognized by that dictionary expands recursively at any depth ([A1.4](docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md)); only *private* (no dictionary entry) defined-length elements, and a small set of dictionary entries with permanently ambiguous VR, remain opaque — see [`docs/roundtrip-contract.md`](docs/roundtrip-contract.md) and [`docs/SUPPORTED_SCOPE.md`](docs/SUPPORTED_SCOPE.md) for the exact boundary. Byte-identical lossless writing applies only to an unmodified, Explicit-VR-parsed structure; writing after mutation produces valid DICOM output, not a byte-identical guarantee — see the same document's "Two write contracts". Review [Scope and safety](#scope-and-safety) before using it with clinical data.

## Current capabilities

- C++20 structural parser for DICOM Part 10 files using Explicit VR Little Endian or Implicit VR Little Endian, plus bare datasets interpreted as Explicit VR Little Endian by default. Bare Implicit VR cannot be selected without File Meta declaring its Transfer Syntax.
- Nested sequence/item representation and recursive inspection.
- Native and encapsulated Pixel Data references without materializing pixel bytes.
- Byte-identical round-trip writing for unmodified, Explicit-VR-parsed lossless structures; mutation-aware writing (element removal, replacement, insertion, private-tag policy) producing valid DICOM output for modified structures.
- A C ABI and Python `ctypes` binding covering both inspection and mutation.
- Validated against 26,636 real-world DICOM files from two public collections, plus two deliberately probed non-DICOM `LICENSE` inputs — 26,634/26,634 byte-identical round-trip among cleanly-parsed DICOM files, and 26,636/26,636 DICOM structural agreement with pydicom; see [`docs/corpus-results.md`](docs/corpus-results.md).
- Benchmarked against pydicom on the same real-world data — see [`docs/benchmarks.md`](docs/benchmarks.md).
- Unit and integration coverage for malformed input, mutation, sequences, Pixel Data, Implicit VR, and round trips.

> A runnable minimal ingestion-pipeline demo built on this library exists at
> [`fastDICOMstructure`'s `python/examples/pipeline_demo.py`](https://github.com/kriskokomoor/fastDICOMstructure/blob/main/python/examples/pipeline_demo.py) —
> moved there at the A0 semantic-engine extraction because it embeds a fixed transformation
> policy (a decision about *what* to remove), which is `fastDICOMstructure`'s concern, not this
> library's. See `docs/architecture/ADR-001-ATTRS-NAMING-AND-LAYERING.md`.

## Build and test

Requirements are a C++20 compiler, CMake 3.16 or newer, Python 3 for the Python tests, and Catch2 3.x for C++ tests. Catch2 3 is not available as a system package on all platforms (e.g. Ubuntu's `catch2` apt package is still 2.x); if `find_package(Catch2 3 REQUIRED)` fails, build and install Catch2 3 from source first, or configure with `-DFDS_BUILD_TESTS=OFF` to build only the library, C ABI, and Python binding without the C++ test suite.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
PYTHONPATH=python python3 -m unittest discover -s tests/python -v
```

To install the C++ library, C ABI library, headers, and CMake package:

```sh
cmake --install build --prefix /desired/prefix
```

Consumers can then use `find_package(fastdicomattrs CONFIG REQUIRED)` and link to `fastdicomattrs::fastdicomattrs` (or `fastdicomattrs::fastdicomattrs_c`). See [`docs/`](docs/) for the API, ABI, architecture, and round-trip contracts.

The project explores a specific architectural problem:

> **Can a DICOM object be inspected, screened, and selectively transformed in flight—before persistence—while preserving everything that policy does not explicitly require changing?**

The intended use case is high-volume medical-imaging ingestion where incoming DICOM objects cannot necessarily be trusted to be free of PII, unwanted private attributes, malformed structures, or other policy violations.

Rather than first persisting the original object and cleaning it downstream, `fastDICOMattrs` is designed to support a **pre-persistence trust boundary**:

```text
Untrusted DICOM input
        │
        ▼
┌────────────────────────────┐
│ fastDICOMattrs         │
│ (structural engine)        │
│                            │
│ structural parsing         │
│ validation                 │
│ tag inspection             │
│ mutation primitives        │
│ (used by a policy layer,   │
│  e.g. fastDICOMstructure,  │
│  to express and apply      │
│  transformation policy,    │
│  private-tag screening,    │
│  and audit evidence)       │
└─────────────┬──────────────┘
              │
      accepted / transformed
              │
              ▼
       Trusted persistence
       PACS / VNA / DICOMweb
       cloud DICOM store
```

## Design goal

The goal is **not** to build another PACS, DICOM viewer, archive, or general-purpose imaging platform.

The goal is to provide the structural machinery required to build fast DICOM ingestion filters.

In particular, the library is intended to determine enough about the structure of an incoming DICOM object to selectively transform it while avoiding unnecessary interpretation or copying of bulk content.

Conceptually:

```text
Incoming DICOM byte stream
        │
        ▼
  structural scan
        │
        ├── identify elements
        ├── identify byte ranges
        ├── identify sequences
        ├── identify bulk data
        └── evaluate policy
        │
        ▼
 selective transformation
        │
        ├── preserve safe spans
        ├── remove selected elements
        ├── replace selected values
        ├── remove private elements
        └── pass bulk data through
        │
        ▼
   Valid DICOM output
```

## Why

A conventional de-identification architecture often looks like:

```text
DICOM
  │
  ▼
Raw persistence
  │
  ▼
De-identification
  │
  ▼
Clean persistence
```

For some environments, that is undesirable.

If the incoming object may contain PII, persisting the original object—even temporarily—means the system has already taken possession of that information. This increases the security, governance, retention, and regulatory scope of the ingestion environment.

An alternative architecture is:

```text
DICOM
  │
  ▼
Ephemeral inspection / transformation
  │
  ▼
Clean persistence
```

`fastDICOMattrs` explores the DICOM-processing component required to make that boundary efficient.

## Structural transformation rather than full decoding

A central design principle is:

> **Do not decode or materialize data that does not need to be interpreted.**

A DICOM object contains structured data elements interspersed with potentially very large bulk values.

For example, Pixel Data:

```text
(7FE0,0010) Pixel Data
```

may account for the overwhelming majority of the object's size.

If policy only requires inspection or modification of metadata, decoding or copying the pixel payload into an object model is unnecessary work.

`fastDICOMattrs` therefore aims to represent DICOM structurally, including information such as:

```text
tag
VR
value length
byte offset
value offset
sequence/item structure
transfer syntax
bulk-data boundaries
```

This allows transformation to be approached as a structural operation over the original byte stream rather than as complete object deserialization followed by reconstruction.

## Example

Given an incoming object containing:

```text
(0008,0060) Modality          = CT
(0010,0010) PatientName       = DOE^JOHN
(0010,0020) PatientID         = 12345678
(0018,0050) SliceThickness    = 1.0
(0019,xxxx) Private Element   = ...
(7FE0,0010) PixelData         = <512 MB>
```

a policy might specify:

```text
preserve  (0008,0060)
remove    (0010,0010)
hash      (0010,0020)
preserve  (0018,0050)
remove    private elements
passthru  (7FE0,0010)
```

This implementation does **not** need to decode 512 MB of pixel data merely to perform these metadata transformations.

Instead, the structural parser identifies the Pixel Data range and allows that byte range to pass through unchanged. `fastDICOMstructure`'s `python/examples/pipeline_demo.py` runs exactly this example end to end, against this exact policy, using this library's mutation and write primitives.

## Scope

The current research scope is intentionally narrow.

`fastDICOMattrs` aims to demonstrate:

1. Parsing a DICOM object from a file or byte stream.
2. Producing a complete structural representation of its data elements.
3. Correctly representing nested sequences and items.
4. Identifying element locations and lengths.
5. Identifying bulk-data regions without materializing their contents.
6. Applying simple deterministic transformation policies.
7. Removing or replacing selected elements.
8. Removing private elements according to policy.
9. Serializing a valid DICOM object after transformation.
10. Preserving untouched content whenever the DICOM encoding permits it.
11. Characterizing cases where byte-for-byte preservation is not possible.
12. Measuring throughput and memory consumption against conventional DICOM processing approaches.

## Explicitly out of scope

Several related problems are deliberately **not** part of the current project.

### Pixel interpretation

`fastDICOMattrs` does not attempt to interpret clinical image content.

### Burned-in PII detection

Detecting patient names or other identifying information rendered directly into image pixels requires image decoding, OCR, computer vision, and modality-aware processing.

That is a separate policy-processing problem.

A future ingestion gateway could route Pixel Data to such a processor when required, but `fastDICOMattrs` itself is concerned with DICOM structure and transformation.

### PACS / VNA functionality

The project is not intended to provide storage, query/retrieve, viewing, lifecycle management, or other PACS/VNA capabilities.

### Full ingestion platform

Cloud Run, Kubernetes, DICOMweb, queues, audit databases, and cloud healthcare services are possible deployment environments for the library, not responsibilities of the library itself.

## fastDICOM family

As of the A0 semantic-engine extraction (see
`docs/architecture/ADR-001-ATTRS-NAMING-AND-LAYERING.md`), this repository *is* the DICOM
attribute-semantics library the rest of the family builds on — it is not one of two peer libraries
answering different questions, as an earlier version of this document described. The layering is
now:

```text
fastDICOM
│
├── fastDICOMattrs        <- this repository: structural DICOM attribute semantics
│                            within documented scope boundaries (see docs/SUPPORTED_SCOPE.md) --
│                            parse, mutate, serialize. Owns no policy content and
│                            depends on nothing else in this family.
├── fastDICOMstructure     thin consumer of fastDICOMattrs: traversal, policy
│                            evaluation (policy.py), orchestration. Depends on
│                            fastDICOMattrs; fastDICOMattrs does not depend on it.
├── fastDICOMscan          independent, cheap, DCMTK-backed shallow attribute probe
│                            (published under the name fastDICOMattrs before the A0
│                            extraction; renamed to free that name for this
│                            repository — see ADR-001). Not a dependency of, or
│                            dependency for, either library above.
│
└── reference applications (independently developed, not one tightly coupled stack)
      ├── fastDICOMgateway
      │     stateless pre-persistence policy boundary demo — the "Longer-term
      │     application" architecture below, actually built and evidence-backed
      │     through local, containerized, Cloud Run, and Healthcare API
      │     persistence boundaries. Depends on fastDICOMstructure.
      └── fastDICOMarchive
            stateful qualified archive/cohort/reconstruction system; historically
            used the pre-rename fastDICOMattrs (now fastDICOMscan) as a fast probe --
            see its own docs for its current dependency configuration.
```

This library's own question remains what it always was:

> **How efficiently can I understand enough of a DICOM object's structure to safely inspect and
> transform it, without unnecessarily materializing bulk data?**

`fastDICOMscan` answers a different, narrower question — fast selective retrieval of well-known
top-level attributes from files, via DCMTK — and is not a dependency of this library or vice versa.

## Research hypothesis

The working hypothesis behind this project is that high-volume DICOM screening does not always require construction of a complete mutable in-memory representation of every object.

For policies primarily concerned with metadata, a C++ structural parser capable of:

* identifying element boundaries,
* preserving source byte ranges,
* selectively replacing or removing elements, and
* passing bulk data through without decoding it

should substantially reduce memory pressure and processing overhead.

The project will test that hypothesis rather than assume it.

## Success criteria

The initial research increment is complete when `fastDICOMattrs` can:

* round-trip representative real-world DICOM objects;
* correctly handle nested structural elements;
* remove and replace selected metadata;
* apply a private-tag policy;
* avoid materializing Pixel Data when it is not being modified;
* demonstrate metadata-level readability using independent DICOM tooling and verify policy effects by re-parsing;
* quantify byte-level preservation of untouched content;
* benchmark CPU time, throughput, and peak memory usage against a conventional implementation; and
* demonstrate the library in a minimal pre-persistence ingestion pipeline.

**All nine criteria are met, each cited to an actual run below rather than merely asserted** — see
each bullet's linked doc for the exact methodology and numbers, including what each measurement
does and does not cover:

* **Round-trip / nested structural elements** — 26,634/26,634 (100%) byte-identical round-trip among cleanly-parsed real-world DICOM CT files, across 26,636 DICOM files from two public collections plus two deliberately probed non-DICOM inputs ([`docs/corpus-results.md`](docs/corpus-results.md)).
* **Remove/replace metadata, private-tag policy** — `DICOMStructure::set`/`set_value`/`erase`/`erase_private_elements` in C++, mirrored through the C ABI and Python binding ([`docs/api-design.md`](docs/api-design.md), [`docs/abi-design.md`](docs/abi-design.md)).
* **Pixel Data never decoded or copied into the element object model**, modified or not — `PixelDataReference` ([`docs/architecture.md`](docs/architecture.md) section 8) holds source byte ranges only, never an allocated `Value`. Two narrower exceptions exist outside that object model: `FileSource` falls back to reading the whole file into an owned buffer when `mmap` is unavailable or fails (still a single copy, not per-element materialization — [`docs/architecture.md`](docs/architecture.md) section 4), and the C ABI/Python `read_buffer()` path makes one library-owned input copy before the zero-copy structural layer sees the bytes.
* **Metadata-level readability and structural agreement** — for every cleanly parsed transformed DICOM file, pydicom can read File Meta and dataset metadata through `stop_before_pixels=True`; this library's own re-parse confirms the policy took effect. This is not full DICOM conformance validation, independent Pixel Data validation, or independent re-verification of every mutation ([`docs/corpus-results.md`](docs/corpus-results.md)).
* **Byte-level preservation quantified** — output that is 99.99% verbatim-from-source by value-payload byte count on real CT data under this README's own worked-example policy, via `Structure.write_with_stats()` ([`docs/corpus-results.md`](docs/corpus-results.md)).
* **Benchmarked against pydicom** — ~5.5x faster at the median on typical files; ~2.1x faster and roughly half the peak memory on large-Pixel-Data files in the latest verification run ([`docs/benchmarks.md`](docs/benchmarks.md)).
* **Minimal pre-persistence pipeline** — [`fastDICOMstructure`'s `python/examples/pipeline_demo.py`](https://github.com/kriskokomoor/fastDICOMstructure/blob/main/python/examples/pipeline_demo.py).

That is the finish line for the current project, and it has been reached. One honest caveat: the 26,636-file real-world corpus above happened to be entirely Explicit VR Little Endian. The Implicit VR Little Endian parser, including dictionary-driven sequence expansion, is validated against real (pydicom-bundled) Implicit VR fixture files in [A1.4](docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md), not merely synthetic fixtures — but that fixture set is small and does not have the scale or scanner-population diversity of the Explicit VR corpus above.

## Corpus probe

The corpus probe is part of the Python package because it is a user-invokable binding tool, not a test case. Its focused regression tests live in `tests/python`.

From a source checkout:

```sh
make build
PYTHONPATH=python python3 -m fastdicomattrs.corpus /homedata/dicom
```

Or let the Makefile build the native binding first and print failure details:

```sh
make corpus CORPUS=/homedata/dicom
```

Every regular file below the supplied path is probed, regardless of extension.

Use `--details` for per-file errors and `--strict` for a nonzero exit status when the corpus contains parse, unsupported-transfer-syntax, or round-trip failures. Add `--reference pydicom` (or `make corpus ... REFERENCE=pydicom`) to additionally cross-check structural parsing against pydicom, or `--transform readme-example` to apply this README's own worked-example policy only to cleanly parsed files, check metadata-level readability with pydicom, verify policy effects by re-parsing, and report byte-level preservation — see [`docs/corpus-results.md`](docs/corpus-results.md) for what these produced against a real corpus. Both require `pip install -r python/requirements-dev.txt`.

## Longer-term application

This is no longer purely hypothetical: [`fastDICOMgateway`](https://github.com/kriskokomoor/fastDICOMgateway) is a sibling
reference application that builds exactly this architecture — an HTTP/Cloud Run ingestion endpoint
in front of this library, with an evidence-backed pre-persistence policy boundary validated through
local, containerized, Cloud Run, and GCP Healthcare API DICOM store deployments. The description
below is retained as the architectural concept this library was designed around; see that
repository for the built, validated result.

A potential deployment architecture is a stateless DICOM ingestion gateway:

```text
External source
      │
      ▼
DICOM ingestion endpoint
      │
      ▼
fastDICOMattrs (structural engine)
      │
      ├── structural validation
      └── mutation primitives
      │
      ▼
fastDICOMstructure (policy layer)
      │
      ├── metadata policy
      ├── PII-tag policy
      ├── private-tag policy
      └── transformation record
      │
      ▼
Trusted DICOM store
```

In this architecture, the original untrusted object need never become part of the persistent imaging environment. `fastDICOMstructure`'s `python/examples/pipeline_demo.py` demonstrates this end to end — structural validation and mutation primitives from this library, composed into metadata policy, private-tag policy, and a transformation/audit record by `fastDICOMstructure`; the surrounding endpoint, queue, and trusted store are deployment concerns outside either library's scope (see [Explicitly out of scope](#explicitly-out-of-scope)).

More sophisticated processors—including burned-in PII detection—could eventually be inserted as independent policy stages.

The library's responsibility remains deliberately smaller:

> **Understand the DICOM structure quickly enough to make deterministic decisions and safely transform the object before persistence.**

## Scope and safety

This library is not a DICOM validator, anonymizer, security boundary by itself, or medical device. It does not detect identifying information burned into pixels. Unsupported transfer syntaxes and malformed input must be handled according to the caller's policy, and applications must treat DICOM metadata and diagnostics as potentially sensitive. Do not assume that a successful structural parse proves an object is safe to persist.

`erase_private_elements()` cannot remove what it cannot see. Dictionary-recognized defined-length Implicit VR sequences now expand recursively ([A1.4](docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md)), so private elements nested inside those are visited and removed like any other nested private element. The remaining gap is narrower: a defined-length element with *no* dictionary entry (private, or one of the small set of standard entries with permanently ambiguous VR) still parses as an opaque value, so any private elements nested inside *that* are not individually visited and pass through as part of the opaque parent value. This does not affect Explicit VR input, where VR is always available on the wire and sequences are always recognized as such.

## Contributing

Bug reports and pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and run the C++ and Python test suites before submitting a change.

## License

This project's own code is released into the public domain under [The Unlicense](LICENSE). This does not cover third-party material vendored into the repository (the NEMA PS3.6 data dictionary XML) — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
