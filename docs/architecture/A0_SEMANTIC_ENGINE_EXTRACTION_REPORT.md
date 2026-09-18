# A0 — Semantic Engine Extraction: Closure Report

## 1. Executive result: **PASS**

The DICOM attribute-semantics engine (parser, object model, mutation primitives, writer, C ABI,
Python bindings) was extracted from `fastDICOMstructure` into a newly-promoted `fastDICOMattrs`
repository, with git history preserved for every moved file. `fastDICOMstructure` was rebuilt as a
pure-Python, thin consumer of that engine, keeping its policy layer unchanged. `fastDICOMgateway`
was adapted only where the new layering required it (container build, CI) — its transform pipeline
and Python imports needed zero changes. Every regression check reproduces the pre-extraction
baseline exactly: 115 C++ tests, 46 Python tests (correctly redistributed 34/12 across the two
repositories), a 2,832-file real-world corpus run with byte-for-byte identical counts, and
`fastDICOMgateway`'s full 74-passed/1-skipped suite including a real Docker build and M3 container
validation. No unexplained regression was found. No character-set work, dictionary work, Implicit
VR improvement, or policy redesign was performed, per A0's explicit non-goals.

## 2. Repository commits before extraction

| Repository | Commit |
|---|---|
| `fastDICOMstructure` | `348663f4552392c241a37269f2b2f7cabee20073` |
| `fastDICOMattrs` (pre-A0, DCMTK scanner) | `fd4622d7f146db1c3bad51d242ae05e9e5f97010` |
| `fastDICOMgateway` | `2467a0f54b9445e4024d94145b7d3277ceca07ff` |

Full baseline detail (test counts, corpus numbers, API surface, benchmark numbers) is in
`docs/architecture/A0_PRE_EXTRACTION_BASELINE.md`.

## 3. Repository commits after extraction

| Repository | Commit | Role |
|---|---|---|
| `fastDICOMattrs` | `6ce77c3` | Promoted semantic engine (new identity; history extracted from `fastDICOMstructure`) |
| `fastDICOMstructure` | `63970e5` | Thin policy/orchestration consumer |
| `fastDICOMgateway` | `c8863c2` | Adapted container build + CI only |
| `fastDICOMscan` | `0c38e2d` | Renamed former `fastDICOMattrs` (DCMTK scanner); code unchanged |

`fastDICOMattrs`' own commit sequence, on top of the imported history (`3b99974` and earlier,
identical to `fastDICOMstructure`'s pre-A0 commits):

1. `e9deb33` — ADR-001, extraction manifest, pre-extraction baseline
2. `2bb8368` — mechanical identifier rename (`fastdicomstructure` → `fastdicomattrs`)
3. `0b50e62` — removal of `Structure.apply(policy)` (the one policy-coupling point)
4. `6ce77c3` — carry-forward of the pre-A0 gap-analysis document

## 4. Naming decision

See `docs/architecture/ADR-001-ATTRS-NAMING-AND-LAYERING.md` for the full record. Summary: the name
`fastDICOMattrs` is repurposed for the promoted engine; the former DCMTK-backed scanner is renamed
to `fastDICOMscan` (directory/repository identity only — no code, namespace, or API change); its
family-diagram README section was corrected to stop claiming the `fastDICOMattrs` name
(`fastDICOMscan` commit `0c38e2d`).

**Not performed, and explicitly out of scope for A0:** any GitHub-side remote rename or creation.
The new local `fastDICOMattrs` repository has no `origin` remote (removed automatically by
`git filter-repo` as a safety measure). Establishing the correct GitHub identity — renaming
`github.com/kriskokomoor/fastDICOMattrs` (currently the DCMTK scanner) to `fastDICOMscan` and
creating a new `fastDICOMattrs` remote for the promoted engine — is an account-level, outward-facing
action left for the user to perform deliberately. Similarly, `fastDICOMarchive`'s dependency on the
pre-rename `fastDICOMattrs` (its `docs/fastdicomattrs-capability-assessment.md`) was not touched;
that repository was out of scope for A0.

## 5. Final dependency diagram

```text
fastDICOMgateway
        |
        v
fastDICOMstructure   (policy.py, orchestration -- pure Python)
        |
        v
fastDICOMattrs        (parser, object model, mutation, writer, C ABI -- C++ + ctypes)
        |
   raw DICOM bytes

fastDICOMscan          independent, DCMTK-backed shallow probe -- not in this dependency chain
```

Verified, not asserted: `fastDICOMattrs`'s own source tree contains no reference to
`fastdicomstructure`/`fastDICOMstructure`/`FASTDICOMSTRUCTURE` outside its own historical decision
documents (checked by repository-wide search after every content change); `fastDICOMstructure` has
no C++ source, CMake file, or compiled artifact of any kind; `fastDICOMgateway`'s transform pipeline
imports `fastdicomstructure`, which imports `fastdicomattrs`, confirmed by direct interpreter
inspection (`transform.fds.__file__` resolves to `fastDICOMstructure/python/fastdicomstructure/__init__.py`,
whose own `Structure`/`read_buffer` are the literal `fastdicomattrs` objects).

## 6. Components moved to `fastDICOMattrs`

Full file-level detail in `docs/architecture/A0_EXTRACTION_MANIFEST.md`. Summary: all 18 public
C++ headers, all `src/` implementation (parsers, writer, byte reader), the C ABI (`abi/`), the
CMake build, `bench/`, all unit and integration C++ tests (15 files, 115 cases), the engine-facing
Python bindings (`__init__.py`, `corpus.py`), two engine-focused Python test files (`test_mutation.py`,
`test_corpus.py`, 34 cases combined with 0 remaining after policy tests moved out), the smoke-test
example, and six design documents (`architecture.md`, `roundtrip-contract.md`, `api-design.md`,
`abi-design.md`, `corpus-results.md`, `benchmarks.md`) plus `README.md`, `LICENSE`,
`CONTRIBUTING.md`, `SECURITY.md`, and CI. `README.md` and `docs/architecture.md` received targeted
content corrections beyond mechanical renaming (see §9 below), not just identifier substitution.

## 7. Components retained in `fastDICOMstructure`

`python/fastdicomstructure/policy.py` (unchanged — see §8), a new thin `__init__.py` re-exporting
`fastdicomattrs`'s API, `python/examples/pipeline_demo.py` and its test (unchanged import — see
§9), `tests/python/test_policy.py` (one mechanical call-site adaptation — see §8),
`initial_requirements.txt` (historical charter document), `LICENSE`, `CONTRIBUTING.md`,
`SECURITY.md`, and a new short `docs/architecture.md` describing the remaining scope.

## 8. Compatibility shims introduced

One, explicit and documented at its point of definition:

**`fastdicomstructure/__init__.py`'s re-export of `fastdicomattrs`'s public API.** Purpose: let
`fastDICOMgateway`'s existing `import fastdicomstructure as fds` continue to resolve without a
gateway-side rewrite in A0, per objective 6 ("do not redesign the gateway"). Its own module
docstring names the removal consideration: once `fastDICOMgateway` is deliberately migrated (a
post-A0 decision, since it uses zero policy-layer functionality today — only raw mutation
primitives), it could import `fastdicomattrs` directly and this re-export could shrink to just the
`policy` submodule. Not removed in A0 because gateway's import surface was explicitly required to
stay stable.

**Not a shim, but a real, permanent removal:** `Structure.apply(policy)` was deleted from the
promoted engine's public API (attrs must not carry a policy-shaped hook, even a one-line
delegation). The one call site needing adaptation, `tests/python/test_policy.py`, now calls
`policy.apply(structure, pol)` instead of `structure.apply(pol)` — a call-order change with
identical behavior, verified by the same test suite passing unchanged in every other respect.

## 9. Corrections beyond the extraction manifest's predictions

Two findings worth recording precisely because they update what the manifest anticipated:

- The manifest predicted `python/examples/pipeline_demo.py` would need an import adaptation
  (`import fastdicomstructure as fds` → explicit `fastdicomattrs` import). In practice **no change
  was needed**: the thin re-export in `fastdicomstructure/__init__.py` preserves the exact same
  `fds.Structure`/`fds.read_buffer` surface, so the existing `import fastdicomstructure as fds`
  continues to work unmodified. This is a stronger compatibility result than planned, not a gap.
- `docs/architecture.md`'s §1 and §9a required substantive rewriting, not mechanical identifier
  substitution — a first mechanical sed pass collapsed both `fastDICOMattrs` (old, DCMTK meaning)
  and `fastDICOMstructure` (this repository, pre-rename) into the same string in sentences that
  named both, producing nonsensical self-referential claims (e.g. "fastDICOMattrs MUST NOT depend
  on fastDICOMattrs"). This was caught during verification (the file's diff was inspected line by
  line, not assumed correct from the sed exit code) and hand-corrected — see `docs/architecture.md`'s
  "Historical note" callout and the rewritten §1/§9a. The same error initially and briefly touched
  the three historical decision documents (`ADR-001`, the extraction manifest, and the baseline)
  before being caught by the harness's own file-change tracking and restored from the prior commit.
  No corrupted version of any of these documents was ever committed to git.

## 10. Test results, before and after

| Suite | Before | After |
|---|---|---|
| C++ (`ctest`, `fastDICOMattrs`) | 115/115 pass | 115/115 pass (identical, fresh clean build against a locally-built Catch2 v3 — see §14) |
| Python, engine-scope (`fastDICOMattrs`) | 46 total (fused) | 34/34 pass (`test_mutation.py`, `test_corpus.py`) |
| Python, policy-scope (`fastDICOMstructure`) | (fused into the 46 above) | 12/12 pass (`test_policy.py`, `test_pipeline_demo.py`) |
| Python, combined | 46 | 34 + 12 = **46** — exact reproduction, correctly split by ownership |
| `fastDICOMgateway` (`pytest`) | 74 passed, 1 skipped | 74 passed, 1 skipped — identical |

The one skip is the pre-existing, credential-gated M4/M5 live-cloud test; unaffected by A0.

## 11. Corpus results, before and after

Both runs against the same local 2,832-file NLST (National Lung Screening Trial CT) directory,
`--reference pydicom`:

| Metric | Before | After |
|---|---|---|
| Files discovered | 2,832 | 2,832 |
| Parsed successfully | 2,831 | 2,831 |
| Success with warnings | 1 | 1 |
| Failed | 0 | 0 |
| Explicit VR LE | 2,832 | 2,832 |
| Native Pixel Data | 2,831 | 2,831 |
| Private tags present | 2,831 | 2,831 |
| Sequences present | 2,831 | 2,831 |
| LOSSLESS eligible / round-trip identical | 2,831 / 2,831 | 2,831 / 2,831 |
| Round-trip failures | 0 | 0 |
| Reference matches (pydicom) | 2,831 | 2,831 |
| Reference mismatches | 1 | 1 |

Every count is identical. The single pre-existing warning and the single pre-existing pydicom
mismatch are unchanged and were not investigated — they are baseline state (see the baseline
document), not something A0 was scoped to fix.

## 12. Performance sanity results

Manual micro-benchmark, same representative 1,499,458-byte NLST CT file, 500 iterations, `Fidelity.LOSSLESS`:

| Metric | Before | After |
|---|---|---|
| Parse | 0.171 ms/iter | 0.185 ms/iter |
| Write | 0.822 ms/iter | 0.736 ms/iter |
| Peak RSS | 29,636 KB | 29,636 KB (exact match) |

Both parse and write differences are within normal run-to-run measurement noise for a sub-
millisecond operation (roughly ±10%, in opposite directions, netting out even). Peak RSS is
identical to the byte. The corpus tool's own median parse time dropped from 0.48 ms to 0.06 ms
between the two full-corpus runs; this is attributed to OS filesystem page-cache warmth from the
first run still being resident for the second (both runs were made minutes apart on the same
corpus, same machine), not to any code change — no parsing or I/O logic was touched during A0.
No gross regression was found in either direction.

## 13. Regressions found and disposition

One regression was found and fixed, not merely noted:

**`fastDICOMgateway`'s M3 container-validation test failed** after the extraction
(`test_m3_end_to_end_run_passes_read_only_with_corrected_scenario_d`), because its `Dockerfile`
compiled `fastDICOMstructure`'s own (now nonexistent) `CMakeLists.txt` inside a build stage. This
was an expected, direct consequence of moving the C++ build to `fastDICOMattrs`, called out as a
required adaptation in ADR-001, not a surprise. Fixed by rewriting the Dockerfile's build stage to
compile `fastDICOMattrs` instead (a second named Docker build context), copying both the compiled
`fastdicomattrs` package and the pure-Python `fastdicomstructure` package into the runtime image,
and switching the runtime environment variable from `FASTDICOMSTRUCTURE_LIB` to
`FASTDICOMATTRS_LIB`. Verified by an actual `docker build` (not just the Python test) and a full
gateway test run reproducing 74 passed / 1 skipped. See `fastDICOMgateway` commit `c8863c2`.

No other regressions were found in any of the four regression categories (unit tests, corpus,
performance, or manual end-to-end smoke checks).

## 14. Known pre-existing limitations, deliberately preserved unchanged

Per A0's explicit non-goals, none of the following were touched:

- No data dictionary; no keyword resolution; no VR inference for Implicit VR (elements still come
  back as `VR::Unknown`).
- No character-set handling of any kind (confirmed absent both before and after by repository-wide
  search).
- No private-tag creator-block identity (only odd/even group-number classification).
- No Explicit VR Big Endian support (detected, rejected as `Unsupported`, unchanged).
- No stream-backed `Source` (`parse_stream` still returns `Unsupported`).
- The documented Implicit VR limitation (a defined-length nested sequence is opaque without a
  dictionary) is unchanged and still covered by the same test asserting it stays true.
- The documented "Pixel Data always written last" writer limitation is unchanged.
- `corpus.py`'s `--transform readme-example` mode still contains a small embedded policy-shaped
  helper (`_apply_readme_example_policy`) inside `fastDICOMattrs`. This is a genuine, minor
  architectural wart under the new "no policy in attrs" rule — it is validation/demonstration
  tooling, not part of `fastDICOMattrs`'s production API surface (no consumer imports or calls it
  outside `corpus.py`'s own CLI), and fixing it (moving that corpus mode to `fastDICOMstructure`)
  was judged out of scope for a pass whose explicit purpose is boundary-preservation without
  behavior change — flagged here rather than silently carried forward or quietly fixed.
- One environment-specific note, not a code limitation: this sandbox's Ubuntu 22.04 (jammy) only
  packages Catch2 v2.13.8, while the C++ test suite requires v3. `fastDICOMattrs`'s existing CI
  workflow's `apt-get install catch2` step (mechanically renamed, otherwise untouched) presumably
  works on GitHub's actual `ubuntu-latest` runners (a newer Ubuntu release with Catch2 v3 in its
  default repositories) since this exact command pre-dates A0 unchanged. Verification in this
  session therefore used a Catch2 v3 built from source into a local, non-system prefix
  (`-DCMAKE_PREFIX_PATH=...`) — a verification-environment workaround, not a repository change.

## 15. Documentation updated

- `fastDICOMattrs/README.md` — family diagram corrected (three distinct entities, correct
  dependency direction); `pipeline_demo.py` references corrected to point at `fastDICOMstructure`.
- `fastDICOMattrs/docs/architecture.md` — historical-note callout added; §1 (family position, old
  "MUST NOT depend" rule) and §9a (policy layer) rewritten to state the current, correct
  relationship, with the original content's substance preserved as explained history, not erased.
- `fastDICOMstructure/README.md` — fully rewritten (the pre-A0 version described a fused engine
  this repository no longer contains).
- `fastDICOMstructure/docs/architecture.md` — replaced with a short document describing the
  remaining policy/orchestration scope, linking to `fastDICOMattrs` for engine detail.
- `fastDICOMscan/README.md` — family section corrected to state the rename and stop claiming the
  `fastDICOMattrs` name.
- CI workflows updated in `fastDICOMattrs` (mechanical rename only) and rewritten in
  `fastDICOMstructure` (no longer builds C++; checks out and builds `fastDICOMattrs` instead) and
  `fastDICOMgateway` (adds the `fastDICOMattrs` checkout/build step, second Docker build context).
- `fastDICOMgateway`'s `Dockerfile` — rewritten build stage (§13).
- Three new durable documents in `fastDICOMattrs`: `ADR-001-ATTRS-NAMING-AND-LAYERING.md`,
  `A0_EXTRACTION_MANIFEST.md`, `A0_PRE_EXTRACTION_BASELINE.md`, plus this report and the carried-
  forward `ATTRS_CONTRACT_V1_AND_GAP_ANALYSIS.md`.

**Not updated, deliberately:** `fastDICOMarchive`'s documentation (its own
`docs/fastdicomattrs-capability-assessment.md` and family diagram) — out of scope for A0, flagged
in ADR-001 as required follow-up for that repository's own maintainers.

## 16. Remaining A1 gaps (recorded for the next phase, not addressed here)

Per the assignment's explicit instruction, two items are recorded as V1 correctness requirements
for A1, distinguished from mere convenience:

- **Dictionary-backed VR resolution for Implicit VR is a V1 MUST.** Tag → correct VR semantics is
  correctness, not convenience — an Implicit-VR element's actual VR determines how its value must
  be interpreted, and `VR::Unknown` today means every Implicit-VR value is opaque regardless of
  what it actually contains.
- **Specific Character Set support is a V1 gating requirement.** Confirmed still completely absent
  (repository-wide search, zero hits for character-set-aware text decoding, before and after A0).

Distinguished from these, tag → friendly keyword/name resolution remains V1 SHOULD, not MUST — a
convenience layered on top of correct VR resolution, not a prerequisite for `fastDICOMstructure` to
avoid implementing DICOM semantics itself. Private-tag creator-block identity remains V1 SHOULD.
The full V1 MUST/SHOULD/deferred classification is in `ATTRS_CONTRACT_V1_AND_GAP_ANALYSIS.md` §"V1
MVP scope" and stands unchanged by A0 (A0 was scoped to not touch semantic completeness at all).

## 17. Go/no-go recommendation for A1

**Go.** The architectural boundary A0 set out to prove — that `fastDICOMattrs` can own DICOM
semantics while `fastDICOMstructure` owns policy, with a real (not conceptual) one-way dependency
between them — is now real, tested, and evidenced end to end, including through the one real
consumer (`fastDICOMgateway`) that exercises it under an actual container build. A1 (dictionary-
backed VR resolution and character-set support) can begin directly against the promoted
`fastDICOMattrs` repository without further restructuring.
