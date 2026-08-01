# fastDICOM

`fastDICOM` is a small, public-domain C++20 library for retrieving one or many
top-level attributes from DICOM Part 10 files with DCMTK. It is designed for
metadata scans over large image collections and for later wrapping in Python.

The multi-tag API opens and parses a file once. DCMTK's deferred loading is used
for values larger than 4 KiB by default, so pixel data is normally not read into
memory. A handle-based overload permits callers to load once and make repeated
queries.

## Build

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

## API

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
which keeps a future pybind11 binding straightforward.

## Command-line example

```sh
./build/fastdicom-cli image.dcm 0010,0010 0010,0020 0008,0060
```

Output is tab-separated. Missing values print as `<missing>` and file/read errors
print as `<error: ...>` with a nonzero process exit status.

## Makefile and testing a DICOM file

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

## Performance guidance

- Request all needed attributes in one `getTags` call rather than repeatedly
  calling the filename-based `getTag` overload.
- For repeated queries against one file, load a `DcmFileFormat` once and use the
  handle overload.
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
