# A0 Pre-Extraction Baseline

Captured before any file was moved. All commands were run against the working trees as they stood
at the commits below; no build flags or test selection were changed from each repository's own
documented defaults.

## Repository commits at baseline

| Repository | HEAD commit | Notes |
|---|---|---|
| `fastDICOMstructure` (source of the extraction) | `348663f4552392c241a37269f2b2f7cabee20073` | Clean working tree, 3 commits ahead of `origin/main`, not yet pushed. |
| `fastDICOMattrs` (pre-A0, DCMTK scanner — renamed to `fastDICOMscan` during A0) | `fd4622d7f146db1c3bad51d242ae05e9e5f97010` | 1 commit ahead of `origin/main`; uncommitted `.gitignore` change and untracked `docs/`, `.vscode/`, `tests/nlst/` present at baseline (unrelated to A0, left untouched). |
| `fastDICOMgateway` | `2467a0f54b9445e4024d94145b7d3277ceca07ff` | Working tree carries pre-existing, unrelated uncommitted changes (adversarial-validation follow-up work: modified `transform.py`, `m2.py`, `m3.py`, `README.md`, `docs/PUBLICATION_BRIEF.md`, two test files, plus several untracked evidence/report files). These predate this task and are not touched by A0. |

## Test results

### `fastDICOMstructure` C++ suite (`ctest --test-dir build`)

```
100% tests passed, 0 tests failed out of 115
Total Test time (real) = 0.44 sec
```

### `fastDICOMstructure` Python suite (`PYTHONPATH=python python3 -m pytest tests/python -q`)

```
46 passed in 0.12s
```

### `fastDICOMgateway` suite (`python3 -m pytest -q`)

```
74 passed, 1 skipped, 2 warnings in 97.99s
```

The one skip is a pre-existing, credential-gated live-cloud test (M4/M5 evidence; self-skips
without GCP credentials in this environment) — unrelated to A0.

## Corpus validation baseline

Run against the local real-world NLST collection (National Lung Screening Trial CT, 2,831 files,
1.5 GB — the same collection `docs/corpus-results.md` reports on), via
`PYTHONPATH=python python3 -m fastdicomstructure.corpus <NLST_DIR> --reference pydicom`:

```
Files discovered             2,832
Parsed successfully          2,831
Success with warnings            1
Failed                            0
Unsupported transfer syntax       0

Explicit VR LE                2,832
Native Pixel Data              2,831
Encapsulated Pixel Data           0

Private tags present           2,831
Sequences present               2,831
Maximum nesting depth              1

LOSSLESS eligible              2,831
Round-trip identical           2,831
Round-trip failures                0

Reference-compared (pydicom)   2,832
Reference matches              2,831
Reference mismatches               1

Median parse                 0.48 ms
P95 parse                    0.75 ms

Wall time: 19.3 s
```

The pre-existing single "success with warnings" file and single "reference mismatch" against
pydicom are **not** investigated or fixed as part of A0 — they are baseline state to be reproduced
identically after extraction, not a defect this task addresses. (`Files discovered` is 2,832 vs.
2,831 parsed because the corpus directory also contains a non-DICOM `LICENSE` file, which the tool
correctly does not count as a parse failure — confirmed by inspection.)

## Performance sanity baseline

Manual micro-benchmark against one representative NLST CT file
(`.../100009/.../1-1.dcm`, 1,499,458 bytes, native Pixel Data), 500 iterations each, via the Python
ctypes binding at `Fidelity.LOSSLESS`:

| Metric | Value |
|---|---|
| Parse (median-equivalent, 500 iters) | 0.171 ms/iter |
| Write (`write_bytes()`, 500 iters) | 0.822 ms/iter, output byte-identical size (1,499,458 bytes) |
| Peak RSS of the Python process for this run | 29,636 KB |

This is consistent in order of magnitude with the corpus tool's own median (0.48 ms) — the
difference is expected: the corpus tool's default fidelity is `Standard`, this baseline used
`Lossless` (more retained detail), and per-file cost varies with file size and element count across
the corpus.

## API surface at baseline

### Python (`fastdicomstructure/__init__.py`, `__all__`)

```
read, read_buffer, Structure, Element, Item, Diagnostic, FdsError, StaleElementError, WriteStats
```

`Structure` additionally exposed `apply(policy)` as an instance method (a one-line delegation into
`policy.py`) — this is the one method removed during A0 (see `A0_EXTRACTION_MANIFEST.md`).

### Python (`fastdicomstructure/policy.py`, `__all__`)

```
Tag, Decision, OperationResult, Diagnostic, PolicyResult, PolicyOperation, Require, Remove,
Replace, AllowListPrune, PrivateTagPolicy, Policy, apply
```

### C ABI exported symbols (`nm -D --defined-only build/libfastdicomstructure_c.so`, `fds_*` only)

```
fds_abi_version, fds_element_is_sequence, fds_element_sequence_item_count,
fds_element_sequence_item_element_at, fds_element_sequence_item_element_count, fds_element_tag,
fds_element_value_bytes, fds_element_vr, fds_parse_buffer, fds_parse_file,
fds_parse_options_init_defaults, fds_status_message, fds_structure_contains,
fds_structure_diagnostic_at, fds_structure_diagnostic_count, fds_structure_element_at,
fds_structure_element_count, fds_structure_erase, fds_structure_erase_private,
fds_structure_erase_recursive, fds_structure_find, fds_structure_free, fds_structure_is_modified,
fds_structure_pixel_data_kind, fds_structure_set, fds_structure_set_value,
fds_structure_set_value_recursive, fds_structure_transfer_syntax_is_explicit_vr,
fds_structure_transfer_syntax_is_little_endian, fds_structure_transfer_syntax_uid,
fds_structure_write_buffer, fds_structure_write_buffer_with_stats, fds_structure_write_file,
fds_structure_write_file_with_stats
```

33 exported `fds_*` symbols. This exact list, unchanged, is the ABI-compatibility target for A0 —
every one of these function names is expected to still exist, doing the same thing, in the
extracted library (only the shared-library filename changes, from `libfastdicomstructure_c.so` to
`libfastdicomattrs_c.so`).

### C++ public headers (18, under `include/fastdicomstructure/`)

`dicom_structure.hpp`, `element.hpp`, `element_path.hpp`, `fastdicomstructure.hpp` (umbrella),
`item.hpp`, `parse.hpp`, `parse_diagnostic.hpp`, `parse_options.hpp`, `parse_result.hpp`,
`pixel_data_reference.hpp`, `sequence.hpp`, `source.hpp`, `source_span.hpp`, `tag.hpp`,
`transfer_syntax.hpp`, `value.hpp`, `value_length.hpp`, `vr.hpp`, `write_result.hpp`.

## Definition of success against this baseline

A0 is successful only if, after extraction:

- the same 115 C++ tests and 46 Python tests (content unchanged except mechanical import-path
  edits) still pass;
- the corpus run against the same local NLST directory reproduces the same counts (2,832
  discovered / 2,831 parsed / 1 warning / 0 failed / 2,831 round-trip-identical / 1 pydicom
  mismatch), not merely "no new failures" but the *same* numbers;
- the same 33 `fds_*` ABI symbols exist and behave the same way;
- `fastDICOMgateway`'s 74-passed/1-skipped result is reproduced unchanged;
- parse/write timing and peak RSS stay in the same order of magnitude (no gross regression;
  minor differences attributable to the rebuild are recorded, not chased).
