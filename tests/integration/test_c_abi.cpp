#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "fastdicomattrs_c/fds.h"
#include "integration/fixture_builder.hpp"

using namespace fds_test;

TEST_CASE("C ABI parse_buffer owns the caller's bytes", "[abi]") {
  auto fixture = make_file_meta("1.2.840.10008.1.2.1");
  fixture.element_short(0x0008, 0x0060, "CS", "CT");
  auto bytes = fixture.bytes();

  fds_parse_options_t options;
  fds_parse_options_init_defaults(&options);
  fds_structure_t* structure = nullptr;
  REQUIRE(fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                           &options, &structure) == FDS_STATUS_OK);
  REQUIRE(structure != nullptr);

  std::fill(bytes.begin(), bytes.end(), std::byte{0});
  const fds_element_t* element = nullptr;
  REQUIRE(fds_structure_find(structure, fds_tag_t{0x0008, 0x0060}, &element) == FDS_STATUS_OK);
  const std::uint8_t* value = nullptr;
  std::size_t length = 0;
  REQUIRE(fds_element_value_bytes(element, &value, &length) == FDS_STATUS_OK);
  REQUIRE(length == 2);
  REQUIRE(value[0] == static_cast<std::uint8_t>('C'));
  REQUIRE(value[1] == static_cast<std::uint8_t>('T'));
  fds_structure_free(structure);
}

TEST_CASE("C ABI rejects SQ Unknown and invalid VR insertion", "[abi]") {
  auto fixture = make_file_meta("1.2.840.10008.1.2.1");
  fds_structure_t* structure = nullptr;
  REQUIRE(fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(fixture.bytes().data()),
                           fixture.bytes().size(), nullptr, &structure) == FDS_STATUS_OK);

  const std::uint8_t value[2] = {'A', 'B'};
  const fds_tag_t tag{0x0008, 0x1111};
  REQUIRE(fds_structure_set(structure, tag, FDS_VR_SQ, value, 2) ==
          FDS_STATUS_INVALID_ARGUMENT);
  REQUIRE(fds_structure_set(structure, tag, FDS_VR_UNKNOWN, value, 2) ==
          FDS_STATUS_INVALID_ARGUMENT);
  REQUIRE(fds_structure_set(structure, tag, static_cast<fds_vr_t>(999), value, 2) ==
          FDS_STATUS_INVALID_ARGUMENT);
  REQUIRE(fds_structure_contains(structure, tag) == 0);
  fds_structure_free(structure);
}

namespace {
fds_parse_options_t lossless_options() {
  fds_parse_options_t options;
  fds_parse_options_init_defaults(&options);
  options.fidelity = FDS_FIDELITY_LOSSLESS;
  return options;
}
}  // namespace

TEST_CASE("C ABI write_buffer produces bytes that reparse successfully", "[abi][write-buffer]") {
  auto fixture = make_file_meta("1.2.840.10008.1.2.1");
  fixture.element_short(0x0010, 0x0020, "LO", "ID1");
  fixture.element_short(0x0008, 0x0060, "CS", "CT");
  auto bytes = fixture.bytes();

  auto options = lossless_options();
  fds_structure_t* structure = nullptr;
  REQUIRE(fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                           &options, &structure) == FDS_STATUS_OK);

  const std::uint8_t* out_data = nullptr;
  std::size_t out_length = 0;
  REQUIRE(fds_structure_write_buffer(structure, &out_data, &out_length) == FDS_STATUS_OK);
  REQUIRE(out_data != nullptr);
  REQUIRE(out_length == bytes.size());

  fds_structure_t* reparsed = nullptr;
  REQUIRE(fds_parse_buffer(out_data, out_length, &options, &reparsed) == FDS_STATUS_OK);
  const fds_element_t* element = nullptr;
  REQUIRE(fds_structure_find(reparsed, fds_tag_t{0x0008, 0x0060}, &element) == FDS_STATUS_OK);
  const std::uint8_t* value = nullptr;
  std::size_t length = 0;
  REQUIRE(fds_element_value_bytes(element, &value, &length) == FDS_STATUS_OK);
  REQUIRE(length == 2);
  REQUIRE(value[0] == static_cast<std::uint8_t>('C'));
  REQUIRE(value[1] == static_cast<std::uint8_t>('T'));

  fds_structure_free(reparsed);
  fds_structure_free(structure);
}

TEST_CASE("C ABI write_buffer is byte-identical to write_file for the same structure",
          "[abi][write-buffer]") {
  auto fixture = make_file_meta("1.2.840.10008.1.2.1");
  fixture.element_short(0x0010, 0x0010, "PN", "Doe^Jane");
  std::vector<std::byte> pixels(32, std::byte{0x7A});
  fixture.pixel_data_native("OW", pixels);
  auto bytes = fixture.bytes();

  auto options = lossless_options();
  fds_structure_t* structure = nullptr;
  REQUIRE(fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                           &options, &structure) == FDS_STATUS_OK);

  const std::uint8_t* out_data = nullptr;
  std::size_t out_length = 0;
  REQUIRE(fds_structure_write_buffer(structure, &out_data, &out_length) == FDS_STATUS_OK);
  std::vector<std::uint8_t> from_buffer(out_data, out_data + out_length);

  auto path = std::filesystem::temp_directory_path() /
              "fds_write_buffer_equivalence_test.dcm";
  std::uint64_t file_bytes_written = 0;
  REQUIRE(fds_structure_write_file(structure, path.string().c_str(), &file_bytes_written) ==
          FDS_STATUS_OK);
  std::ifstream in(path, std::ios::binary);
  std::vector<std::uint8_t> from_file((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
  in.close();
  std::remove(path.string().c_str());

  REQUIRE(file_bytes_written == out_length);
  REQUIRE(from_buffer == from_file);

  fds_structure_free(structure);
}

TEST_CASE("C ABI write_buffer_with_stats reports the same counters as write_file_with_stats",
          "[abi][write-buffer]") {
  auto fixture = make_file_meta("1.2.840.10008.1.2.1");
  fixture.element_short(0x0010, 0x0020, "LO", "ID1");  // will be replaced
  fixture.element_short(0x0008, 0x0060, "CS", "CT");   // stays untouched
  auto bytes = fixture.bytes();

  auto options = lossless_options();
  fds_structure_t* structure = nullptr;
  REQUIRE(fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                           &options, &structure) == FDS_STATUS_OK);

  const std::uint8_t replacement[10] = {'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A'};
  REQUIRE(fds_structure_set_value(structure, fds_tag_t{0x0010, 0x0020}, replacement, 10) ==
          FDS_STATUS_OK);

  const std::uint8_t* out_data = nullptr;
  std::size_t out_length = 0;
  std::uint64_t source_backed = 0;
  std::uint64_t regenerated = 0;
  REQUIRE(fds_structure_write_buffer_with_stats(structure, &out_data, &out_length, &source_backed,
                                                 &regenerated) == FDS_STATUS_OK);
  REQUIRE(regenerated == 10 + 4);  // +4: recomputed File Meta group length
  REQUIRE(source_backed >= 2);     // "CT" survives untouched

  fds_structure_free(structure);
}

TEST_CASE("C ABI write_buffer is Unsupported for a STANDARD-fidelity unmodified structure",
          "[abi][write-buffer]") {
  auto fixture = make_file_meta("1.2.840.10008.1.2.1");
  auto bytes = fixture.bytes();

  fds_parse_options_t options;
  fds_parse_options_init_defaults(&options);
  options.fidelity = FDS_FIDELITY_STANDARD;
  fds_structure_t* structure = nullptr;
  REQUIRE(fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                           &options, &structure) == FDS_STATUS_OK);

  const std::uint8_t* out_data = nullptr;
  std::size_t out_length = 0;
  REQUIRE(fds_structure_write_buffer(structure, &out_data, &out_length) == FDS_STATUS_UNSUPPORTED);
  REQUIRE(out_data == nullptr);
  REQUIRE(out_length == 0);

  fds_structure_free(structure);
}

TEST_CASE("C ABI write_buffer rejects null arguments", "[abi][write-buffer]") {
  auto fixture = make_file_meta("1.2.840.10008.1.2.1");
  auto bytes = fixture.bytes();
  auto options = lossless_options();
  fds_structure_t* structure = nullptr;
  REQUIRE(fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                           &options, &structure) == FDS_STATUS_OK);

  const std::uint8_t* out_data = nullptr;
  std::size_t out_length = 0;
  REQUIRE(fds_structure_write_buffer(nullptr, &out_data, &out_length) ==
          FDS_STATUS_INVALID_ARGUMENT);
  REQUIRE(fds_structure_write_buffer(structure, nullptr, &out_length) ==
          FDS_STATUS_INVALID_ARGUMENT);
  REQUIRE(fds_structure_write_buffer(structure, &out_data, nullptr) ==
          FDS_STATUS_INVALID_ARGUMENT);

  fds_structure_free(structure);
}
