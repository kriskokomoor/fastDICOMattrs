#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

using namespace fds_test;
using fds::Fidelity;
using fds::ParseOptions;
using fds::ParseStatus;
using fds::Tag;
using fds::VR;

namespace {
fds::ParseResult parse(const FixtureBuilder& b, Fidelity fidelity = Fidelity::Standard) {
  ParseOptions options;
  options.fidelity = fidelity;
  return fds::parse_buffer(b.bytes(), options);
}
}  // namespace

TEST_CASE("Parses a minimal Part-10 file with File Meta and one dataset element", "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ANON001");

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure != nullptr);
  REQUIRE(result.structure->transfer_syntax().uid() == "1.2.840.10008.1.2.1");
  REQUIRE(result.structure->has_file_preamble());

  const auto* e = result.structure->find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
  REQUIRE(e->value().as_string() == "ANON001");
}

TEST_CASE("Top-level elements() preserves original source order", "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0010, "PN", "Doe^Jane");  // already even length (8)
  b.element_short(0x0010, 0x0020, "LO", "ID1");
  b.element_short(0x0008, 0x0060, "CS", "CT");

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);

  const auto& elements = result.structure->elements();
  // File meta elements come first, then the dataset elements in write order.
  REQUIRE(elements.back().tag() == Tag(0x0008, 0x0060));
  REQUIRE(elements[elements.size() - 2].tag() == Tag(0x0010, 0x0020));
  REQUIRE(elements[elements.size() - 3].tag() == Tag(0x0010, 0x0010));
}

TEST_CASE("Long-form explicit VR element (e.g. OB) parses its 4-byte length", "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  std::vector<std::byte> value = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  b.element_long(0x0009, 0x0011, "OB", value);  // private tag, long form

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);
  const auto* e = result.structure->find(Tag(0x0009, 0x0011));
  REQUIRE(e != nullptr);
  REQUIRE(e->tag().is_private());
  REQUIRE(e->vr() == VR::OB);
  REQUIRE(e->value().size() == 4);
  auto bytes = e->value().bytes();
  REQUIRE(bytes[0] == std::byte{0xDE});
  REQUIRE(bytes[3] == std::byte{0xEF});
}

TEST_CASE("Unknown (unrecognized) tag is preserved, not dropped", "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0043, 0x1010, "US", std::string(2, '\x07'));  // manufacturer-specific-looking

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->contains(Tag(0x0043, 0x1010)));
}

TEST_CASE("Empty value (zero length) round-trips through Value::bytes", "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "");

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);
  const auto* e = result.structure->find(Tag(0x0010, 0x0020));
  REQUIRE(e->value().size() == 0);
  REQUIRE(e->value().as_string().empty());
}

TEST_CASE("Odd-length value is padded to even length per DICOM convention", "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1");  // "ID1" (3 chars) + NUL pad = 4 bytes

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);
  const auto* e = result.structure->find(Tag(0x0010, 0x0020));
  REQUIRE(e->value().size() == 4);       // raw bytes() keeps the pad byte
  REQUIRE(e->value().as_string() == "ID1");  // as_string() trims it
}

TEST_CASE("A bare dataset with no preamble is parsed as Explicit VR Little Endian by default",
          "[parser]") {
  FixtureBuilder b;  // no preamble, no file meta
  b.element_short(0x0010, 0x0020, "LO", "ID1");

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);  // documented default-TS warning
  REQUIRE_FALSE(result.structure->has_file_preamble());
  REQUIRE(result.structure->transfer_syntax().uid() == "1.2.840.10008.1.2.1");
  REQUIRE(result.structure->find(Tag(0x0010, 0x0020)) != nullptr);
}

// Implicit VR Little Endian is a supported Transfer Syntax as of this
// increment -- see tests/integration/test_parse_implicit_vr_le.cpp for
// dedicated coverage of that parser. This file covers only the Explicit VR
// LE path, so what belongs here is the boundary: File Meta (always
// Explicit VR) correctly hands off once (0002,0010) resolves to Implicit VR
// LE, without misreading the dataset that follows as Explicit VR.
TEST_CASE("File Meta declaring Implicit VR Little Endian hands off instead of failing",
          "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2");  // Implicit VR Little Endian
  b.element_implicit_ascii(0x0010, 0x0020, "ID1");

  auto result = parse(b);
  REQUIRE(result.status != ParseStatus::Failed);
  REQUIRE(result.structure != nullptr);
  REQUIRE_FALSE(result.structure->transfer_syntax().explicit_vr());
  REQUIRE(result.structure->find(Tag(0x0010, 0x0020))->value().as_string() == "ID1");
}

TEST_CASE("Explicit VR Big Endian is detected and rejected as Unsupported", "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.2");
  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Failed);
}

// Deflated Explicit VR Little Endian's *entire dataset* is zlib-deflated,
// not just Pixel Data -- this library has no inflate step, so it must be
// rejected cleanly rather than misread as plain bytes. Its UID also starts
// with the "1.2.840.10008.1.2" prefix shared by every compressed Transfer
// Syntax, so without an explicit special case it would fall into the
// generic ExplicitVREncapsulated bucket and be silently accepted -- exactly
// what this test guards against.
TEST_CASE("Deflated Explicit VR Little Endian is detected and rejected as Unsupported, even with "
          "well-formed-looking plain bytes",
          "[parser]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1.99");
  b.element_short(0x0008, 0x0060, "CS", "CT");  // plain (non-deflated) bytes: must not matter
  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Failed);
  REQUIRE(result.structure == nullptr);
  bool found_unsupported = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == fds::DiagnosticSeverity::Unsupported) found_unsupported = true;
  }
  REQUIRE(found_unsupported);
}

TEST_CASE("A compressed (encapsulated) Transfer Syntax UID still parses dataset metadata",
          "[parser]") {
  // JPEG Baseline Process 1 -- dataset metadata is still Explicit VR LE.
  auto b = make_file_meta("1.2.840.10008.1.2.4.50");
  b.element_short(0x0010, 0x0020, "LO", "ID1");

  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->transfer_syntax().encapsulated_pixel_data());
  REQUIRE(result.structure->find(Tag(0x0010, 0x0020)) != nullptr);
}

// A1.4 regression: dictionary-backed VR resolution is an Implicit-VR-only
// concern. Explicit VR must keep taking its VR from the wire, even when it
// conflicts with what the standard dictionary would say for that tag --
// wiring the dictionary into the Implicit VR parser must never make the
// Explicit VR parser start "correcting" wire-declared VRs.
TEST_CASE("an Explicit VR element's wire-declared VR is never replaced by the dictionary's, "
          "even when they disagree",
          "[parser][a1_4_regression]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  // (0010,0020) PatientID's dictionary VR is LO; declare it as SH on the
  // wire instead -- a deliberate mismatch.
  b.element_short(0x0010, 0x0020, "SH", "ID1 ");

  auto result = parse(b, Fidelity::Lossless);
  REQUIRE(result.status == ParseStatus::Success);
  const auto* e = result.structure->find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::SH);  // wire VR honored, not silently corrected to LO
  REQUIRE(e->has_explicit_vr_in_source());
  REQUIRE(e->vr_provenance() == fds::VRProvenance::Explicit);

  // And the unmodified byte-identical write contract still reproduces
  // exactly that (mismatched-but-wire-honored) encoding.
  std::ostringstream out;
  REQUIRE(result.structure->write(out).status == fds::WriteStatus::Success);
  std::vector<std::byte> written(out.str().size());
  for (std::size_t i = 0; i < written.size(); ++i) {
    written[i] = static_cast<std::byte>(static_cast<unsigned char>(out.str()[i]));
  }
  REQUIRE(written == b.bytes());
}
