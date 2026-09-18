#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// A1.2 -- private-creator/block identity, per PS3.5 section 7.8.1:
//
//   Private Creator elements sit at (gggg,0010-00FF); the low byte of that
//   element number ("cc") is the block number the creator reserves. Private
//   data elements belonging to that block sit at (gggg,ccXX) for XX in
//   00-FF, i.e. element numbers 0x1000-0xFFFF whose high byte equals cc.
//   Resolution must happen relative to the dataset (top-level, or one
//   specific sequence Item) that actually contains the element -- a
//   registration in one scope must never be visible to a different scope.
//
// All fixtures below are real, parsed Explicit VR LE bytes (via
// fds::parse_buffer), not hand-built DICOMStructure objects, so resolution
// is exercised against genuine source-backed Values exactly as production
// callers would see them.

using namespace fds_test;
using fds::DICOMStructure;
using fds::Element;
using fds::ElementPath;
using fds::ParseStatus;
using fds::PrivateCreatorStatus;
using fds::PrivateElementKind;
using fds::resolve_private_creator;
using fds::Tag;

namespace {
DICOMStructure& parse_or_fail(fds::ParseResult& result) {
  REQUIRE(result.status == ParseStatus::Success);
  return *result.structure;
}
}  // namespace

TEST_CASE("basic resolution: one creator, one block, one private element", "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0010, "LO", "ACME CORP");   // creator, block 0x10
  b.element_short(0x0009, 0x1001, "LO", "widget");      // data element in block 0x10
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x1001)));
  REQUIRE(res.kind == PrivateElementKind::Data);
  REQUIRE(res.status == PrivateCreatorStatus::Resolved);
  REQUIRE(res.block.has_value());
  REQUIRE(*res.block == 0x10);
  REQUIRE(res.creator.has_value());
  REQUIRE(*res.creator == "ACME CORP");
}

TEST_CASE("multiple creators, multiple blocks: each element resolves to its own creator",
          "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0010, "LO", "CREATOR_A");  // block 0x10
  b.element_short(0x0009, 0x0011, "LO", "CREATOR_B");  // block 0x11
  b.element_short(0x0009, 0x00FF, "LO", "CREATOR_C");  // block 0xFF
  b.element_short(0x0009, 0x1005, "SH", "a-val");      // block 0x10 data
  b.element_short(0x0009, 0x11AA, "SH", "b-val");      // block 0x11 data
  b.element_short(0x0009, 0xFF00, "SH", "c-val");      // block 0xFF data
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto a = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x1005)));
  auto bres = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x11AA)));
  auto c = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0xFF00)));

  REQUIRE(a.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*a.block == 0x10);
  REQUIRE(*a.creator == "CREATOR_A");

  REQUIRE(bres.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*bres.block == 0x11);
  REQUIRE(*bres.creator == "CREATOR_B");

  REQUIRE(c.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*c.block == 0xFF);
  REQUIRE(*c.creator == "CREATOR_C");
}

TEST_CASE("missing creator registration resolves as unresolved, never guessed", "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  // No (0009,0010) creator element at all.
  b.element_short(0x0009, 0x1001, "SH", "orphan");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x1001)));
  REQUIRE(res.kind == PrivateElementKind::Data);
  REQUIRE(res.status == PrivateCreatorStatus::NoCreator);
  REQUIRE(*res.block == 0x10);
  REQUIRE_FALSE(res.creator.has_value());
}

TEST_CASE("a legitimately blank creator value is Resolved, not confused with NoCreator",
          "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0010, "LO", "");  // reserved, but not yet named
  b.element_short(0x0009, 0x1001, "SH", "x");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x1001)));
  REQUIRE(res.status == PrivateCreatorStatus::Resolved);
  REQUIRE(res.creator.has_value());
  REQUIRE(res.creator->empty());
}

TEST_CASE("malformed creator registration (declared as a Sequence) never crashes and never "
          "fabricates a creator",
          "[private_creator]") {
  FixtureBuilder empty_item_content;
  auto item = wrap_in_item(empty_item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  // (0009,0010) is a Sequence, not a valid Private Creator LO value.
  b.sequence_defined(0x0009, 0x0010, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  b.element_short(0x0009, 0x1001, "SH", "x");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x1001)));
  REQUIRE(res.kind == PrivateElementKind::Data);
  REQUIRE(res.status == PrivateCreatorStatus::Malformed);
  REQUIRE_FALSE(res.creator.has_value());
}

TEST_CASE("creator and private data element both inside a nested Item resolve correctly",
          "[private_creator]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0009, 0x0010, "LO", "NESTED_CO");
  item_content.element_short(0x0009, 0x1002, "SH", "in-item");
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0009, 0x1002));
  auto res = resolve_private_creator(structure, path);
  REQUIRE(res.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*res.creator == "NESTED_CO");
}

TEST_CASE("scope isolation: a root-level creator does not resolve a nested Item's private element",
          "[private_creator]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0009, 0x1002, "SH", "in-item");  // no creator inside this item
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0010, "LO", "ROOT_CO");  // creator declared only at root
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0009, 0x1002));
  auto res = resolve_private_creator(structure, path);
  REQUIRE(res.kind == PrivateElementKind::Data);
  REQUIRE(res.status == PrivateCreatorStatus::NoCreator);
}

TEST_CASE("scope isolation: Item 0's creator does not leak into Item 1", "[private_creator]") {
  FixtureBuilder item0_content;
  item0_content.element_short(0x0009, 0x0010, "LO", "ITEM0_CO");
  item0_content.element_short(0x0009, 0x1001, "SH", "item0-data");
  auto item0 = wrap_in_item(item0_content);

  FixtureBuilder item1_content;
  item1_content.element_short(0x0009, 0x1001, "SH", "item1-data");  // no creator in Item 1
  auto item1 = wrap_in_item(item1_content);

  FixtureBuilder items;
  items.append(item0).append(item1);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(items.size()));
  b.append(items);
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  ElementPath path0;
  path0.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0009, 0x1001));
  auto res0 = resolve_private_creator(structure, path0);
  REQUIRE(res0.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*res0.creator == "ITEM0_CO");

  ElementPath path1;
  path1.push(Tag(0x0008, 0x1140), 1).push(Tag(0x0009, 0x1001));
  auto res1 = resolve_private_creator(structure, path1);
  REQUIRE(res1.status == PrivateCreatorStatus::NoCreator);
}

TEST_CASE("scope isolation: an outer Item's creator does not leak into a nested child Item",
          "[private_creator]") {
  FixtureBuilder inner_content;
  inner_content.element_short(0x0009, 0x1001, "SH", "inner-data");  // no creator in inner Item
  auto inner_item = wrap_in_item(inner_content);

  FixtureBuilder outer_content;
  outer_content.element_short(0x0009, 0x0010, "LO", "OUTER_CO");  // creator declared in outer Item
  outer_content.sequence_defined(0x300A, 0x00B6, static_cast<std::uint32_t>(inner_item.size()));
  outer_content.append(inner_item);
  auto outer_item = wrap_in_item(outer_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(outer_item.size()));
  b.append(outer_item);
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x0009, 0x1001));
  auto res = resolve_private_creator(structure, path);
  REQUIRE(res.kind == PrivateElementKind::Data);
  REQUIRE(res.status == PrivateCreatorStatus::NoCreator);
}

TEST_CASE("an ordinary public element is explicitly NotPrivate", "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0060, "CS", "CT");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0008, 0x0060)));
  REQUIRE(res.kind == PrivateElementKind::NotPrivate);
  REQUIRE(res.status == PrivateCreatorStatus::NotApplicable);
  REQUIRE_FALSE(res.block.has_value());
  REQUIRE_FALSE(res.creator.has_value());
}

TEST_CASE("resolving a path that does not exist reports NotPrivate/NotApplicable by design",
          "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x1001)));
  REQUIRE(res.kind == PrivateElementKind::NotPrivate);
  REQUIRE(res.status == PrivateCreatorStatus::NotApplicable);
}

TEST_CASE("requesting resolution for the Private Creator element itself is intentional, not an "
          "error",
          "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0010, "LO", "SELF_CO");
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x0010)));
  REQUIRE(res.kind == PrivateElementKind::Creator);
  REQUIRE(res.status == PrivateCreatorStatus::NotApplicable);
  REQUIRE(res.block.has_value());
  REQUIRE(*res.block == 0x10);
  // A creator element is not itself "owned" by a creator -- no self-lookup.
  REQUIRE_FALSE(res.creator.has_value());
}

TEST_CASE("(gggg,0000) group length is classified separately from the creator scheme",
          "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0000, "UL", std::string(4, '\0'));
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x0000)));
  REQUIRE(res.kind == PrivateElementKind::GroupLength);
  REQUIRE(res.status == PrivateCreatorStatus::NotApplicable);
}

TEST_CASE("element numbers below the valid creator-block range are Reserved, never guessed",
          "[private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0009, 0x0005, "SH", "legacy");  // below 0x0010: not a valid creator address
  b.element_short(0x0009, 0x0200, "SH", "unused");  // above 0x00FF but below 0x1000: no valid block
  auto result = fds::parse_buffer(b.bytes());
  auto& structure = parse_or_fail(result);

  auto low = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x0005)));
  REQUIRE(low.kind == PrivateElementKind::Reserved);
  REQUIRE(low.status == PrivateCreatorStatus::NotApplicable);

  auto mid = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x0200)));
  REQUIRE(mid.kind == PrivateElementKind::Reserved);
  REQUIRE(mid.status == PrivateCreatorStatus::NotApplicable);
}
