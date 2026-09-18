#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

using namespace fds_test;
using fds::DiagnosticSeverity;
using fds::ParseStatus;
using fds::Tag;

namespace {
bool has_severity(const fds::ParseResult& r, DiagnosticSeverity severity) {
  for (const auto& d : r.diagnostics) {
    if (d.severity == severity) return true;
  }
  return false;
}
}  // namespace

TEST_CASE("Native (non-encapsulated) Pixel Data is referenced, not allocated as a Value",
          "[pixel_data]") {
  std::vector<std::byte> pixels(16, std::byte{0x42});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_native("OW", pixels);

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::Success);

  // Pixel Data must not appear in the flat element list -- it is exposed
  // exclusively via pixel_data(). See docs/architecture.md section 8.
  REQUIRE_FALSE(result.structure->contains(Tag(0x7FE0, 0x0010)));

  const auto* ref = result.structure->pixel_data();
  REQUIRE(ref != nullptr);
  REQUIRE_FALSE(ref->is_encapsulated());
  REQUIRE(ref->native_span().length() == 16);

  const std::byte* ptr = nullptr;
  REQUIRE(result.structure->source()->try_get(ref->native_span(), &ptr));
  REQUIRE(ptr[0] == std::byte{0x42});
  REQUIRE(ptr[15] == std::byte{0x42});
}

TEST_CASE("Encapsulated Pixel Data exposes Basic Offset Table and fragment spans",
          "[pixel_data]") {
  auto b = make_file_meta("1.2.840.10008.1.2.4.50");  // JPEG Baseline
  b.pixel_data_encapsulated_header("OB");
  b.item_defined_header(0);  // empty Basic Offset Table

  std::vector<std::byte> fragment1(8, std::byte{0xAA});
  std::vector<std::byte> fragment2(4, std::byte{0xBB});
  b.item_defined_header(static_cast<std::uint32_t>(fragment1.size()));
  b.raw_bytes(fragment1);
  b.item_defined_header(static_cast<std::uint32_t>(fragment2.size()));
  b.raw_bytes(fragment2);
  b.sequence_delimiter();

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::Success);

  const auto* ref = result.structure->pixel_data();
  REQUIRE(ref != nullptr);
  REQUIRE(ref->is_encapsulated());
  REQUIRE(ref->basic_offset_table().has_value());
  REQUIRE(ref->basic_offset_table()->length() == 0);
  REQUIRE(ref->fragments().size() == 2);
  REQUIRE(ref->fragments()[0].span.length() == 8);
  REQUIRE(ref->fragments()[1].span.length() == 4);

  const auto& source = *result.structure->source();
  const std::byte* p0 = nullptr;
  REQUIRE(source.try_get(ref->fragments()[0].span, &p0));
  REQUIRE(p0[0] == std::byte{0xAA});
  const std::byte* p1 = nullptr;
  REQUIRE(source.try_get(ref->fragments()[1].span, &p1));
  REQUIRE(p1[0] == std::byte{0xBB});
}

// A truncated Pixel Data element must never be silently accepted as a clean
// Success with Pixel Data just absent -- every failure path in
// parse_pixel_data() must diagnose. See docs/roundtrip-contract.md and the
// external release review this closes: a file ending right after
// "(7FE0,0010) OW" previously parsed with zero diagnostics.
namespace {
void require_truncated_pixel_data_is_recoverable(const FixtureBuilder& b) {
  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(has_severity(result, DiagnosticSeverity::RecoverableError));
  REQUIRE(result.structure != nullptr);
  REQUIRE(result.structure->pixel_data() == nullptr);
  REQUIRE_FALSE(result.structure->contains(Tag(0x7FE0, 0x0010)));
}
}  // namespace

TEST_CASE("Pixel Data truncated right after tag+VR (missing reserved bytes) is recoverable",
          "[pixel_data][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.tag(0x7FE0, 0x0010);
  b.ascii("OW");
  // nothing else -- this is the exact repro from the external review.
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Pixel Data truncated after reserved bytes (missing length) is recoverable",
          "[pixel_data][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.tag(0x7FE0, 0x0010);
  b.ascii("OW");
  b.raw_u16(0);  // reserved
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Pixel Data with a Basic Offset Table item but no length is recoverable",
          "[pixel_data][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_encapsulated_header("OB");
  b.tag(0xFFFE, 0xE000);  // Basic Offset Table item tag, then nothing
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Pixel Data whose Basic Offset Table runs past the source is recoverable",
          "[pixel_data][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_encapsulated_header("OB");
  b.tag(0xFFFE, 0xE000);
  b.raw_u32(100);  // declares 100 bytes; none follow
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Pixel Data with only a partial fragment tag after the BOT is recoverable",
          "[pixel_data][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_encapsulated_header("OB");
  b.item_defined_header(0);  // empty Basic Offset Table
  b.raw_u16(0xFFFE);         // half a tag: 2 bytes, not the full 4
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Pixel Data with a fragment Item tag but no length is recoverable",
          "[pixel_data][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_encapsulated_header("OB");
  b.item_defined_header(0);  // empty Basic Offset Table
  b.tag(0xFFFE, 0xE000);     // fragment item tag, then nothing
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Pixel Data whose fragment runs past the source is recoverable",
          "[pixel_data][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.pixel_data_encapsulated_header("OB");
  b.item_defined_header(0);  // empty Basic Offset Table
  b.tag(0xFFFE, 0xE000);
  b.raw_u32(50);  // declares 50 bytes; none follow
  require_truncated_pixel_data_is_recoverable(b);
}
