#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

using namespace fds_test;
using fds::ElementPath;
using fds::ParseStatus;
using fds::Tag;
using fds::Value;
using fds::VR;

TEST_CASE("set_value replaces an existing top-level element's value", "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;

  // Value::from_string() does not auto-pad to DICOM's required even length
  // (docs/architecture.md section 9); "ANON002 " is deliberately
  // space-padded to 8 bytes, and as_string() trims exactly one trailing
  // pad byte back off below.
  REQUIRE(structure.set_value(ElementPath(Tag(0x0010, 0x0020)), Value::from_string("ANON002 ")));
  REQUIRE(structure.find(Tag(0x0010, 0x0020))->value().as_string() == "ANON002");
  REQUIRE(structure.find(Tag(0x0010, 0x0020))->is_modified());
  REQUIRE(structure.is_modified());
}

TEST_CASE("set upserts a brand-new top-level element with an explicit VR", "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;

  REQUIRE_FALSE(structure.contains(Tag(0x0010, 0x0020)));
  REQUIRE(structure.set(Tag(0x0010, 0x0020), VR::LO, Value::from_string("NEWID ")));
  const auto* e = structure.find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
  REQUIRE(e->value().as_string() == "NEWID");
}

TEST_CASE("erase removes a top-level element", "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;

  REQUIRE(structure.contains(Tag(0x0010, 0x0020)));
  REQUIRE(structure.erase(Tag(0x0010, 0x0020)));
  REQUIRE_FALSE(structure.contains(Tag(0x0010, 0x0020)));
  REQUIRE_FALSE(structure.erase(Tag(0x0010, 0x0020)));  // already gone
}

TEST_CASE("set_value and erase work through a nested ElementPath", "[mutation]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));

  REQUIRE(structure.set_value(path, Value::from_string("99999 ")));
  REQUIRE(structure.find(path)->value().as_string() == "99999");

  REQUIRE(structure.erase(path));
  REQUIRE(structure.find(path) == nullptr);
  // The Sequence and its Item survive; only the leaf element was removed.
  const auto* seq = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq != nullptr);
  REQUIRE(seq->sequence().items()[0].elements().empty());
}

TEST_CASE("set_value on a sequence element itself is rejected", "[mutation]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "AB");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;
  REQUIRE_FALSE(structure.set_value(ElementPath(Tag(0x0008, 0x1140)), Value::from_string("x")));
}

TEST_CASE("erase_if removes matching elements at every nesting depth", "[mutation]") {
  // A private element nested two levels deep, alongside a non-private
  // sibling that must survive.
  FixtureBuilder inner_content;
  inner_content.element_short(0x0009, 0x0010, "LO", "PRIV");   // private, depth 2
  inner_content.element_short(0x0008, 0x0100, "SH", "KEEP1");  // non-private, depth 2
  auto inner_item = wrap_in_item(inner_content);

  FixtureBuilder outer_content;
  outer_content.element_short(0x0009, 0x0011, "LO", "PRIV2");  // private, depth 1
  outer_content.sequence_defined(0x300A, 0x00B6, static_cast<std::uint32_t>(inner_item.size()));
  outer_content.append(inner_item);
  auto outer_item = wrap_in_item(outer_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0012, "LO", "PRIV3");  // private, top level
  b.element_short(0x0008, 0x0060, "CS", "CT");     // non-private, top level, must survive
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(outer_item.size()));
  b.append(outer_item);

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;

  std::size_t removed = structure.erase_if([](const fds::Element& e) { return e.tag().is_private(); });
  REQUIRE(removed == 3);
  REQUIRE(structure.is_modified());

  REQUIRE_FALSE(structure.contains(Tag(0x0009, 0x0012)));
  REQUIRE(structure.contains(Tag(0x0008, 0x0060)));

  const auto* outer_seq = structure.find(Tag(0x300A, 0x00B0));
  REQUIRE(outer_seq != nullptr);
  const auto& outer_elements = outer_seq->sequence().items()[0].elements();
  // Only the nested sequence (300A,00B6) should remain in the outer item;
  // the private (0009,0011) sibling was removed.
  REQUIRE(outer_elements.size() == 1);
  REQUIRE(outer_elements[0].tag() == Tag(0x300A, 0x00B6));

  const auto& inner_elements = outer_elements[0].sequence().items()[0].elements();
  REQUIRE(inner_elements.size() == 1);
  REQUIRE(inner_elements[0].tag() == Tag(0x0008, 0x0100));
}

TEST_CASE("erase_private_elements is erase_if restricted to private tags", "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0010, "LO", "PRIV");
  b.element_short(0x0008, 0x0060, "CS", "CT");

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;

  REQUIRE(structure.erase_private_elements() == 1);
  REQUIRE_FALSE(structure.contains(Tag(0x0009, 0x0010)));
  REQUIRE(structure.contains(Tag(0x0008, 0x0060)));
}

TEST_CASE("erase_if on a structure with no matches removes nothing and leaves it unmodified",
          "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0060, "CS", "CT");

  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;

  REQUIRE(structure.erase_if([](const fds::Element& e) { return e.tag().is_private(); }) == 0);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("set rejects scalar insertion with SQ or Unknown VR", "[mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = *result.structure;

  REQUIRE_FALSE(structure.set(Tag(0x0008, 0x1111), VR::SQ, Value::from_string("AB")));
  REQUIRE_FALSE(
      structure.set(Tag(0x0008, 0x1112), VR::Unknown, Value::from_string("AB")));
  REQUIRE_FALSE(structure.is_modified());
  REQUIRE_FALSE(structure.contains(Tag(0x0008, 0x1111)));
  REQUIRE_FALSE(structure.contains(Tag(0x0008, 0x1112)));
}
