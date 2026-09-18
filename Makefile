BUILD_DIR ?= build
PYTHON ?= python3
CORPUS ?=
REFERENCE ?=

.PHONY: all configure build test test-cpp test-python corpus clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR)

build: configure
	cmake --build $(BUILD_DIR)

test: test-cpp test-python

test-cpp: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

test-python: build
	PYTHONPATH=python $(PYTHON) -m unittest discover -s tests/python -v

corpus: build
	@test -n "$(CORPUS)" || { echo "usage: make corpus CORPUS=/path/to/dicom [REFERENCE=pydicom]" >&2; exit 2; }
	PYTHONPATH=python $(PYTHON) -m fastdicomattrs.corpus "$(CORPUS)" --details \
		$(if $(REFERENCE),--reference $(REFERENCE),)

clean:
	cmake --build $(BUILD_DIR) --target clean
