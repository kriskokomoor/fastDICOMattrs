#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fastdicomattrs/pixel_data_reference.hpp"
#include "fastdicomattrs/source.hpp"
#include "fastdicomattrs/transfer_syntax.hpp"
#include "fastdicomattrs/value.hpp"
#include "fastdicomattrs/value_length.hpp"
#include "fixture_builder.hpp"

// A1.3 -- Pixel-Data-relative ordering. See
// docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md for the full design.
// tests/integration/test_roundtrip_lossless.cpp's "An element after Pixel
// Data round-trips byte-identically (A1.3)" test covers the original
// regression fixture (Data Set Trailing Padding); this file covers the
// rest of the matrix: middle position, encapsulated Pixel Data, a
// non-special-cased trailing tag, mutation/removal interaction with the
// positional invariant, and programmatic (non-parsed) construction.

using namespace fds_test;
using fds::DICOMStructure;
using fds::Element;
using fds::Fidelity;
using fds::LengthForm;
using fds::MemorySource;
using fds::ParseOptions;
using fds::ParseStatus;
using fds::PixelDataReference;
using fds::SourceSpan;
using fds::Tag;
using fds::TransferSyntax;
using fds::Value;
using fds::VR;
using fds::WriteStatus;

namespace {
std::vector<std::byte> as_bytes_vec(const std::string& s) {
  std::vector<std::byte> out(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
  return out;
}
}  // namespace

TEST_CASE("native Pixel Data in the middle of the top-level list round-trips byte-identically",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixels(8, std::byte{0x11});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0060, "CS", "CT");  // precedes Pixel Data
  b.pixel_data_native("OW", pixels);
  std::vector<std::byte> padding(4, std::byte{0});
  b.element_long(0xFFFC, 0xFFFC, "OB", padding);  // follows Pixel Data

  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  auto result = fds::parse_buffer(b.bytes(), options);
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;
  REQUIRE_FALSE(structure.is_modified());
  REQUIRE(structure.pixel_data_position().has_value());
  REQUIRE(*structure.pixel_data_position() == 7);  // 6 File Meta elements + (0008,0060)

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto written = as_bytes_vec(out.str());
  REQUIRE(written == b.bytes());
}

TEST_CASE("encapsulated Pixel Data followed by a trailing element remains trailing",
          "[pixel_data_ordering]") {
  auto b = make_file_meta("1.2.840.10008.1.2.5");  // JPEG Lossless (encapsulated)
  b.pixel_data_encapsulated_header("OB");
  b.item_defined_header(0);  // empty Basic Offset Table
  std::vector<std::byte> fragment(6, std::byte{0x22});
  b.item_defined_header(static_cast<std::uint32_t>(fragment.size()));
  b.raw_bytes(fragment);
  b.sequence_delimiter();
  std::vector<std::byte> padding(4, std::byte{0});
  b.element_long(0xFFFC, 0xFFFC, "OB", padding);

  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  auto result = fds::parse_buffer(b.bytes(), options);
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;
  REQUIRE(structure.pixel_data() != nullptr);
  REQUIRE(structure.pixel_data()->is_encapsulated());
  REQUIRE(structure.pixel_data_position().has_value());
  REQUIRE(*structure.pixel_data_position() == 6);  // exactly the 6 File Meta elements

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto written = as_bytes_vec(out.str());
  REQUIRE(written == b.bytes());
}

TEST_CASE("a generic tag after Pixel Data is preserved too, not special-cased to "
          "Data Set Trailing Padding",
          "[pixel_data_ordering]") {
  // PS3.5 7.5's own note names (FFFC,FFFC) as the standard-sanctioned
  // trailing construct; this fixture uses an different, non-special tag
  // to prove the fix is a general positional mechanism, not a check for
  // that one tag specifically. This is a parser-permissiveness case, not a
  // claim that arbitrary post-Pixel-Data content is standard-conformant.
  std::vector<std::byte> pixels(4, std::byte{0x33});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_native("OW", pixels);
  b.element_short(0x0009, 0x0010, "LO", "TRAILER");  // arbitrary private tag, not FFFC,FFFC

  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  auto result = fds::parse_buffer(b.bytes(), options);
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;
  REQUIRE(*structure.pixel_data_position() == 6);  // exactly the 6 File Meta elements

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  REQUIRE(as_bytes_vec(out.str()) == b.bytes());
}

TEST_CASE("mutating an element before Pixel Data preserves its position via tag order, "
          "not a stale recorded index",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixels(4, std::byte{0x44});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");  // low tag, precedes Pixel Data
  b.pixel_data_native("OW", pixels);
  std::vector<std::byte> padding(4, std::byte{0});
  b.element_long(0xFFFC, 0xFFFC, "OB", padding);  // high tag, follows Pixel Data

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;
  REQUIRE(structure.set_value(fds::ElementPath(Tag(0x0010, 0x0020)), Value::from_string("ID2 ")));
  REQUIRE(structure.is_modified());

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  std::string written = out.str();
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
  REQUIRE(reparsed.status == ParseStatus::Success);
  REQUIRE(reparsed.structure->pixel_data() != nullptr);
  // The mutated element still precedes Pixel Data (6 File Meta + the one
  // dataset element); the trailing tag still follows it.
  REQUIRE(*reparsed.structure->pixel_data_position() == 7);
  REQUIRE(reparsed.structure->contains(Tag(0xFFFC, 0xFFFC)));
  REQUIRE(reparsed.structure->find(Tag(0x0010, 0x0020))->value().as_string() == "ID2");
}

TEST_CASE("mutating an element after Pixel Data preserves its trailing position",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixels(4, std::byte{0x55});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_native("OW", pixels);
  // Tag group 0xFFFB > Pixel Data's 0x7FE0, so this element genuinely
  // belongs after Pixel Data under *both* source order and ascending-tag
  // order -- unlike a lower-tag'd trailing element, which the modified
  // (semantic-reconstruction) path would legitimately re-sort before
  // Pixel Data, per DICOMStructure::set()'s existing ascending-tag-order
  // contract for top-level elements.
  b.element_short(0xFFFB, 0x0010, "LO", "TAG1");  // follows Pixel Data

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;
  REQUIRE(structure.set_value(fds::ElementPath(Tag(0xFFFB, 0x0010)), Value::from_string("TAG2")));

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status == ParseStatus::Success);
  REQUIRE(*reparsed.structure->pixel_data_position() == 6);  // just the 6 File Meta elements
  REQUIRE(reparsed.structure->find(Tag(0xFFFB, 0x0010))->value().as_string() == "TAG2");
}

TEST_CASE("inserting a new element with a tag before Pixel Data's tag places it before "
          "Pixel Data",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixels(4, std::byte{0x66});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_native("OW", pixels);
  std::vector<std::byte> padding(4, std::byte{0});
  b.element_long(0xFFFC, 0xFFFC, "OB", padding);

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;
  REQUIRE(structure.set(Tag(0x0010, 0x0010), VR::PN, Value::from_string("NEW ")));

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status == ParseStatus::Success);
  REQUIRE(*reparsed.structure->pixel_data_position() == 7);  // 6 File Meta + the new element
  REQUIRE(reparsed.structure->contains(Tag(0xFFFC, 0xFFFC)));
}

TEST_CASE("inserting a new element with a tag after Pixel Data's tag places it after "
          "Pixel Data",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixels(4, std::byte{0x77});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0010, "PN", "A ");
  b.pixel_data_native("OW", pixels);

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;
  std::vector<std::byte> padding(4, std::byte{0});
  REQUIRE(structure.set(Tag(0xFFFC, 0xFFFC), VR::OB, Value::from_owned(padding)));

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status == ParseStatus::Success);
  REQUIRE(*reparsed.structure->pixel_data_position() == 7);  // 6 File Meta + (0010,0010)
  REQUIRE(reparsed.structure->contains(Tag(0xFFFC, 0xFFFC)));
}

TEST_CASE("removing the element before Pixel Data leaves it correctly positioned relative "
          "to the remaining trailing element",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixels(4, std::byte{0x88});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  b.pixel_data_native("OW", pixels);
  std::vector<std::byte> padding(4, std::byte{0});
  b.element_long(0xFFFC, 0xFFFC, "OB", padding);

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;
  REQUIRE(structure.erase(Tag(0x0010, 0x0020)));

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status == ParseStatus::Success);
  REQUIRE(*reparsed.structure->pixel_data_position() == 6);  // just the 6 File Meta elements
  REQUIRE(reparsed.structure->contains(Tag(0xFFFC, 0xFFFC)));
  REQUIRE_FALSE(reparsed.structure->contains(Tag(0x0010, 0x0020)));
}

TEST_CASE("removing the element after Pixel Data leaves it correctly positioned as last",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixels(4, std::byte{0x99});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  b.pixel_data_native("OW", pixels);
  std::vector<std::byte> padding(4, std::byte{0});
  b.element_long(0xFFFC, 0xFFFC, "OB", padding);

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;
  REQUIRE(structure.erase(Tag(0xFFFC, 0xFFFC)));

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status == ParseStatus::Success);
  REQUIRE(*reparsed.structure->pixel_data_position() == reparsed.structure->element_count());
  REQUIRE_FALSE(reparsed.structure->contains(Tag(0xFFFC, 0xFFFC)));
}

TEST_CASE("a structure with no Pixel Data is unaffected by the ordering mechanism",
          "[pixel_data_ordering]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  auto result = fds::parse_buffer(b.bytes(), options);
  auto& structure = *result.structure;
  REQUIRE(structure.pixel_data() == nullptr);
  REQUIRE_FALSE(structure.pixel_data_position().has_value());

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  REQUIRE(as_bytes_vec(out.str()) == b.bytes());
}

TEST_CASE("a programmatically-constructed structure with no recorded position writes "
          "Pixel Data last by default",
          "[pixel_data_ordering]") {
  std::vector<std::byte> pixel_bytes(8, std::byte{0xAA});
  auto source = MemorySource::own(pixel_bytes, "synthetic");
  SourceSpan span(0, pixel_bytes.size());
  auto pixel_ref = PixelDataReference::native(VR::OW, std::array<std::byte, 2>{},
                                               TransferSyntax::from_uid("1.2.840.10008.1.2.1"),
                                               span);

  std::vector<Element> elements;
  elements.emplace_back(Tag(0x0008, 0x0060), VR::CS, fds::VRProvenance::Explicit,
                         LengthForm::Short16, /*undefined_length=*/false,
                         std::array<std::byte, 2>{}, Value::from_string("CT"));

  // Constructed without the optional pixel_data_position argument -- it
  // defaults to nullopt.
  DICOMStructure structure(source, TransferSyntax::from_uid("1.2.840.10008.1.2.1"),
                            Fidelity::Lossless, std::move(elements), std::nullopt, pixel_ref);
  REQUIRE_FALSE(structure.pixel_data_position().has_value());
  REQUIRE_FALSE(structure.is_modified());

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  // No File Meta was constructed for this bare, programmatic dataset, so
  // the parser falls back to its documented "assume Explicit VR LE" default
  // with a Warning diagnostic -- SuccessWithWarnings, not Success. That
  // fallback is unrelated to what this test checks (Pixel Data placement),
  // so only failure is ruled out here, matching the same pattern other
  // bare-dataset tests in this suite use.
  REQUIRE(reparsed.status != ParseStatus::Failed);
  REQUIRE(reparsed.structure->pixel_data() != nullptr);
  // "Last" -- nothing follows it -- confirming the documented default.
  REQUIRE(*reparsed.structure->pixel_data_position() == reparsed.structure->element_count());
  REQUIRE(reparsed.structure->contains(Tag(0x0008, 0x0060)));
}
