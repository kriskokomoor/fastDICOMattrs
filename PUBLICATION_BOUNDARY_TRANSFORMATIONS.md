# Publication-boundary transformations — fastDICOMattrs

Derived from internally qualified commit `24efab8a5fecc159ca88f1ad94e03b0d5dee2c61`
(qualified software-tree manifest SHA-256 `025be34e9124582f0ff40455aeefb02a1753da42e2fe3a9acc4e04e3f7814b16`).

The public tree is **not** byte-identical to the internal qualified tree — it derives from it
through the following explicitly enumerated, minimal publication-boundary transformations. No
executable, DICOM-semantic, or build-behavior change is included.

| Path | Category | Reason | Executable semantics changed? |
|---|---|---|---|
| `RELEASE_PROVENANCE.md` | C — public provenance document | Explains the internal→public relationship, content-equivalence proof, and non-ancestry, per the established clean-history model. | No — new file, not referenced by any build/test/runtime code. |
| `PUBLICATION_BOUNDARY_TRANSFORMATIONS.md` (this file) | C — public provenance document | Records this exact transformation set for auditability. | No. |

No personal-identity removal (category A) was needed — the internal hygiene scan found no
personal email, local path, or credential in this component's tracked tree. No public dependency
identity reconciliation (category B) applies — attrs has no dependency on the other two
components. No install/CI correction (category D) was needed — attrs' build system contains no
repository-identity reference of any kind (see the internal tuple attestation §15A).
