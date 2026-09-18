import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

import fastdicomattrs as fds
from fastdicomattrs.corpus import (
    CorpusReport, TransformReport, _apply_readme_example_policy, _blocking_diagnostics,
    _compare_structural,
    discover_files,
)


class CorpusHelpersTest(unittest.TestCase):
    def test_discovery_is_recursive_and_sorted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "nested").mkdir()
            (root / "z.dcm").write_bytes(b"z")
            (root / "a.dcm").write_bytes(b"a")
            (root / "nested" / "b.dcm").write_bytes(b"b")
            self.assertEqual(
                [path.relative_to(root).as_posix() for path in discover_files(root)],
                ["a.dcm", "z.dcm", "nested/b.dcm"],
            )

    def test_report_uses_nearest_rank_p95_and_requested_layout(self):
        report = CorpusReport(files_discovered=3142, parsed_successfully=3137,
                              success_with_warnings=5,
                              parse_times_ms=[0.10, 0.20, 0.34, 1.12])
        rendered = str(report)
        self.assertIn("Files discovered             3,142", rendered)
        self.assertIn("Median parse                 0.27 ms", rendered)
        self.assertIn("P95 parse                    1.12 ms", rendered)

    def test_report_omits_reference_rows_when_not_compared(self):
        report = CorpusReport(files_discovered=1, parsed_successfully=1)
        self.assertNotIn("Reference", str(report))


# Minimal stand-ins for the two libraries' element shapes, exercising
# _compare_structural() without requiring pydicom to be installed for the
# core test suite (it's an optional dependency of --reference pydicom only
# -- see corpus.py's module docstring). Real cross-library validation
# happens by actually running --reference pydicom against a real corpus
# (see docs/corpus-results.md), which this unit-level test can't replace.
class _FdsElement:
    def __init__(self, tag, is_sequence=False, items=None):
        self.tag = tag
        self.is_sequence = is_sequence
        self._items = items or []

    def items(self):
        return self._items


class _PydicomTag:
    def __init__(self, group, element):
        self.group = group
        self.element = element


class _PydicomElement:
    def __init__(self, tag, vr="LO", value=None):
        self.tag = _PydicomTag(*tag)
        self.VR = vr
        self.value = value if value is not None else []


class CompareStructuralTest(unittest.TestCase):
    def test_matches_when_tag_sets_and_sequence_shapes_agree(self):
        fds = [_FdsElement((0x0008, 0x0060)), _FdsElement((0x0010, 0x0020))]
        py = [_PydicomElement((0x0008, 0x0060)), _PydicomElement((0x0010, 0x0020))]
        self.assertEqual(_compare_structural(fds, py, "top-level"), [])

    def test_reports_tags_only_pydicom_sees(self):
        fds = [_FdsElement((0x0008, 0x0060))]
        py = [_PydicomElement((0x0008, 0x0060)), _PydicomElement((0x0010, 0x0020))]
        mismatches = _compare_structural(fds, py, "top-level")
        self.assertEqual(len(mismatches), 1)
        self.assertIn("pydicom sees but fastdicomattrs does not", mismatches[0])

    def test_reports_tags_only_fastdicomattrs_sees(self):
        fds = [_FdsElement((0x0008, 0x0060)), _FdsElement((0x0009, 0x0010))]
        py = [_PydicomElement((0x0008, 0x0060))]
        mismatches = _compare_structural(fds, py, "top-level")
        self.assertEqual(len(mismatches), 1)
        self.assertIn("fastdicomattrs sees but pydicom does not", mismatches[0])

    def test_pixel_data_is_excluded_from_the_pydicom_side(self):
        fds = [_FdsElement((0x0008, 0x0060))]
        py = [_PydicomElement((0x0008, 0x0060)), _PydicomElement((0x7FE0, 0x0010), vr="OW")]
        self.assertEqual(_compare_structural(fds, py, "top-level"), [])

    def test_matching_sequence_recurses_into_items(self):
        fds_item_elements = [_FdsElement((0x0008, 0x0100))]
        fds = [_FdsElement((0x0008, 0x1140), is_sequence=True, items=[fds_item_elements])]
        py_item_elements = [_PydicomElement((0x0008, 0x0100))]
        py = [_PydicomElement((0x0008, 0x1140), vr="SQ", value=[py_item_elements])]
        self.assertEqual(_compare_structural(fds, py, "top-level"), [])

    def test_sequence_item_count_mismatch_is_reported(self):
        fds = [_FdsElement((0x0008, 0x1140), is_sequence=True, items=[[]])]
        py = [_PydicomElement((0x0008, 0x1140), vr="SQ", value=[[], []])]
        mismatches = _compare_structural(fds, py, "top-level")
        self.assertEqual(len(mismatches), 1)
        self.assertIn("item count differs", mismatches[0])

    def test_nested_sequence_mismatch_is_reported_with_its_own_path(self):
        inner_mismatched_fds = [_FdsElement((0x0008, 0x0100))]
        inner_mismatched_py = [_PydicomElement((0x0008, 0x0100)), _PydicomElement((0x0008, 0x0102))]
        fds = [_FdsElement((0x0008, 0x1140), is_sequence=True, items=[inner_mismatched_fds])]
        py = [_PydicomElement((0x0008, 0x1140), vr="SQ", value=[inner_mismatched_py])]
        mismatches = _compare_structural(fds, py, "top-level")
        self.assertEqual(len(mismatches), 1)
        self.assertIn("top-level (8, 4416)[0]", mismatches[0])


def _u16(value):
    return struct.pack("<H", value)


def _u32(value):
    return struct.pack("<I", value)


def _tag(group, element):
    return _u16(group) + _u16(element)


def _element_short(group, element, vr, value: bytes) -> bytes:
    if len(value) % 2 != 0:
        value += b"\x00"
    return _tag(group, element) + vr.encode("ascii") + _u16(len(value)) + value


def _build_dataset() -> bytes:
    ts_uid = b"1.2.840.10008.1.2.1"
    group_body = (
        _element_short(0x0002, 0x0002, "UI", b"1.2.3.4")
        + _element_short(0x0002, 0x0003, "UI", b"1.2.3.4.5")
        + _element_short(0x0002, 0x0010, "UI", ts_uid)
    )
    file_meta = _element_short(0x0002, 0x0000, "UL", _u32(len(group_body))) + group_body
    dataset = (
        _element_short(0x0008, 0x0060, "CS", b"CT")
        + _element_short(0x0009, 0x0010, "LO", b"PRIVATE")
        + _element_short(0x0010, 0x0010, "PN", b"Doe^Jane")
        + _element_short(0x0010, 0x0020, "LO", b"ID1")
        + _element_short(0x0018, 0x0050, "DS", b"1.0")
    )
    return b"\x00" * 128 + b"DICM" + file_meta + dataset


class ApplyReadmeExamplePolicyTest(unittest.TestCase):
    """_apply_readme_example_policy() needs no pydicom -- it's pure use of
    this library's own mutation API. The pydicom-dependent validation half
    (_validate_transformed_output/transform_and_validate) is exercised by
    actually running --transform readme-example against a real corpus (see
    docs/corpus-results.md), not here.
    """

    def test_removes_name_hashes_id_removes_private_preserves_the_rest(self):
        structure = fds.read_buffer(_build_dataset(), fidelity="lossless")
        try:
            original_id_bytes = structure.get((0x0010, 0x0020)).value  # b"ID1\x00", as stored
            expected_hash = hashlib.sha256(original_id_bytes).hexdigest().encode("ascii")

            _apply_readme_example_policy(structure)

            self.assertNotIn((0x0010, 0x0010), structure)  # PatientName removed
            self.assertNotIn((0x0009, 0x0010), structure)  # private element removed
            self.assertEqual(structure.get((0x0010, 0x0020)).value, expected_hash)  # hashed
            self.assertEqual(structure.get((0x0008, 0x0060)).value, b"CT")  # Modality preserved
            self.assertIn((0x0018, 0x0050), structure)  # SliceThickness preserved
        finally:
            structure.close()

    def test_is_a_noop_when_patient_id_is_absent(self):
        # Guards against a crash if a file has no PatientID -- "hash" should
        # just have nothing to do, not raise.
        data = _build_dataset()
        structure = fds.read_buffer(data, fidelity="lossless")
        try:
            structure.erase((0x0010, 0x0020))
            _apply_readme_example_policy(structure)  # must not raise
            self.assertNotIn((0x0010, 0x0020), structure)
        finally:
            structure.close()


class TransformReportTest(unittest.TestCase):
    def test_preserved_fraction_is_vacuously_one_when_nothing_written(self):
        self.assertEqual(TransformReport().preserved_fraction, 1.0)

    def test_str_includes_preserved_fraction(self):
        report = TransformReport(files_discovered=3, transformed=2, ineligible_diagnostics=1,
                                  pydicom_valid=2,
                                  total_source_backed_value_bytes=99,
                                  total_regenerated_value_bytes=1)
        rendered = str(report)
        self.assertIn("Preserved fraction", rendered)
        self.assertIn("99.00%", rendered)
        self.assertIn("Ineligible (diagnostics)", rendered)
        self.assertIn("pydicom metadata-readable", rendered)

    def test_noninformational_diagnostic_blocks_transformation(self):
        class _Diagnostic:
            def __init__(self, severity):
                self.severity = severity

        class _Structure:
            diagnostics = [_Diagnostic("info"), _Diagnostic("recoverable_error")]

        self.assertEqual(len(_blocking_diagnostics(_Structure())), 1)


if __name__ == "__main__":
    unittest.main()
