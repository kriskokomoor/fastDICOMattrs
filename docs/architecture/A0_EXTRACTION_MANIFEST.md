# A0 Extraction Manifest

Exact file/component-level classification of `fastDICOMstructure`'s pre-A0 content (commit
`348663f4552392c241a37269f2b2f7cabee20073`), used to drive the `git filter-repo` extraction that
produced this repository. Every path below was inspected individually; none is assumed.

Legend: **MOVE** = relocated to `fastDICOMattrs` with history preserved. **REMAIN** = stays in
`fastDICOMstructure`. **SPLIT/ADAPT** = content divided or edited at the boundary. **RETIRE** =
dropped as no-longer-applicable. **SHIM** = a compatibility layer introduced, not present before.

## C++ core

| Path | Classification | Notes |
|---|---|---|
| `include/fastdicomstructure/*.hpp` (18 headers) | MOVE → `include/fastdicomattrs/` | `Tag`, `VR`, `ValueLength`, `SourceSpan`, `Source`, `TransferSyntax`, `Value`, `Element`, `Item`, `Sequence`, `ElementPath`, `PixelDataReference`, `ParseOptions`/`Fidelity`, `ParseDiagnostic`, `ParseResult`, `WriteResult`, `DICOMStructure`, `parse.hpp`. Directory renamed; internal `fds` C++ namespace and file names unchanged (not necessary to establish the boundary — see ADR-001's "minimal necessary renaming" principle applied throughout this manifest). |
| `include/fastdicomstructure/fastdicomstructure.hpp` (umbrella header) | MOVE, renamed → `include/fastdicomattrs/fastdicomattrs.hpp` | Filename directly encodes the old repo name; renamed as part of the mechanical identifier pass (commit after the raw history move), not the history-preserving move itself. |
| `src/*.cpp`, `src/byte_order.hpp` | MOVE, unchanged | Tag/VR/TransferSyntax/Source/Value/ElementPath/Element/PixelDataReference/DICOMStructure/parse implementations. |
| `src/parser/*.{hpp,cpp}` | MOVE, unchanged | Explicit VR LE and Implicit VR LE parsers, byte reader. |
| `src/writer/*.{hpp,cpp}` | MOVE, unchanged | Lossless writer. |
| `abi/include/fastdicomstructure_c/fds.h` | MOVE, renamed → `abi/include/fastdicomattrs_c/fds.h` | C ABI header. The `fds_` function prefix and all type/enum names inside are **unchanged** — a stable C ABI's symbol names are not something to churn for a repository rename, and changing them was not necessary to establish the attrs/structure boundary. |
| `abi/src/fds_abi.cpp` | MOVE, unchanged | ABI implementation. |
| `cmake/fastdicomstructureConfig.cmake.in` | MOVE, renamed → `cmake/fastdicomattrsConfig.cmake.in` | Content updated in the mechanical rename pass (package name only). |
| `CMakeLists.txt` (top-level) | MOVE, content adapted | `project(fastDICOMstructure ...)` → `project(fastDICOMattrs ...)`; target names `fastdicomstructure`/`fastdicomstructure_c` → `fastdicomattrs`/`fastdicomattrs_c`; install namespace updated. Adaptation is mechanical (identifier substitution), not a build-logic redesign. |
| `Makefile` | MOVE, content adapted | References to `fastdicomstructure` module path updated to `fastdicomattrs`; `corpus` target's module invocation updated. |
| `bench/*` | MOVE, unchanged | `bench_compare.py`, `bench_parse.cpp`, `bench/CMakeLists.txt` — engine-level throughput characterization, not policy. |

## Tests

| Path | Classification | Notes |
|---|---|---|
| `tests/CMakeLists.txt` | MOVE, content adapted | Target-link references to renamed library targets. |
| `tests/unit/*.cpp` (5 files) | MOVE, unchanged | `Tag`/`VR`/`ValueLength`/`SourceSpan`/`ElementPath` unit tests — pure data-type tests, no policy involvement. |
| `tests/integration/*.{cpp,hpp}` (10 files incl. `fixture_builder`) | MOVE, unchanged | Parser, mutation, round-trip, pixel data, malformed-input, write-stats, and C ABI integration tests — all prove engine semantics, none touch policy. |
| `tests/fixtures/README.md` | MOVE, unchanged | Fixture provenance notes. |
| `tests/python/test_mutation.py` | MOVE, unchanged except import | Exercises `Structure` mutation through the full Python → C ABI → C++ stack; `import fastdicomstructure as fds` → `import fastdicomattrs as fds`. No behavioral change. |
| `tests/python/test_corpus.py` | MOVE, unchanged except import | Tests `fastdicomstructure.corpus` helpers (`CorpusReport`, `discover_files`, etc.) — engine-level differential/round-trip validation tooling. Import path updated the same way. |
| `tests/python/test_pipeline_demo.py` | **REMAIN** | Tests `python/examples/pipeline_demo.py`, which itself embeds a fixed tag-removal policy (see below) — this is a decision-pipeline test, not an engine test. |
| `tests/python/test_policy.py` | **REMAIN** | Tests `policy.py` directly: `Require`/`Remove`/`Replace`/`AllowListPrune`/`PrivateTagPolicy`. Canonical policy-layer test; must stay with `structure`. One mechanical adaptation was required: `self.structure.apply(pol)` → `policy.apply(self.structure, pol)`, because the `Structure.apply()` convenience method itself was retired from the promoted engine (see "Policy coupling removed from attrs" below) — the underlying assertions and fixtures are otherwise untouched. |

## Python bindings

| Path | Classification | Notes |
|---|---|---|
| `python/fastdicomstructure/__init__.py` | MOVE → `python/fastdicomattrs/__init__.py`, **one method removed** | The ctypes wrapper (`Structure`, `Element`, `Item`, `Diagnostic`, `FdsError`, `StaleElementError`, `WriteStats`, `read`, `read_buffer`) moves essentially verbatim. `Structure.apply(policy)` — a one-line delegation to `policy_module.apply(self, policy)`, imported lazily — is **removed**. Even though it contained no policy *logic* itself, its presence on the promoted engine's public class is a policy-shaped hook that the target architecture's rule ("no policy logic belongs in attrs") does not permit; keeping a stub method whose only purpose is to accept a `Policy` object would leave `attrs` aware of a `structure`-owned concept. Library-name/path constants (`_LIB_NAMES`, `_candidate_dirs`) updated to look for `libfastdicomattrs_c.so` instead of `libfastdicomstructure_c.so`. |
| `python/fastdicomstructure/corpus.py` | MOVE → `python/fastdicomattrs/corpus.py`, unchanged | Discovers files, parses each, and reports parse/round-trip/pixel/diagnostic statistics against a reference (pydicom). This is engine-validation tooling. **One known wart preserved as-is, not fixed in A0**: its `--transform readme-example` mode calls a private `_apply_readme_example_policy()` helper that removes/hashes specific tags — a small embedded policy example used only for corpus-level round-trip evidence. This is flagged, not corrected, in the closure report's "intentionally left duplicated/non-compliant" section, per A0's explicit non-goal of avoiding unrelated cleanup. |
| `python/fastdicomstructure/policy.py` | **REMAIN**, unchanged | `Require`, `Remove`, `Replace`, `AllowListPrune`, `PrivateTagPolicy`, `Policy`, `apply()`. This module already only imports `Structure`/`Element` under `TYPE_CHECKING` (never at runtime — it operates by duck-typed method calls), so **no import changes were needed here at all**. This is the clearest possible evidence the boundary was already latent in the code, not invented for this task. |
| `python/requirements-dev.txt` | MOVE, unchanged | `pydicom` — used only by `corpus.py`/engine tests for differential comparison. |
| `python/examples/smoke_test.py` | MOVE, unchanged except import | Pure ctypes-stack smoke test (parse, iterate, navigate sequences, read diagnostics) with no policy content. |
| `python/examples/pipeline_demo.py` | **REMAIN**, import adapted | Implements its own inline fixed policy (`apply_transformation_policy`: remove `PatientName`, hash `PatientID`, remove private elements) directly with `Structure` mutation primitives — a decision pipeline, same category as `fastDICOMgateway`'s `transform.py`. Its import changes from implicit (`fds` was the same package) to explicit `import fastdicomattrs as fds`, since it now calls into a separate package for the primitives it composes. |

## Documentation

| Path | Classification | Notes |
|---|---|---|
| `docs/architecture.md` | MOVE, **content corrected** | 92% of this document (§2–§8, §10) describes the parser/object-model/writer/pixel-data engine and moves essentially as historical record. §1 ("Position in the family") and §9a ("Policy layer") describe the *old* dependency model and are corrected in the post-move documentation pass (not a silent rewrite — see the closure report's "Documentation updated" section for exactly what changed and why, per objective 10's instruction not to erase history while still not leaving the old dependency claim standing as current). |
| `docs/roundtrip-contract.md` | MOVE, unchanged | 100% engine-level (byte-identical/semantic round-trip contract, Implicit VR limitations, known gaps). No policy content to adapt. |
| `docs/api-design.md` | MOVE, unchanged | Describes the C++/ABI/Python API design rationale — engine-level. |
| `docs/abi-design.md` | MOVE, unchanged | C ABI design (pointer validity, error model, no-exceptions-across-boundary). |
| `docs/corpus-results.md` | MOVE, unchanged | Real-world 26,636-file TCIA corpus validation results — this is exactly the Level 3/4 qualification evidence `fastDICOMattrs` needs to carry forward as its own proof, not `structure`'s. |
| `docs/benchmarks.md` | MOVE, unchanged | Engine throughput characterization. |
| `README.md` (top-level) | MOVE, **substantially rewritten** | The pre-A0 README described `fastDICOMstructure`'s full scope including the policy layer. Rewritten to describe `fastDICOMattrs`'s narrower, promoted scope; this is required by objective 10 (a repository's front-door document cannot keep asserting a role it no longer has) and is not "unrelated cleanup" — see the closure report. |
| `LICENSE`, `CONTRIBUTING.md`, `SECURITY.md`, `.gitignore`, `.github/workflows/ci.yml` | MOVE | Administrative/legal files and CI travel with the promoted engine's identity. CI workflow content adapted mechanically (build/test commands reference renamed targets; C++ build steps kept, since `fastDICOMattrs` still builds C++). |
| `initial_requirements.txt` | **RETIRE** (left behind in `fastDICOMstructure`) | Pre-dates both the engine and the policy layer; describes the original, broader charter for the whole family before the fastDICOMattrs/fastDICOMstructure split existed. Kept with `fastDICOMstructure` as the repository that inherited the original identity, not duplicated into the new repo. |

## What remains in `fastDICOMstructure` after A0 (new content, not previously existing)

| Component | Classification |
|---|---|
| `python/fastdicomstructure/__init__.py` | **SHIM** (new, thin) — re-exports `read`, `read_buffer`, `Structure`, `Element`, `Item`, `Diagnostic`, `FdsError`, `StaleElementError`, `WriteStats` from `fastdicomattrs`, and exposes `policy` as a submodule. Documented in the closure report as an explicit, temporary compatibility shim: it exists so `fastDICOMgateway`'s `import fastdicomstructure as fds` continues to resolve without a gateway-side rewrite in A0. Its intended removal/reduction point is whenever `fastDICOMgateway` is deliberately migrated to import `fastdicomattrs` directly for the primitive operations it actually uses (it uses no policy-layer functionality today) — tracked as a post-A0 decision, not performed here. |
| `Makefile`, CI workflow | **SPLIT/ADAPT** — C++/CMake build steps removed entirely (nothing left in this repository needs compiling); replaced with a pure-Python test workflow that checks out `fastDICOMattrs` as a sibling and builds it, then runs `fastDICOMstructure`'s own `pytest`. |
| `docs/architecture.md` (new, small, structure-scoped) | **SPLIT/ADAPT** — a new, short document describing `fastDICOMstructure`'s remaining scope (policy, traversal, orchestration) and linking to `fastDICOMattrs`'s `docs/architecture.md` for engine detail, replacing the old fused document that used to live here. |

## Confirmation this manifest is exhaustive

Every top-level path present in `fastDICOMstructure` at commit `348663f` was inspected and appears
in exactly one row above: `include/`, `src/`, `abi/`, `cmake/`, `CMakeLists.txt`, `Makefile`,
`bench/`, `tests/` (all four subdirectories), `python/` (all three subdirectories), `docs/` (all
six files), `README.md`, `LICENSE`, `CONTRIBUTING.md`, `SECURITY.md`, `.gitignore`,
`.github/workflows/ci.yml`, `initial_requirements.txt`. No path was left unclassified.
