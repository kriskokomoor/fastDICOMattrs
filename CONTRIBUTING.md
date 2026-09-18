# Contributing to fastDICOMattrs

Bug reports, focused feature proposals, documentation improvements, and pull requests are welcome.

## Before opening a change

Please open an issue before undertaking a large API or architecture change. Keep the public API compatible where practical, preserve unknown DICOM content, and include tests for behavior changes. Never commit clinical DICOM data or protected health information; test fixtures must be synthetic or explicitly license-cleared.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
PYTHONPATH=python python3 -m unittest discover -s tests/python -v
```

Pull requests should explain the motivation, user-visible behavior, limitations, and test coverage. Update the relevant design contract under `docs/` when changing parsing, ownership, ABI, or round-trip behavior.

## Optional: corpus probing against real DICOM data

`python/fastdicomattrs/corpus.py` (see its module docstring and `make corpus`) can probe a real, license-cleared local DICOM corpus you supply -- none is vendored in this repository (see `tests/fixtures/README.md`). Its `--reference pydicom` mode additionally cross-checks structural parsing against [pydicom](https://pydicom.github.io/), an independent DICOM implementation; install it with `pip install -r python/requirements-dev.txt` (or just `pip install pydicom`) if you want to use that mode. It is never required for building, testing, or using the library itself. See `docs/corpus-results.md` for the results of the most recent such run.

## License

By contributing, you agree to dedicate your contribution to the public domain under the terms in `LICENSE`.
