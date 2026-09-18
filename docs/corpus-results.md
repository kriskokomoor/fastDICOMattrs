# fastDICOMattrs — Corpus Validation Results

This document records the results of running `python/fastdicomattrs/corpus.py` against a
real-world, license-cleared DICOM corpus — both read-only probing/differential testing against
pydicom (the curated-corpus work flagged as future work in `docs/roundtrip-contract.md` "Known
gaps") and applying README.md's own worked-example transformation policy to cleanly parsed inputs,
then checking metadata-level readability with pydicom and policy effects with this library's
reparse. Numbers below are from an actual run, not aspirational; re-run the commands
yourself to reproduce them (see "How to reproduce").

## Corpus

Two collections from [The Cancer Imaging Archive](https://www.cancerimagingarchive.net/) (TCIA),
downloaded via the NBIA Data Retriever, each distributed under CC BY 4.0 with a `LICENSE` file
documenting terms:

| Collection | Description | Size | `.dcm` files | Modality |
|---|---|---|---|---|
| [CMB-MEL](https://doi.org/10.7937/GWSPWH72) | Cancer Moonshot Biobank — Melanoma | 14 GB | 23,805 | CT |
| [NLST](https://doi.org/10.7937/TCIA.HMQ8-J677) | National Lung Screening Trial | 1.5 GB | 2,831 | CT |

Neither collection is vendored in this repository — see `tests/fixtures/README.md` for why real
sample files never enter version control here. This corpus lives only on the machine it was
downloaded to; reproduce the run yourself against your own license-cleared corpus.

**Observed characteristic of this corpus:** every real DICOM file in both collections is Explicit
VR Little Endian. This run therefore validates the Explicit VR path against real-world scanner
output extensively, but does not exercise the Implicit VR Little Endian parser
(`docs/roundtrip-contract.md` "Implicit VR Little Endian") against real data at all — that parser
is proven today only against the synthetic fixtures in
`tests/integration/test_parse_implicit_vr_le.cpp`. A future run against a corpus containing
Implicit VR LE files would extend that coverage.

## How to reproduce

```sh
cmake --build build --parallel
pip install -r python/requirements-dev.txt   # pydicom, for --reference pydicom / --transform
make corpus CORPUS=/path/to/CMB-MEL REFERENCE=pydicom
make corpus CORPUS=/path/to/NLST REFERENCE=pydicom

# Transformation policy + pydicom metadata-readability check (see below):
PYTHONPATH=python python3 -m fastdicomattrs.corpus /path/to/CMB-MEL --transform readme-example --details
PYTHONPATH=python python3 -m fastdicomattrs.corpus /path/to/NLST --transform readme-example --details
```

Add `--details`/pass through via the underlying `fastdicomattrs.corpus` CLI for per-file
messages.

## Results

| | CMB-MEL | NLST |
|---|---:|---:|
| Files discovered (all files, not just `.dcm`) | 23,806 | 2,832 |
| Parsed successfully (zero diagnostics) | 23,803 | 2,831 |
| Success with warnings | 3 | 1 |
| Failed / Unsupported transfer syntax | 0 / 0 | 0 / 0 |
| Explicit VR LE | 23,806 | 2,832 |
| Private tags present | 23,805 | 2,831 |
| Sequences present | 23,805 | 2,831 |
| **LOSSLESS round-trip: byte-identical** | **23,803 / 23,803 (100%)** | **2,831 / 2,831 (100%)** |
| **pydicom structural cross-check: matches** | **23,805 / 23,806** | **2,831 / 2,832** |
| Median parse time | 0.25 ms | 0.20 ms |

Every file this library parsed with zero diagnostics round-tripped **byte-for-byte identical** to
the original, and every real DICOM file's structural view (tag set, at every nesting depth, plus
Sequence item counts) agreed exactly with pydicom's independent parse. Both corpora also probed one
non-DICOM `LICENSE` text file each — `corpus.py` deliberately probes every file regardless of
extension (real PACS exports don't always use `.dcm`) — accounting for the "files discovered" count
exceeding the `.dcm` count above, and for the one pydicom "mismatch" per collection below.

## Two things this run found, and why neither is a bug

### 1. Two genuinely truncated real DICOM files (CMB-MEL)

Two files under `CMB-MEL/MSB-04863/.../CTChest-98574/` declare a native Pixel Data element of
524,288 bytes (`OW`, 512×512×2 — an entirely ordinary CT slice size) but the files themselves are
truncated on disk before that many bytes exist (one is short by ~192 KB, the other — at only 61,440
bytes total — is short by over 450 KB). This is corruption/truncation in the source files
themselves, not something introduced by this library.

The parser detects this correctly: a `RecoverableError` diagnostic
("truncated native Pixel Data value"), Pixel Data left unset, and every other element (File Meta,
dataset metadata, sequences) still parsed and available — exactly the "stop cleanly, keep what's
usable" behavior `docs/architecture.md` §7 describes, and the same class of case
`tests/integration/test_malformed.cpp`'s "value length that runs past the end of the source"
synthetic fixture already covers. A byte-identical round-trip is impossible for a file missing data
in the first place, so these two are correctly excluded from the round-trip statistic (see the
`corpus.py` fix below) rather than counted as a regression.

### 2. `corpus.py` bug this run exposed: round-trip was being attempted on already-incomplete parses

Before this run, `probe()` attempted the byte-identical round-trip check on every file that parsed
without an outright failure, including ones that only parsed *with a warning* (like the two
truncated files above, and the LICENSE files — see "Corpus" above). A warning means parsing stopped
early; expecting that output to reproduce bytes the parser never had is not a meaningful check, and
was inflating "round-trip failures" with cases that aren't round-trip regressions at all. Fixed in
`python/fastdicomattrs/corpus.py`: round-trip is now only attempted on a *cleanly* parsed file
(zero diagnostics), so the statistic means what it claims to mean. This is exactly the kind of issue
real-corpus testing is for — no synthetic fixture would have surfaced a corpus-tool accounting bug
like this.

## pydicom cross-check methodology

For each file, this library's structural view (top-level tag set including File Meta, plus
recursive Sequence item counts and nested tag sets) is compared against an independent pydicom
parse (`pydicom.dcmread(..., stop_before_pixels=True)`) of the same file — see `_compare_structural`
in `corpus.py`. Pixel Data is excluded and never decoded on either side; this is a structural
agreement check (does the object have the same shape?), not a claim about typed value decoding,
which this library deliberately does not attempt (`docs/architecture.md` §9).

## Transformation policy: fail-closed eligibility, metadata readability, and byte preservation

`--transform readme-example` applies README.md's own worked-example policy verbatim to every
cleanly parsed file
— preserve Modality, remove PatientName, hash PatientID, preserve SliceThickness, remove private
elements, pass Pixel Data through untouched. Any non-informational parse diagnostic makes an input
ineligible, so partial or ambiguous parses are never transformed. For eligible output, pydicom can
read File Meta and dataset metadata through `stop_before_pixels=True`, and this library's own
re-parse confirms the policy took effect (no PatientName, no private elements, tag order still
ascending). This demonstrates metadata-level readability, not full conformance or independent
Pixel Data validation. See `_apply_readme_example_policy` and
`_validate_transformed_output` in `corpus.py`.

| | CMB-MEL | NLST |
|---|---:|---:|
| Files discovered | 23,806 | 2,832 |
| Transformed (cleanly parsed inputs) | 23,803 | 2,831 |
| Ineligible because parsing produced diagnostics | 3 | 1 |
| Failed | 0 | 0 |
| **pydicom metadata-readable output** | **23,803 / 23,803** | **2,831 / 2,831** |
| Source-backed value bytes | 14,237,529,230 | 1,499,071,566 |
| Regenerated value bytes | 1,618,604 | 192,508 |
| **Verbatim fraction of written bytes** | **99.99%** | **99.99%** |

Every eligible transformed DICOM file was metadata-readable by pydicom and passed the policy
reparse. CMB-MEL's two truncated DICOM files and its non-DICOM `LICENSE` input were ineligible;
NLST's non-DICOM `LICENSE` input was ineligible. None was written or counted as transformed.

**Byte-preservation reading:** `regenerated_value_bytes` counts only *replacement* value bytes
actually written — essentially just the hashed PatientID values (a 64-character SHA-256 hex digest
per file). Removed elements (PatientName, private elements) contribute to neither counter: they
simply do not appear in the output at all, so their original bytes are not "preserved" in any sense
that matters, but they are not counted against `source_backed_value_bytes` either — the two
counters cover only the value-payload bytes of elements present in the written output. Everything
present and untouched — overwhelmingly Pixel Data, which this library never decodes or copies into
an owned buffer — is written back verbatim from the original source and counted as source-backed.
This is the concrete number behind the README's "quantify byte-level preservation of untouched
content" success criterion: on real-world CT data, a policy that only touches three small metadata
fields writes output that is **99.99% verbatim-from-source by value-payload byte count**, via
`Structure.write_with_stats()` (`WriteResult::source_backed_value_bytes`/`regenerated_value_bytes`
in C++).
