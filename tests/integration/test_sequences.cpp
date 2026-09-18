#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

using namespace fds_test;
using fds::ElementPath;
using fds::ParseStatus;
using fds::Tag;
using fds::VR;

namespace {
fds::ParseResult parse(const FixtureBuilder& b) {
  return fds::parse_buffer(b.bytes());
}
}  // namespace

TEST_CASE("Defined-length sequence with one item and one element", "[sequences]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "12345");  // Code Value

  FixtureBuilder item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);

  const auto* seq_element = result.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(seq_element != nullptr);
  REQUIRE(seq_element->is_sequence());
  REQUIRE_FALSE(seq_element->sequence().has_undefined_length());
  REQUIRE(seq_element->sequence().items().size() == 1);

  const auto& item0 = seq_element->sequence().items()[0];
  REQUIRE_FALSE(item0.has_undefined_length());
  REQUIRE(item0.elements().size() == 1);
  REQUIRE(item0.elements()[0].tag() == Tag(0x0008, 0x0100));
  REQUIRE(item0.elements()[0].value().as_string() == "12345");
}

TEST_CASE("Undefined-length sequence with undefined-length item", "[sequences]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "ABCDE");

  FixtureBuilder item = wrap_in_undefined_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_undefined(0x0008, 0x1140);
  b.append(item);
  b.sequence_delimiter();

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);

  const auto* seq_element = result.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(seq_element != nullptr);
  REQUIRE(seq_element->sequence().has_undefined_length());
  REQUIRE(seq_element->sequence().items().size() == 1);
  REQUIRE(seq_element->sequence().items()[0].has_undefined_length());
  REQUIRE(seq_element->sequence().items()[0].elements()[0].value().as_string() == "ABCDE");
}

TEST_CASE("Nested sequences resolve through ElementPath", "[sequences]") {
  // (300A,00B0) Beam Sequence -> item[0] -> (300A,00B6) Control Point
  // Sequence -> item[2] -> (300A,0072) Nominal Beam Energy
  FixtureBuilder inner_item_content;
  inner_item_content.element_short(0x300A, 0x0072, "DS", "6.0");
  FixtureBuilder inner_item = wrap_in_item(inner_item_content);

  FixtureBuilder inner_items;
  for (int i = 0; i < 3; ++i) inner_items.append(inner_item);  // 3 identical control points

  FixtureBuilder outer_item_content;
  outer_item_content.sequence_defined(0x300A, 0x00B6, static_cast<std::uint32_t>(inner_items.size()));
  outer_item_content.append(inner_items);
  FixtureBuilder outer_item = wrap_in_item(outer_item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(outer_item.size()));
  b.append(outer_item);

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);

  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 2).push(Tag(0x300A, 0x0072));
  const auto* leaf = result.structure->find(path);
  REQUIRE(leaf != nullptr);
  REQUIRE(leaf->value().as_string() == "6.0");
  REQUIRE(path.to_string() == "(300A,00B0)[0]/(300A,00B6)[2]/(300A,0072)");
}

TEST_CASE("visit() recursively reaches elements nested inside sequences", "[sequences]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "XYZ");
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);

  bool found_nested = false;
  int visited = 0;
  result.structure->visit([&](const fds::Element& e, const ElementPath& path) {
    ++visited;
    if (e.tag() == Tag(0x0008, 0x0100)) {
      found_nested = true;
      REQUIRE(path.to_string() == "(0008,1140)[0]/(0008,0100)");
    }
  });
  REQUIRE(found_nested);
  REQUIRE(visited > 1);
}
