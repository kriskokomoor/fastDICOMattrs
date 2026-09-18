"""Thin ctypes wrapper over the fastDICOMattrs C ABI (see
docs/abi-design.md). See docs/architecture.md section 10 for why ctypes was
chosen over a compiled pybind11 extension.

Exposes read-only inspection (top-level and recursive iteration, tag
lookup, typed VR, raw value bytes, nested sequence/item navigation, parse
diagnostics), structure metadata, writing (to a path via
write()/write_with_stats(), or to an in-memory bytes object via
write_bytes()/write_bytes_with_stats()), and mutation -- both top-level
(set_value/set/erase/erase_private/erase_recursive/set_value_recursive)
and, as of A1.7, path-aware nested mutation at any depth: find(path),
insert()/insert_text() (absent-only, explicit or dictionary-inferred VR),
set_text()/decode_text() (charset-aware), and path-capable set_value()/
erase(). See docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
for the full design.

Path representation, used consistently by every path-taking method below:
a bare ``(group, element)`` tuple names a top-level element (unchanged from
before A1.7); a *nested* path is a ``list`` of ``(tag, item_index)`` steps,
e.g. ``[((0x0008, 0x1140), 0), ((0x0010, 0x0010), None)]`` -- every step but
the last carries a concrete Item index (descend into that Sequence Item),
and the last step's index is ``None`` (name the element itself). A
*container locator* (the ``parent=`` argument to insert()/insert_text())
uses the same step shape but every entry (no exception for the last) must
carry a concrete Item index; ``None`` or an empty list means the root
dataset. This mirrors the C++ ElementPath/`parent` distinction exactly --
see dicom_structure.hpp's insert() doc comment.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, List, Optional, Sequence, Tuple, Union

__all__ = ["read", "read_buffer", "Structure", "Element", "Item", "Diagnostic", "FdsError",
           "StaleElementError", "VRRequiredError", "AlreadyExistsError",
           "UnrepresentableCharacterError", "InvalidUnicodeInputError", "WriteStats"]

# ---------------------------------------------------------------------------
# Library loading
# ---------------------------------------------------------------------------

_LIB_NAMES = ["libfastdicomattrs_c.so", "libfastdicomattrs_c.dylib",
              "fastdicomattrs_c.dll"]


def _packaged_library_path() -> Optional[Path]:
    """The shared library a `pip install`-ed wheel places directly beside
    this file (see pyproject.toml's [tool.scikit-build.install] components
    and CMakeLists.txt's FDS_PYTHON_WHEEL_INSTALL) -- present for a normal
    installed package, absent for a plain PYTHONPATH-injected source-tree
    checkout, which instead falls back to _candidate_dirs() below.
    """
    here = Path(__file__).resolve().parent
    for name in _LIB_NAMES:
        candidate = here / name
        if candidate.is_file():
            return candidate
    return None


def _candidate_dirs():
    here = Path(__file__).resolve().parent
    repo_root = here.parent.parent
    # Common CMake build directory names used in this repository's docs and
    # by the person building it locally; not an exhaustive search path.
    for build_dir in ("build", "build-release", "cmake-build-debug", "cmake-build-release"):
        yield repo_root / build_dir
        yield repo_root / build_dir / "abi"


def _find_library() -> str:
    override = os.environ.get("FASTDICOMATTRS_LIB")
    if override:
        return override
    packaged = _packaged_library_path()
    if packaged is not None:
        return str(packaged)
    for directory in _candidate_dirs():
        for name in _LIB_NAMES:
            candidate = directory / name
            if candidate.is_file():
                return str(candidate)
    system_path = ctypes.util.find_library("fastdicomattrs_c")
    if system_path:
        return system_path
    raise FileNotFoundError(
        "could not locate libfastdicomattrs_c; build the project (see CMakeLists.txt) "
        "or set the FASTDICOMATTRS_LIB environment variable to the shared library path"
    )


_lib = ctypes.CDLL(_find_library())


# ---------------------------------------------------------------------------
# ctypes declarations mirroring abi/include/fastdicomattrs_c/fds.h
# ---------------------------------------------------------------------------

class _fds_tag_t(ctypes.Structure):
    _fields_ = [("group", ctypes.c_uint16), ("element", ctypes.c_uint16)]


class _fds_parse_options_t(ctypes.Structure):
    _fields_ = [
        ("fidelity", ctypes.c_int),
        ("max_element_count", ctypes.c_uint64),
        ("max_sequence_depth", ctypes.c_uint64),
    ]


class _fds_diagnostic_t(ctypes.Structure):
    _fields_ = [
        ("severity", ctypes.c_int),
        ("message", ctypes.c_char_p),
        ("offset", ctypes.c_uint64),
        ("has_tag", ctypes.c_int),
        ("tag", _fds_tag_t),
    ]


class _fds_path_step_t(ctypes.Structure):
    _fields_ = [("tag", _fds_tag_t), ("has_item_index", ctypes.c_int),
                ("item_index", ctypes.c_size_t)]


_StructurePtr = ctypes.c_void_p
_ElementPtr = ctypes.c_void_p

_FDS_STATUS_OK = 0
_FDS_STATUS_NOT_FOUND = 1
_FDS_STATUS_INVALID_ARGUMENT = 2
_FDS_STATUS_IO_ERROR = 3
_FDS_STATUS_PARSE_FAILED = 4
_FDS_STATUS_UNSUPPORTED = 5
_FDS_STATUS_INTERNAL_ERROR = 6
# A1.7: additive -- see abi/include/fastdicomattrs_c/fds.h's own comment on
# these four. Returned only by insert()/insert_text() below.
_FDS_STATUS_ALREADY_EXISTS = 7
_FDS_STATUS_VR_REQUIRED = 8
_FDS_STATUS_UNREPRESENTABLE_CHARACTER = 9
_FDS_STATUS_INVALID_UNICODE_INPUT = 10

_STATUS_MESSAGES = {
    _FDS_STATUS_OK: "ok",
    _FDS_STATUS_NOT_FOUND: "not found",
    _FDS_STATUS_INVALID_ARGUMENT: "invalid argument",
    _FDS_STATUS_IO_ERROR: "I/O error",
    _FDS_STATUS_PARSE_FAILED: "parse failed",
    _FDS_STATUS_UNSUPPORTED: "unsupported",
    _FDS_STATUS_INTERNAL_ERROR: "internal error (library bug)",
    _FDS_STATUS_ALREADY_EXISTS: "already exists",
    _FDS_STATUS_VR_REQUIRED: "VR required (inference was ambiguous or unavailable)",
    _FDS_STATUS_UNREPRESENTABLE_CHARACTER: "unrepresentable character",
    _FDS_STATUS_INVALID_UNICODE_INPUT: "invalid Unicode input",
}

_FIDELITY = {"fast": 0, "standard": 1, "lossless": 2}

_VR_NAMES = [
    "AE", "AS", "AT", "CS", "DA", "DS", "DT", "FL", "FD", "IS", "LO", "LT", "OB", "OD", "OF",
    "OL", "OV", "OW", "PN", "SH", "SL", "SQ", "SS", "ST", "SV", "TM", "UC", "UI", "UL", "UN",
    "UR", "US", "UT", "UV", "UNKNOWN",
]

_SEVERITY_NAMES = ["info", "warning", "recoverable_error", "fatal_error", "unsupported", "io_error"]

_VR_INDEX = {name: index for index, name in enumerate(_VR_NAMES)}

_lib.fds_abi_version.restype = ctypes.c_uint32
_lib.fds_status_message.restype = ctypes.c_char_p
_lib.fds_parse_options_init_defaults.argtypes = [ctypes.POINTER(_fds_parse_options_t)]

_lib.fds_parse_file.argtypes = [ctypes.c_char_p, ctypes.POINTER(_fds_parse_options_t),
                                 ctypes.POINTER(_StructurePtr)]
_lib.fds_parse_file.restype = ctypes.c_int

_lib.fds_parse_buffer.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                   ctypes.POINTER(_fds_parse_options_t),
                                   ctypes.POINTER(_StructurePtr)]
_lib.fds_parse_buffer.restype = ctypes.c_int

_lib.fds_structure_free.argtypes = [_StructurePtr]

_lib.fds_structure_element_count.argtypes = [_StructurePtr]
_lib.fds_structure_element_count.restype = ctypes.c_size_t

_lib.fds_structure_element_at.argtypes = [_StructurePtr, ctypes.c_size_t,
                                           ctypes.POINTER(_ElementPtr)]
_lib.fds_structure_element_at.restype = ctypes.c_int

_lib.fds_structure_find.argtypes = [_StructurePtr, _fds_tag_t, ctypes.POINTER(_ElementPtr)]
_lib.fds_structure_find.restype = ctypes.c_int

_lib.fds_structure_contains.argtypes = [_StructurePtr, _fds_tag_t]
_lib.fds_structure_contains.restype = ctypes.c_int

_lib.fds_structure_diagnostic_count.argtypes = [_StructurePtr]
_lib.fds_structure_diagnostic_count.restype = ctypes.c_size_t

_lib.fds_structure_diagnostic_at.argtypes = [_StructurePtr, ctypes.c_size_t,
                                              ctypes.POINTER(_fds_diagnostic_t)]
_lib.fds_structure_diagnostic_at.restype = ctypes.c_int

_lib.fds_structure_transfer_syntax_uid.argtypes = [_StructurePtr]
_lib.fds_structure_transfer_syntax_uid.restype = ctypes.c_char_p
_lib.fds_structure_transfer_syntax_is_explicit_vr.argtypes = [_StructurePtr]
_lib.fds_structure_transfer_syntax_is_explicit_vr.restype = ctypes.c_int
_lib.fds_structure_transfer_syntax_is_little_endian.argtypes = [_StructurePtr]
_lib.fds_structure_transfer_syntax_is_little_endian.restype = ctypes.c_int
_lib.fds_structure_pixel_data_kind.argtypes = [_StructurePtr]
_lib.fds_structure_pixel_data_kind.restype = ctypes.c_int
_lib.fds_structure_write_file.argtypes = [_StructurePtr, ctypes.c_char_p,
                                           ctypes.POINTER(ctypes.c_uint64)]
_lib.fds_structure_write_file.restype = ctypes.c_int

_lib.fds_structure_write_file_with_stats.argtypes = [
    _StructurePtr, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64),
    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
]
_lib.fds_structure_write_file_with_stats.restype = ctypes.c_int

_lib.fds_structure_write_buffer.argtypes = [
    _StructurePtr, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)), ctypes.POINTER(ctypes.c_size_t),
]
_lib.fds_structure_write_buffer.restype = ctypes.c_int

_lib.fds_structure_write_buffer_with_stats.argtypes = [
    _StructurePtr, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)), ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
]
_lib.fds_structure_write_buffer_with_stats.restype = ctypes.c_int

_lib.fds_structure_is_modified.argtypes = [_StructurePtr]
_lib.fds_structure_is_modified.restype = ctypes.c_int

_lib.fds_structure_set_value.argtypes = [_StructurePtr, _fds_tag_t,
                                          ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
_lib.fds_structure_set_value.restype = ctypes.c_int

_lib.fds_structure_set.argtypes = [_StructurePtr, _fds_tag_t, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
_lib.fds_structure_set.restype = ctypes.c_int

_lib.fds_structure_erase.argtypes = [_StructurePtr, _fds_tag_t]
_lib.fds_structure_erase.restype = ctypes.c_int

_lib.fds_structure_erase_private.argtypes = [_StructurePtr, ctypes.POINTER(ctypes.c_size_t)]
_lib.fds_structure_erase_private.restype = ctypes.c_int

_lib.fds_structure_erase_recursive.argtypes = [_StructurePtr, _fds_tag_t,
                                                ctypes.POINTER(ctypes.c_size_t)]
_lib.fds_structure_erase_recursive.restype = ctypes.c_int

_lib.fds_structure_set_value_recursive.argtypes = [_StructurePtr, _fds_tag_t,
                                                    ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                                    ctypes.POINTER(ctypes.c_size_t)]
_lib.fds_structure_set_value_recursive.restype = ctypes.c_int

# --- A1.7: path-based (nested) mutation -------------------------------------

_lib.fds_structure_find_path.argtypes = [_StructurePtr, ctypes.POINTER(_fds_path_step_t),
                                          ctypes.c_size_t, ctypes.POINTER(_ElementPtr)]
_lib.fds_structure_find_path.restype = ctypes.c_int

_lib.fds_structure_set_value_path.argtypes = [_StructurePtr, ctypes.POINTER(_fds_path_step_t),
                                               ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint8),
                                               ctypes.c_size_t]
_lib.fds_structure_set_value_path.restype = ctypes.c_int

_lib.fds_structure_erase_path.argtypes = [_StructurePtr, ctypes.POINTER(_fds_path_step_t),
                                           ctypes.c_size_t]
_lib.fds_structure_erase_path.restype = ctypes.c_int

_lib.fds_structure_insert_path.argtypes = [
    _StructurePtr, ctypes.POINTER(_fds_path_step_t), ctypes.c_size_t, _fds_tag_t, ctypes.c_int,
    ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
]
_lib.fds_structure_insert_path.restype = ctypes.c_int

_lib.fds_structure_decode_text_path.argtypes = [_StructurePtr, ctypes.POINTER(_fds_path_step_t),
                                                 ctypes.c_size_t, ctypes.POINTER(ctypes.c_char_p)]
_lib.fds_structure_decode_text_path.restype = ctypes.c_int

_lib.fds_structure_set_text_path.argtypes = [_StructurePtr, ctypes.POINTER(_fds_path_step_t),
                                              ctypes.c_size_t, ctypes.c_char_p]
_lib.fds_structure_set_text_path.restype = ctypes.c_int

_lib.fds_structure_insert_text_path.argtypes = [
    _StructurePtr, ctypes.POINTER(_fds_path_step_t), ctypes.c_size_t, _fds_tag_t, ctypes.c_int,
    ctypes.c_char_p,
]
_lib.fds_structure_insert_text_path.restype = ctypes.c_int

_lib.fds_element_tag.argtypes = [_ElementPtr]
_lib.fds_element_tag.restype = _fds_tag_t

_lib.fds_element_vr.argtypes = [_ElementPtr]
_lib.fds_element_vr.restype = ctypes.c_int

_lib.fds_element_is_sequence.argtypes = [_ElementPtr]
_lib.fds_element_is_sequence.restype = ctypes.c_int

_lib.fds_element_value_bytes.argtypes = [_ElementPtr, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                          ctypes.POINTER(ctypes.c_size_t)]
_lib.fds_element_value_bytes.restype = ctypes.c_int

_lib.fds_element_sequence_item_count.argtypes = [_ElementPtr, ctypes.POINTER(ctypes.c_size_t)]
_lib.fds_element_sequence_item_count.restype = ctypes.c_int

_lib.fds_element_sequence_item_element_count.argtypes = [_ElementPtr, ctypes.c_size_t,
                                                           ctypes.POINTER(ctypes.c_size_t)]
_lib.fds_element_sequence_item_element_count.restype = ctypes.c_int

_lib.fds_element_sequence_item_element_at.argtypes = [_ElementPtr, ctypes.c_size_t, ctypes.c_size_t,
                                                       ctypes.POINTER(_ElementPtr)]
_lib.fds_element_sequence_item_element_at.restype = ctypes.c_int


class StaleElementError(RuntimeError):
    """Raised when an Element/Item is used after its Structure was mutated.

    Every mutation call on Structure can reallocate or shift the underlying
    element list (see docs/abi-design.md "Pointer validity"), so a
    previously-obtained fds_element_t* is no longer safe to dereference.
    ctypes has no way to detect that on its own -- using a stale pointer
    would be undefined behavior, not a clean Python exception -- so
    Structure stamps each Element/Item with a generation counter at
    creation time and every accessor checks it before touching the pointer.
    """


class FdsError(RuntimeError):
    def __init__(self, status: int, context: str = ""):
        self.status = status
        message = _STATUS_MESSAGES.get(status, "unknown status (%d)" % status)
        super().__init__(f"{context}: {message}" if context else message)


class VRRequiredError(FdsError):
    """insert()/insert_text() with vr=None (infer): the tag's VR could not
    be safely inferred (ambiguous, unknown to the standard dictionary, or
    private data) -- supply an explicit `vr` and retry."""


class AlreadyExistsError(FdsError):
    """insert()/insert_text(): an element already exists at the requested
    tag in that container -- these never overwrite; use set_value()/
    set_text() to replace an existing element instead."""


class UnrepresentableCharacterError(FdsError):
    """insert_text()/set_text(): no repertoire in the effective Specific
    Character Set context can encode some character in the given text."""


class InvalidUnicodeInputError(FdsError):
    """insert_text()/set_text(): the given text is not well-formed
    Unicode/UTF-8."""


# A1.7: legacy top-level mutation (set_value/set/erase/erase_private/
# erase_recursive/set_value_recursive) keeps its pre-A1.7 bool-return
# convention (False = "not found/no-op," not an error) -- unchanged. The
# new path-aware operations that can fail for many distinguishable reasons
# (insert/insert_text especially) raise instead: a caller genuinely needs
# to tell "already exists" from "VR required" from "container not found"
# apart to react correctly, which a single collapsed False cannot express.
# This is a deliberate, documented inconsistency with the older bool
# convention, not an oversight -- see docs/architecture/
# A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md section 12/14.
_STATUS_EXCEPTIONS = {
    _FDS_STATUS_ALREADY_EXISTS: AlreadyExistsError,
    _FDS_STATUS_VR_REQUIRED: VRRequiredError,
    _FDS_STATUS_UNREPRESENTABLE_CHARACTER: UnrepresentableCharacterError,
    _FDS_STATUS_INVALID_UNICODE_INPUT: InvalidUnicodeInputError,
}


def _check(status: int, context: str = "") -> None:
    if status != _FDS_STATUS_OK:
        raise _STATUS_EXCEPTIONS.get(status, FdsError)(status, context)


# ---------------------------------------------------------------------------
# Path representation helpers (A1.7) -- see this module's docstring for the
# path/container-locator shapes these normalize.
# ---------------------------------------------------------------------------

def _is_bare_tag(value) -> bool:
    return (isinstance(value, tuple) and len(value) == 2
            and isinstance(value[0], int) and isinstance(value[1], int))


def _normalize_element_path(path) -> List[Tuple[tuple, Optional[int]]]:
    """Accepts a bare (group, element) tuple (a root leaf) or a list of
    (tag, item_index_or_None) steps, and returns the list form."""
    if _is_bare_tag(path):
        return [(path, None)]
    if isinstance(path, list):
        return list(path)
    raise TypeError(
        f"invalid path {path!r}: expected a (group, element) tuple or a list of "
        "(tag, item_index) steps"
    )


def _normalize_parent(parent) -> List[Tuple[tuple, int]]:
    """Accepts None/[] (root) or a list of (tag, item_index) steps, every
    one with a concrete (non-None) item_index -- a container locator, never
    a bare tag (there is no such thing as a one-tag container locator; use
    parent=None for root)."""
    if parent is None:
        return []
    if _is_bare_tag(parent):
        raise TypeError(
            "parent must be a list of (tag, item_index) steps, or None for the root dataset "
            "-- not a bare tag"
        )
    if not isinstance(parent, list):
        raise TypeError(f"invalid parent {parent!r}: expected None or a list of (tag, item_index) steps")
    for tag, item_index in parent:
        if item_index is None:
            raise TypeError(
                f"invalid parent step {(tag, item_index)!r}: every container-locator step needs "
                "a concrete Item index"
            )
    return list(parent)


def _steps_to_c(steps: Sequence[Tuple[tuple, Optional[int]]]):
    arr = (_fds_path_step_t * len(steps))()
    for i, (tag, item_index) in enumerate(steps):
        group, element = tag
        arr[i].tag = _fds_tag_t(group, element)
        if item_index is None:
            arr[i].has_item_index = 0
            arr[i].item_index = 0
        else:
            arr[i].has_item_index = 1
            arr[i].item_index = item_index
    return arr, len(steps)


def _vr_index_or_infer(vr: Optional[str]) -> int:
    if vr is None:
        return _VR_INDEX["UNKNOWN"]  # the ABI's "infer the VR" sentinel -- insert paths only
    try:
        return _VR_INDEX[vr.upper()]
    except KeyError:
        raise ValueError(f"unknown VR {vr!r}; expected one of {sorted(_VR_INDEX)}") from None


def _join_text_values(values: Union[str, Sequence[str]]) -> bytes:
    if isinstance(values, str):
        joined = values
    else:
        joined = "\\".join(values)
    return joined.encode("utf-8")


def _split_text_values(joined: bytes) -> List[str]:
    return joined.decode("utf-8").split("\\")


@dataclass(frozen=True)
class Diagnostic:
    severity: str
    message: str
    offset: int
    tag: Optional[tuple]


@dataclass(frozen=True)
class WriteStats:
    """Byte-level preservation of untouched content -- the concrete number
    behind the README's "quantify byte-level preservation of untouched
    content" success criterion. See WriteResult in write_result.hpp for
    exactly what is and isn't counted.
    """
    bytes_written: int
    source_backed_value_bytes: int
    regenerated_value_bytes: int

    @property
    def preserved_fraction(self) -> float:
        """Fraction (0.0-1.0) of written value-payload bytes that came
        verbatim from the source, unmodified. 1.0 if nothing was written
        (vacuously fully preserved -- avoids a ZeroDivisionError)."""
        total = self.source_backed_value_bytes + self.regenerated_value_bytes
        return 1.0 if total == 0 else self.source_backed_value_bytes / total


class Element:
    """A read-only view of one fds_element_t. Valid only as long as the
    Structure it came from has not been garbage-collected (see
    docs/abi-design.md pointer validity rules) -- this wrapper keeps a
    reference to its Structure to guarantee that -- and only as long as the
    Structure has not been mutated since this Element was obtained (see
    StaleElementError).
    """

    __slots__ = ("_structure", "_ptr", "_generation")

    def __init__(self, structure: "Structure", ptr):
        self._structure = structure
        self._ptr = ptr
        self._generation = structure._generation

    def _check_live(self) -> None:
        if self._generation != self._structure._generation:
            raise StaleElementError(
                "this Element is stale: its Structure was mutated after the Element was "
                "obtained; re-fetch it via Structure.get()/iteration instead of reusing it"
            )

    @property
    def tag(self) -> tuple:
        self._check_live()
        t = _lib.fds_element_tag(self._ptr)
        return (t.group, t.element)

    @property
    def vr(self) -> str:
        self._check_live()
        index = _lib.fds_element_vr(self._ptr)
        return _VR_NAMES[index] if 0 <= index < len(_VR_NAMES) else "UNKNOWN"

    @property
    def is_sequence(self) -> bool:
        self._check_live()
        return bool(_lib.fds_element_is_sequence(self._ptr))

    @property
    def value(self) -> bytes:
        self._check_live()
        if self.is_sequence:
            raise ValueError("sequence elements have no scalar value; iterate items() instead")
        data = ctypes.POINTER(ctypes.c_uint8)()
        length = ctypes.c_size_t()
        _check(_lib.fds_element_value_bytes(self._ptr, ctypes.byref(data), ctypes.byref(length)),
               "fds_element_value_bytes")
        return ctypes.string_at(data, length.value)

    def items(self) -> Iterator["Item"]:
        self._check_live()
        if not self.is_sequence:
            raise ValueError("not a sequence element")
        count = ctypes.c_size_t()
        _check(_lib.fds_element_sequence_item_count(self._ptr, ctypes.byref(count)))
        for i in range(count.value):
            yield Item(self._structure, self._ptr, i, self._generation)

    def __repr__(self):
        g, e = self.tag
        return f"Element(({g:#06x},{e:#06x}), vr={self.vr!r})"


class Item:
    __slots__ = ("_structure", "_seq_ptr", "_index", "_generation")

    def __init__(self, structure: "Structure", seq_ptr, index: int, generation: int):
        self._structure = structure
        self._seq_ptr = seq_ptr
        self._index = index
        self._generation = generation

    def __iter__(self) -> Iterator[Element]:
        if self._generation != self._structure._generation:
            raise StaleElementError(
                "this Item is stale: its Structure was mutated after the Item was obtained; "
                "re-fetch it via Element.items() instead of reusing it"
            )
        count = ctypes.c_size_t()
        _check(_lib.fds_element_sequence_item_element_count(self._seq_ptr, self._index,
                                                              ctypes.byref(count)))
        for i in range(count.value):
            out = _ElementPtr()
            _check(_lib.fds_element_sequence_item_element_at(self._seq_ptr, self._index, i,
                                                               ctypes.byref(out)))
            yield Element(self._structure, out)


class Structure:
    """A parsed DICOMStructure. Owns the underlying fds_structure_t and
    frees it on garbage collection. Every Element/Item obtained from this
    object holds a reference back to it, so as long as you keep any of
    them alive, the Structure (and its backing memory) stays alive too.
    """

    def __init__(self, handle):
        self._handle = handle
        # Bumped by every successful mutation call; stamped onto each
        # Element/Item at creation so they can detect they've gone stale
        # instead of dereferencing an invalidated pointer. See
        # StaleElementError and docs/abi-design.md "Pointer validity".
        self._generation = 0

    def close(self) -> None:
        if getattr(self, "_handle", None):
            _lib.fds_structure_free(self._handle)
            self._handle = None

    def __del__(self):
        self.close()

    def __len__(self) -> int:
        return _lib.fds_structure_element_count(self._handle)

    def __iter__(self) -> Iterator[Element]:
        for i in range(len(self)):
            out = _ElementPtr()
            _check(_lib.fds_structure_element_at(self._handle, i, ctypes.byref(out)))
            yield Element(self, out)

    def get(self, tag: tuple) -> Optional[Element]:
        """Top-level-only convenience -- identical to find((group, element))."""
        group, element = tag
        out = _ElementPtr()
        status = _lib.fds_structure_find(self._handle, _fds_tag_t(group, element), ctypes.byref(out))
        if status == _FDS_STATUS_NOT_FOUND:
            return None
        _check(status, "fds_structure_find")
        return Element(self, out)

    def find(self, path) -> Optional[Element]:
        """Finds an element at any nesting depth -- `path` a bare
        (group, element) tuple (root) or a list of (tag, item_index) steps
        (see this module's docstring). Returns None if `path` doesn't
        resolve.
        """
        steps, count = _steps_to_c(_normalize_element_path(path))
        out = _ElementPtr()
        status = _lib.fds_structure_find_path(self._handle, steps, count, ctypes.byref(out))
        if status == _FDS_STATUS_NOT_FOUND:
            return None
        _check(status, "fds_structure_find_path")
        return Element(self, out)

    def __contains__(self, tag: tuple) -> bool:
        group, element = tag
        return bool(_lib.fds_structure_contains(self._handle, _fds_tag_t(group, element)))

    @property
    def diagnostics(self) -> list:
        count = _lib.fds_structure_diagnostic_count(self._handle)
        result = []
        d = _fds_diagnostic_t()
        for i in range(count):
            _check(_lib.fds_structure_diagnostic_at(self._handle, i, ctypes.byref(d)))
            tag = (d.tag.group, d.tag.element) if d.has_tag else None
            result.append(Diagnostic(
                severity=_SEVERITY_NAMES[d.severity] if 0 <= d.severity < len(_SEVERITY_NAMES) else "info",
                message=(d.message or b"").decode("utf-8", "replace"),
                offset=d.offset,
                tag=tag,
            ))
        return result

    @property
    def transfer_syntax_uid(self) -> str:
        value = _lib.fds_structure_transfer_syntax_uid(self._handle)
        return (value or b"").decode("ascii", "replace")

    @property
    def is_explicit_vr(self) -> bool:
        return bool(_lib.fds_structure_transfer_syntax_is_explicit_vr(self._handle))

    @property
    def is_little_endian(self) -> bool:
        return bool(_lib.fds_structure_transfer_syntax_is_little_endian(self._handle))

    @property
    def pixel_data_kind(self) -> Optional[str]:
        return {0: None, 1: "native", 2: "encapsulated"}.get(
            _lib.fds_structure_pixel_data_kind(self._handle)
        )

    def write(self, path: Union[str, os.PathLike]) -> int:
        """Write this structure to `path`.

        An unmodified structure requires ``fidelity='lossless'`` (a
        byte-identical reproduction). A modified structure (after
        set_value/set/erase/erase_private) requires ``fidelity='lossless'``
        or ``fidelity='standard'`` (a valid, semantically-correct
        reconstruction -- not byte-identical). Returns the number of bytes
        written; raises :class:`FdsError` with an unsupported status
        otherwise. See docs/roundtrip-contract.md "Two write contracts".
        """
        bytes_written = ctypes.c_uint64()
        encoded = os.fspath(path).encode(sys.getfilesystemencoding())
        _check(_lib.fds_structure_write_file(self._handle, encoded,
                                              ctypes.byref(bytes_written)),
               f"fds_structure_write_file({os.fspath(path)!r})")
        return bytes_written.value

    def write_with_stats(self, path: Union[str, os.PathLike]) -> WriteStats:
        """Same as write(), but returns a WriteStats reporting byte-level
        preservation of untouched content instead of a bare byte count.
        """
        bytes_written = ctypes.c_uint64()
        source_backed = ctypes.c_uint64()
        regenerated = ctypes.c_uint64()
        encoded = os.fspath(path).encode(sys.getfilesystemencoding())
        _check(_lib.fds_structure_write_file_with_stats(
                   self._handle, encoded, ctypes.byref(bytes_written),
                   ctypes.byref(source_backed), ctypes.byref(regenerated)),
               f"fds_structure_write_file_with_stats({os.fspath(path)!r})")
        return WriteStats(bytes_written=bytes_written.value,
                           source_backed_value_bytes=source_backed.value,
                           regenerated_value_bytes=regenerated.value)

    def write_bytes(self) -> bytes:
        """Serializes this structure in memory and returns the result as an
        independent `bytes` object -- no filesystem path involved anywhere
        in the call. Same write contract as write()/write_with_stats() (see
        write()'s docstring for the fidelity/modified-state rules).

        The C ABI returns a pointer owned by this Structure (valid only
        until the next write_bytes(_with_stats) call, a mutation, or
        close()); this copies it into a new Python bytes object before
        returning, so the result remains valid independently of that.
        """
        data = ctypes.POINTER(ctypes.c_uint8)()
        length = ctypes.c_size_t()
        _check(_lib.fds_structure_write_buffer(self._handle, ctypes.byref(data),
                                                ctypes.byref(length)),
               "fds_structure_write_buffer")
        return ctypes.string_at(data, length.value)

    def write_bytes_with_stats(self) -> tuple:
        """Same as write_bytes(), but also returns a WriteStats reporting
        byte-level preservation of untouched content -- see
        write_with_stats(). Returns (bytes, WriteStats).
        """
        data = ctypes.POINTER(ctypes.c_uint8)()
        length = ctypes.c_size_t()
        source_backed = ctypes.c_uint64()
        regenerated = ctypes.c_uint64()
        _check(_lib.fds_structure_write_buffer_with_stats(
                   self._handle, ctypes.byref(data), ctypes.byref(length),
                   ctypes.byref(source_backed), ctypes.byref(regenerated)),
               "fds_structure_write_buffer_with_stats")
        output = ctypes.string_at(data, length.value)
        stats = WriteStats(bytes_written=length.value,
                            source_backed_value_bytes=source_backed.value,
                            regenerated_value_bytes=regenerated.value)
        return output, stats

    @property
    def is_modified(self) -> bool:
        return bool(_lib.fds_structure_is_modified(self._handle))

    def set_value(self, path, value: bytes) -> bool:
        """Replaces an existing element's value (VR preserved), at any
        nesting depth -- `path` a bare (group, element) tuple (root) or a
        list of (tag, item_index) steps (see this module's docstring).
        Returns False, leaving the element unchanged, if `path` doesn't
        resolve, names a sequence element, or `value` can't be encoded in
        the element's length form -- see StaleElementError before reusing
        any Element/Item obtained before this call.

        `value` is written exactly as given -- this library does not
        auto-pad it to DICOM's required even length (PS3.5 6.4). An
        odd-length `value` produces a structurally non-conformant element;
        pad it yourself (trailing b'\\x00', or b' ' for text VRs other than
        UI) before calling, the same way tests/integration/fixture_builder.cpp
        does for C++ test fixtures.
        """
        steps, count = _steps_to_c(_normalize_element_path(path))
        buf = (ctypes.c_uint8 * len(value)).from_buffer_copy(value)
        status = _lib.fds_structure_set_value_path(self._handle, steps, count, buf, len(value))
        if status in (_FDS_STATUS_NOT_FOUND, _FDS_STATUS_INVALID_ARGUMENT):
            return False
        _check(status, "fds_structure_set_value_path")
        self._generation += 1
        return True

    def set(self, tag: tuple, vr: str, value: bytes) -> bool:
        """Upserts a top-level element: replaces the value if `tag` already
        exists (its VR preserved, `vr` ignored), otherwise inserts a
        brand-new element with `vr` at its sorted tag position. Returns
        False, applying no change, if the existing element is a sequence or
        `value` can't be encoded for the relevant length form.

        As with set_value, `value` is written exactly as given and is not
        auto-padded to DICOM's required even length -- see set_value's note.
        """
        group, element = tag
        try:
            vr_index = _VR_INDEX[vr.upper()]
        except KeyError:
            raise ValueError(f"unknown VR {vr!r}; expected one of {sorted(_VR_INDEX)}") from None
        buf = (ctypes.c_uint8 * len(value)).from_buffer_copy(value)
        status = _lib.fds_structure_set(self._handle, _fds_tag_t(group, element), vr_index,
                                         buf, len(value))
        if status == _FDS_STATUS_INVALID_ARGUMENT:
            return False
        _check(status, "fds_structure_set")
        self._generation += 1
        return True

    def erase(self, path) -> bool:
        """Removes an element at any nesting depth -- `path` a bare
        (group, element) tuple (root) or a list of (tag, item_index) steps.
        If `path` names a Sequence element, its entire subtree (every Item
        and everything in it) is removed with it. Returns False if `path`
        doesn't resolve.
        """
        steps, count = _steps_to_c(_normalize_element_path(path))
        status = _lib.fds_structure_erase_path(self._handle, steps, count)
        if status == _FDS_STATUS_NOT_FOUND:
            return False
        _check(status, "fds_structure_erase_path")
        self._generation += 1
        return True

    def erase_private(self) -> int:
        """Removes every element with an odd group number (DICOM's
        private-tag convention), at any nesting depth. Returns the count
        removed (0 is not an error).
        """
        count = ctypes.c_size_t()
        _check(_lib.fds_structure_erase_private(self._handle, ctypes.byref(count)),
               "fds_structure_erase_private")
        if count.value > 0:
            self._generation += 1
        return count.value

    def erase_recursive(self, tag: tuple) -> int:
        """Removes every element whose tag equals `tag`, at any nesting
        depth -- the recursive counterpart to erase(tag), which only
        matches a top-level occurrence. Returns the count removed (0 is
        not an error).
        """
        group, element = tag
        count = ctypes.c_size_t()
        _check(_lib.fds_structure_erase_recursive(self._handle, _fds_tag_t(group, element),
                                                    ctypes.byref(count)),
               "fds_structure_erase_recursive")
        if count.value > 0:
            self._generation += 1
        return count.value

    def set_value_recursive(self, tag: tuple, value: bytes) -> int:
        """Replaces the value of every non-sequence element whose tag
        equals `tag`, at any nesting depth, with `value` -- the recursive
        counterpart to set_value(tag, value), which only matches a
        top-level occurrence. An occurrence that is itself a sequence
        element is left unchanged and not counted, same as set_value. Not
        auto-padded, same as set_value -- see its docstring. Returns the
        count changed (0 is not an error).
        """
        group, element = tag
        buf = (ctypes.c_uint8 * len(value)).from_buffer_copy(value)
        count = ctypes.c_size_t()
        _check(_lib.fds_structure_set_value_recursive(self._handle, _fds_tag_t(group, element),
                                                        buf, len(value), ctypes.byref(count)),
               "fds_structure_set_value_recursive")
        if count.value > 0:
            self._generation += 1
        return count.value

    # --- A1.7: path-aware insertion, charset-aware text, recursive iteration

    def insert(self, tag: tuple, value: bytes, vr: Optional[str] = None, parent=None) -> None:
        """Inserts a brand-new element -- `tag` -- into the container named
        by `parent` (None or [] for root; a list of (tag, item_index) steps
        for nested -- see this module's docstring). `tag` must not already
        exist in that container.

        `vr`, if given, is the caller's own authoritative VR (never
        consulted against the dictionary). If omitted (None, the default),
        the VR is inferred from the standard PS3.6 dictionary when
        unambiguous; raises VRRequiredError if inference is ambiguous,
        unknown, or (private data) never attempted -- supply an explicit
        `vr` and retry in that case.

        Raises AlreadyExistsError, VRRequiredError, or FdsError (bad
        container path, invalid VR, value too long/odd) rather than
        returning False -- unlike set_value()/erase(), see this module's
        docstring on the bool-vs-exception convention. `value` is not
        auto-padded -- see set_value()'s docstring.
        """
        parent_steps, parent_count = _steps_to_c(_normalize_parent(parent))
        group, element = tag
        vr_index = _vr_index_or_infer(vr)
        buf = (ctypes.c_uint8 * len(value)).from_buffer_copy(value)
        status = _lib.fds_structure_insert_path(self._handle, parent_steps, parent_count,
                                                  _fds_tag_t(group, element), vr_index, buf,
                                                  len(value))
        _check(status, "fds_structure_insert_path")
        self._generation += 1

    def insert_text(self, tag: tuple, values: Union[str, Sequence[str]],
                     vr: Optional[str] = None, parent=None) -> None:
        """The Unicode-text counterpart to insert(): `values` is a single
        str (one VM component) or a sequence of str (multi-valued),
        encoded under the target container's effective Specific Character
        Set context (A1.5/A1.6) -- resolved AT the container the new
        element is inserted into, not merely its parent's parent. Same
        `parent`/`vr` (None = infer) conventions and exception model as
        insert(); also raises UnrepresentableCharacterError or
        InvalidUnicodeInputError for charset-specific failures.
        """
        parent_steps, parent_count = _steps_to_c(_normalize_parent(parent))
        group, element = tag
        vr_index = _vr_index_or_infer(vr)
        joined = _join_text_values(values)
        status = _lib.fds_structure_insert_text_path(self._handle, parent_steps, parent_count,
                                                       _fds_tag_t(group, element), vr_index, joined)
        _check(status, "fds_structure_insert_text_path")
        self._generation += 1

    def set_text(self, path, values: Union[str, Sequence[str]]) -> None:
        """Replaces an existing text-VR element's value with Unicode text
        (`values`: a single str, or a sequence of str for a multi-valued
        VR), encoded under its already-effective Specific Character Set
        context. Raises FdsError (not found, not a text VR, unsupported/
        malformed charset declaration), UnrepresentableCharacterError, or
        InvalidUnicodeInputError -- see insert_text()'s exception model.
        """
        steps, count = _steps_to_c(_normalize_element_path(path))
        joined = _join_text_values(values)
        status = _lib.fds_structure_set_text_path(self._handle, steps, count, joined)
        _check(status, "fds_structure_set_text_path")
        self._generation += 1

    def decode_text(self, path) -> List[str]:
        """Decodes an existing text-VR element's value into Unicode text
        under its already-effective Specific Character Set context.
        Returns one str per VM component (mirroring set_text()'s own input
        convention, so decode_text(path) round-trips through
        set_text(path, ...)). Raises FdsError for a non-text VR, or a
        charset declaration outside this library's V1 envelope.
        """
        steps, count = _steps_to_c(_normalize_element_path(path))
        out = ctypes.c_char_p()
        status = _lib.fds_structure_decode_text_path(self._handle, steps, count, ctypes.byref(out))
        _check(status, "fds_structure_decode_text_path")
        return _split_text_values(out.value or b"")

    def iter_elements(self, recursive: bool = True) -> Iterator[Tuple[Element, list]]:
        """Yields (element, path) for every element -- top-level only if
        `recursive` is False (equivalent to iter(self), but each element is
        paired with its one-step path), or, when True (the default), every
        element at every nesting depth in a deterministic, document-order
        (depth-first, tag-ascending-within-each-container) traversal. Each
        yielded `path` is in this module's standard list-of-steps form,
        directly reusable with find()/set_value()/erase()/etc.

        Composed entirely from existing indexed element/sequence/item
        accessors -- no dedicated recursive-traversal ABI call exists or is
        needed (see docs/abi-design.md).
        """
        def walk(elements: Iterator[Element], prefix: list) -> Iterator[Tuple[Element, list]]:
            for element in elements:
                path = prefix + [(element.tag, None)]
                yield element, path
                if recursive and element.is_sequence:
                    for item_index, item in enumerate(element.items()):
                        yield from walk(iter(item), prefix + [(element.tag, item_index)])

        yield from walk(iter(self), [])


def _make_options(fidelity: str) -> _fds_parse_options_t:
    options = _fds_parse_options_t()
    _lib.fds_parse_options_init_defaults(ctypes.byref(options))
    try:
        options.fidelity = _FIDELITY[fidelity]
    except KeyError:
        raise ValueError(f"unknown fidelity {fidelity!r}; expected one of {sorted(_FIDELITY)}")
    return options


def read(path: Union[str, os.PathLike], fidelity: str = "standard") -> Structure:
    """Parses a DICOM file. Raises FdsError if the Transfer Syntax is
    unsupported or the file could not be read at all; a partially
    successful parse (warnings but a usable structure) does not raise --
    check .diagnostics.
    """
    options = _make_options(fidelity)
    handle = _StructurePtr()
    status = _lib.fds_parse_file(os.fspath(path).encode(sys.getfilesystemencoding()),
                                  ctypes.byref(options), ctypes.byref(handle))
    structure = Structure(handle) if handle else None
    if status != _FDS_STATUS_OK:
        if structure is not None:
            structure.close()
        raise FdsError(status, f"fds_parse_file({os.fspath(path)!r})")
    return structure


def read_buffer(data: bytes, fidelity: str = "standard") -> Structure:
    """Parses an in-memory DICOM buffer.

    The C ABI copies `data` into storage owned by the returned structure, so
    callers do not need to retain their own reference after this call.
    """
    options = _make_options(fidelity)
    handle = _StructurePtr()
    source = ctypes.c_char_p(data)
    data_ptr = ctypes.cast(source, ctypes.POINTER(ctypes.c_uint8))
    status = _lib.fds_parse_buffer(data_ptr, len(data), ctypes.byref(options), ctypes.byref(handle))
    structure = Structure(handle) if handle else None
    if status != _FDS_STATUS_OK:
        if structure is not None:
            structure.close()
        raise FdsError(status, "fds_parse_buffer")
    return structure
