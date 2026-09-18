#!/usr/bin/env python3
"""A1.7 nested mutation walkthrough: builds a small synthetic DICOM byte
buffer with a nested Sequence/Item, then exercises the full path-aware
mutation surface end to end -- recursive/root/nested read, raw and
Unicode replacement, nested explicit- and inferred-VR insertion, nested
text insertion, private creator/data insertion, erase, and write/reparse.

Run after building the project, e.g.:
    cmake --build build
    python3 python/examples/nested_mutation.py
(set FASTDICOMATTRS_LIB if the .so isn't in a directory this package
searches automatically -- see python/fastdicomattrs/__init__.py)
"""

import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import fastdicomattrs as fds


def u16(v):
    return struct.pack("<H", v)


def u32(v):
    return struct.pack("<I", v)


def tag(group, element):
    return u16(group) + u16(element)


def element_short(group, element, vr, value: bytes) -> bytes:
    if len(value) % 2 != 0:
        value += b"\x00"
    return tag(group, element) + vr.encode("ascii") + u16(len(value)) + value


def item(content: bytes) -> bytes:
    return tag(0xFFFE, 0xE000) + u32(len(content)) + content


def build_dataset() -> bytes:
    ts_uid = b"1.2.840.10008.1.2.1"  # Explicit VR Little Endian
    group_body = (
        element_short(0x0002, 0x0002, "UI", b"1.2.3.4")
        + element_short(0x0002, 0x0003, "UI", b"1.2.3.4.5")
        + element_short(0x0002, 0x0010, "UI", ts_uid)
    )
    file_meta = element_short(0x0002, 0x0000, "UL", u32(len(group_body))) + group_body

    sc_set = element_short(0x0008, 0x0005, "CS", b"ISO_IR 100")  # Latin-1
    modality = element_short(0x0008, 0x0060, "CS", b"CT")

    # One Sequence, one Item, one nested scalar -- a minimal but genuine
    # nested container to mutate into.
    inner_element = element_short(0x0008, 0x0100, "SH", b"12345")
    seq = tag(0x0008, 0x1140) + b"SQ" + b"\x00\x00" + u32(len(item(inner_element))) + item(
        inner_element
    )

    return b"\x00" * 128 + b"DICM" + file_meta + sc_set + modality + seq


def main():
    data = build_dataset()
    structure = fds.read_buffer(data, fidelity="lossless")

    # --- root and nested read -------------------------------------------
    root_element = structure.get((0x0008, 0x0060))
    print(f"root read: {root_element.tag} = {root_element.value!r}")

    nested_path = [((0x0008, 0x1140), 0), ((0x0008, 0x0100), None)]
    nested_element = structure.find(nested_path)
    print(f"nested read: {nested_element.tag} = {nested_element.value!r}")

    # --- recursive read ---------------------------------------------------
    print("recursive read (element, path):")
    for element, path in structure.iter_elements(recursive=True):
        depth = len(path) - 1
        print(f"  {'  ' * depth}{element.tag} {element.vr}" + ("" if depth else ""))

    # --- raw replacement ----------------------------------------------------
    structure.set_value(nested_path, b"99999 ")
    print(f"raw replacement: nested value now {structure.find(nested_path).value!r}")

    # --- Unicode insertion, then Unicode replacement (needs the root's
    # ISO_IR 100 declaration) -------------------------------------------------
    structure.insert_text((0x0010, 0x0010), "Müller^Anna", vr="PN")  # root, explicit VR
    print(f"Unicode insert (root): {structure.decode_text((0x0010, 0x0010))!r}")
    structure.set_text((0x0010, 0x0010), "Müller^Anna-Maria")  # replace the element just inserted
    print(f"Unicode replacement (root): {structure.decode_text((0x0010, 0x0010))!r}")

    # --- nested explicit-VR insertion ---------------------------------------
    parent = [((0x0008, 0x1140), 0)]
    structure.insert((0x0010, 0x0020), b"ID1 ", vr="LO", parent=parent)
    print(f"nested explicit-VR insert: {structure.find(parent + [((0x0010, 0x0020), None)]).vr}")

    # --- nested inferred-VR insertion --------------------------------------
    structure.insert((0x0008, 0x0102), b"12345 ", parent=parent)  # MappingResource -- SH, unambiguous
    inferred = structure.find(parent + [((0x0008, 0x0102), None)])
    print(f"nested inferred-VR insert: vr inferred as {inferred.vr}")

    # --- nested text insertion (Unicode, charset-aware) ---------------------
    structure.insert_text((0x0008, 0x0080), "Hôpital", vr="LO", parent=parent)
    print(f"nested text insert: {structure.decode_text(parent + [((0x0008, 0x0080), None)])!r}")

    # --- private creator + private data insertion, nested -------------------
    structure.insert((0x0009, 0x0010), b"ACME CORP ", parent=parent)  # inferred LO (Private Creator)
    structure.insert((0x0009, 0x1001), b"widget", vr="LO", parent=parent)  # explicit VR
    print("private creator + data inserted nested")

    # --- erase ---------------------------------------------------------------
    structure.erase(parent + [((0x0008, 0x0102), None)])
    print("erased the nested MappingResource element")

    # --- write / reparse -------------------------------------------------------
    with tempfile.TemporaryDirectory() as directory:
        path = f"{directory}/out.dcm"
        structure.write(path)
        reparsed = fds.read(path, fidelity="lossless")
        try:
            print(f"reparsed nested PatientID: {reparsed.find(parent + [((0x0010, 0x0020), None)]).value!r}")
            print(f"reparsed nested Unicode text: {reparsed.decode_text(parent + [((0x0008, 0x0080), None)])!r}")
        finally:
            reparsed.close()

    structure.close()
    print("PASS")


if __name__ == "__main__":
    main()
