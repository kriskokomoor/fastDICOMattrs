CXX ?= c++
CPPFLAGS ?=
CXXFLAGS ?= -O3 -DNDEBUG -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -ldcmdata -lofstd -loflog -lz -pthread

CPPFLAGS += -Iinclude
CXXFLAGS += -std=c++20

BUILD_DIR := build-make
LIB_OBJECT := $(BUILD_DIR)/tags.o
CLI := $(BUILD_DIR)/fastdicom-cli
UNIT_TEST := $(BUILD_DIR)/fastdicom-tests
FILE_TEST := $(BUILD_DIR)/fastdicom-file-test

.PHONY: all clean test test-file

all: $(CLI) $(UNIT_TEST) $(FILE_TEST)

$(BUILD_DIR):
	mkdir -p $@

$(LIB_OBJECT): src/tags.cpp include/fastdicom/tags.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(CLI): examples/fastdicom.cpp $(LIB_OBJECT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB_OBJECT) $(LDFLAGS) $(LDLIBS) -o $@

$(UNIT_TEST): tests/tags_test.cpp $(LIB_OBJECT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB_OBJECT) $(LDFLAGS) $(LDLIBS) -o $@

$(FILE_TEST): tests/dicom_file_test.cpp $(LIB_OBJECT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIB_OBJECT) $(LDFLAGS) $(LDLIBS) -o $@

test: $(UNIT_TEST)
	./$(UNIT_TEST)

# Usage: make test-file DICOM_FILE=/path/image.dcm
# Optional: make test-file DICOM_FILE=image.dcm TAGS="0010,0010 0008,0060"
test-file: $(FILE_TEST)
	@test -n "$(DICOM_FILE)" || { echo "DICOM_FILE is required" >&2; exit 2; }
	./$(FILE_TEST) "$(DICOM_FILE)" $(TAGS)

clean:
	rm -rf $(BUILD_DIR)

