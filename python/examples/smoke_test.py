#!/usr/bin/env python3
"""Smoke test for the ctypes wrapper: builds a small synthetic DICOM byte
buffer directly in Python (independent of the C++ test fixture builder, to
exercise the whole Python -> C ABI -> C++ stack end to end) and exercises
read_buffer(), top-level iteration, tag lookup, sequence/item navigation,
and diagnostics.

Run after building the project, e.g.:
    cmake --build build
    python3 python/examples/smoke_test.py
(set FASTDICOMATTRS_LIB if the .so isn't in a directory this package
searches automatically -- see python/fastdicomattrs/__init__.py)
"""

import struct
import sys
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


def element_long(group, element, vr, value: bytes) -> bytes:
    if len(value) % 2 != 0:
        value += b"\x00"
    return tag(group, element) + vr.encode("ascii") + b"\x00\x00" + u32(len(value)) + value


def item(content: bytes) -> bytes:
    return tag(0xFFFE, 0xE000) + u32(len(content)) + content


def sequence_header(group, element, content_length) -> bytes:
    return tag(group, element) + b"SQ" + b"\x00\x00" + u32(content_length)


def build_dataset() -> bytes:
    ts_uid = b"1.2.840.10008.1.2.1"  # already even length (20)
    group_body = (
        element_short(0x0002, 0x0002, "UI", b"1.2.3.4")
        + element_short(0x0002, 0x0003, "UI", b"1.2.3.4.5")
        + element_short(0x0002, 0x0010, "UI", ts_uid)
    )
    file_meta = element_short(0x0002, 0x0000, "UL", u32(len(group_body))) + group_body

    item_content = element_short(0x0008, 0x0100, "SH", b"12345")
    sequence = sequence_header(0x0008, 0x1140, len(item(item_content))) + item(item_content)

    dataset = (
        element_short(0x0010, 0x0020, "LO", b"ANON001")
        + element_short(0x0008, 0x0060, "CS", b"OT")
        + sequence
        + element_long(0x7FE0, 0x0010, "OW", bytes([0x2A]) * 16)  # OW is a long-form VR
    )

    return b"\x00" * 128 + b"DICM" + file_meta + dataset


def main() -> int:
    print(f"fastdicomattrs ABI version: {fds._lib.fds_abi_version():#010x}")

    data = build_dataset()
    structure = fds.read_buffer(data)

    print(f"top-level element count: {len(structure)}")
    for element in structure:
        print(f"  {element!r}")

    patient_id = structure.get((0x0010, 0x0020))
    assert patient_id is not None, "expected (0010,0020) to be present"
    # .value is the raw, untrimmed byte value -- "ANON001" is odd-length (7)
    # so it carries a trailing NUL pad byte; the ABI exposes raw bytes only
    # in this increment (no typed/trimmed string accessor -- see
    # docs/abi-design.md "Deliberately absent from this ABI").
    assert patient_id.value == b"ANON001\x00", patient_id.value
    print(f"(0010,0020) raw value: {patient_id.value!r}")

    assert (0x7FE0, 0x0010) not in structure, "Pixel Data must not appear in the flat element list"

    sequence_element = structure.get((0x0008, 0x1140))
    assert sequence_element is not None and sequence_element.is_sequence
    items = list(sequence_element.items())
    assert len(items) == 1
    nested = list(items[0])
    assert len(nested) == 1 and nested[0].tag == (0x0008, 0x0100)
    assert nested[0].value == b"12345\x00"  # odd-length "12345" is NUL-padded, like above
    print(f"nested element via sequence/item navigation: {nested[0]!r} = {nested[0].value!r}")

    print(f"diagnostics: {structure.diagnostics}")

    print("OK: Python -> C ABI -> C++ round trip verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
