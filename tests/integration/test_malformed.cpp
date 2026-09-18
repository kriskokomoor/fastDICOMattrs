#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

using namespace fds_test;
using fds::DiagnosticSeverity;
using fds::ParseStatus;

namespace {
bool has_severity(const fds::ParseResult& r, DiagnosticSeverity severity) {
  for (const auto& d : r.diagnostics) {
    if (d.severity == severity) return true;
  }
  return false;
}
}  // namespace

TEST_CASE("A value length that runs past the end of the source is recoverable", "[malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  // Declare a 100-byte value but supply none: truncated element.
  b.tag(0x0010, 0x0020);
  b.ascii("LO");
  b.raw_u16(100);
  // (no value bytes follow)

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(result.structure != nullptr);  // File Meta elements are still usable
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
}

TEST_CASE("A file with only a tag and no VR/length is truncated but recoverable", "[malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.raw_u16(0x0010);
  b.raw_u16(0x0020);
  // nothing else

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
}

TEST_CASE("An undefined-length sequence missing its Sequence Delimitation Item is recoverable",
          "[malformed]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "AB");
  auto item = wrap_in_undefined_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_undefined(0x0008, 0x1140);
  b.append(item);
  // no sequence_delimiter() -- source ends abruptly

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  // The Sequence element itself failed to parse and is dropped, but File
  // Meta parsed before it remains usable.
  REQUIRE(result.structure != nullptr);
}

TEST_CASE("A non-sequence element claiming undefined length is rejected as malformed",
          "[malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.tag(0x0009, 0x0011);
  b.ascii("OB");
  b.raw_u16(0);
  b.raw_u32(0xFFFFFFFFu);  // undefined length on a non-SQ element: not legal

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
}

TEST_CASE("Exceeding max_sequence_depth stops cleanly instead of recursing unboundedly",
          "[malformed]") {
  // Build a sequence nested one level deeper than the configured limit.
  FixtureBuilder innermost;
  innermost.element_short(0x0008, 0x0100, "SH", "AB");

  FixtureBuilder level1_content = innermost;
  FixtureBuilder level1_item = wrap_in_item(level1_content);

  FixtureBuilder level0_content;
  level0_content.sequence_defined(0x0008, 0x9092, static_cast<std::uint32_t>(level1_item.size()));
  level0_content.append(level1_item);
  FixtureBuilder level0_item = wrap_in_item(level0_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(level0_item.size()));
  b.append(level0_item);

  fds::ParseOptions options;
  options.max_sequence_depth = 1;  // the fixture nests 2 levels deep
  auto result = fds::parse_buffer(b.bytes(), options);

  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
}

TEST_CASE("An element that overruns its containing Item's declared length is rejected",
          "[malformed]") {
  // The Item declares only 4 bytes of content, but its one child element
  // actually needs 16 bytes (tag 4 + VR 2 + length 2 + value 8) to parse.
  // Without an exact-boundary check, the parser would silently consume 12
  // bytes belonging to whatever follows the Item and just move on.
  FixtureBuilder item;
  item.item_defined_header(4);
  item.element_short(0x0008, 0x0100, "SH", "ABCDEFGH");

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  // The malformed Sequence element itself failed to parse and is dropped,
  // but File Meta parsed before it remains usable.
  REQUIRE_FALSE(result.structure->contains(fds::Tag(0x0008, 0x1140)));
}

TEST_CASE("max_element_count bounds nested elements, not just the top level", "[malformed]") {
  // One top-level Sequence containing 10 items, each with one element: 11
  // "real" elements total if nested elements are counted, but only 1
  // (the Sequence itself) if only the top-level element list is counted --
  // which is exactly the bypass this guards against. File Meta contributes
  // a handful more elements ahead of the Sequence; max_element_count is set
  // well above that (so this isn't just tripping during File Meta) but
  // comfortably below the 11-if-nested-counted total.
  FixtureBuilder content;
  for (int i = 0; i < 10; ++i) {
    FixtureBuilder item_content;
    item_content.element_short(0x0008, 0x0100, "SH", "AB");
    content.append(wrap_in_item(item_content));
  }
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(content.size()));
  b.append(content);

  fds::ParseOptions options;
  options.max_element_count = 15;
  auto result = fds::parse_buffer(b.bytes(), options);

  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  // The Sequence element failed partway through and is dropped entirely --
  // it never silently succeeds with only some of its items.
  REQUIRE_FALSE(result.structure->contains(fds::Tag(0x0008, 0x1140)));
}

TEST_CASE("nonzero Item Delimitation Item length prevents a clean lossless parse", "[malformed]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "AB");

  FixtureBuilder sequence_content;
  sequence_content.item_undefined_header().append(item_content);
  sequence_content.tag(0xFFFE, 0xE00D).raw_u32(2);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_undefined(0x0008, 0x1140).append(sequence_content).sequence_delimiter();

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::Warning));
}

TEST_CASE("nonzero Sequence Delimitation Item length prevents a clean lossless parse",
          "[malformed]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "AB");

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_undefined(0x0008, 0x1140)
      .append(wrap_in_item(item_content))
      .tag(0xFFFE, 0xE0DD)
      .raw_u32(4);

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::Warning));
}
