#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <span>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// A1.4 adversarial Sequence/Item matrix + declared-length boundary
// discipline, under Implicit VR Little Endian. See
// docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md.
//
// The defined/defined, one-level case (the primary correctness target) is
// covered by tests/integration/test_parse_implicit_vr_le.cpp's "a
// defined-length standard Sequence is recursively parsed (A1.4)" and is
// not duplicated here; this file covers the remaining seven
// length-form x nesting combinations plus structural extras and malformed
// boundaries.

using namespace fds_test;
using fds::DiagnosticSeverity;
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

namespace {

ParseResult parse_implicit(const FixtureBuilder& b, Fidelity fidelity = Fidelity::Lossless) {
  ParseOptions options;
  options.fidelity = fidelity;
  return fds::parse_buffer(b.bytes(), options);
}

bool has_severity(const fds::ParseResult& r, DiagnosticSeverity severity) {
  for (const auto& d : r.diagnostics) {
    if (d.severity == severity) return true;
  }
  return false;
}

// One Item containing a single scalar element (0008,0100) CodeValue, SH.
FixtureBuilder build_item(bool item_undefined, const std::string& value) {
  FixtureBuilder content;
  content.element_implicit_ascii(0x0008, 0x0100, value);
  return item_undefined ? wrap_in_undefined_item(content) : wrap_in_item(content);
}

// Wraps `items` as one Sequence element at (group,element), defined or
// undefined length per `seq_undefined`.
FixtureBuilder build_sequence(std::uint16_t group, std::uint16_t element, bool seq_undefined,
                               const std::vector<FixtureBuilder>& items) {
  FixtureBuilder items_bytes;
  for (const auto& it : items) items_bytes.append(it);

  FixtureBuilder out;
  if (seq_undefined) {
    out.element_implicit_header(group, element, fds::ValueLength::kUndefinedMarker);
    out.append(items_bytes);
    out.sequence_delimiter();
  } else {
    out.element_implicit_header(group, element, static_cast<std::uint32_t>(items_bytes.size()));
    out.append(items_bytes);
  }
  return out;
}

// Common assertions shared by every one-level matrix cell: correct parse,
// VR/provenance, Item count, nested ElementPath lookup, recursive visit(),
// mutation of the nested scalar, and serialize/reparse.
void check_one_level_sequence(const ParseResult& result, bool seq_undefined, bool item_undefined) {
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;

  const auto* e = structure.find(Tag(0x0008, 0x1140));  // ReferencedImageSequence
  REQUIRE(e != nullptr);
  REQUIRE(e->is_sequence());
  REQUIRE(e->vr() == VR::SQ);
  REQUIRE(e->vr_provenance() == VRProvenance::Dictionary);
  REQUIRE(e->sequence().has_undefined_length() == seq_undefined);
  REQUIRE(e->sequence().items().size() == 1);
  const auto& item = e->sequence().items()[0];
  REQUIRE(item.has_undefined_length() == item_undefined);
  REQUIRE(item.elements().size() == 1);
  REQUIRE(item.elements()[0].tag() == Tag(0x0008, 0x0100));
  REQUIRE(item.elements()[0].vr() == VR::SH);
  REQUIRE(item.elements()[0].value().as_string() == "12345");

  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.find(path) != nullptr);
  REQUIRE(structure.find(path)->value().as_string() == "12345");

  std::size_t visited = 0;
  structure.visit([&](const fds::Element& el, const ElementPath&) {
    if (el.tag() == Tag(0x0008, 0x0100)) ++visited;
  });
  REQUIRE(visited == 1);

  REQUIRE(structure.set_value(path, Value::from_string("6789")));
  REQUIRE(structure.find(path)->value().as_string() == "6789");
  REQUIRE(structure.is_modified());

  std::ostringstream out;
  auto write_result = structure.write(out);
  REQUIRE(write_result.status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status != ParseStatus::Failed);
  ElementPath reparsed_path;
  reparsed_path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(reparsed.structure->find(reparsed_path)->value().as_string() == "6789");
}

}  // namespace

// --- One-level matrix: the (defined SQ, defined Item) cell is covered in
// test_parse_implicit_vr_le.cpp; the other three cells are here. ---

TEST_CASE("SQ matrix: defined Sequence, undefined Item, one level", "[implicit][sq_matrix]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  auto item = build_item(/*item_undefined=*/true, "12345");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item}));
  check_one_level_sequence(parse_implicit(b), /*seq_undefined=*/false, /*item_undefined=*/true);
}

TEST_CASE("SQ matrix: undefined Sequence, defined Item, one level", "[implicit][sq_matrix]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  auto item = build_item(/*item_undefined=*/false, "12345");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/true, {item}));
  check_one_level_sequence(parse_implicit(b), /*seq_undefined=*/true, /*item_undefined=*/false);
}

TEST_CASE("SQ matrix: undefined Sequence, undefined Item, one level", "[implicit][sq_matrix]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  auto item = build_item(/*item_undefined=*/true, "12345");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/true, {item}));
  check_one_level_sequence(parse_implicit(b), /*seq_undefined=*/true, /*item_undefined=*/true);
}

// --- Multi-level nesting: all four combinations, two levels deep.
// Outer: (300A,00B0) BeamSequence -> Item -> (300A,00B6)
// BeamLimitingDeviceSequence -> Item -> (0008,0100) CodeValue. ---

namespace {
void check_multi_level(bool outer_undefined, bool outer_item_undefined, bool inner_undefined,
                        bool inner_item_undefined) {
  FixtureBuilder inner_item_content;
  inner_item_content.element_implicit_ascii(0x0008, 0x0100, "DEEP");
  FixtureBuilder inner_item =
      inner_item_undefined ? wrap_in_undefined_item(inner_item_content) : wrap_in_item(inner_item_content);
  FixtureBuilder inner_seq = build_sequence(0x300A, 0x00B6, inner_undefined, {inner_item});

  FixtureBuilder outer_item = outer_item_undefined ? wrap_in_undefined_item(inner_seq) : wrap_in_item(inner_seq);
  FixtureBuilder outer_seq = build_sequence(0x300A, 0x00B0, outer_undefined, {outer_item});

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(outer_seq);

  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;

  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x0008, 0x0100));
  const auto* deep = structure.find(path);
  REQUIRE(deep != nullptr);
  REQUIRE(deep->vr() == VR::SH);
  REQUIRE(deep->value().as_string() == "DEEP");

  const auto* outer = structure.find(Tag(0x300A, 0x00B0));
  REQUIRE(outer->vr_provenance() == VRProvenance::Dictionary);
  REQUIRE(outer->sequence().has_undefined_length() == outer_undefined);
  const auto* inner = outer->sequence().items()[0].find(Tag(0x300A, 0x00B6));
  REQUIRE(inner != nullptr);
  REQUIRE(inner->vr_provenance() == VRProvenance::Dictionary);
  REQUIRE(inner->sequence().has_undefined_length() == inner_undefined);
}
}  // namespace

TEST_CASE("SQ matrix: defined/defined, multi-level", "[implicit][sq_matrix]") {
  check_multi_level(false, false, false, false);
}
TEST_CASE("SQ matrix: defined/undefined, multi-level", "[implicit][sq_matrix]") {
  check_multi_level(false, false, false, true);
}
TEST_CASE("SQ matrix: undefined/defined, multi-level", "[implicit][sq_matrix]") {
  check_multi_level(true, true, true, false);
}
TEST_CASE("SQ matrix: undefined/undefined, multi-level", "[implicit][sq_matrix]") {
  check_multi_level(true, true, true, true);
}
TEST_CASE("SQ matrix: mixed length forms across nesting levels, multi-level",
          "[implicit][sq_matrix]") {
  // Outer defined-length Sequence containing a defined-length Item, whose
  // content is an undefined-length inner Sequence containing an
  // undefined-length Item -- proves length-form independence between
  // levels, not just uniform-per-object.
  check_multi_level(/*outer_undefined=*/false, /*outer_item_undefined=*/false,
                     /*inner_undefined=*/true, /*inner_item_undefined=*/true);
}

// --- Structural extras ---

TEST_CASE("an empty defined-length Sequence has zero Items", "[implicit][sq_matrix]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {}));
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  const auto* e = result.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(e != nullptr);
  REQUIRE(e->is_sequence());
  REQUIRE(e->sequence().items().empty());
}

TEST_CASE("an empty undefined-length Sequence has zero Items", "[implicit][sq_matrix]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/true, {}));
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  const auto* e = result.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(e->sequence().items().empty());
}

TEST_CASE("a Sequence containing one empty Item parses correctly", "[implicit][sq_matrix]") {
  FixtureBuilder empty_content;
  auto item = wrap_in_item(empty_content);
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item}));
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  const auto* e = result.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(e->sequence().items().size() == 1);
  REQUIRE(e->sequence().items()[0].elements().empty());
}

TEST_CASE("a Sequence with multiple Items preserves order and content", "[implicit][sq_matrix]") {
  auto item0 = build_item(false, "AAAAA");
  auto item1 = build_item(false, "BBBBB");
  auto item2 = build_item(true, "CCCCC");
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item0, item1, item2}));
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  const auto& items = result.structure->find(Tag(0x0008, 0x1140))->sequence().items();
  REQUIRE(items.size() == 3);
  REQUIRE(items[0].elements()[0].value().as_string() == "AAAAA");
  REQUIRE(items[1].elements()[0].value().as_string() == "BBBBB");
  REQUIRE(items[2].elements()[0].value().as_string() == "CCCCC");
}

TEST_CASE("multiple sibling Sequences, each independently nested, are both correct",
          "[implicit][sq_matrix]") {
  auto item_a = build_item(false, "AAAAA");
  auto item_b = build_item(true, "BBBBB");
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item_a}));   // ReferencedImageSequence
  b.append(build_sequence(0x0040, 0xA730, /*seq_undefined=*/true, {item_b}));    // ContentSequence
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->find(Tag(0x0008, 0x1140))->sequence().items()[0].elements()[0].value().as_string() ==
          "AAAAA");
  REQUIRE(result.structure->find(Tag(0x0040, 0xA730))->sequence().items()[0].elements()[0].value().as_string() ==
          "BBBBB");
}

TEST_CASE("a Sequence followed by an ordinary element parses both correctly",
          "[implicit][sq_matrix]") {
  auto item = build_item(false, "12345");
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item}));
  b.element_implicit_ascii(0x0008, 0x0060, "CT");  // Modality, ordinary trailing element
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->find(Tag(0x0008, 0x1140))->is_sequence());
  const auto* modality = result.structure->find(Tag(0x0008, 0x0060));
  REQUIRE(modality != nullptr);
  REQUIRE(modality->vr() == VR::CS);
  REQUIRE(modality->value().as_string() == "CT");
}

TEST_CASE("a Sequence precedes Pixel Data and both parse correctly under Implicit VR",
          "[implicit][sq_matrix]") {
  auto item = build_item(false, "12345");
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item}));
  std::vector<std::byte> pixels(8, std::byte{0x77});
  b.element_implicit(0x7FE0, 0x0010, pixels);
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->find(Tag(0x0008, 0x1140))->is_sequence());
  REQUIRE(result.structure->pixel_data() != nullptr);
  REQUIRE(result.structure->pixel_data_position().has_value());
  REQUIRE(*result.structure->pixel_data_position() > 0);
}

TEST_CASE("a Sequence follows Pixel Data and both parse correctly under Implicit VR",
          "[implicit][sq_matrix]") {
  auto item = build_item(false, "12345");
  auto b = make_file_meta("1.2.840.10008.1.2");
  std::vector<std::byte> pixels(8, std::byte{0x88});
  b.element_implicit(0x7FE0, 0x0010, pixels);
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item}));
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->find(Tag(0x0008, 0x1140))->is_sequence());
  REQUIRE(result.structure->pixel_data() != nullptr);
  // Pixel Data is at the position recorded when it was consumed; the
  // Sequence after it does not disturb that.
  REQUIRE(result.structure->pixel_data_position().has_value());
}

// --- Declared-length boundary discipline (section 8) ---

TEST_CASE("a defined-length Sequence's declared length extending beyond the source is "
          "recoverable, not a crash",
          "[implicit][sq_matrix][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  // Declare far more content than actually follows.
  b.element_implicit_header(0x0008, 0x1140, 10000);
  auto item = build_item(false, "X");
  b.append(item);
  // Source ends well short of the declared 10000 bytes.

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  REQUIRE(result.structure != nullptr);  // File Meta still usable
  REQUIRE_FALSE(result.structure->contains(Tag(0x0008, 0x1140)));
}

TEST_CASE("a child element overrunning its enclosing defined-length Item is recoverable",
          "[implicit][sq_matrix][malformed]") {
  FixtureBuilder bad_item;
  // Item declares only 4 bytes of content, but the one element inside
  // claims a much longer value -- the element's own declared length runs
  // past the Item's declared boundary.
  bad_item.tag(0xFFFE, 0xE000);
  bad_item.raw_u32(4);  // Item content length: 4 bytes
  bad_item.tag(0x0008, 0x0100);
  bad_item.raw_u32(100);  // element claims 100 bytes, far more than the Item allows
  bad_item.ascii("XXXX");  // only 4 bytes actually present

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x0008, 0x1140, static_cast<std::uint32_t>(bad_item.size()));
  b.append(bad_item);
  b.element_implicit_ascii(0x0008, 0x0060, "CT");  // sibling that must not be corrupted/consumed

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  REQUIRE(result.structure != nullptr);
  REQUIRE_FALSE(result.structure->contains(Tag(0x0008, 0x1140)));
  // The parser stopped at the malformed Sequence; it must not have guessed
  // its way past the corruption and picked up the trailing sibling either.
  REQUIRE_FALSE(result.structure->contains(Tag(0x0008, 0x0060)));
}

TEST_CASE("an Item overrunning its enclosing defined-length Sequence is recoverable",
          "[implicit][sq_matrix][malformed]") {
  // A single Item whose own declared length is longer than the Sequence's
  // declared length that contains it.
  FixtureBuilder item_content;
  item_content.element_implicit_ascii(0x0008, 0x0100, "1234567890");
  auto item = wrap_in_item(item_content);  // a real, well-formed, longer Item

  auto b = make_file_meta("1.2.840.10008.1.2");
  // Declare the Sequence's content shorter than the Item actually is.
  b.element_implicit_header(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()) - 4);
  b.append(item);

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  REQUIRE_FALSE(result.structure->contains(Tag(0x0008, 0x1140)));
}

TEST_CASE("extra, non-Item bytes inside a declared Sequence value are recoverable",
          "[implicit][sq_matrix][malformed]") {
  auto item = build_item(false, "12345");
  FixtureBuilder extra;
  extra.raw_bytes(std::vector<std::byte>(8, std::byte{0xAB}));  // garbage, not an Item header

  FixtureBuilder content;
  content.append(item).append(extra);

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x0008, 0x1140, static_cast<std::uint32_t>(content.size()));
  b.append(content);

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  REQUIRE_FALSE(result.structure->contains(Tag(0x0008, 0x1140)));
}

TEST_CASE("declared-length Sequence/Item boundaries land exactly with no over/under-read",
          "[implicit][sq_matrix]") {
  // Positive control for the boundary tests above: a well-formed
  // multi-item defined-length Sequence, followed by a real trailing
  // sibling that must be reached correctly -- proves the parser consumes
  // exactly the declared length, no more, no less.
  auto item0 = build_item(false, "AAAAA");
  auto item1 = build_item(false, "BBBBB");
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(build_sequence(0x0008, 0x1140, /*seq_undefined=*/false, {item0, item1}));
  b.element_implicit_ascii(0x0008, 0x0060, "CT");

  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->find(Tag(0x0008, 0x1140))->sequence().items().size() == 2);
  const auto* modality = result.structure->find(Tag(0x0008, 0x0060));
  REQUIRE(modality != nullptr);
  REQUIRE(modality->value().as_string() == "CT");
}

// --- Mutation deep inside a multi-level Implicit-VR-parsed Sequence tree ---

TEST_CASE("erase() removes a nested scalar three levels deep inside Implicit-VR-parsed "
          "Sequences, and write/reparse confirms it",
          "[implicit][sq_matrix][mutation]") {
  FixtureBuilder innermost;
  innermost.element_implicit_ascii(0x0008, 0x0100, "AB");  // to be erased
  innermost.element_implicit_ascii(0x0008, 0x0060, "CT");  // must survive
  auto innermost_item = wrap_in_item(innermost);
  auto middle_seq = build_sequence(0x300A, 0x00B6, /*seq_undefined=*/false, {innermost_item});

  auto middle_item = wrap_in_item(middle_seq);
  auto outer_seq = build_sequence(0x300A, 0x00B0, /*seq_undefined=*/false, {middle_item});

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(outer_seq);

  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;

  ElementPath to_erase;
  to_erase.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(structure.find(to_erase) != nullptr);
  REQUIRE(structure.erase(to_erase));
  REQUIRE(structure.is_modified());
  REQUIRE(structure.find(to_erase) == nullptr);

  ElementPath survivor;
  survivor.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x0008, 0x0060));
  REQUIRE(structure.find(survivor) != nullptr);

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status != ParseStatus::Failed);
  REQUIRE(reparsed.structure->find(to_erase) == nullptr);
  REQUIRE(reparsed.structure->find(survivor) != nullptr);
  REQUIRE(reparsed.structure->find(survivor)->value().as_string() == "CT");
}
