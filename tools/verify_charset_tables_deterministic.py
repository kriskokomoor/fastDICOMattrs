#!/usr/bin/env python3
"""Verifies tools/generate_charset_tables.py is byte-deterministic and that
the committed src/charset_tables.generated.cpp matches what the generator
produces right now, from this checkout, in this environment.

This is an offline, dev-time check -- not part of the CMake build or
ordinary `ctest` run (matching project policy: ordinary test execution does
not depend on Python regeneration). Run it directly, or via the automated
regression in tests/python/test_charset_tables_generation.py, which invokes
the same generator module this script uses and asserts the same properties
as part of `pytest tests/python`.

Usage:
    python3 tools/verify_charset_tables_deterministic.py
"""

from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GENERATOR = REPO_ROOT / "tools" / "generate_charset_tables.py"
COMMITTED = REPO_ROOT / "src" / "charset_tables.generated.cpp"


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def regenerate(output_path: Path) -> str:
    subprocess.run(
        [sys.executable, str(GENERATOR), "--output", str(output_path), "--report", "-"],
        check=True, capture_output=True,
    )
    return sha256_of(output_path)


def main() -> int:
    if not COMMITTED.exists():
        print(f"committed file not found: {COMMITTED}", file=sys.stderr)
        return 2

    committed_hash = sha256_of(COMMITTED)

    with tempfile.TemporaryDirectory(prefix="fastdicomattrs-charset-gen-") as tmp:
        out1 = Path(tmp, "regen1.cpp")
        out2 = Path(tmp, "regen2.cpp")
        hash1 = regenerate(out1)
        hash2 = regenerate(out2)

        bytes1 = out1.read_bytes()
        bytes2 = out2.read_bytes()

    ok = True
    print(f"committed file hash:   {committed_hash}")
    print(f"regeneration #1 hash:  {hash1}")
    print(f"regeneration #2 hash:  {hash2}")

    if hash1 != hash2:
        print("FAIL: two consecutive regenerations are not byte-identical "
              "-- the generator is not deterministic", file=sys.stderr)
        ok = False
    elif bytes1 != bytes2:
        print("FAIL: hashes matched but byte comparison did not (should be impossible)",
              file=sys.stderr)
        ok = False

    if hash1 != committed_hash:
        print("FAIL: regenerating from the current source does not reproduce the "
              "committed src/charset_tables.generated.cpp -- it is stale or was hand-edited",
              file=sys.stderr)
        ok = False

    if ok:
        print("OK: generation is byte-deterministic and matches the committed table")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
