# ADR-001: `fastDICOMattrs` Naming and Layering

Status: **Accepted and executed** (this ADR documents a decision that has been carried out as part
of task A0 — Semantic Engine Extraction, not a proposal awaiting a separate implementation step).

## Context

An architecture assessment (`ATTRS_CONTRACT_V1_AND_GAP_ANALYSIS.md`, produced prior to this task)
found that the repository named `fastDICOMattrs` and the repository named `fastDICOMstructure`
disagreed with the target architecture in an unusual way: the *name* `fastDICOMattrs` was attached
to the wrong content, while the *content* that belongs under that name already existed, fused
together with a policy layer, inside `fastDICOMstructure`.

Three repositories had already committed documentation asserting the old model:

- `fastDICOMattrs`'s own README described itself as "fast shallow/top-level attribute inspection"
  and stated "sequence traversal is deliberately not implicit."
- `fastDICOMstructure/docs/architecture.md` §1 stated, as a hard constraint: "`fastDICOMstructure`
  MUST NOT depend on `fastDICOMattrs`," describing the two as independent, non-dependent tiers.
- `fastDICOMarchive/docs/fastdicomattrs-capability-assessment.md` independently documented using
  `fastDICOMattrs` only "as a fast readability probe, a root-level tag extractor, and a parser
  capability telemetry source... not currently the full transform engine."

The new architecture requires the opposite relationship: `fastDICOMstructure` depends on
`fastDICOMattrs`, and `fastDICOMattrs` owns complete DICOM attribute semantics (parsing, VR
resolution, mutation, serialization). Two different things cannot both be called
`fastDICOMattrs` at once, and the dependency direction cannot be inverted while the name still
points at the DCMTK-backed scanner.

## Decision

1. **The name `fastDICOMattrs` is repurposed** to mean the promoted semantic DICOM attribute
   engine — the parser, object model, mutation primitives, writer, C ABI, and Python bindings that
   previously lived inside `fastDICOMstructure`.
2. **The former DCMTK-backed shallow scanner no longer owns that name.** Its repository has been
   renamed, at the filesystem/repository level, from `fastDICOMattrs` to **`fastDICOMscan`**. No
   code inside it changed as part of this rename — its C++ namespace (`fastdicom`), Python package
   name (`fastdicom`), and public API (`getTag`/`getTags`/`get_tag`/`get_tags`) are unchanged in
   this pass. Only its repository/product identity is renamed.
3. **`fastdicomscan` is preserved, not deleted.** It still provides real, working value as a fast,
   DCMTK-backed, read-only, top-level tag probe — exactly the role `fastDICOMarchive` already uses
   it for (see that repository's `docs/fastdicomattrs-capability-assessment.md`, which will need
   its own follow-up rename once this ADR is reviewed by that repository's maintainers). Nothing
   about its narrower engineering tradeoffs (DCMTK dependency, no sequence traversal, no mutation,
   no write path) is a defect for the problem it actually solves.
4. **The engine promoted into `fastDICOMattrs` is extracted from `fastDICOMstructure`, with git
   history preserved**, using `git filter-repo` to keep only the commits/paths relevant to the
   parser, object model, mutation primitives, writer, C ABI, and engine-level Python bindings and
   tests (see `A0_EXTRACTION_MANIFEST.md` for the exact file-level classification). It is not a
   rewrite: the algorithms, tests, and design documents move essentially unchanged.
5. **`fastDICOMstructure` becomes a thin consumer of `fastDICOMattrs`.** Its own repository keeps
   its identity, its own git history, and its policy layer (`policy.py`), but no longer contains a
   parser, writer, or object model of its own. Its Python package re-exports the relevant
   `fastdicomattrs` API surface for compatibility (see `A0_SEMANTIC_ENGINE_EXTRACTION_REPORT.md`
   §"Compatibility shims"), and its own C++/CMake build has been removed since nothing remaining in
   that repository requires compilation.

## Dependency direction after A0

```text
fastDICOMgateway
        |
        v
fastDICOMstructure   (policy.py, traversal/orchestration; pure Python)
        |
        v
fastDICOMattrs        (parser, object model, mutation, writer, C ABI; C++ + ctypes)
        |
   raw DICOM bytes
```

`fastDICOMscan` (the renamed former `fastDICOMattrs`) is not part of this dependency chain. It
remains an independent, cheap probe tier that `fastDICOMarchive` may continue to use for triage,
exactly as before this rename — only its name changed.

## Why the collision had to be resolved before any code moved

Moving files without first deciding the name would have produced a repository literally named
`fastDICOMattrs` that still, at the moment of the move, meant "DCMTK scanner" in every other
repository's documentation and in `fastDICOMarchive`'s dependency configuration. Any partial state
between "decide" and "execute" would have made the name ambiguous in a way that is worse than
either extreme (all-old or all-new) on its own. The rename in item 2 above was therefore performed
as the first physical step of A0, before any history extraction began.

## Compatibility implications

- **`fastDICOMarchive`** currently depends on the DCMTK scanner under the name `fastDICOMattrs`
  (see its `docs/fastdicomattrs-capability-assessment.md` and its own family diagram). That
  dependency continues to work unchanged as long as it is repointed at the renamed
  `fastDICOMscan` repository/package name. This repointing is **not** performed as part of A0 —
  `fastDICOMarchive` was not in scope for this task — and is called out here as required follow-up.
- **`fastDICOMgateway`** depends on `fastDICOMstructure` via a sibling-checkout `PYTHONPATH`
  convention (`transform.py`'s `_add_fastdicomstructure_to_path`). Because `fastDICOMstructure`'s
  Python package now re-exports `fastdicomattrs`'s API rather than implementing it, gateway also
  needs `fastDICOMattrs`'s `python/` directory on `sys.path` and its compiled shared library
  locatable. This is a mechanical, additive change to `transform.py`'s path-setup helper (see the
  closure report) — the gateway's own transform logic, imports of `fds.*` names, and observable
  behavior are unchanged.
- **No GitHub remote was repointed, renamed, or pushed to as part of this ADR or A0.** The new
  local `fastDICOMattrs` repository was created with no `origin` remote at all (git-filter-repo
  removes the remote of the source clone by design, to prevent an accidental push of rewritten
  history back to the original location). Establishing the correct GitHub-side identity — renaming
  `github.com/kriskokomoor/fastDICOMattrs` (currently the DCMTK scanner) to
  `fastDICOMscan`, and creating a new `fastDICOMattrs` remote for the promoted engine — is an
  account-level, outward-facing action that this task does not perform. It is called out explicitly
  as an open follow-up in the A0 closure report.

## Alternatives considered

- **Give the promoted engine a new, third name** (e.g. `fastDICOMcore`) instead of reclaiming
  `fastDICOMattrs`, leaving the DCMTK scanner as `fastDICOMattrs`. Rejected: this is exactly the
  outcome the original architecture assessment argued against — the new architecture's contract
  document, public API sketch, and every consumer-facing description already call the target layer
  `fastDICOMattrs`. Introducing a fourth name would only add confusion without resolving the
  original collision.
- **Leave both repositories' names unchanged and route around the collision in prose only**
  (e.g., "when we say `fastDICOMattrs` in this document, we mean..."). Rejected: the task's own
  premise is that this decision must not be left implicit, and a documentation-only fix does not
  change the dependency graph consumers actually import.
