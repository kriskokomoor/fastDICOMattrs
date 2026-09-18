#include <catch2/catch_test_macros.hpp>

#include <span>
#include <sstream>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fastdicomattrs/pixel_data_reference.hpp"
#include "fixture_builder.hpp"

// A1.7 -- validates the structural primitive
// DICOMStructure::insert(parent, tag, vr, value) and the centralized
// container-location helper (src/internal/container_locate.hpp) behind
// it. See docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 1 for the parent-path/tag design this substrate implements, and
// section 2 for why there is no upsert() here (removed after the design
// checkpoint -- see git history for the short-lived experimental version).
//
// `parent` is a *container locator*: every step (no exception for the
// last) must be a descent step (Sequence tag + Item index); empty means
// root. This is a different ElementPath shape from the *element locator*
// find()/set_value()/erase() use (last step bare, naming the leaf) --
// tests below exercise both the nested-descent cases and the "a caller
// passed an element-locator path where a container-locator was required"
// failure mode.

using namespace fds_test;
using fds::DICOMStructure;
using fds::ElementPath;
using fds::Fidelity;
using fds::ParseOptions;
using fds::ParseResult;
using fds::ParseStatus;
using fds::resolve_private_creator;
using fds::PrivateCreatorStatus;
using fds::PrivateElementKind;
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

ParseResult write_then_reparse(const DICOMStructure& structure, std::string& written) {
  std::ostringstream out;
  auto write_result = structure.write(out);
  REQUIRE(write_result.status == WriteStatus::Success);
  written = out.str();
  return fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
}

// The root container locator -- an empty ElementPath.
ElementPath root_parent() { return ElementPath(); }

}  // namespace

// ---------------------------------------------------------------------
// Basic insertion at every nesting depth
// ---------------------------------------------------------------------

TEST_CASE("insert creates a brand-new top-level element", "[mutation][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE_FALSE(structure.contains(Tag(0x0010, 0x0020)));
  REQUIRE(structure.insert(root_parent(), Tag(0x0010, 0x0020), VR::LO, Value::from_string("NEWID ")));
  const auto* e = structure.find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
  REQUIRE(e->value().as_string() == "NEWID");
  REQUIRE(structure.is_modified());
}

TEST_CASE("insert creates a new element one level nested inside a Sequence Item",
          "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("99")));

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0102));
  const auto* e = structure.find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->value().as_string() == "99");
  // Sibling untouched.
  ElementPath sibling;
  sibling.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.find(sibling)->value().as_string() == "12345");
}

TEST_CASE("insert creates a new element two levels nested", "[mutation][insert]") {
  FixtureBuilder inner_content;
  inner_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto inner_item = wrap_in_item(inner_content);

  FixtureBuilder outer_content;
  outer_content.sequence_defined(0x300A, 0x00B6, static_cast<std::uint32_t>(inner_item.size()));
  outer_content.append(inner_item);
  auto outer_item = wrap_in_item(outer_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(outer_item.size()));
  b.append(outer_item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0);
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("77")));

  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x0008, 0x0102));
  const auto* e = structure.find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->value().as_string() == "77");

  // Item counts at every level are unchanged -- structural insertion only
  // adds a leaf to the innermost container's element list.
  const auto* outer_seq = structure.find(Tag(0x300A, 0x00B0));
  REQUIRE(outer_seq->sequence().items().size() == 1);
  const auto& outer_elements = outer_seq->sequence().items()[0].elements();
  const Tag inner_seq_tag(0x300A, 0x00B6);
  const fds::Element* inner_seq = nullptr;
  for (const auto& el : outer_elements) {
    if (el.tag() == inner_seq_tag) inner_seq = &el;
  }
  REQUIRE(inner_seq != nullptr);
  REQUIRE(inner_seq->sequence().items().size() == 1);
}

TEST_CASE("insert into an empty Item", "[mutation][insert]") {
  FixtureBuilder empty_content;  // no elements at all
  auto item = wrap_in_item(empty_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  const auto* seq_before = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq_before->sequence().items()[0].elements().empty());

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0100), VR::SH, Value::from_string("AB")));

  const auto* seq_after = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq_after->sequence().items()[0].elements().size() == 1);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.find(path)->value().as_string() == "AB");
}

// ---------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------

TEST_CASE("insert reconstructs ascending tag order within its container", "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0060, "CS", "CT");   // (0008,0060)
  item_content.element_short(0x0020, 0x000D, "UI", "1.2");  // (0020,000D)
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);

  REQUIRE(structure.insert(parent, Tag(0x0010, 0x0020), VR::LO, Value::from_string("NEWID ")));  // between
  REQUIRE(structure.insert(parent, Tag(0x0002, 0x0013), VR::SH, Value::from_string("X ")));       // first
  REQUIRE(structure.insert(parent, Tag(0x7FE1, 0x0001), VR::LO, Value::from_string("Y ")));       // last

  const auto& elements = structure.find(Tag(0x0008, 0x1140))->sequence().items()[0].elements();
  REQUIRE(elements.size() == 5);
  for (std::size_t i = 1; i < elements.size(); ++i) {
    INFO("index " << i);
    REQUIRE(elements[i - 1].tag() < elements[i].tag());
  }
}

// ---------------------------------------------------------------------
// Rejections -- every one must leave the structure completely untouched
// ---------------------------------------------------------------------

TEST_CASE("insert rejects a duplicate tag", "[mutation][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE_FALSE(structure.insert(root_parent(), Tag(0x0010, 0x0020), VR::LO, Value::from_string("XX")));
  REQUIRE(structure.find(Tag(0x0010, 0x0020))->value().as_string() == "ID1");
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert rejects a duplicate tag in a nested container", "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE_FALSE(structure.insert(parent, Tag(0x0008, 0x0100), VR::SH, Value::from_string("ZZ")));

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.find(path)->value().as_string() == "12345");
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert rejects a parent path through a nonexistent container", "[mutation][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);  // (0008,1140) doesn't exist
  REQUIRE_FALSE(structure.insert(parent, Tag(0x0008, 0x0100), VR::SH, Value::from_string("AB")));
  REQUIRE_FALSE(structure.is_modified());
  REQUIRE_FALSE(structure.contains(Tag(0x0008, 0x1140)));
}

TEST_CASE("insert rejects descending through a non-Sequence element", "[mutation][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x1140, "SH", "NOTASEQ");  // same tag, but not SQ
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE_FALSE(structure.insert(parent, Tag(0x0008, 0x0100), VR::SH, Value::from_string("AB")));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert rejects an out-of-range Item index", "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);  // exactly one Item, index 0
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 1);  // no Item at index 1
  REQUIRE_FALSE(structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("AB")));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert rejects a parent path whose last step is bare (an element-locator path, "
          "not a container locator)",
          "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  // A caller mistakenly passing an element-locator-shaped path (bare last
  // step) as `parent` must fail cleanly, not silently insert somewhere
  // unintended and not crash.
  ElementPath malformed_parent(Tag(0x0008, 0x1140));  // single bare step, no item_index
  REQUIRE_FALSE(
      structure.insert(malformed_parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("AB")));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert rejects SQ and Unknown VR for a new nested scalar", "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);

  REQUIRE_FALSE(structure.insert(parent, Tag(0x300A, 0x00B6), VR::SQ, Value::from_string("AB")));
  REQUIRE_FALSE(structure.insert(parent, Tag(0x0009, 0x1001), VR::Unknown, Value::from_string("AB")));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert rejects an odd-length value at a nested path", "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE_FALSE(
      structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("ODD")));  // 3 bytes
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert rejects a value too long for a short-form VR at a nested path",
          "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  std::vector<std::byte> oversized(70000, std::byte{0x41});
  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE_FALSE(structure.insert(parent, Tag(0x0010, 0x0020), VR::LO, Value::from_owned(oversized)));
  REQUIRE_FALSE(structure.is_modified());
}

// ---------------------------------------------------------------------
// is_modified()
// ---------------------------------------------------------------------

TEST_CASE("a failed insertion leaves is_modified() false", "[mutation][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  auto result = parse_lossless(b);
  auto& structure = *result.structure;
  REQUIRE_FALSE(structure.is_modified());

  REQUIRE_FALSE(structure.insert(root_parent(), Tag(0x0010, 0x0020), VR::LO, Value::from_string("XX")));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("a successful nested insertion sets is_modified() true", "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;
  REQUIRE_FALSE(structure.is_modified());

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("99")));
  REQUIRE(structure.is_modified());
}

// ---------------------------------------------------------------------
// Write / reparse round-trip
// ---------------------------------------------------------------------

TEST_CASE("a nested insertion round-trips through write and is visible at the same ElementPath",
          "[mutation][insert][roundtrip]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("99")));

  std::string written;
  auto reparsed = write_then_reparse(structure, written);
  REQUIRE(reparsed.status != ParseStatus::Failed);

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0102));
  const auto* e = reparsed.structure->find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->value().as_string() == "99");
}

// ---------------------------------------------------------------------
// Non-interference with unrelated structure
// ---------------------------------------------------------------------

TEST_CASE("nested insertion does not disturb Pixel Data's reference or reported position",
          "[mutation][insert]") {
  std::vector<std::byte> pixels(8, std::byte{0x11});
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0060, "CS", "CT");  // precedes Pixel Data
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  b.pixel_data_native("OW", pixels);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  REQUIRE(structure.pixel_data() != nullptr);
  auto span_before = structure.pixel_data()->native_span();
  auto position_before = structure.pixel_data_position();

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("99")));

  REQUIRE(structure.pixel_data() != nullptr);
  REQUIRE(structure.pixel_data()->native_span().offset() == span_before.offset());
  REQUIRE(structure.pixel_data()->native_span().length() == span_before.length());
  REQUIRE(structure.pixel_data_position() == position_before);
}

TEST_CASE("nested insertion does not disturb unrelated siblings or Sequence Item count",
          "[mutation][insert]") {
  FixtureBuilder item_a_content;
  item_a_content.element_short(0x0008, 0x0100, "SH", "AAAAA");
  auto item_a = wrap_in_item(item_a_content);
  FixtureBuilder item_b_content;
  item_b_content.element_short(0x0008, 0x0100, "SH", "BBBBB");
  auto item_b = wrap_in_item(item_b_content);

  FixtureBuilder seq_content;
  seq_content.append(item_a);
  seq_content.append(item_b);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0060, "CS", "CT");  // unrelated top-level sibling
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(seq_content.size()));
  b.append(seq_content);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  const auto* seq_before = structure.find(Tag(0x0008, 0x1140));
  std::size_t item_count_before = seq_before->sequence().items().size();

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);  // insert into Item 0 only
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0102), VR::SH, Value::from_string("99")));

  const auto* seq_after = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq_after->sequence().items().size() == item_count_before);
  // Item 1 (item_b) is untouched.
  REQUIRE(seq_after->sequence().items()[1].elements().size() == 1);
  REQUIRE(seq_after->sequence().items()[1].elements()[0].value().as_string() == "BBBBB");
  // Unrelated top-level sibling untouched.
  REQUIRE(structure.find(Tag(0x0008, 0x0060))->value().as_string() == "CT");
}

TEST_CASE("nested insertion does not disturb private-creator scope resolution",
          "[mutation][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0009, 0x0010, "LO", "ACME CORP");  // creator, block 0x10
  item_content.element_short(0x0009, 0x1001, "LO", "widget");     // data element, block 0x10
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto result = parse_lossless(b);
  auto& structure = *result.structure;

  ElementPath data_path;
  data_path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x0009, 0x1001));
  auto before = resolve_private_creator(structure, data_path);
  REQUIRE(before.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*before.creator == "ACME CORP");

  // Insert an unrelated element into the same Item -- must not perturb
  // creator resolution for the pre-existing private element.
  ElementPath parent;
  parent.push(Tag(0x300A, 0x00B0), 0);
  REQUIRE(structure.insert(parent, Tag(0x0008, 0x0100), VR::SH, Value::from_string("XX")));

  auto after = resolve_private_creator(structure, data_path);
  REQUIRE(after.kind == PrivateElementKind::Data);
  REQUIRE(after.status == PrivateCreatorStatus::Resolved);
  REQUIRE(*after.creator == "ACME CORP");
  REQUIRE(*after.block == 0x10);
}
