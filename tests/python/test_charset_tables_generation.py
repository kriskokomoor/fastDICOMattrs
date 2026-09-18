"""Automated checks on the charset-table generator itself -- determinism
and freshness of the committed generated artifact. No pydicom/DCMTK
dependency; part of the ordinary test suite (mirrors
test_dictionary_generation.py's role for the PS3.6 dictionary generator).

See docs/architecture/A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "tools" / "generate_charset_tables.py"
COMMITTED_OUTPUT = REPO_ROOT / "src" / "charset_tables.generated.cpp"


def _run_generator(output_path: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(GENERATOR), "--output", str(output_path)],
        cwd=str(REPO_ROOT), capture_output=True, text=True,
    )
    assert result.returncode == 0, f"generator failed: {result.stderr}"


class CharsetTablesGenerationTest(unittest.TestCase):
    def test_two_independent_runs_produce_byte_identical_output(self):
        """No timestamp, machine path, hostname, or other volatile content
        may be embedded in the generated file -- two runs from the same
        committed source and environment must be byte-identical."""
        with tempfile.TemporaryDirectory() as tmp:
            out_a = Path(tmp) / "run_a.cpp"
            out_b = Path(tmp) / "run_b.cpp"
            _run_generator(out_a)
            _run_generator(out_b)
            self.assertEqual(
                out_a.read_bytes(), out_b.read_bytes(),
                "two generator runs produced different output -- volatile content "
                "(timestamp, etc.) likely reintroduced",
            )

    def test_committed_output_matches_fresh_generation(self):
        """The committed src/charset_tables.generated.cpp must always be
        exactly what tools/generate_charset_tables.py produces right now --
        catches a generator change that was not followed by regeneration."""
        with tempfile.TemporaryDirectory() as tmp:
            fresh = Path(tmp) / "fresh.cpp"
            _run_generator(fresh)
            self.assertEqual(
                fresh.read_bytes(), COMMITTED_OUTPUT.read_bytes(),
                "the committed generated charset tables are stale: re-run "
                "tools/generate_charset_tables.py and commit its output",
            )


if __name__ == "__main__":
    unittest.main()
