"""Exercises A1.7's path-aware nested mutation surface (find/set_value/
erase-by-path, insert/insert_text, set_text/decode_text, iter_elements,
stale-reference safety, and the exception model) through the full
Python -> C ABI -> C++ stack. Mirrors test_mutation.py's byte-building
style.
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


def _file_meta(sc_terms=None) -> bytes:
    ts_uid = b"1.2.840.10008.1.2.1"  # already even length
    group_body = (
        _element_short(0x0002, 0x0002, "UI", b"1.2.3.4")
        + _element_short(0x0002, 0x0003, "UI", b"1.2.3.4.5")
        + _element_short(0x0002, 0x0010, "UI", ts_uid)
    )
    file_meta = _element_short(0x0002, 0x0000, "UL", _u32(len(group_body))) + group_body
    header = b"\x00" * 128 + b"DICM" + file_meta
    if sc_terms:
        header += _element_short(0x0008, 0x0005, "CS", "\\".join(sc_terms).encode("ascii"))
    return header


def _build_nested_dataset() -> bytes:
    """One top-level (0008,0060), one nested Sequence (0008,1140) with a
    single Item carrying (0008,0100)="12345"."""
    item_content = _element_short(0x0008, 0x0100, "SH", b"12345")
    seq = _element_sq(0x0008, 0x1140, _dicom_item(item_content))
    dataset = _element_short(0x0008, 0x0060, "CS", b"CT") + seq
    return _file_meta() + dataset


class NestedFindSetEraseTest(unittest.TestCase):
    def setUp(self):
        self.structure = fds.read_buffer(_build_nested_dataset(), fidelity="lossless")

    def tearDown(self):
        self.structure.close()

    def test_find_reaches_a_nested_element(self):
        path = [((0x0008, 0x1140), 0), ((0x0008, 0x0100), None)]
        element = self.structure.find(path)
        self.assertIsNotNone(element)
        self.assertEqual(element.value, b"12345\x00")

    def test_find_returns_none_for_a_missing_nested_path(self):
        path = [((0x0008, 0x1140), 0), ((0x9999, 0x9999), None)]
        self.assertIsNone(self.structure.find(path))

    def test_find_with_a_bare_tag_matches_get(self):
        self.assertEqual(self.structure.find((0x0008, 0x0060)).value,
                          self.structure.get((0x0008, 0x0060)).value)

    def test_set_value_replaces_a_nested_element(self):
        path = [((0x0008, 0x1140), 0), ((0x0008, 0x0100), None)]
        self.assertTrue(self.structure.set_value(path, b"99999 "))
        self.assertEqual(self.structure.find(path).value, b"99999 ")
        self.assertTrue(self.structure.is_modified)

    def test_erase_removes_a_nested_element(self):
        path = [((0x0008, 0x1140), 0), ((0x0008, 0x0100), None)]
        self.assertTrue(self.structure.erase(path))
        self.assertIsNone(self.structure.find(path))
        self.assertFalse(self.structure.erase(path))


class InsertTest(unittest.TestCase):
    def setUp(self):
        self.structure = fds.read_buffer(_build_nested_dataset(), fidelity="lossless")

    def tearDown(self):
        self.structure.close()

    def test_insert_at_root_with_explicit_vr(self):
        self.structure.insert((0x0010, 0x0020), b"ID1 ", vr="LO")
        element = self.structure.get((0x0010, 0x0020))
        self.assertEqual(element.vr, "LO")
        self.assertEqual(element.value, b"ID1 ")
        self.assertTrue(self.structure.is_modified)

    def test_insert_at_root_with_inferred_vr(self):
        # (0010,0020) PatientID -- unambiguous LO.
        self.structure.insert((0x0010, 0x0020), b"ID1 ")
        self.assertEqual(self.structure.get((0x0010, 0x0020)).vr, "LO")

    def test_insert_auto_pads_an_odd_length_value(self):
        # Section 19: insert()/insert_inferred() know the resolved VR, so
        # unlike set_value(), an odd-length value here is auto-padded
        # (SPACE for a text VR) rather than rejected.
        self.structure.insert((0x0010, 0x0020), b"ODD", vr="LO")
        self.assertEqual(self.structure.get((0x0010, 0x0020)).value, b"ODD ")

    def test_insert_nested_with_explicit_vr(self):
        parent = [((0x0008, 0x1140), 0)]
        self.structure.insert((0x0010, 0x0020), b"ID1 ", vr="LO", parent=parent)
        path = [((0x0008, 0x1140), 0), ((0x0010, 0x0020), None)]
        element = self.structure.find(path)
        self.assertIsNotNone(element)
        self.assertEqual(element.vr, "LO")

    def test_insert_raises_already_exists_for_a_duplicate_tag(self):
        with self.assertRaises(fds.AlreadyExistsError):
            self.structure.insert((0x0008, 0x0060), b"CT")
        self.assertFalse(self.structure.is_modified)

    def test_insert_raises_vr_required_for_an_ambiguous_tag(self):
        # (0028,0106) SmallestImagePixelValue -- "US or SS".
        with self.assertRaises(fds.VRRequiredError):
            self.structure.insert((0x0028, 0x0106), b"\x00\x00")
        self.assertFalse(self.structure.is_modified)

    def test_insert_raises_fds_error_for_a_nonexistent_parent(self):
        with self.assertRaises(fds.FdsError):
            self.structure.insert((0x0010, 0x0010), b"AB", vr="SH",
                                   parent=[((0x9999, 0x9999), 0)])
        self.assertFalse(self.structure.is_modified)

    def test_insert_raises_type_error_for_a_malformed_parent(self):
        with self.assertRaises(TypeError):
            self.structure.insert((0x0010, 0x0010), b"AB", vr="SH",
                                   parent=[((0x0008, 0x1140), None)])


class TextTest(unittest.TestCase):
    def setUp(self):
        self.structure = fds.read_buffer(_file_meta(["ISO_IR 100"]) +
                                          _element_short(0x0010, 0x0010, "PN", b"X"),
                                          fidelity="lossless")

    def tearDown(self):
        self.structure.close()

    def test_set_text_then_decode_text_round_trips(self):
        self.structure.set_text((0x0010, 0x0010), "Riesmeier^Jörg")
        self.assertEqual(self.structure.decode_text((0x0010, 0x0010)), ["Riesmeier^Jörg"])

    def test_set_text_accepts_multi_valued_list(self):
        seq = _element_short(0x0010, 0x1002, "LO", b"X")
        structure = fds.read_buffer(_file_meta() + seq, fidelity="lossless")
        try:
            structure.set_text((0x0010, 0x1002), ["Smith^John", "Doe^Jane"])
            self.assertEqual(structure.decode_text((0x0010, 0x1002)), ["Smith^John", "Doe^Jane"])
        finally:
            structure.close()

    def test_set_text_raises_unrepresentable_character(self):
        # Cyrillic 'р' is not representable under this structure's Latin1
        # declaration.
        with self.assertRaises(fds.UnrepresentableCharacterError):
            self.structure.set_text((0x0010, 0x0010), "AрB")

    def test_insert_text_root_explicit_and_inferred(self):
        self.structure.insert_text((0x0010, 0x1001), "Doe^Jane", vr="PN")
        self.assertEqual(self.structure.decode_text((0x0010, 0x1001)), ["Doe^Jane"])

        self.structure.insert_text((0x0008, 0x0080), "Hôpital")  # LO, inferred, needs Latin1
        self.assertEqual(self.structure.decode_text((0x0008, 0x0080)), ["Hôpital"])

    def test_insert_text_nested_uses_local_declaration_not_root(self):
        item_content = _element_short(0x0008, 0x0005, "CS", b"ISO_IR 144")  # Cyrillic, local
        seq = _element_sq(0x0008, 0x1140, _dicom_item(item_content))
        structure = fds.read_buffer(_file_meta(["ISO_IR 100"]) + seq, fidelity="lossless")
        try:
            parent = [((0x0008, 0x1140), 0)]
            # Cyrillic-only text -- only representable under the Item's own
            # local override, never under the root's Latin1: proves the
            # target container's own declaration governs, not the root's.
            structure.insert_text((0x0010, 0x0010), "AрB", vr="PN", parent=parent)
            path = [((0x0008, 0x1140), 0), ((0x0010, 0x0010), None)]
            self.assertEqual(structure.decode_text(path), ["AрB"])
        finally:
            structure.close()


class IterElementsTest(unittest.TestCase):
    def setUp(self):
        self.structure = fds.read_buffer(_build_nested_dataset(), fidelity="lossless")

    def tearDown(self):
        self.structure.close()

    def test_non_recursive_matches_top_level_iteration(self):
        got = [(element.tag, path) for element, path in self.structure.iter_elements(recursive=False)]
        expected = [(element.tag, [(element.tag, None)]) for element in self.structure]
        self.assertEqual(got, expected)

    def test_recursive_reaches_nested_elements_with_reusable_paths(self):
        found = {}
        for element, path in self.structure.iter_elements(recursive=True):
            found[tuple(path)] = element
        nested_path = ((0x0008, 0x1140), 0), ((0x0008, 0x0100), None)
        self.assertIn(nested_path, found)
        self.assertEqual(found[nested_path].value, b"12345\x00")
        # The path is directly reusable with find().
        self.assertEqual(self.structure.find(list(nested_path)).value, b"12345\x00")

    def test_recursive_order_is_deterministic_document_order(self):
        tags = [element.tag for element, _ in self.structure.iter_elements(recursive=True)]
        # (0008,0060), then (0008,1140) [the Sequence itself], then its
        # nested (0008,0100) -- depth-first, matching declaration order
        # (File Meta group elements precede them, in their own order).
        dataset_tags = [t for t in tags if t[0] == 0x0008]
        self.assertEqual(dataset_tags, [(0x0008, 0x0060), (0x0008, 0x1140), (0x0008, 0x0100)])


class StaleReferenceTest(unittest.TestCase):
    def setUp(self):
        self.structure = fds.read_buffer(_build_nested_dataset(), fidelity="lossless")

    def tearDown(self):
        self.structure.close()

    def test_insert_invalidates_a_previously_obtained_element(self):
        element = self.structure.get((0x0008, 0x0060))
        self.structure.insert((0x0010, 0x0020), b"ID1 ", vr="LO")
        with self.assertRaises(fds.StaleElementError):
            _ = element.value

    def test_insert_text_invalidates_a_previously_obtained_element(self):
        element = self.structure.get((0x0008, 0x0060))
        self.structure.insert_text((0x0010, 0x0010), "X", vr="SH")
        with self.assertRaises(fds.StaleElementError):
            _ = element.tag

    def test_set_text_invalidates_a_previously_obtained_element(self):
        seq = _element_short(0x0010, 0x0010, "PN", b"X")
        structure = fds.read_buffer(_file_meta(["ISO_IR 100"]) + seq, fidelity="lossless")
        try:
            element = structure.get((0x0010, 0x0010))
            structure.set_text((0x0010, 0x0010), "Y")
            with self.assertRaises(fds.StaleElementError):
                _ = element.value
        finally:
            structure.close()

    def test_a_failed_insert_does_not_bump_generation(self):
        # A no-op/failed mutation must not invalidate live handles -- only
        # an *applied* mutation does.
        element = self.structure.get((0x0008, 0x0060))
        with self.assertRaises(fds.AlreadyExistsError):
            self.structure.insert((0x0008, 0x0060), b"CT")
        self.assertEqual(element.value, b"CT")  # still live, no exception

    def test_write_after_nested_insert_round_trips(self):
        parent = [((0x0008, 0x1140), 0)]
        self.structure.insert((0x0010, 0x0020), b"ID1 ", vr="LO", parent=parent)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "out.dcm")
            self.structure.write(path)
            reparsed = fds.read(path, fidelity="lossless")
            try:
                nested_path = [((0x0008, 0x1140), 0), ((0x0010, 0x0020), None)]
                element = reparsed.find(nested_path)
                self.assertIsNotNone(element)
                self.assertEqual(element.value, b"ID1 ")
            finally:
                reparsed.close()


if __name__ == "__main__":
    unittest.main()
