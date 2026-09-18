#include <catch2/catch_test_macros.hpp>

#include <span>
#include <sstream>

#include "fastdicomattrs/charset.hpp"
#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/mutation.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// A1.7 section 18: Implicit-VR-origin qualification. Proves the full A1.7
// mutation surface (nested lookup, nested set_value, inferred- and
// explicit-VR nested insertion, nested text insertion, nested erase,
// write/reparse) behaves identically for a structure whose elements
// originated from Implicit VR Little Endian input (VRProvenance::
// Dictionary/ContextResolved, per A1.4) as it does for Explicit VR input
// (VRProvenance::Explicit) -- exercised elsewhere throughout this test
// suite. No Implicit VR *writer* is added or implied: the writer re-emits
// whatever transfer syntax the structure already carries, exactly as
// A1.4's own round-trip tests already established.

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
using fds::VRProvenance;
using fds::WriteStatus;
namespace mut = fds::mutation;
namespace cs = fds::charset;

namespace {

// One Item containing (0008,0100) CodeValue, SH, implicit-encoded.
FixtureBuilder build_item(const std::string& value) {
  FixtureBuilder content;
  content.element_implicit_ascii(0x0008, 0x0100, value);
  return wrap_in_item(content);
}

FixtureBuilder build_dataset() {
  auto b = make_file_meta("1.2.840.10008.1.2");  // Implicit VR Little Endian
  b.element_implicit_ascii(0x0008, 0x0005, "ISO_IR 100");  // SpecificCharacterSet, CS
  b.element_implicit_ascii(0x0008, 0x0060, "CT");          // Modality, CS

  auto item = build_item("12345");
  // (0008,1140) Referenced Image Sequence -- genuinely SQ per the standard
  // dictionary, resolved via A1.4's Implicit-VR dictionary-backed rule.
  b.element_implicit_header(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  return b;
}

ParseResult parse_implicit(const FixtureBuilder& b) {
  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  return fds::parse_buffer(b.bytes(), options);
}

}  // namespace

TEST_CASE("Implicit-VR origin: nested lookup and dictionary-resolved provenance",
          "[implicit_vr][mutation]") {
  auto result = parse_implicit(build_dataset());
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  const auto* e = structure.find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::SH);
  REQUIRE(e->vr_provenance() == VRProvenance::Dictionary);
  REQUIRE(e->value().as_string() == "12345");

  const auto* seq = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq->vr() == VR::SQ);
  REQUIRE(seq->vr_provenance() == VRProvenance::Dictionary);
}

TEST_CASE("Implicit-VR origin: nested set_value", "[implicit_vr][mutation]") {
  auto result = parse_implicit(build_dataset());
  auto& structure = *result.structure;

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.set_value(path, Value::from_string("99999 ")));
  REQUIRE(structure.find(path)->value().as_string() == "99999");
}

TEST_CASE("Implicit-VR origin: nested insertion with an inferred unambiguous standard VR",
          "[implicit_vr][mutation]") {
  auto result = parse_implicit(build_dataset());
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  // (0008,0104) CodeMeaning -- unambiguous LO.
  REQUIRE(mut::insert_inferred(structure, parent, Tag(0x0008, 0x0104),
                                Value::from_string("Finding ")) == mut::InsertStatus::Success);
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0104));
  const auto* e = structure.find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
}

TEST_CASE("Implicit-VR origin: nested insertion with an explicit (private) VR",
          "[implicit_vr][mutation]") {
  auto result = parse_implicit(build_dataset());
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(structure.insert(parent, Tag(0x0009, 0x0010), VR::LO, Value::from_string("ACME CORP ")));
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0009, 0x0010));
  REQUIRE(structure.find(path)->vr() == VR::LO);
}

TEST_CASE("Implicit-VR origin: nested Unicode text insertion", "[implicit_vr][mutation]") {
  auto result = parse_implicit(build_dataset());
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  // (0008,0102) MappingResource -- SH, needs Latin1 (inherited from root's
  // ISO_IR 100 declaration -- proves charset inheritance works correctly
  // for an Implicit-VR-origin structure, not just an Explicit-VR one).
  auto status = cs::insert_text(structure, parent, Tag(0x0008, 0x0102), VR::SH, {"A\xC3\xA9" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0102));
  auto ctx = cs::resolve_character_set_context(structure, path);
  auto decoded = cs::decode_text(*structure.find(path), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "A\xC3\xA9" "B");
}

TEST_CASE("Implicit-VR origin: nested erase", "[implicit_vr][mutation]") {
  auto result = parse_implicit(build_dataset());
  auto& structure = *result.structure;

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.erase(path));
  REQUIRE(structure.find(path) == nullptr);
  const auto* seq = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq->sequence().items()[0].elements().empty());
}

TEST_CASE("Implicit-VR origin: full mutation sequence round-trips through write/reparse",
          "[implicit_vr][mutation][roundtrip]") {
  auto result = parse_implicit(build_dataset());
  auto& structure = *result.structure;

  ElementPath nested_scalar;
  nested_scalar.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.set_value(nested_scalar, Value::from_string("99999 ")));

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(mut::insert_inferred(structure, parent, Tag(0x0008, 0x0104),
                                Value::from_string("Finding ")) == mut::InsertStatus::Success);
  REQUIRE(structure.insert(parent, Tag(0x0009, 0x0010), VR::LO, Value::from_string("ACME CORP ")));
  REQUIRE(cs::insert_text(structure, parent, Tag(0x0008, 0x0102), VR::SH, {"A\xC3\xA9" "B"}) ==
          cs::SetTextStatus::Success);
  REQUIRE(structure.erase(Tag(0x0008, 0x0060)));

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  std::string written = out.str();
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
  REQUIRE(reparsed.status != ParseStatus::Failed);
  auto& rs = *reparsed.structure;

  REQUIRE_FALSE(rs.contains(Tag(0x0008, 0x0060)));
  REQUIRE(rs.find(nested_scalar)->value().as_string() == "99999");

  ElementPath code_meaning, creator, text_path;
  code_meaning.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0104));
  creator.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0009, 0x0010));
  text_path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0102));

  REQUIRE(rs.find(code_meaning) != nullptr);
  REQUIRE(rs.find(code_meaning)->vr() == VR::LO);
  REQUIRE(rs.find(creator) != nullptr);
  auto ctx = cs::resolve_character_set_context(rs, text_path);
  auto decoded = cs::decode_text(*rs.find(text_path), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "A\xC3\xA9" "B");
}
