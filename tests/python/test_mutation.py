"""Exercises Structure mutation (set_value/set/erase/erase_private) and
write() through the full Python -> C ABI -> C++ stack. Mirrors
python/examples/smoke_test.py's byte-building style but is a proper
unittest so it runs under `python3 -m unittest discover -s tests/python`.
"""

import gc
import os
import struct
import tempfile
import unittest

import fastdicomattrs as fds


def _u16(v):
    return struct.pack("<H", v)


def _u32(v):
    return struct.pack("<I", v)


def _tag(group, element):
    return _u16(group) + _u16(element)


def _element_sq(group, element, item_bytes: bytes) -> bytes:
    return _tag(group, element) + b"SQ" + b"\x00\x00" + _u32(len(item_bytes)) + item_bytes


def _dicom_item(content: bytes) -> bytes:
    return _tag(0xFFFE, 0xE000) + _u32(len(content)) + content


def _element_short(group, element, vr, value: bytes) -> bytes:
    if len(value) % 2 != 0:
        value += b"\x00"
    return _tag(group, element) + vr.encode("ascii") + _u16(len(value)) + value


def _build_dataset() -> bytes:
    ts_uid = b"1.2.840.10008.1.2.1"  # already even length
    group_body = (
        _element_short(0x0002, 0x0002, "UI", b"1.2.3.4")
        + _element_short(0x0002, 0x0003, "UI", b"1.2.3.4.5")
        + _element_short(0x0002, 0x0010, "UI", ts_uid)
    )
    file_meta = _element_short(0x0002, 0x0000, "UL", _u32(len(group_body))) + group_body

    # Deliberately in ascending tag order, like a conformant real-world file.
    dataset = (
        _element_short(0x0008, 0x0060, "CS", b"CT")
        + _element_short(0x0009, 0x0010, "LO", b"PRIVATE")
        + _element_short(0x0010, 0x0010, "PN", b"Doe^Jane")
        + _element_short(0x0010, 0x0020, "LO", b"ID1")
    )
    return b"\x00" * 128 + b"DICM" + file_meta + dataset


def _build_dataset_with_nested_patient_id() -> bytes:
    """Same as _build_dataset(), plus a standard-shaped one-item sequence
    ((0010,1002) Other Patient IDs Sequence) carrying a second, nested
    (0010,0020) PatientID -- for exercising erase_recursive/
    set_value_recursive against an occurrence set_value/erase (top-level
    only) cannot reach."""
    base = _build_dataset()
    nested_patient_id = _element_short(0x0010, 0x0020, "LO", b"NESTED-ID")
    other_patient_ids_seq = _element_sq(0x0010, 0x1002, _dicom_item(nested_patient_id))
    return base + other_patient_ids_seq


class MutationTest(unittest.TestCase):
    def setUp(self):
        self.structure = fds.read_buffer(_build_dataset(), fidelity="lossless")

    def tearDown(self):
        self.structure.close()

    def test_read_buffer_owns_source_bytes_after_caller_drops_them(self):
        # The C ABI owns a copy, so the caller's Python bytes need not remain
        # alive after read_buffer returns.
        data = _build_dataset()
        structure = fds.read_buffer(data, fidelity="lossless")
        del data
        gc.collect()
        try:
            self.assertEqual(structure.get((0x0002, 0x0002)).value.rstrip(b"\x00"), b"1.2.3.4")
        finally:
            structure.close()

    def test_set_value_replaces_an_existing_element(self):
        self.assertTrue(self.structure.set_value((0x0010, 0x0020), b"ANON001 "))
        self.assertEqual(self.structure.get((0x0010, 0x0020)).value, b"ANON001 ")
        self.assertTrue(self.structure.is_modified)

    def test_set_value_returns_false_for_an_absent_tag(self):
        self.assertFalse(self.structure.set_value((0x9999, 0x9999), b"x "))
        self.assertFalse(self.structure.is_modified)

    def test_set_value_returns_false_when_value_overflows_short_form(self):
        oversized = b"A" * 70000
        self.assertFalse(self.structure.set_value((0x0010, 0x0020), oversized))
        self.assertEqual(self.structure.get((0x0010, 0x0020)).value, b"ID1\x00")
        self.assertFalse(self.structure.is_modified)

    def test_set_inserts_a_new_element(self):
        self.assertTrue(self.structure.set((0x0018, 0x0050), "DS", b"1.0\x00"))
        self.assertEqual(self.structure.get((0x0018, 0x0050)).value, b"1.0\x00")
        self.assertTrue(self.structure.is_modified)

    def test_erase_removes_an_element(self):
        self.assertTrue(self.structure.erase((0x0010, 0x0010)))
        self.assertNotIn((0x0010, 0x0010), self.structure)
        self.assertFalse(self.structure.erase((0x0010, 0x0010)))

    def test_erase_private_removes_only_private_elements(self):
        count = self.structure.erase_private()
        self.assertEqual(count, 1)
        self.assertNotIn((0x0009, 0x0010), self.structure)
        self.assertIn((0x0008, 0x0060), self.structure)
        self.assertTrue(self.structure.is_modified)

    def test_erase_recursive_removes_top_level_occurrence_like_erase(self):
        # No nesting involved: erase_recursive must agree with erase() for
        # the plain top-level case it subsumes.
        count = self.structure.erase_recursive((0x0010, 0x0010))
        self.assertEqual(count, 1)
        self.assertNotIn((0x0010, 0x0010), self.structure)
        self.assertTrue(self.structure.is_modified)

    def test_erase_recursive_returns_zero_for_absent_tag(self):
        count = self.structure.erase_recursive((0x9999, 0x9999))
        self.assertEqual(count, 0)
        self.assertFalse(self.structure.is_modified)

    def test_set_value_recursive_replaces_top_level_occurrence_like_set_value(self):
        count = self.structure.set_value_recursive((0x0010, 0x0020), b"ANON001 ")
        self.assertEqual(count, 1)
        self.assertEqual(self.structure.get((0x0010, 0x0020)).value, b"ANON001 ")
        self.assertTrue(self.structure.is_modified)

    def test_erase_recursive_removes_a_nested_occurrence_erase_cannot_reach(self):
        structure = fds.read_buffer(_build_dataset_with_nested_patient_id(), fidelity="lossless")
        try:
            # Confirm the fixture actually has both occurrences before
            # testing removal: one top-level (0010,0020), and one nested
            # inside the (0010,1002) sequence's one item.
            seq_before = structure.get((0x0010, 0x1002))
            self.assertTrue(seq_before.is_sequence)
            nested_before = [e.tag for e in list(seq_before.items())[0]]
            self.assertIn((0x0010, 0x0020), nested_before)

            count = structure.erase_recursive((0x0010, 0x0020))
            # One top-level + one nested occurrence, both removed in this pass.
            self.assertEqual(count, 2)
            self.assertTrue(structure.is_modified)

            # Confirm the nested occurrence specifically: the sequence
            # element itself is untouched (not a (0010,0020) match), but its
            # item no longer carries a (0010,0020) element.
            seq_element = structure.get((0x0010, 0x1002))
            self.assertIsNotNone(seq_element)
            self.assertTrue(seq_element.is_sequence)
            items = list(seq_element.items())
            self.assertEqual(len(items), 1)
            nested_tags = [element.tag for element in items[0]]
            self.assertNotIn((0x0010, 0x0020), nested_tags)
        finally:
            structure.close()

    def test_set_value_recursive_replaces_a_nested_occurrence_set_value_cannot_reach(self):
        structure = fds.read_buffer(_build_dataset_with_nested_patient_id(), fidelity="lossless")
        try:
            # set_value() is top-level-only: it cannot see the nested
            # occurrence to replace it (confirmed structurally, not just by
            # set_value's own return value, since (0010,0020) *does* exist
            # at the top level too and set_value only ever addresses one
            # single top-level element by tag).
            self.assertTrue(structure.set_value((0x0010, 0x0020), b"TOPLEVEL"))

            count = structure.set_value_recursive((0x0010, 0x0020), b"REPLACED")
            # The top-level occurrence (already "TOPLEVEL ") and the nested
            # one are both matched and rewritten by this single call.
            self.assertEqual(count, 2)
            self.assertEqual(structure.get((0x0010, 0x0020)).value, b"REPLACED")

            seq_element = structure.get((0x0010, 0x1002))
            items = list(seq_element.items())
            nested_values = [
                element.value for element in items[0] if element.tag == (0x0010, 0x0020)
            ]
            self.assertEqual(nested_values, [b"REPLACED"])
        finally:
            structure.close()

    def test_mutation_invalidates_previously_obtained_elements(self):
        element = self.structure.get((0x0010, 0x0010))
        self.structure.erase((0x0009, 0x0010))
        with self.assertRaises(fds.StaleElementError):
            _ = element.tag

    def test_write_after_mutation_round_trips(self):
        self.structure.set_value((0x0010, 0x0020), b"ANON001 ")
        self.structure.erase((0x0010, 0x0010))
        self.structure.erase_private()
        self.structure.set((0x0018, 0x0050), "DS", b"1.0\x00")

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "out.dcm")
            self.structure.write(path)
            reparsed = fds.read(path, fidelity="lossless")
            try:
                self.assertNotIn((0x0010, 0x0010), reparsed)
                self.assertNotIn((0x0009, 0x0010), reparsed)
                self.assertEqual(reparsed.get((0x0010, 0x0020)).value, b"ANON001 ")
                self.assertEqual(reparsed.get((0x0018, 0x0050)).value, b"1.0\x00")
                tags = [element.tag for element in reparsed]
                self.assertEqual(tags, sorted(tags))
            finally:
                reparsed.close()

    def test_write_with_stats_separates_source_backed_from_regenerated_bytes(self):
        original_id = self.structure.get((0x0010, 0x0020)).value  # b"ID1\x00", 4 bytes
        self.assertEqual(len(original_id), 4)
        self.structure.set_value((0x0010, 0x0020), b"ANON001 ")  # 8 bytes, unrelated content

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "out.dcm")
            stats = self.structure.write_with_stats(path)
            # +4: recomputed File Meta Group Length (0002,0000) is itself a
            # synthesized 4-byte value on any modified write -- see
            # lossless_writer.cpp's compute_file_meta_group_length.
            self.assertEqual(stats.regenerated_value_bytes, 8 + 4)
            # (0008,0060)="CT" (2 bytes) plus File Meta's own values must
            # still be source-backed.
            self.assertGreaterEqual(stats.source_backed_value_bytes, 2)
            self.assertEqual(stats.bytes_written, os.path.getsize(path))
            expected_fraction = (stats.source_backed_value_bytes /
                                  (stats.source_backed_value_bytes + stats.regenerated_value_bytes))
            self.assertAlmostEqual(stats.preserved_fraction, expected_fraction)

    def test_write_bytes_after_mutation_round_trips(self):
        """write_bytes() is a pure in-memory equivalent of write() -- same
        mutations, no filesystem path involved, reparses to the same
        result."""
        self.structure.set_value((0x0010, 0x0020), b"ANON001 ")
        self.structure.erase((0x0010, 0x0010))
        self.structure.erase_private()
        self.structure.set((0x0018, 0x0050), "DS", b"1.0\x00")

        output = self.structure.write_bytes()
        self.assertIsInstance(output, bytes)
        reparsed = fds.read_buffer(output, fidelity="lossless")
        try:
            self.assertNotIn((0x0010, 0x0010), reparsed)
            self.assertNotIn((0x0009, 0x0010), reparsed)
            self.assertEqual(reparsed.get((0x0010, 0x0020)).value, b"ANON001 ")
            self.assertEqual(reparsed.get((0x0018, 0x0050)).value, b"1.0\x00")
            tags = [element.tag for element in reparsed]
            self.assertEqual(tags, sorted(tags))
        finally:
            reparsed.close()

    def test_write_bytes_matches_write_to_file_byte_for_byte(self):
        """The in-memory and path-based write APIs must agree exactly on
        output for the same structure and the same mutations."""
        self.structure.set_value((0x0010, 0x0020), b"ANON001 ")
        self.structure.erase((0x0010, 0x0010))

        via_bytes = self.structure.write_bytes()
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "out.dcm")
            self.structure.write(path)
            with open(path, "rb") as handle:
                via_file = handle.read()
        self.assertEqual(via_bytes, via_file)

    def test_write_bytes_with_stats_matches_write_with_stats_counters(self):
        self.structure.set_value((0x0010, 0x0020), b"ANON001 ")  # 8 bytes, unrelated content

        via_bytes, stats_from_bytes = self.structure.write_bytes_with_stats()
        self.assertEqual(stats_from_bytes.bytes_written, len(via_bytes))
        # +4: recomputed File Meta Group Length, same as the write_with_stats test above.
        self.assertEqual(stats_from_bytes.regenerated_value_bytes, 8 + 4)
        self.assertGreaterEqual(stats_from_bytes.source_backed_value_bytes, 2)

    def test_write_bytes_is_unsupported_for_an_unmodified_standard_structure(self):
        structure = fds.read_buffer(_build_dataset(), fidelity="standard")
        try:
            with self.assertRaises(fds.FdsError):
                structure.write_bytes()
        finally:
            structure.close()


if __name__ == "__main__":
    unittest.main()
