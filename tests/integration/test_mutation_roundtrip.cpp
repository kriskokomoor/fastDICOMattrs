#include <catch2/catch_test_macros.hpp>

#include <span>
#include <sstream>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// End-to-end proof that a *modified* structure writes valid DICOM: parse ->
// mutate -> write -> re-parse -> assert on the re-parsed structure. This is
// a distinct contract from tests/integration/test_roundtrip_lossless.cpp's
// byte-identical guarantee (unmodified LOSSLESS only) -- see
// docs/roundtrip-contract.md "Two write contracts".

using namespace fds_test;
using fds::DICOMStructure;
using fds::ElementPath;
using fds::Fidelity;
using fds::ParseOptions;
using fds::ParseResult;
using fds::ParseStatus;
using fds::Tag;
using fds::Value;
using fds::VR;
using fds::WriteStatus;

namespace {

ParseResult parse_lossless(const FixtureBuilder& b) {
  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  return fds::parse_buffer(b.bytes(), options);
}

// Writes `structure`, returning the written bytes re-parsed as a brand-new
// structure. `written` receives the underlying bytes so the caller can keep
// them alive for as long as the returned structure is used (parse_buffer
// never takes ownership of its input).
ParseResult write_then_reparse(const DICOMStructure& structure, std::string& written) {
  std::ostringstream out;
  auto write_result = structure.write(out);
  REQUIRE(write_result.status == WriteStatus::Success);
  written = out.str();
  REQUIRE(write_result.bytes_written == written.size());
  return fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
}

}  // namespace

TEST_CASE("modified short-form value round-trips through write", "[mutation][roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = parse_lossless(b);
  REQUIRE(result.structure->set_value(ElementPath(Tag(0x0010, 0x0020)), Value::from_string("ANON")));

  std::string written;
  auto reparsed = write_then_reparse(*result.structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);
  REQUIRE(reparsed.structure->find(Tag(0x0010, 0x0020))->value().as_string() == "ANON");
}

TEST_CASE("modified long-form value round-trips through write", "[mutation][roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  std::vector<std::byte> original = {std::byte{0x01}, std::byte{0x02}};
  b.element_long(0x0009, 0x0011, "OB", original);
  auto result = parse_lossless(b);

  std::vector<std::byte> replacement(64, std::byte{0xAB});
  REQUIRE(result.structure->set_value(ElementPath(Tag(0x0009, 0x0011)),
                                       Value::from_owned(replacement)));

  std::string written;
  auto reparsed = write_then_reparse(*result.structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);
  auto* e = reparsed.structure->find(Tag(0x0009, 0x0011));
  REQUIRE(e != nullptr);
  auto bytes = e->value().bytes();
  REQUIRE(std::vector<std::byte>(bytes.begin(), bytes.end()) == replacement);
}

TEST_CASE("erasing a nested sequence element round-trips through write", "[mutation][roundtrip]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  item_content.element_short(0x0008, 0x0102, "SH", "99");
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(result.structure->erase(path));

  std::string written;
  auto reparsed = write_then_reparse(*result.structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);
  const auto* seq = reparsed.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(seq != nullptr);
  const auto& elements = seq->sequence().items()[0].elements();
  REQUIRE(elements.size() == 1);
  REQUIRE(elements[0].tag() == Tag(0x0008, 0x0102));
}

TEST_CASE("erasing an element inside an undefined-length item round-trips through write",
          "[mutation][roundtrip]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "ABCDE");
  auto item = wrap_in_undefined_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_undefined(0x0008, 0x1140);
  b.append(item);
  b.sequence_delimiter();
  auto result = parse_lossless(b);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(result.structure->erase(path));

  std::string written;
  auto reparsed = write_then_reparse(*result.structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);
  const auto* seq = reparsed.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(seq != nullptr);
  REQUIRE(seq->sequence().items()[0].elements().empty());
}

TEST_CASE("a newly-inserted top-level element lands in ascending tag order", "[mutation][roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0060, "CS", "CT");   // (0008,0060)
  b.element_short(0x0020, 0x000D, "UI", "1.2");  // (0020,000D)
  auto result = parse_lossless(b);

  // Values are space-padded to even length by hand -- set()/set_value()
  // don't auto-pad (see docs/architecture.md section 9).
  // Insert between the two existing top-level elements.
  REQUIRE(result.structure->set(Tag(0x0010, 0x0020), VR::LO, Value::from_string("NEWID ")));
  // Insert before every existing top-level element.
  REQUIRE(result.structure->set(Tag(0x0002, 0x0013), VR::SH, Value::from_string("X ")));
  // Insert after every existing top-level element.
  REQUIRE(result.structure->set(Tag(0x7FE1, 0x0001), VR::LO, Value::from_string("Y ")));

  std::string written;
  auto reparsed = write_then_reparse(*result.structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);

  const auto& elements = reparsed.structure->elements();
  for (std::size_t i = 1; i < elements.size(); ++i) {
    INFO("index " << i);
    REQUIRE(elements[i - 1].tag() < elements[i].tag());
  }
  REQUIRE(reparsed.structure->contains(Tag(0x0010, 0x0020)));
  REQUIRE(reparsed.structure->contains(Tag(0x0002, 0x0013)));
  REQUIRE(reparsed.structure->contains(Tag(0x7FE1, 0x0001)));
}

TEST_CASE("set_value rejects growing a short-form element past the encodable length",
          "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");  // LO is short-form
  auto result = parse_lossless(b);

  std::vector<std::byte> oversized(70000, std::byte{0x41});
  REQUIRE_FALSE(result.structure->set_value(ElementPath(Tag(0x0010, 0x0020)),
                                             Value::from_owned(oversized)));
  // Rejected mutation must leave the element and structure untouched.
  REQUIRE(result.structure->find(Tag(0x0010, 0x0020))->value().as_string() == "ID1");
  REQUIRE_FALSE(result.structure->is_modified());
}

TEST_CASE("set_value rejects an odd-length replacement value", "[mutation]") {
  // DICOM values must have even length (PS3.5 6.4) regardless of VR --
  // verified against the external release review that found this
  // previously unenforced: set_value(tag, b"ODD") (3 bytes) used to
  // succeed and the writer emitted a genuinely non-conformant odd-VL
  // element while reporting WriteStatus::Success.
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = parse_lossless(b);

  REQUIRE_FALSE(result.structure->set_value(ElementPath(Tag(0x0010, 0x0020)),
                                             Value::from_string("ODD")));  // 3 bytes, odd
  REQUIRE(result.structure->find(Tag(0x0010, 0x0020))->value().as_string() == "ID1");
  REQUIRE_FALSE(result.structure->is_modified());
}

TEST_CASE("set rejects inserting a new element with an odd-length value", "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);

  REQUIRE_FALSE(result.structure->set(Tag(0x0018, 0x0050), VR::DS, Value::from_string("1.0")));
  REQUIRE_FALSE(result.structure->contains(Tag(0x0018, 0x0050)));
}

namespace {
// Encoded size of `e` as this writer would emit it: tag(4) + VR(2) +
// length field (2 for Short16, 2 reserved + 4 length for Long32) + value.
std::uint32_t encoded_element_size(const fds::Element& e) {
  std::uint32_t header = 4 + 2 + (e.length_form() == fds::LengthForm::Short16 ? 2 : 6);
  return header + static_cast<std::uint32_t>(e.value().size());
}
}  // namespace

TEST_CASE("erasing a File Meta element on a modified write recomputes Group Length correctly",
          "[mutation][roundtrip]") {
  // File Meta Group Length (0002,0000) previously went stale after a File
  // Meta mutation -- verified against the external release review that
  // found this: erasing a File Meta element left the group-length value
  // byte-for-byte unchanged even though the group's actual encoded size
  // shrank.
  auto b = make_file_meta("1.2.840.10008.1.2.1");  // includes (0002,0012) ImplementationClassUID
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = parse_lossless(b);

  REQUIRE(result.structure->erase(Tag(0x0002, 0x0012)));

  std::string written;
  auto reparsed = write_then_reparse(*result.structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);
  REQUIRE_FALSE(reparsed.structure->contains(Tag(0x0002, 0x0012)));

  // Independently recompute the expected group length from the reparsed
  // structure's own surviving File Meta elements, rather than trusting the
  // same computation the writer just did.
  std::uint32_t expected_group_length = 0;
  for (const auto& element : reparsed.structure->elements()) {
    if (element.tag().group != 0x0002 || element.tag() == Tag(0x0002, 0x0000)) continue;
    expected_group_length += encoded_element_size(element);
  }
  REQUIRE(reparsed.structure->find(Tag(0x0002, 0x0000))->value().as_uint32() ==
          expected_group_length);
}

TEST_CASE("erase_private_elements round-trips through write", "[mutation][roundtrip]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0009, 0x0010, "LO", "PRIV");   // private, nested
  item_content.element_short(0x0008, 0x0100, "SH", "KEEP1");  // survives
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0011, "LO", "PRIV2");  // private, top level
  b.element_short(0x0008, 0x0060, "CS", "CT");     // survives
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);

  REQUIRE(result.structure->erase_private_elements() == 2);

  std::string written;
  auto reparsed = write_then_reparse(*result.structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);

  REQUIRE_FALSE(reparsed.structure->contains(Tag(0x0009, 0x0011)));
  REQUIRE(reparsed.structure->contains(Tag(0x0008, 0x0060)));

  const auto* seq = reparsed.structure->find(Tag(0x300A, 0x00B0));
  REQUIRE(seq != nullptr);
  const auto& elements = seq->sequence().items()[0].elements();
  REQUIRE(elements.size() == 1);
  REQUIRE(elements[0].tag() == Tag(0x0008, 0x0100));
}

TEST_CASE("set rejects inserting a new short-form element past the encodable length", "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);

  std::vector<std::byte> oversized(70000, std::byte{0x41});
  REQUIRE_FALSE(result.structure->set(Tag(0x0010, 0x0020), VR::LO, Value::from_owned(oversized)));
  REQUIRE_FALSE(result.structure->contains(Tag(0x0010, 0x0020)));
}
