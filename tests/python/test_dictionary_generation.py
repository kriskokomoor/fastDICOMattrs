"""Automated checks on the dictionary generator itself -- determinism and
freshness of the committed generated artifact. No pydicom/DCMTK dependency
(unlike tools/validate_against_pydicom.py).

As of the PS3.6.xml public-release remediation, the complete NEMA source
XML this class regenerates from is no longer redistributed in the
repository tree (see THIRD_PARTY_NOTICES.md) -- only a maintainer who has
run tools/fetch_ps3.6_source.py has it locally. This class is therefore a
maintainer-only regeneration qualification, not an ordinary correctness
test of the library itself (that coverage lives in
tests/unit/test_dictionary.cpp and this suite's other files, which exercise
the already-compiled-in generated data and never touch the source XML).
It skips cleanly, as a whole class, when the source is absent -- the same
established convention this project family already uses for
Docker/pydicom/DCMTK-dependent tests elsewhere.

See docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md for the full
picture this is one piece of.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "tools" / "generate_dictionary.py"
SOURCE = REPO_ROOT / "thirdparty" / "dicom_standard" / "PS3.6.xml"
COMMITTED_OUTPUT = REPO_ROOT / "src" / "dictionary_data.generated.cpp"

_SKIP_REASON = (
    f"{SOURCE} is not present -- it is a maintainer-only acquisition "
    "(see tools/fetch_ps3.6_source.py), not part of the redistributed "
    "release tree. This class qualifies dictionary regeneration for "
    "maintainers who have that file locally; it is not an ordinary "
    "correctness test of the compiled-in dictionary."
)


def _run_generator(output_path: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(GENERATOR), "--source", str(SOURCE), "--output", str(output_path)],
        cwd=str(REPO_ROOT), capture_output=True, text=True,
    )
    assert result.returncode == 0, f"generator failed: {result.stderr}"


@unittest.skipUnless(SOURCE.exists(), _SKIP_REASON)
class DictionaryGenerationTest(unittest.TestCase):
    def test_two_independent_runs_produce_byte_identical_output(self):
        """Requirement: 'Run the generator twice from the same pinned
        source and prove the generated outputs are identical.'"""
        with tempfile.TemporaryDirectory() as tmp:
            out_a = Path(tmp) / "run_a.cpp"
            out_b = Path(tmp) / "run_b.cpp"
            _run_generator(out_a)
            _run_generator(out_b)
            content_a = out_a.read_bytes()
            content_b = out_b.read_bytes()
            self.assertEqual(
                content_a, content_b,
                "two generator runs against the same pinned source produced different output",
            )

    def test_committed_output_matches_fresh_generation(self):
        """The committed src/dictionary_data.generated.cpp must always be
        exactly what tools/generate_dictionary.py produces from the
        currently-pinned source -- catches a source or generator change
        that was not followed by regeneration."""
        with tempfile.TemporaryDirectory() as tmp:
            fresh = Path(tmp) / "fresh.cpp"
            _run_generator(fresh)
            self.assertEqual(
                fresh.read_bytes(), COMMITTED_OUTPUT.read_bytes(),
                "the committed generated dictionary is stale: re-run "
                "tools/generate_dictionary.py and commit its output",
            )

    def test_generator_rejects_a_corrupted_source_row_count(self):
        """Requirement: 'reject malformed or unsupported input rather than
        silently skipping it.' Deleting one complete, well-formed row from
        table_6-1 (leaving the rest of the XML document entirely valid)
        must trip the EXPECTED_TOTAL_ROWS guard, not silently produce a
        smaller-but-plausible-looking table."""
        import re
        sys.path.insert(0, str(REPO_ROOT / "tools"))
        import importlib
        gen = importlib.import_module("generate_dictionary")

        text = SOURCE.read_text(encoding="utf-8")
        # Remove exactly one well-formed <tr valign="top">...</tr> data row
        # from *inside table_6-1 specifically* (anchored between its own
        # opening tag and the next table's, table_7-1's) -- the rest of the
        # document, including every other table, stays byte-for-byte valid
        # XML, so a resulting failure can only come from the row-count
        # guard, not a parse error.
        table_start = text.index('xml:id="table_6-1"')
        # Skip past <thead>...</thead> so the removed row comes from
        # <tbody> (what extract() actually counts), not the header row.
        start = text.index("<tbody>", table_start)
        end = text.index('xml:id="table_7-1"', start)
        pattern = re.compile(r'<tr valign="top">.*?</tr>\s*', re.S)
        section, count = pattern.subn("", text[start:end], count=1)
        self.assertEqual(count, 1, "test setup failed to find a removable row")
        one_row_removed = text[:start] + section + text[end:]

        with tempfile.TemporaryDirectory() as tmp:
            bad_source = Path(tmp) / "one_row_missing.xml"
            bad_source.write_text(one_row_removed, encoding="utf-8")
            with self.assertRaises(gen.GenerationError):
                gen.extract(str(bad_source))


if __name__ == "__main__":
    unittest.main()
