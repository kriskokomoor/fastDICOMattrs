#include <catch2/catch_test_macros.hpp>

#include <span>
#include <sstream>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

using namespace fds_test;
using fds::Fidelity;
using fds::ParseOptions;
using fds::ParseStatus;
using fds::Tag;
using fds::Value;
using fds::VR;
using fds::WriteStatus;

namespace {

// The core promise of docs/roundtrip-contract.md: parse at LOSSLESS, write
// back, and require the output bytes are IDENTICAL to the input -- never
// weakened to a semantic comparison.
void assert_byte_identical_roundtrip(const FixtureBuilder& b) {
  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  auto result = fds::parse_buffer(b.bytes(), options);
  INFO("parse status: " << static_cast<int>(result.status));
  REQUIRE(result.status != ParseStatus::Failed);
  REQUIRE(result.structure != nullptr);

  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Success);

  std::string written = out.str();
  REQUIRE(written.size() == b.size());

  std::vector<std::byte> written_bytes(written.size());
  for (std::size_t i = 0; i < written.size(); ++i) {
    written_bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(written[i]));
  }
  REQUIRE(written_bytes == b.bytes());
  REQUIRE(write_result.bytes_written == b.size());
}

}  // namespace

TEST_CASE("Lossless round-trip: File Meta + simple short-form elements", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0010, "PN", "Doe^Jane");  // already even length (8)
  b.element_short(0x0010, 0x0020, "LO", "ID1");       // odd-length text, auto NUL-padded
  b.element_short(0x0008, 0x0060, "CS", "CT");
  assert_byte_identical_roundtrip(b);
}

TEST_CASE("Lossless round-trip: long-form (OB) private element", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  std::vector<std::byte> value = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  b.element_long(0x0009, 0x0011, "OB", value);
  assert_byte_identical_roundtrip(b);
}

TEST_CASE("Lossless round-trip: defined-length nested sequence", "[roundtrip]") {
  FixtureBuilder inner_content;
  inner_content.element_short(0x300A, 0x0072, "DS", "6.0");
  auto inner_item = wrap_in_item(inner_content);

  FixtureBuilder outer_content;
  outer_content.sequence_defined(0x300A, 0x00B6, static_cast<std::uint32_t>(inner_item.size()));
  outer_content.append(inner_item);
  auto outer_item = wrap_in_item(outer_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(outer_item.size()));
  b.append(outer_item);
  assert_byte_identical_roundtrip(b);
}

TEST_CASE("Lossless round-trip: undefined-length sequence and item", "[roundtrip]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "ABCDE");
  auto item = wrap_in_undefined_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_undefined(0x0008, 0x1140);
  b.append(item);
  b.sequence_delimiter();
  assert_byte_identical_roundtrip(b);
}

TEST_CASE("Lossless round-trip: native Pixel Data", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  std::vector<std::byte> pixels(32, std::byte{0x7A});
  b.pixel_data_native("OW", pixels);
  assert_byte_identical_roundtrip(b);
}

TEST_CASE("Lossless round-trip: encapsulated Pixel Data", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.4.50");
  b.pixel_data_encapsulated_header("OB");
  b.item_defined_header(0);
  std::vector<std::byte> fragment(10, std::byte{0x11});
  b.item_defined_header(static_cast<std::uint32_t>(fragment.size()));
  b.raw_bytes(fragment);
  b.sequence_delimiter();
  assert_byte_identical_roundtrip(b);
}

TEST_CASE("Lossless round-trip: bare dataset with no preamble", "[roundtrip]") {
  FixtureBuilder b;
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  b.element_short(0x0008, 0x0060, "CS", "MR");
  assert_byte_identical_roundtrip(b);
}

TEST_CASE("write() is Unsupported for a STANDARD-fidelity structure", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  ParseOptions options;
  options.fidelity = Fidelity::Standard;
  auto result = fds::parse_buffer(b.bytes(), options);
  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Unsupported);
}

TEST_CASE("write() succeeds for a modified LOSSLESS structure", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  auto result = fds::parse_buffer(b.bytes(), options);
  // Value::from_string() does not auto-pad to DICOM's required even length
  // (see docs/architecture.md section 9) -- "CHANGED " is deliberately
  // space-padded to 8 bytes; as_string() below trims exactly one trailing
  // pad byte back off.
  REQUIRE(result.structure->set_value(fds::ElementPath(Tag(0x0010, 0x0020)),
                                       Value::from_string("CHANGED ")));
  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Success);

  std::string written = out.str();
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
  REQUIRE(reparsed.status != ParseStatus::Failed);
  REQUIRE(reparsed.structure->find(Tag(0x0010, 0x0020))->value().as_string() == "CHANGED");
}

TEST_CASE("write() succeeds for a modified STANDARD structure", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  ParseOptions options;
  options.fidelity = Fidelity::Standard;
  auto result = fds::parse_buffer(b.bytes(), options);
  REQUIRE(result.structure->erase(Tag(0x0010, 0x0020)));
  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Success);

  std::string written = out.str();
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
  REQUIRE(reparsed.status != ParseStatus::Failed);
  REQUIRE_FALSE(reparsed.structure->contains(Tag(0x0010, 0x0020)));
}

TEST_CASE("write() is Unsupported for a modified FAST structure", "[roundtrip]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  ParseOptions options;
  options.fidelity = Fidelity::Fast;
  auto result = fds::parse_buffer(b.bytes(), options);
  REQUIRE(result.structure->erase(Tag(0x0010, 0x0020)));
  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Unsupported);
}

// Pins the documented gap in docs/roundtrip-contract.md "Known gaps": the
// writer always emits Pixel Data last (see write_lossless's main loop in
// lossless_writer.cpp), so a source file with an element positioned *after*
// Pixel Data on the wire -- e.g. a Data Set Trailing Padding element
// (FFFC,FFFC), which PS3.10 permits -- does not round-trip byte-identically
// even when nothing was modified, because that trailing element comes out
// ahead of Pixel Data in the output instead of after it.
// A1.3 regression: this test used to pin a documented, un-fixed limitation
// (the trailing element and Pixel Data swapped relative order on write,
// because the writer always emitted Pixel Data last regardless of where it
// actually sat in the source). DICOMStructure now records Pixel Data's
// original position (pixel_data_position()) and the writer honors it on
// the unmodified path -- see
// docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md. The fixture is
// unchanged; only the assertion is corrected to what byte-identical
// round-tripping actually requires.
TEST_CASE("An element after Pixel Data round-trips byte-identically (A1.3)", "[roundtrip]") {
  std::vector<std::byte> pixels(16, std::byte{0x42});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_native("OW", pixels);
  std::vector<std::byte> padding = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                     std::byte{0x00}};
  b.element_long(0xFFFC, 0xFFFC, "OB", padding);  // Data Set Trailing Padding

  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  auto result = fds::parse_buffer(b.bytes(), options);
  INFO("parse status: " << static_cast<int>(result.status));
  REQUIRE(result.status == ParseStatus::Success);  // clean parse, not modified
  REQUIRE_FALSE(result.structure->is_modified());
  REQUIRE(result.structure->pixel_data() != nullptr);
  REQUIRE(result.structure->contains(Tag(0xFFFC, 0xFFFC)));
  // Pixel Data preceded exactly the File Meta + ordinary dataset elements
  // that came before it on the wire; the trailing padding element is not
  // among them.
  REQUIRE(result.structure->pixel_data_position().has_value());
  REQUIRE(*result.structure->pixel_data_position() == result.structure->element_count() - 1);

  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Success);

  std::string written = out.str();
  std::vector<std::byte> written_bytes(written.size());
  for (std::size_t i = 0; i < written.size(); ++i) {
    written_bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(written[i]));
  }
  REQUIRE(written_bytes == b.bytes());

  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
  REQUIRE(reparsed.status == ParseStatus::Success);
  REQUIRE(reparsed.structure->pixel_data() != nullptr);
  REQUIRE(reparsed.structure->contains(Tag(0xFFFC, 0xFFFC)));
}
