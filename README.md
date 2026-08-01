# fastDICOM

`fastDICOM` is a small, public-domain C++20 library for retrieving one or many
top-level attributes from DICOM Part 10 files with DCMTK, plus a thin Python
layer over the same core. It is designed for metadata scans over large image
collections — not as a general DICOM toolkit; DCMTK already provides that.

The multi-tag API opens and parses a file once. DCMTK's deferred loading is used
for values larger than 4 KiB by default, so pixel data is normally not read into
memory. A handle-based overload permits callers to load once and make repeated
queries.

## Architecture

- **`include/fastdicom/` + `src/tags.cpp`** — the C++ core and the only place
  DCMTK types are used. The public header forward-declares `DcmFileFormat` and
  never includes a DCMTK header; every returned value (`Tag`, `TagResult`,
  `ReadOptions`) is an owned standard-library type, so DCMTK stays an
  implementation detail.
- **`python/fastdicom/`** — a pybind11 extension module that wraps the C++
  `getTag`/`getTags` functions and translates their results into Python
  idioms (`None` for a missing attribute, an exception for a read error). It
  does not reimplement any DICOM parsing.
- **`examples/fastdicom.cpp`** — the `fastdicom-cli` command-line tool.
- **`tests/`** — C++ unit/file-round-trip tests (`tests/*.cpp`) and a pytest
  suite (`tests/python/`) that checks the Python bindings against pydicom.
- **`benchmark/`** — throughput comparisons against pydicom.

## Build (C++ library and CLI)

Install a C++20 compiler, CMake, and the DCMTK development package. On Debian or
Ubuntu, the latter is `libdcmtk-dev`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To install the library and headers:

```sh
cmake --install build --prefix /desired/prefix
```

## C++ API

```cpp
#include <fastdicom/tags.hpp>

using fastdicom::Tag;
auto one = fastdicom::getTag("image.dcm", Tag{0x0010, 0x0010});

auto many = fastdicom::getTags(
    "image.dcm",
    {{0x0010, 0x0010},  // Patient Name
     {0x0010, 0x0020},  // Patient ID
     {0x0008, 0x0060}}  // Modality
);
```

Each `TagResult` has a `status`:

- `found`: `value` contains DCMTK's backslash-separated string representation.
- `missing`: the requested top-level attribute is absent.
- `error`: the file could not be read or the value could not be converted;
  `message` contains a diagnostic.

File Meta Information tags (group `0002`) are looked up in the meta header. All
other tags are looked up at the data-set root. Sequence traversal is deliberately
not implicit because the same tag can occur in many nested items.

For an already loaded `DcmFileFormat`, use the handle overload:

```cpp
DcmFileFormat file;
file.loadFile("image.dcm");
auto result = fastdicom::getTag(file, {0x0008, 0x0060});
```

The result types use owned standard-library values and expose no DCMTK ownership,
which is what keeps the pybind11 binding a thin wrapper rather than a second
implementation.

### Command-line example

```sh
./build/fastdicom-cli image.dcm 0010,0010 0010,0020 0008,0060
```

Output is tab-separated. Missing values print as `<missing>` and file/read errors
print as `<error: ...>` with a nonzero process exit status.

### Makefile and testing a DICOM file

The supplied Makefile provides a direct alternative to CMake:

```sh
make
make test
make test-file DICOM_FILE=/path/to/image.dcm
```

`test-file` runs both `getTag()` and `getTags()` against the supplied file and
checks that their status and value agree. By default it checks SOP Class UID,
Modality, Patient Name, and Patient ID. Missing tags are valid results; file or
value-reading errors fail the test. Supply a custom tag list with:

```sh
make test-file DICOM_FILE=image.dcm TAGS="0020,000d 0020,000e 0008,0018"
```

The same input-file test can be built with CMake and run directly:

```sh
./build/fastdicom-file-test image.dcm 0010,0010 0008,0060
```

## Python bindings

The Python bindings are a pybind11 module built by the same CMake project,
gated by the `FASTDICOM_BUILD_PYTHON` option (default `OFF`, so the C++ library
builds standalone without a Python toolchain). Install with:

```sh
pip install .
```

This uses `pyproject.toml` + `scikit-build-core`, which configures CMake with
`-DFASTDICOM_BUILD_PYTHON=ON` and builds only the extension module and Python
package into the wheel (the C++ install/export tree — headers, static
library, CMake package config — is skipped for wheel builds since it isn't
useful there). Requires the same DCMTK development package as the C++ build;
`pip install .` does not vendor or statically link DCMTK.

To build the extension directly with CMake instead (e.g. alongside the CLI and
C++ tests):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFASTDICOM_BUILD_PYTHON=ON
cmake --build build --parallel
```

### Python API

```python
import fastdicom

value = fastdicom.get_tag("image.dcm", "0010,0010")

values = fastdicom.get_tags(
    "image.dcm",
    ["0010,0010", "0010,0020", "0008,0060"],
)
```

- `get_tag(filename, tag)` returns the attribute's string value, or `None` if
  the attribute is absent from the file — a normal, expected outcome, not an
  error. `tag` is a `"gggg,eeee"` hex string, same format as the CLI accepts.
- `get_tags(filename, tags)` returns a `dict` mapping each requested tag
  string to its value (or `None`); the file is parsed once for every tag in
  the list, same as the C++ `getTags()` batch call. Duplicate tags in the
  input collapse to one dict key.
- A file that can't be read, or a value that can't be converted, raises
  `fastdicom.FastDicomError` (a `RuntimeError` subclass) with a diagnostic
  message. A malformed tag string raises `ValueError`.

## Testing

C++ tests:

```sh
ctest --test-dir build --output-on-failure
```

Python tests (checks fastdicom's results against pydicom's own reading of the
same files):

```sh
pip install ".[test]"
pytest
```

## Benchmarking

`benchmark/` compares fastdicom against pydicom on single-tag lookup,
multi-tag lookup, and directory traversal, using an identical tag list for
both. DICOM files aren't committed to this repository, so point the scripts
at your own directory:

```sh
pip install ".[test]"  # for pydicom
python benchmark/benchmark_fastdicom.py /path/to/dicom/directory
python benchmark/benchmark_pydicom.py /path/to/dicom/directory
```

See `benchmark/README.md` for details on what each scenario measures.

## Performance guidance

- Request all needed attributes in one `getTags`/`get_tags` call rather than
  repeatedly calling the filename-based `getTag`/`get_tag` overload.
- For repeated queries against one file, load a `DcmFileFormat` once and use the
  handle overload (C++ only; the Python bindings only expose the filename-based
  calls today).
- Tune `ReadOptions::max_value_bytes` if legitimate metadata values larger than
  4 KiB must be returned. Raising it may also increase allocation and I/O.
- Build benchmarks and production scanners in `Release` mode.

## Scope and safety

This library reads attributes; it does not validate an entire DICOM object or
anonymize protected health information. Applications must handle DICOM values as
sensitive data and must not log them unintentionally.

## Contributing

Bug reports and pull requests are welcome. Run the test suite before submitting
a change and keep the public API compatible where practical. By contributing,
you agree to dedicate your contribution under the Unlicense terms in `LICENSE`.
