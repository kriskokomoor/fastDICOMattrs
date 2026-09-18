#!/usr/bin/env python3
"""Maintainer-only acquisition of the pinned NEMA PS3.6 source XML.

This is NOT part of the normal build, install, or test path -- no normal
user, `pip install`, `cmake` invocation, or CI job for ordinary builds/tests
runs this script or needs network access. It exists solely so a maintainer
who wants to *regenerate* the dictionary (see tools/generate_dictionary.py)
can reproducibly re-obtain the exact same authoritative source this project
already used, without that source being redistributed inside this
repository's own git history going forward (see THIRD_PARTY_NOTICES.md for
why: NEMA's own published policy does not clearly authorize redistributing
the complete DocBook XML of a Standard Part as a repository dependency).

What this script does, and nothing else:
    1. Prints the documented authoritative source URL and expected edition.
    2. Downloads that URL to thirdparty/dicom_standard/PS3.6.xml (a path
       this repository's .gitignore excludes, so a re-acquired copy is
       never accidentally re-committed).
    3. Verifies the downloaded file's SHA-256 against the value already
       recorded in thirdparty/dicom_standard/PROVENANCE.md.
    4. Prints the next manual steps (regenerate, diff against committed
       output) -- it does not itself invoke the generator or touch any
       other file, so a maintainer always sees and reviews what changes.

If NEMA's source or edition has changed and the SHA-256 no longer matches,
this script deliberately stops and does not proceed -- adopting a new
edition is a maintainer decision (see tools/generate_dictionary.py's own
docstring and docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md), not
something this script should silently paper over.

Usage:
    python3 tools/fetch_ps3.6_source.py
"""

from __future__ import annotations

import hashlib
import sys
import urllib.request
from pathlib import Path

SOURCE_URL = "https://dicom.nema.org/medical/dicom/current/source/docbook/part06/part06.xml"
EXPECTED_EDITION = "DICOM PS3.6 2026c - Data Dictionary"
EXPECTED_SHA256 = "ff1dcdfb557d57db96420614fcaf6d739bb76aa74b73eba77f367be9fab0be3e"

REPO_ROOT = Path(__file__).resolve().parent.parent
DEST = REPO_ROOT / "thirdparty" / "dicom_standard" / "PS3.6.xml"


def main() -> int:
    print(f"Fetching authoritative PS3.6 source from:\n  {SOURCE_URL}")
    print(f"Expected edition: {EXPECTED_EDITION}")
    print(f"Expected SHA-256: {EXPECTED_SHA256}\n")

    if DEST.exists():
        print(f"Refusing to overwrite an existing file at {DEST}.")
        print("Remove it yourself first if you intend to re-fetch.")
        return 1

    try:
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as response:
            data = response.read()
    except OSError as exc:
        print(f"Download failed: {exc}", file=sys.stderr)
        return 1

    actual_sha256 = hashlib.sha256(data).hexdigest()
    if actual_sha256 != EXPECTED_SHA256:
        print(
            "SHA-256 MISMATCH -- the retrieved file does not match the pinned "
            "provenance recorded in thirdparty/dicom_standard/PROVENANCE.md.\n"
            f"  expected: {EXPECTED_SHA256}\n"
            f"  actual:   {actual_sha256}\n"
            "This likely means NEMA has published a new edition. Adopting a new "
            "edition is a deliberate maintainer decision (re-run "
            "tools/generate_dictionary.py, review the diff, update PROVENANCE.md "
            "and this script's EXPECTED_* constants, and re-verify against "
            "docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md's own row-count "
            "guard) -- not something this script does automatically. The file was "
            "NOT written to disk.",
            file=sys.stderr,
        )
        return 1

    DEST.parent.mkdir(parents=True, exist_ok=True)
    DEST.write_bytes(data)
    print(f"SHA-256 verified. Wrote {len(data)} bytes to {DEST}.")
    print(
        "\nNext steps (manual, reviewed by you):\n"
        "  python3 tools/generate_dictionary.py \\\n"
        "      --source thirdparty/dicom_standard/PS3.6.xml \\\n"
        "      --output /tmp/dictionary_data.generated.cpp --report -\n"
        "  diff /tmp/dictionary_data.generated.cpp src/dictionary_data.generated.cpp"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
