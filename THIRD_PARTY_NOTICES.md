# Third-party notices

This repository's own code (including the generated files listed below) is released under
[The Unlicense](LICENSE) — a public-domain dedication. **The Unlicense applies to this project's
own original work. It does not, and cannot, relicense third-party material.** NEMA owns the DICOM
Standard material discussed below; this project claims no ownership of it.

## NEMA DICOM PS3.6 (Data Dictionary), docbook XML — not redistributed

**The complete `PS3.6.xml` source file is not present in this repository's current tree, and is
not redistributed going forward.** It was previously vendored at
`thirdparty/dicom_standard/PS3.6.xml`, introduced at commit `290cb90`; that historical commit is
unchanged and still contains it (see "Historical note" below), but no commit from this point
forward carries it. The generated dictionary this project actually ships and uses
(`src/dictionary_data.generated.{hpp,cpp}`) remains committed — see "What ships instead," below.

- **Copyright, as recorded from the file when it was present:**
  `<copyright><year>2026</year><holder>NEMA</holder></copyright>`. The document is a NEMA/DICOM
  Standards Committee publication.
- **Trademark notice, as recorded from the file when it was present:** "DICOM® is the registered
  trademark of the National Electrical Manufacturers Association for its standards publications
  relating to digital communications of medical information, all rights reserved."
- **Source, edition, and SHA-256:** recorded in full in
  [`thirdparty/dicom_standard/PROVENANCE.md`](thirdparty/dicom_standard/PROVENANCE.md) — retrieval
  URL, retrieval date, edition string, file size, and SHA-256, exactly as originally recorded and
  unweakened by this remediation, so the exact source the shipped dictionary derives from remains
  independently verifiable.

### Redistribution finding (researched 2026-09-17; not reopened by this remediation)

**Authoritative source consulted:** "Policies and Procedures for the DICOM Standards Committee,"
April 2020, published by the DICOM Standards Committee (Secretariat: MITA/NEMA), retrieved from
`https://www.dicomstandard.org/docs/librariesprovider2/dicomdocuments/wp-content/uploads/2017/11/dicom-policies-and-procedures-2020-04.pdf`.
Section 10 ("Trademark and Copyrights") is the operative text.

- **§10.2 Copyright:** "The Secretariat is entrusted with ownership of the copyright in the
  Standard (regardless of format, whether in print or digital)..." — PS3.6 in XML form is
  therefore squarely inside this copyright, not outside it merely because it is machine-readable.
- **§10.3 Excerpts from the Standard:** grants a specific, narrow, royalty-free permission: "to
  copy, use, and publish **excerpts** from a DICOM Standards Publication... for the purpose of
  incorporating such excerpts in other works, including product manuals, other standards
  publications, user guidelines, and educational materials," conditioned on (1) attribution
  ("DICOM Part(s) ___, © NEMA"), (2) a stated continuous-maintenance/current-edition notice
  pointing at dicomstandard.org, (3) **the text may NOT be modified**, plus standard
  liability/warranty disclaimers.
- **Closing sentence of §10.3 (decisive):** "Nothing in this permission is intended to reduce,
  limit, or restrict any rights arising from fair use, first sale or other limitations... **To
  copy, use, publish, and distribute portions of a DICOM Standards Publication for which
  permission is not granted hereby, written permission must be obtained.**"

**Interpretation, evidence versus inference (no legal conclusion is asserted beyond this):**
§10.3's granted permission is scoped to *excerpts incorporated into another work* (a manual, a
guideline, educational material) — not to committing an entire Part's complete docbook XML source,
essentially verbatim in full (every chapter and annex, not only the one table this project uses),
into a public software repository as a build-time dependency. That specific redistribution mode is
not one of the enumerated permitted uses in the text found, and no separate license or terms file
was found alongside the source XML tree itself
(`https://dicom.nema.org/medical/dicom/current/source/docbook/`, checked directly — no
README/LICENSE present there). The fact that other DICOM software projects are known to vendor
similar source material is **not** treated as evidence of permission here.

**Disposition: B — redistribution permission unclear/conditional. Resolved by exclusion.** This
project has not obtained written permission from NEMA/the DICOM Standards Committee for
redistributing the complete `PS3.6.xml` file, so it is no longer part of the current tree. If
written permission is ever obtained, this section should be updated accordingly rather than the
file being silently re-added.

**Historical note:** the file was committed at `290cb90` ("A1.1: pin PS3.6 source artifact with
full provenance"), well before the frozen `46bf7d3` A1.7 HEAD. That commit, and the file inside it,
are unchanged — removing the file from the current tree is an ordinary forward `git rm` commit, not
a history rewrite, and does not erase or alter that historical record.

## What ships instead: the generated dictionary tables

`src/dictionary_data.generated.{hpp,cpp}` are what the library actually compiles and uses, and
**remain committed** — nothing about their content or provenance metadata changed in this
remediation. They do not reproduce PS3.6's text, artwork, or document structure; they are a table
of extracted facts (tag numbers, VR letters, keywords, repeating-group ranges) in this project's
own generated C++ source form, produced by this project's own extraction/normalization pass (see
`tools/generate_dictionary.py` and the A1.1 freeze report) applied to publicly known facts. As a
general matter of copyright law (not a DICOM-specific rule, and not confirmed against DICOM-specific
case law), bare facts and short data values are typically treated differently from the expressive
text/arrangement of a document — this is a materially different, and considerably weaker,
redistribution concern than committing the complete source XML was. It is still noted here rather
than asserted as fully settled, since it was not independently confirmed against authoritative
legal guidance.

The generated dictionary contains machine-oriented tag metadata derived from the cited standard
source. The project does not claim ownership of the underlying DICOM Standard.

## Maintainer regeneration workflow

Adopting a new PS3.6 edition, or independently re-verifying the committed dictionary, requires a
maintainer to explicitly re-acquire the source themselves — this is never automatic:

1. Run `python3 tools/fetch_ps3.6_source.py`. It downloads the exact URL recorded in
   `PROVENANCE.md`, verifies the result's SHA-256 against the pinned expected value, and refuses
   to proceed (with no file written) if they don't match — a mismatch means NEMA has published a
   new edition, which is a deliberate maintainer decision, not something handled silently.
   The file is written to `thirdparty/dicom_standard/PS3.6.xml`, a path this repository's
   `.gitignore` excludes, so it is never accidentally re-committed.
2. Run `python3 tools/generate_dictionary.py --source thirdparty/dicom_standard/PS3.6.xml --output
   /tmp/dictionary_data.generated.cpp --report -` and diff the result against the committed
   `src/dictionary_data.generated.cpp`.
3. Adopting a new edition (a real diff, not just reproducing the same one) means committing that
   diff deliberately, updating `PROVENANCE.md` and this script's expected SHA-256, and re-running
   the full test suite — the same review discipline as any other A-series change.

**Normal users, `pip install`, `cmake` configuration/build, `pytest`, and `import fastdicomattrs`
never invoke this workflow, never need `PS3.6.xml`, and never make a network request.** This was
independently verified: a from-scratch CMake configure/build with no `PS3.6.xml` anywhere on the
system passed all 312 native tests; the Python suite passed 62/65 with the remaining 3 (in
`tests/python/test_dictionary_generation.py`, a maintainer-only regeneration-qualification class)
skipping cleanly with an explicit reason, rather than failing.

Regenerating from the qualified source was independently re-verified to reproduce the committed
dictionary byte-for-byte (SHA-256 `a0ba845c8f7cc6a5cfba895200f63d51f06872259990b5119ecc341adb` for
`src/dictionary_data.generated.cpp`, both from the pre-existing pinned source and from a fresh
re-download via `tools/fetch_ps3.6_source.py`).

## Generated: charset tables

- **Files:** `src/charset_tables.generated.{hpp,cpp}`, produced by `tools/generate_charset_tables.py`.
- **Source:** Python's standard-library `codecs` module, reflecting the frozen ISO/IEC 8859 and TIS 620 character-set standards — not a DICOM-specific artifact, and not vendored source text (no third-party file is committed for this generator; it reads encoding tables already present in a standard Python installation at generation time).
- **Redistribution basis:** the generated output is this project's own transformation of publicly specified, standard character encodings; no separate third-party file is redistributed.

## Fixture data

Test fixtures under `tests/fixtures/` and `tests/integration/` are synthetic, built in-code or as small synthetic byte sequences for this project's own tests — not derived from real patient data or a third-party corpus. The real-world corpora referenced in `docs/corpus-results.md` and `docs/benchmarks.md` (CMB-MEL, NLST, via public TCIA collections) are **not committed to this repository**; only the resulting aggregate counts and measurements are.

## What is NOT covered by the above

Nothing else in this repository is currently known to embed third-party copyrighted material. This is the result of a pattern/path scan performed during release preparation, not an exhaustive legal audit.
