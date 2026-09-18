#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/mutation.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fastdicomattrs/dicom_structure.hpp"
#include "fixture_builder.hpp"

// A1.7 -- fds::mutation::insert_inferred(): the dictionary-aware
// convenience layer on top of DICOMStructure::insert(). See
// docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// sections 3, 5, 6.

using namespace fds_test;
using fds::DICOMStructure;
using fds::ElementPath;
using fds::Fidelity;
using fds::ParseOptions;
using fds::ParseResult;
using fds::Tag;
using fds::Value;
using fds::VR;
using fds::mutation::insert_inferred;
using fds::mutation::InsertStatus;
using fds::resolve_private_creator;
using fds::PrivateCreatorStatus;
using fds::PrivateElementKind;

namespace {
ParseResult parse_lossless(const FixtureBuilder& b) {
  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  return fds::parse_buffer(b.bytes(), options);
}
ElementPath root_parent() { return ElementPath(); }
}  // namespace

TEST_CASE("insert_inferred infers the VR for an unambiguous standard tag at root",
          "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  // (0010,0020) PatientID -- LO, unambiguous.
  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0010, 0x0020),
                           Value::from_string("ID1 ")) == InsertStatus::Success);
  const auto* e = structure.find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
  REQUIRE(e->value().as_string() == "ID1");
}

TEST_CASE("insert_inferred infers the VR for an unambiguous standard tag nested in an Item",
          "[mutation][insert][inferred]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0060, "CS", "CT");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(insert_inferred(structure, parent, Tag(0x0010, 0x0020), Value::from_string("ID1 ")) ==
          InsertStatus::Success);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0010, 0x0020));
  const auto* e = structure.find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
}

TEST_CASE("insert_inferred requires an explicit VR for an ambiguous standard tag",
          "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  // (0028,0106) SmallestImagePixelValue -- "US or SS", ambiguous.
  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0028, 0x0106),
                           Value::from_owned({std::byte{0x00}, std::byte{0x00}})) ==
          InsertStatus::VRRequired);
  REQUIRE_FALSE(structure.contains(Tag(0x0028, 0x0106)));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_inferred requires an explicit VR for a tag unknown to the dictionary",
          "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  // (0000,0002) is a PS3.7 Command-group element -- outside the standard
  // PS3.6 dictionary's scope by design (dictionary.hpp), even though the
  // group number is even ("public"). A genuine dictionary miss.
  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0000, 0x0002),
                           Value::from_string("AB")) == InsertStatus::VRRequired);
  REQUIRE_FALSE(structure.contains(Tag(0x0000, 0x0002)));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_inferred requires an explicit VR for a private data element",
          "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0009, 0x1001),
                           Value::from_string("AB")) == InsertStatus::VRRequired);
  REQUIRE_FALSE(structure.contains(Tag(0x0009, 0x1001)));
}

TEST_CASE("insert_inferred infers LO for a Private Creator declaration",
          "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  // (0009,0010) -- a Private Creator declaration (block 0x10), never a
  // dictionary lookup: PS3.5 7.8.1 normatively fixes this at LO.
  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0009, 0x0010),
                           Value::from_string("ACME CORP ")) == InsertStatus::Success);
  const auto* e = structure.find(Tag(0x0009, 0x0010));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
  REQUIRE(e->value().as_string() == "ACME CORP");
}

TEST_CASE("insert_inferred infers LO for a Private Creator declaration nested in an Item",
          "[mutation][insert][inferred]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0060, "CS", "CT");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x300A, 0x00B0), 0);
  REQUIRE(insert_inferred(structure, parent, Tag(0x0009, 0x00FF), Value::from_string("VENDOR X")) ==
          InsertStatus::Success);

  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x0009, 0x00FF));
  const auto* e = structure.find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
}

TEST_CASE("insert_inferred propagates container-location failures distinctly",
          "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x1140, "SH", "NOTASEQ");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath missing_parent;
  missing_parent.push(Tag(0x0009, 0x1140), 0);
  REQUIRE(insert_inferred(structure, missing_parent, Tag(0x0010, 0x0020),
                           Value::from_string("X ")) == InsertStatus::ContainerNotFound);

  ElementPath not_seq_parent;
  not_seq_parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(insert_inferred(structure, not_seq_parent, Tag(0x0010, 0x0020),
                           Value::from_string("X ")) == InsertStatus::NotASequence);

  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_inferred rejects a duplicate tag", "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0010, 0x0020),
                           Value::from_string("XX")) == InsertStatus::AlreadyExists);
  REQUIRE(structure.find(Tag(0x0010, 0x0020))->value().as_string() == "ID1");
  REQUIRE_FALSE(structure.is_modified());
}

// ---------------------------------------------------------------------
// fds::mutation::insert (explicit VR) -- the status-returning twin of
// DICOMStructure::insert(), added for the ABI's benefit (see mutation.hpp);
// never consults the dictionary.
// ---------------------------------------------------------------------

TEST_CASE("mutation::insert with an explicit VR succeeds and never infers",
          "[mutation][insert][explicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  // (0028,0106) is ambiguous ("US or SS") -- insert_inferred would reject
  // it, but the explicit-VR form must accept the caller's own VR outright.
  REQUIRE(fds::mutation::insert(structure, root_parent(), Tag(0x0028, 0x0106), VR::US,
                                 Value::from_owned({std::byte{0x00}, std::byte{0x00}})) ==
          InsertStatus::Success);
  REQUIRE(structure.find(Tag(0x0028, 0x0106))->vr() == VR::US);
}

TEST_CASE("mutation::insert rejects a duplicate tag", "[mutation][insert][explicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(fds::mutation::insert(structure, root_parent(), Tag(0x0010, 0x0020), VR::LO,
                                 Value::from_string("XX")) == InsertStatus::AlreadyExists);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("mutation::insert rejects SQ/Unknown VR", "[mutation][insert][explicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(fds::mutation::insert(structure, root_parent(), Tag(0x0008, 0x1111), VR::SQ,
                                 Value::from_string("AB")) == InsertStatus::InvalidVR);
  REQUIRE(fds::mutation::insert(structure, root_parent(), Tag(0x0008, 0x1112), VR::Unknown,
                                 Value::from_string("AB")) == InsertStatus::InvalidVR);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("mutation::insert auto-pads an odd-length value: SPACE for text VR, NUL for UI/binary",
          "[mutation][insert][explicit][padding]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(fds::mutation::insert(structure, root_parent(), Tag(0x0010, 0x0020), VR::LO,
                                 Value::from_string("ODD")) == InsertStatus::Success);
  auto lo_bytes = structure.find(Tag(0x0010, 0x0020))->value().bytes();
  REQUIRE(lo_bytes.size() == 4);
  REQUIRE(lo_bytes[3] == std::byte{0x20});

  REQUIRE(fds::mutation::insert(structure, root_parent(), Tag(0x0008, 0x0018), VR::UI,
                                 Value::from_string("1.2.3")) == InsertStatus::Success);
  auto ui_bytes = structure.find(Tag(0x0008, 0x0018))->value().bytes();
  REQUIRE(ui_bytes.size() == 6);
  REQUIRE(ui_bytes[5] == std::byte{0x00});
}

TEST_CASE("mutation::insert propagates container-location failures", "[mutation][insert][explicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath missing_parent;
  missing_parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(fds::mutation::insert(structure, missing_parent, Tag(0x0010, 0x0020), VR::LO,
                                 Value::from_string("X ")) == InsertStatus::ContainerNotFound);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_inferred auto-pads an odd-length value with SPACE for a text VR",
          "[mutation][insert][inferred][padding]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  // (0010,0020) PatientID -- LO, unambiguous; "ODD" is 3 bytes.
  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0010, 0x0020),
                           Value::from_string("ODD")) == InsertStatus::Success);
  const auto* e = structure.find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->value().size() == 4);
  REQUIRE(e->value().as_string() == "ODD");  // as_string() trims the trailing pad byte
  auto bytes = e->value().bytes();
  REQUIRE(bytes[3] == std::byte{0x20});
}

TEST_CASE("insert_inferred rejects a value too long for a short-form VR even after padding",
          "[mutation][insert][inferred]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  std::vector<std::byte> oversized(70001, std::byte{0x41});  // odd length, still too long once padded
  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0010, 0x0020),
                           Value::from_owned(oversized)) == InsertStatus::ValueTooLong);
  REQUIRE_FALSE(structure.contains(Tag(0x0010, 0x0020)));
  REQUIRE_FALSE(structure.is_modified());
}

// ---------------------------------------------------------------------
// A1.7 section 17: private-creator insertion qualification -- inferred
// Private Creator declaration + explicit-VR private data element, then
// resolve_private_creator(), at root, nested in one Item, and across two
// sibling Items -- proving the A1.2 scoping rule (never cascading, unlike
// A1.5/A1.6 charset inheritance) survives insertion unchanged.
// ---------------------------------------------------------------------

TEST_CASE("insert a Private Creator (inferred LO) and private data (explicit VR) at root, "
          "then resolve_private_creator",
          "[mutation][insert][inferred][private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0009, 0x0010),
                           Value::from_string("ACME CORP ")) == InsertStatus::Success);
  REQUIRE(structure.insert(root_parent(), Tag(0x0009, 0x1001), VR::LO, Value::from_string("widget")));

  auto res = resolve_private_creator(structure, ElementPath(Tag(0x0009, 0x1001)));
  REQUIRE(res.kind == PrivateElementKind::Data);
  REQUIRE(res.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*res.block == 0x10);
  REQUIRE(*res.creator == "ACME CORP");
}

TEST_CASE("insert a Private Creator and private data nested in one Item, then "
          "resolve_private_creator",
          "[mutation][insert][inferred][private_creator]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0060, "CS", "CT");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x300A, 0x00B0), 0);
  REQUIRE(insert_inferred(structure, parent, Tag(0x0009, 0x0010), Value::from_string("ACME CORP ")) ==
          InsertStatus::Success);
  REQUIRE(structure.insert(parent, Tag(0x0009, 0x1001), VR::LO, Value::from_string("widget")));

  ElementPath data_path;
  data_path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x0009, 0x1001));
  auto res = resolve_private_creator(structure, data_path);
  REQUIRE(res.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*res.creator == "ACME CORP");
}

TEST_CASE("Private Creator declarations inserted into two sibling Items stay independently "
          "scoped, never cascading",
          "[mutation][insert][inferred][private_creator]") {
  FixtureBuilder item0_content;
  item0_content.element_short(0x0008, 0x0060, "CS", "CT");
  auto item0 = wrap_in_item(item0_content);
  FixtureBuilder item1_content;
  item1_content.element_short(0x0008, 0x0060, "CS", "MR");
  auto item1 = wrap_in_item(item1_content);
  FixtureBuilder items;
  items.append(item0).append(item1);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(items.size()));
  b.append(items);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent0, parent1;
  parent0.push(Tag(0x300A, 0x00B0), 0);
  parent1.push(Tag(0x300A, 0x00B0), 1);

  // Item 0: creator "ACME CORP" at block 0x10, with private data.
  REQUIRE(insert_inferred(structure, parent0, Tag(0x0009, 0x0010),
                           Value::from_string("ACME CORP ")) == InsertStatus::Success);
  REQUIRE(structure.insert(parent0, Tag(0x0009, 0x1001), VR::LO, Value::from_string("widget")));

  // Item 1: a *different* creator, same block number 0x10, with its own
  // private data -- must resolve to ITS OWN creator, never Item 0's,
  // despite the identical block number and identical parent Sequence.
  REQUIRE(insert_inferred(structure, parent1, Tag(0x0009, 0x0010),
                           Value::from_string("OTHER VENDOR")) == InsertStatus::Success);
  REQUIRE(structure.insert(parent1, Tag(0x0009, 0x1001), VR::LO, Value::from_string("gadget")));

  ElementPath data0, data1;
  data0.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x0009, 0x1001));
  data1.push(Tag(0x300A, 0x00B0), 1).push(Tag(0x0009, 0x1001));

  auto res0 = resolve_private_creator(structure, data0);
  auto res1 = resolve_private_creator(structure, data1);
  REQUIRE(res0.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*res0.creator == "ACME CORP");
  REQUIRE(res1.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*res1.creator == "OTHER VENDOR");
}

TEST_CASE("insert_inferred never infers a VR for private DATA from its creator's identity",
          "[mutation][insert][inferred][private_creator]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0009, 0x0010),
                           Value::from_string("ACME CORP ")) == InsertStatus::Success);
  // Even with a resolvable creator now present, private DATA (element
  // 0x1001, not the 0x0010-0x00FF creator range) is always VRRequired --
  // the creator's identity is never consulted for this, by policy.
  REQUIRE(insert_inferred(structure, root_parent(), Tag(0x0009, 0x1001), Value::from_string("AB")) ==
          InsertStatus::VRRequired);
}
