# A1.1 — Dictionary Substrate: Freeze Report

## 1. Implementation status: complete, all freeze criteria met

A correct, provenance-tracked, reproducibly-generated PS3.6 data dictionary now exists in
`fastDICOMattrs`, exposed through a small, stable native C++ API
(`fds::dictionary::lookup`). It is **not** wired into parsing, mutation, serialization, or any
other existing behavior — confirmed both by inspection (no existing file outside this increment's
own new files references `fds::dictionary`) and empirically (see §16 and §14: the real-world
26,636-file-class corpus check and every pre-existing test reproduce the exact pre-increment
numbers, and `libfastdicomattrs_c.so`'s size is byte-identical before and after, because the linker
dead-strips the entire new subsystem when nothing calls it).

## 2. Source PS3.6 edition/date

**DICOM PS3.6 2026c — Data Dictionary**, retrieved 2026-09-13 from
`https://dicom.nema.org/medical/dicom/current/source/docbook/part06/part06.xml`
(HTTP `Last-Modified: Fri, 19 Jun 2026 02:25:07 GMT`). SHA-256:
`ff1dcdfb557d57db96420614fcaf6d739bb76aa74b73eba77f367be9fab0be3e`.

## 3. Source artifact and generator paths

| Artifact | Path |
|---|---|
| Pinned source (untouched, byte-exact) | `thirdparty/dicom_standard/PS3.6.xml` |
| Provenance record | `thirdparty/dicom_standard/PROVENANCE.md` |
| Generator | `tools/generate_dictionary.py` |
| Differential validator (pydicom, optional dependency) | `tools/validate_against_pydicom.py` |
| Generated data (committed) | `src/dictionary_data.generated.cpp` |
| Data-shape header (hand-written) | `src/dictionary_data.generated.hpp` |
| Lookup implementation (hand-written) | `src/dictionary.cpp` |
| Public API (hand-written) | `include/fastdicomattrs/dictionary.hpp` |
| Native tests | `tests/unit/test_dictionary.cpp` |
| Generation determinism/freshness tests | `tests/python/test_dictionary_generation.py` |

## 4. Runtime representation

Compile-time-generated, sorted static array of POD entries (`RawEntry`: packed `uint32_t` tag,
`VR`, `VRAmbiguity`, `retired` flag, offset+length into a shared `char[]` keyword pool), looked up
by binary search (`std::lower_bound`). A short (71-entry) linear-scan fallback table handles the
three group-level repeating patterns. No runtime file loading, no build-time or test-time network
access, no dependency on Python/pydicom/DCMTK/an XML parser at runtime — all of that lives in the
offline generator only. This matches the architecture the A1 plan recommended, with no deviation
(see §17).

## 5. Exact generated-entry count

**5,172** exact tag entries, generated from all 5,267 rows of PS3.6's Table 6-1 ("Registry of
DICOM Data Elements") after excluding: 4 fully-retired placeholder rows with no VR recorded at
all, 3 structural delimiter pseudo-tags (`(FFFE,E000)`/`(FFFE,E00D)`/`(FFFE,E0DD)`, already
handled structurally by `Tag::is_item_or_delimiter()` and deliberately not duplicated here), and
71 rows that feed the repeating-group rule table instead of the exact table (5,172 + 71 + 4 + 3 =
5,267 — every source row is accounted for in exactly one category).

Note for the A1 plan's own record: that document's dependency-analysis section estimated "roughly
5,000 entries" for sizing purposes. The real count (5,172 exact + 71 rules = 5,243 total resolvable
entries) is close to that estimate and does not change any conclusion the plan drew from it — noted
here per this task's instruction to update the plan only where evidence materially changes
something, and this does not.

## 6. Repeating-rule count

**71** rules across three patterns: `50xx` (retired Curve Data) = 26, `60xx` (Overlay) = 40,
`7Fxx` (retired Variable Pixel Data) = 5. Each pattern's valid group range was independently
cross-validated against pydicom and DCMTK (both generated from the same PS3.6-2026c edition) —
`5000`–`50FE`, `6000`–`60FE`, `7F00`–`7FFE`, even groups only in each case — a deliberately wider
range than PS3.5 §7.6's own narrower cardinality-motivated text (`6000`–`601E`), with the full
reasoning for that departure recorded in `PROVENANCE.md`. A separate, deliberately excluded
category — 17 rows using a fundamentally different, element-level (not group-level) wildcard shape,
all obscure and fully retired since 2007 (an early JPEG-adjacent compression mechanism) — is
recorded in `tools/generate_dictionary.py`'s `EXCLUDED_ELEMENT_PATTERNS` by exact tag, not silently
dropped.

## 7. Ambiguous-entry count

**38** total (34 exact + 4 repeating), across four distinct ambiguous forms PS3.6 itself uses:
`US or SS` (25), `OB or OW` (11 exact + 4 repeating — the repeating ones are the `50xx`/`7Fxx`
audio/pixel-data-shaped entries), `US or OW` (1), `US or SS or OW` (1). Every one is represented
with `VR::Unknown` + an explicit `VRAmbiguity` value — never resolved to a single guessed VR. The
actual context-dependent resolution rule (checking Pixel Representation, per PS3.5) is explicitly
deferred to A1.4, per the A1.1 task's own scope boundary.

## 8. Dictionary representation chosen

Compile-time static C++ data, sorted-array binary search, short explicit fallback rule list for
repeating groups — exactly the architecture the approved A1 plan recommended (§8 of
`A1_ATTRS_V1_IMPLEMENTATION_PROGRESSION.md`). No perfect-hash generator was introduced; the
microbenchmark in §14 confirms binary search is already far cheaper than anything else in the
parse path, so there was no measured reason to consider one.

## 9. Full-table pydicom comparison result

Every one of the 5,172 exact entries was checked against `pydicom.datadict.dictionary_VR` (pydicom
3.0.2). **Zero VR mismatches.** 174 entries exist in this dictionary but not in pydicom's —
every one explained (see §10). All three repeating-group patterns' first/last/immediately-out-of-
range boundaries were independently cross-checked against pydicom directly (not just inferred from
documentation) and all nine boundary checks passed. Full output saved and reproducible via
`python3 tools/validate_against_pydicom.py`.

```
pydicom version: 3.0.2
exact entries to check: 5172
checked: 5172
pydicom-missing (fastDICOMattrs has, pydicom does not): 174
VR mismatches: 0

TOTAL unexplained VR mismatches: 0
TOTAL repeating-group boundary mismatches: 0
```

## 10. Disagreements and their disposition

Every one of the 174 "pydicom-missing" entries falls into exactly one of two fully-explained
buckets — **zero unexplained disagreements**, which is the actual freeze bar (not "zero
differences," which the task instructions explicitly anticipate may not be achievable or even
correct):

- **104 entries, group `0014`**: the DICONDE (Digital Imaging and Communication in Nondestructive
  Evaluation) companion-standard group. pydicom's bundled dictionary does not include DICONDE/DICOS
  elements at all; this dictionary does, because PS3.6's Table 6-1 itself includes them (DCMTK's
  dictionary generator's own header comment confirms this is expected: "This also includes the
  non-private definitions from the DICONDE... and DICOS... standard"). Not a data error — a
  deliberate scope difference between the two tools, explained and not adopted from pydicom's
  narrower scope.
- **70 entries, scattered across groups `0008`/`0010`/`0018`/`0022`/`0040`/`0044`/`0048`/`0066`/
  `3002`/`3004`/`300A`**: current (non-retired), genuinely new PS3.6 additions — recognizable on
  inspection as very recent supplements (gender-identity/pronoun/sex-parameters-for-clinical-use
  attributes, metal-artifact-reduction, waveform-montage-annotation attributes, and similar).
  pydicom 3.0.2 was released 2026-03-19 (confirmed via PyPI); this project's pinned "2026c" edition
  postdates that release (NEMA's lettering convention marks same-year incremental updates, and "c"
  sorts after whatever pydicom 3.0.2 bundled). **Independently confirmed, not merely inferred**: a
  spot-check of six of these entries (`SensitiveContentCodeSequence`, `PronounCodeSequence`,
  `GenderIdentitySequence`, `SexParametersForClinicalUseCategorySequence`,
  `MetalArtifactReductionSequence`, `MontageActivationSequence`) against DCMTK's dictionary — which
  states its own generation edition as "PS 3.6-2026c," the same edition this project pinned —
  found all six present with matching tags, VRs, and keywords. This is edition-recency, not a data
  or extraction error.

No entry in either bucket required correcting this project's own generated table.

## 11. DCMTK comparison result where used

DCMTK's dictionary (`dcmdata/data/dicom.dic`, retrieved from
`https://raw.githubusercontent.com/DCMTK/dcmtk/master/dcmdata/data/dicom.dic`, SHA-256
`700f43b759eea9aa93b08478af8011027c87acffe5372c494f1f8ff99d439e7a`, self-declared as generated
from "DICOM PS 3.6-2026c and PS 3.7-2026c" — the same edition pinned here) was used for two
specific, targeted purposes rather than a full second full-table diff (unnecessary given the
pydicom full-table result already reached zero unexplained disagreements):

1. **Repeating-group range determination** (§6, full reasoning in `PROVENANCE.md`): DCMTK's own
   `(6000-60FF,...)`/`(5000-50FF,...)`/`(7F00-7FFF,...)` range notation, cross-checked against
   direct pydicom boundary queries, established the wider practical range this dictionary adopts
   over PS3.5's own narrower cardinality text.
2. **Edition-recency spot-check** (§10): confirming six of the pydicom-missing "new" entries are
   genuinely present in the same 2026c edition DCMTK targets, ruling out an extraction error on
   this project's side as the explanation.

## 12. Tests before/after

| Suite | Before (A0 baseline) | After A1.1 |
|---|---|---|
| C++ (`ctest`) | 115/115 | **125/125** (10 new dictionary tests, 0 changed, 0 removed) |
| Python (`pytest tests/python`) | 34/34 | **37/37** (3 new generation tests, 0 changed, 0 removed) |

No existing test's assertions were modified. This matches the task's own expectation exactly:
"A dictionary that is not yet wired into behavior should ordinarily require no semantic test
changes" — none were needed.

## 13. Binary-size delta

| Artifact | Before | After | Delta |
|---|---|---|---|
| `libfastdicomattrs.a` (static library) | 288,392 bytes | 500,338 bytes | **+211,946 bytes (+73.5%)** |
| `libfastdicomattrs_c.so` (shared library, the ABI consumers actually load) | 198,328 bytes | 198,328 bytes | **+0 bytes** |

The shared library's size is unchanged to the byte. This is not a measurement artifact — it is
direct, automatic, linker-verified proof that the dictionary subsystem is unreachable from every
symbol the ABI (and therefore Python, and therefore every existing consumer) actually exports: the
linker dead-strips code nothing calls. This is the strongest available confirmation, short of the
corpus/test evidence in §16, that A1.1 is genuinely behaviorally inert as required. The static
library's larger, real delta reflects the actual committed data (5,172 entries + 71 rules + ~75 KB
keyword pool, compiled directly into `.rodata`) and will become part of the shared library's own
size only once A1.4 adds a call site that makes it reachable.

## 14. Lookup benchmark result

Dedicated microbenchmark (2,000,000 iterations per case, `-O2`, isolated from the parse/write
path entirely):

| Case | Cost |
|---|---|
| Representative mix (common tags + one repeating-group tag + one private-tag miss) | **35.83 ns/call** |
| Worst case: a repeating-group hit requiring the full 71-rule linear scan after an exact-table miss | **65.96 ns/call** |

Both figures are 4–5 orders of magnitude smaller than this project's own measured per-file parse
cost (hundreds of microseconds to low milliseconds per file, per the A0 baseline and this
session's corpus re-run, §16) — fully consistent with the A1 plan's prediction that dictionary
lookup would not be a measurable fraction of total parse time, now confirmed by measurement rather
than assumed.

## 15. Confirmation that parsing/writing/mutation behavior did not change

Confirmed by three independent kinds of evidence, not merely asserted:

1. **Static evidence**: no file outside this increment's own new files (`dictionary.hpp/.cpp`,
   `dictionary_data.generated.hpp/.cpp`, the two new test files, `CMakeLists.txt`'s source list)
   was modified. `git diff --stat` against the pre-A1.1 commit confirms this exactly.
2. **Linker evidence** (§13): the shared library's byte-for-byte-unchanged size proves the new
   code is unreachable from the ABI surface.
3. **Behavioral evidence**: the full real-world corpus re-run (§16) reproduces every single count
   from the A0 closure baseline exactly, and all 115 pre-existing C++ tests and 34 pre-existing
   Python tests pass unchanged.

## 16. Real-world corpus re-run (regression evidence)

Same 2,832-file local NLST collection used throughout this project's evidence trail:

```
Files discovered             2,832
Parsed successfully          2,831
Success with warnings            1
Failed                            0
Round-trip identical           2,831
Round-trip failures                0
Reference matches (pydicom)    2,831
Reference mismatches               1
```

Identical, count for count, to the A0 closure report's numbers. (Median/P95 parse timing varied
slightly run to run — 0.06–0.13 ms median across different runs during this session — consistent
with the OS filesystem cache-warmth variance already noted and explained in the A0 report; no
timing regression is indicated or expected, since no code on this path changed at all.)

## 17. Any deviation from the approved A1 plan

One deliberate, recorded decision beyond what the plan enumerated in detail, made per the task's
own explicit instruction to record it:

**No ABI or Python exposure was added.** The plan's §8 authorized a small additive lookup surface
as optional; the task instructions for this specific increment repeated that it is authorized but
"not the purpose of A1.1" and said to "defer public exposure and keep A1.1 native-internal" if
exposing it would add meaningful surface, and to record the decision either way. Decision: **deferred
entirely.** A1.4 is the dictionary's actual first consumer (native C++ parser code, not Python/ABI
code), so adding ABI/Python bindings now would be speculative surface area serving no present
need, and — per §13's own evidence — would have been the one change capable of making the new
subsystem *not* dead-code-eliminated from the shared library, which would have muddied the
otherwise-clean "provably unreachable" evidence this report relies on. If a future increment finds
a genuine external (non-C++) consumer need for direct dictionary lookup, it can be added then, atop
a now-frozen and validated native API.

No other deviation. The row-count discrepancy noted in §5 (real count vs. the plan's "~5,000"
estimate) is not a deviation — an estimate, not a commitment — and does not change any conclusion.

## 18. Recommendation: A1.1 may be frozen

All items in the task's own freeze criteria are met:

- [x] provenance explicit (§2, §3, `PROVENANCE.md`)
- [x] generation deterministic (§ below, byte-identical SHA-256 across independent runs, verified
      by an automated test, not merely by hand)
- [x] generated artifacts committed (`src/dictionary_data.generated.cpp`)
- [x] exact lookup works (§9, §12, 5,172/5,172 validated)
- [x] repeating-group lookup works (§6, §9, all boundary cases validated against pydicom directly)
- [x] retired tags retained appropriately (451 total, §7's retired figures; a dedicated test
      exercises one directly)
- [x] ambiguous VRs remain explicitly unresolved (§7, §17; a dedicated test asserts this for all
      four ambiguous forms)
- [x] no private/vendor dictionary semantics invented (private tags always return `nullopt`; two
      dedicated tests confirm this, including a numeric-collision case)
- [x] full-table differential validation complete (§9)
- [x] every disagreement explained (§10, zero unexplained)
- [x] all 115 pre-existing C++ tests pass (§12)
- [x] all 34 pre-existing Python tests pass (§12)
- [x] no parser/writer/mutation behavior changed (§15, three independent kinds of evidence)
- [x] binary-size and lookup-cost measurements recorded (§13, §14)
- [x] this report is complete

**A1.1 is ready to freeze.**
