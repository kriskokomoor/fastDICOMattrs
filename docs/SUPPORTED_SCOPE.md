# Supported scope

This is a concrete list of what `fastDICOMattrs` demonstrably does, does not, and does
conditionally — as opposed to the summary prose in `README.md`. Every row cites the freeze
report or test file that is the actual evidence; "supported" here always means "supported and
qualified by that evidence," not "should work."

Avoid reading "complete," "all," or "fully" anywhere else in this repository's documentation as
literal claims broader than this table. Where such wording remains (e.g. in historical ADRs), it
describes an architectural *ownership boundary* ("this repository, not `fastDICOMstructure`, owns
DICOM attribute semantics"), not exhaustive coverage of every VR, charset, or transfer syntax.

## Demonstrated support

| Area | Scope | Evidence |
|---|---|---|
| Parsing | Explicit VR Little Endian; Implicit VR Little Endian (including bare datasets, defaulting to Explicit VR LE unless File Meta declares otherwise) | `src/parser/`, `tests/integration/test_parse_*.cpp` |
| Dictionary | Generated from a pinned NEMA PS3.6 edition; 5,172 exact tag entries + 71 repeating-group rules | [A1.1](architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md) |
| Implicit VR sequence expansion | Defined-length sequences/items recognized by the dictionary expand recursively at any depth | [A1.4](architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md) |
| Private creator resolution | An element resolves to its own container's declared private creator (no inheritance from parent/sibling) | [A1.2](architecture/A1_2_PRIVATE_CREATOR_IDENTITY_REPORT.md) |
| Pixel Data | Referenced by source byte range only (native and encapsulated), never decoded or copied into the element object model; source-relative placement survives trailing elements and mutation | [A1.3](architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md), `include/fastdicomattrs/pixel_data_reference.hpp` |
| Charset decode | Selected single-byte repertoires, UTF-8, and single-byte ISO 2022 forms; inherited/overridden Specific Character Set, VR-specific delimiter handling | [A1.5](architecture/A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md) |
| Charset encode/mutate | Supported text can be encoded and mutated without silent substitution; unrepresentable/unsupported text is rejected, not truncated | [A1.6](architecture/A1_6_CHARACTER_SET_REENCODING_REPORT.md) |
| Mutation | Root and nested insertion/replacement/removal; dictionary-aware VR inference for the unambiguous case via `fds::mutation` | [A1.7](architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md) |
| Round-trip | 26,634/26,634 byte-identical round-trip on cleanly-parsed real-world Explicit VR LE files (CMB-MEL + NLST); 26,636/26,636 structural agreement with pydicom | `docs/corpus-results.md` |

## Partial / context-dependent support

| Area | Boundary |
|---|---|
| Implicit VR opacity | A defined-length element with **no** dictionary entry (private, or one of the retired/excluded wildcard patterns) still parses as an opaque value, not expanded — any private elements nested inside it are not individually visited |
| Ambiguous VR | A small, fixed set of standard dictionary entries have inherently ambiguous VR (e.g. content-dependent OB-or-OW) and resolve to `VR::Unknown` rather than a guess |
| Real-world Implicit VR validation | The 26,636-file real-world corpus is entirely Explicit VR LE; Implicit VR parsing (including dictionary-driven expansion) is validated against real pydicom-bundled fixture files, not against a corpus of comparable scale or scanner diversity |
| Byte-identical write | Guaranteed only for an unmodified, Explicit-VR-parsed, `Fidelity::Lossless` structure; writing after mutation, or from Implicit VR / `Fidelity::Standard` input, produces valid DICOM output but not a byte-identical guarantee (`docs/roundtrip-contract.md`, "Two write contracts") |
| Charset round-trip | Decode/re-encode can canonicalize padding/escape-sequence choices; this is a semantic round trip, not byte-exact undo |

## Unsupported

- Explicit VR Big Endian (detected, rejected as `Unsupported`).
- Pixel decoding of any kind (native or encapsulated) — Pixel Data is never materialized into the element object model.
- Vendor-private dictionary knowledge (only PS3.6's public dictionary is generated; private-creator *identity* resolves, private-creator *meaning* does not).
- Multi-byte code-extension charsets (e.g. ISO 2022 JP/KR/CN-style CJK repertoires) — only single-byte repertoires, UTF-8, and single-byte ISO 2022 forms are supported.
- Burned-in pixel PHI detection.
- Transactionality across a policy's operations (that is `fastDICOMstructure`'s concern, not this library's — see its own architecture docs).
- UID remapping, date shifting, pseudonymization — this library provides mutation primitives; it does not implement or endorse any specific de-identification profile.

## Intentionally unresolved semantics

- A missing private creator leaves the private element unresolved; there is no inheritance from a parent or sibling container.
- 174 PS3.6 dictionary entries newer than the pydicom edition used for cross-validation are present in the generated dictionary but were not independently adjudicated against pydicom; 17 retired wildcard patterns are deliberately excluded (see [A1.1](architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md) for the exact accounting — note that report's own row-subtotal prose omits these 17 exclusions from its displayed equation; prefer the generator's own output).
