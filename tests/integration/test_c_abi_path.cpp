#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "fastdicomattrs_c/fds.h"
#include "integration/fixture_builder.hpp"

// A1.7 -- C ABI path-based (nested) mutation: fds_structure_find_path,
// _set_value_path, _erase_path, _insert_path, _decode_text_path,
// _set_text_path, _insert_text_path, and the fds_path_step_t
// container-locator/element-locator distinction. See docs/abi-design.md
// and docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md.

using namespace fds_test;

namespace {
fds_path_step_t leaf(fds_tag_t tag) { return fds_path_step_t{tag, 0, 0}; }
fds_path_step_t descend(fds_tag_t tag, std::size_t item_index) {
  return fds_path_step_t{tag, 1, item_index};
}

fds_structure_t* parse(const FixtureBuilder& b) {
  fds_structure_t* structure = nullptr;
  auto status = fds_parse_buffer(reinterpret_cast<const std::uint8_t*>(b.bytes().data()),
                                  b.bytes().size(), nullptr, &structure);
  REQUIRE(status == FDS_STATUS_OK);
  return structure;
}
}  // namespace

TEST_CASE("ABI find_path/set_value_path/erase_path work at every nesting depth", "[abi][path]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto* structure = parse(b);

  std::vector<fds_path_step_t> path = {descend(fds_tag_t{0x0008, 0x1140}, 0),
                                        leaf(fds_tag_t{0x0008, 0x0100})};

  const fds_element_t* element = nullptr;
  REQUIRE(fds_structure_find_path(structure, path.data(), path.size(), &element) == FDS_STATUS_OK);
  REQUIRE(element != nullptr);
  REQUIRE(fds_element_tag(element).element == 0x0100);

  const std::uint8_t new_value[6] = {'9', '9', '9', '9', '9', ' '};
  REQUIRE(fds_structure_set_value_path(structure, path.data(), path.size(), new_value, 6) ==
          FDS_STATUS_OK);
  REQUIRE(fds_structure_find_path(structure, path.data(), path.size(), &element) == FDS_STATUS_OK);
  const std::uint8_t* bytes = nullptr;
  std::size_t length = 0;
  REQUIRE(fds_element_value_bytes(element, &bytes, &length) == FDS_STATUS_OK);
  REQUIRE(length == 6);
  REQUIRE(std::memcmp(bytes, new_value, 6) == 0);

  REQUIRE(fds_structure_erase_path(structure, path.data(), path.size()) == FDS_STATUS_OK);
  REQUIRE(fds_structure_find_path(structure, path.data(), path.size(), &element) ==
          FDS_STATUS_NOT_FOUND);
  REQUIRE(fds_structure_erase_path(structure, path.data(), path.size()) == FDS_STATUS_NOT_FOUND);

  fds_structure_free(structure);
}

TEST_CASE("ABI insert_path with an explicit VR inserts, root and nested", "[abi][path]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0060, "CS", "CT");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto* structure = parse(b);

  const std::uint8_t value[4] = {'I', 'D', '1', ' '};
  REQUIRE(fds_structure_insert_path(structure, nullptr, 0, fds_tag_t{0x0010, 0x0020}, FDS_VR_LO,
                                     value, 4) == FDS_STATUS_OK);
  REQUIRE(fds_structure_contains(structure, fds_tag_t{0x0010, 0x0020}) != 0);

  std::vector<fds_path_step_t> parent = {descend(fds_tag_t{0x0008, 0x1140}, 0)};
  REQUIRE(fds_structure_insert_path(structure, parent.data(), parent.size(),
                                     fds_tag_t{0x0010, 0x0020}, FDS_VR_LO, value, 4) ==
          FDS_STATUS_OK);
  std::vector<fds_path_step_t> path = {descend(fds_tag_t{0x0008, 0x1140}, 0),
                                        leaf(fds_tag_t{0x0010, 0x0020})};
  const fds_element_t* element = nullptr;
  REQUIRE(fds_structure_find_path(structure, path.data(), path.size(), &element) == FDS_STATUS_OK);
  REQUIRE(fds_element_vr(element) == FDS_VR_LO);

  fds_structure_free(structure);
}

TEST_CASE("ABI insert_path rejects a duplicate tag as FDS_STATUS_ALREADY_EXISTS", "[abi][path]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto* structure = parse(b);

  const std::uint8_t value[2] = {'X', 'X'};
  REQUIRE(fds_structure_insert_path(structure, nullptr, 0, fds_tag_t{0x0010, 0x0020}, FDS_VR_LO,
                                     value, 2) == FDS_STATUS_ALREADY_EXISTS);
  REQUIRE(fds_structure_is_modified(structure) == 0);
  fds_structure_free(structure);
}

TEST_CASE("ABI insert_path with FDS_VR_UNKNOWN infers the VR from the dictionary", "[abi][path]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto* structure = parse(b);

  const std::uint8_t value[4] = {'I', 'D', '1', ' '};
  // (0010,0020) PatientID -- unambiguous LO.
  REQUIRE(fds_structure_insert_path(structure, nullptr, 0, fds_tag_t{0x0010, 0x0020},
                                     FDS_VR_UNKNOWN, value, 4) == FDS_STATUS_OK);
  const fds_element_t* element = nullptr;
  REQUIRE(fds_structure_find(structure, fds_tag_t{0x0010, 0x0020}, &element) == FDS_STATUS_OK);
  REQUIRE(fds_element_vr(element) == FDS_VR_LO);

  fds_structure_free(structure);
}

TEST_CASE("ABI insert_path with FDS_VR_UNKNOWN returns FDS_STATUS_VR_REQUIRED for ambiguous VR",
          "[abi][path]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto* structure = parse(b);

  const std::uint8_t value[2] = {0, 0};
  // (0028,0106) SmallestImagePixelValue -- "US or SS", ambiguous.
  REQUIRE(fds_structure_insert_path(structure, nullptr, 0, fds_tag_t{0x0028, 0x0106},
                                     FDS_VR_UNKNOWN, value, 2) == FDS_STATUS_VR_REQUIRED);
  REQUIRE(fds_structure_contains(structure, fds_tag_t{0x0028, 0x0106}) == 0);
  fds_structure_free(structure);
}

TEST_CASE("ABI insert_path rejects SQ and Unknown as an explicit VR", "[abi][path]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto* structure = parse(b);
  const std::uint8_t value[2] = {'A', 'B'};

  REQUIRE(fds_structure_insert_path(structure, nullptr, 0, fds_tag_t{0x0008, 0x1111}, FDS_VR_SQ,
                                     value, 2) == FDS_STATUS_INVALID_ARGUMENT);
  fds_structure_free(structure);
}

TEST_CASE("ABI decode_text_path / set_text_path round-trip Unicode text", "[abi][path]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0005, "CS", "ISO_IR 100");  // Latin1
  b.element_short(0x0010, 0x0010, "PN", "X");
  auto* structure = parse(b);

  std::vector<fds_path_step_t> path = {leaf(fds_tag_t{0x0010, 0x0010})};
  // 'é' (Latin1), UTF-8: C3 A9.
  const char* new_text = "A\xC3\xA9" "B";
  REQUIRE(fds_structure_set_text_path(structure, path.data(), path.size(), new_text) ==
          FDS_STATUS_OK);

  const char* decoded = nullptr;
  REQUIRE(fds_structure_decode_text_path(structure, path.data(), path.size(), &decoded) ==
          FDS_STATUS_OK);
  REQUIRE(decoded != nullptr);
  REQUIRE(std::string(decoded) == new_text);

  fds_structure_free(structure);
}

TEST_CASE("ABI insert_text_path inserts new Unicode text, root and nested, explicit and inferred",
          "[abi][path]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 100");  // local Latin1
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0005, "CS", "ISO_IR 100");  // root Latin1
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto* structure = parse(b);

  const char* text = "A\xC3\xA9" "B";  // needs Latin1

  // Explicit VR, root.
  REQUIRE(fds_structure_insert_text_path(structure, nullptr, 0, fds_tag_t{0x0010, 0x0010},
                                          FDS_VR_PN, text) == FDS_STATUS_OK);

  // Inferred VR, nested.
  std::vector<fds_path_step_t> parent = {descend(fds_tag_t{0x0008, 0x1140}, 0)};
  REQUIRE(fds_structure_insert_text_path(structure, parent.data(), parent.size(),
                                          fds_tag_t{0x0010, 0x0010}, FDS_VR_UNKNOWN,
                                          text) == FDS_STATUS_OK);

  std::vector<fds_path_step_t> nested_path = {descend(fds_tag_t{0x0008, 0x1140}, 0),
                                               leaf(fds_tag_t{0x0010, 0x0010})};
  const char* decoded = nullptr;
  REQUIRE(fds_structure_decode_text_path(structure, nested_path.data(), nested_path.size(),
                                          &decoded) == FDS_STATUS_OK);
  REQUIRE(std::string(decoded) == text);

  fds_structure_free(structure);
}

TEST_CASE("ABI insert_text_path reports unrepresentable/invalid Unicode input distinctly",
          "[abi][path]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0005, "CS", "ISO_IR 100");  // Latin1 only
  auto* structure = parse(b);

  // Cyrillic -- not representable under Latin1.
  REQUIRE(fds_structure_insert_text_path(structure, nullptr, 0, fds_tag_t{0x0010, 0x0010},
                                          FDS_VR_PN, "A\xD1\x80" "B") ==
          FDS_STATUS_UNREPRESENTABLE_CHARACTER);

  // Malformed UTF-8.
  REQUIRE(fds_structure_insert_text_path(structure, nullptr, 0, fds_tag_t{0x0010, 0x0011},
                                          FDS_VR_PN, "A\xFF" "B") ==
          FDS_STATUS_INVALID_UNICODE_INPUT);

  REQUIRE(fds_structure_is_modified(structure) == 0);
  fds_structure_free(structure);
}
