# Benchmarks

Compares `fastdicom` against `pydicom` on identical work: the same DICOM
files and the same tag list (defined once in `common.py`, shared by both
scripts). `benchmark_pydicom.py` reads with `stop_before_pixels=True` and
`specific_tags=` so pydicom decodes the same restricted scope fastdicom
does, rather than the full dataset.

Each script runs three scenarios against a directory you provide:

1. **single tag lookup** — one attribute (Patient Name) per file
2. **multiple tag lookup** — a fixed list of 7 attributes per file, one call
3. **directory traversal** — recursive directory walk, same 7 attributes
   per file discovered

DICOM files aren't committed to this repository (see `.gitignore` and
`CONTRIBUTING.md`), so point these scripts at your own local directory.

## Usage

```sh
pip install pydicom  # not a fastdicom dependency, only needed to run this
pip install .         # build and install fastdicom itself

python benchmark/benchmark_fastdicom.py /path/to/dicom/directory
python benchmark/benchmark_pydicom.py /path/to/dicom/directory
```

Each run prints elapsed time, files/sec, and tags/sec per scenario using
`time.perf_counter_ns()`. Run in a `Release`/optimized build for
meaningful numbers.
