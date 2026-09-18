# Test fixture provenance

This directory intentionally contains no binary `.dcm` sample files.

All fixtures used by `tests/unit/` and `tests/integration/` are synthesized at test
compile/run time via `tests/integration/fixture_builder.{hpp,cpp}`, a small byte-stream
builder purpose-built for these tests (not part of the public library). Each test constructs
exactly the bytes it needs — explicit VR short/long form, defined/undefined length elements,
nested sequences, private/unknown tags, native and encapsulated pixel data, truncated/malformed
input — with no ambiguity about what construct is being exercised and no redistribution/licensing
question about a third-party sample file.

This was a deliberate choice over vendoring sample DICOM files: it keeps the repository free of
any provenance/licensing question, keeps every fixture small and single-purpose, and makes the
exact bytes under test visible in the test source itself rather than opaque inside a binary blob.

Validating against real-world scanner output (see `docs/roundtrip-contract.md`) has since been
done this way: a curated, license-cleared corpus (two TCIA collections) and differential testing
against pydicom, both on files that never entered this repository — see `docs/corpus-results.md`
for the results. That approach, not vendoring sample files here, is how this project intends to
keep validating against real-world data going forward too.
