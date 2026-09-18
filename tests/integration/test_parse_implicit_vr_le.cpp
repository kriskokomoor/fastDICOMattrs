#include <catch2/catch_test_macros.hpp>

#include <span>
#include <sstream>

#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// Implicit VR Little Endian (1.2.840.10008.1.2): tag + 4-byte length, no VR
// on the wire. As of A1.4, standard (dictionary-recognized) tags get a
// correct, dictionary-resolved VR -- including VR::SQ, which is now
// recursively parsed regardless of defined/undefined length -- via
// fds::dictionary::lookup(). A tag with no dictionary entry (private, or
// genuinely unrecognized) still gets VR::Unknown, its raw bytes preserved
// exactly as before; nothing about that case changed. See
// docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md.

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

namespace {

ParseResult parse_implicit(const FixtureBuilder& b, Fidelity fidelity = Fidelity::Standard) {
  ParseOptions options;
  options.fidelity = fidelity;
  return fds::parse_buffer(b.bytes(), options);
}

}  // namespace

// A1.4 regression: before A1.4, this test (under its original name, "a
// defined-length element parses as an opaque VR::Unknown value") used
// (0010,0020) PatientID -- a standard, dictionary-recognized tag -- as its
// fixture, to document that *no* Implicit-VR element could get a real VR
// without a dictionary. Now that a dictionary exists and is wired in, that
// same fixture tag resolves correctly (see the next test) and would no
// longer test "unknown tag stays unknown" -- so this test's fixture moves
// to a genuinely private tag, preserving its original intent, per
// docs/architecture/A1_ATTRS_V1_IMPLEMENTATION_PROGRESSION.md's own
// anticipation of exactly this migration.
TEST_CASE("a defined-length element with no dictionary entry parses as an opaque VR::Unknown "
          "value",
          "[parser][implicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_ascii(0x0009, 0x1001, "ID1");  // private, no dictionary entry possible

  auto result = parse_implicit(b, Fidelity::Lossless);
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0009, 0x1001));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::Unknown);
  REQUIRE(e->vr_provenance() == VRProvenance::Unknown);
  REQUIRE_FALSE(e->is_sequence());
  REQUIRE_FALSE(e->has_explicit_vr_in_source());
  REQUIRE(e->length_form() == fds::LengthForm::Long32);
  REQUIRE(e->value().as_string() == "ID1");
}

// A1.4's central promise for ordinary (non-ambiguous) standard scalars:
// dictionary-backed VR resolution works under Implicit VR too.
TEST_CASE("a defined-length standard tag resolves its correct dictionary VR", "[parser][implicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_ascii(0x0010, 0x0020, "ID1");  // PatientID, dictionary VR LO

  auto result = parse_implicit(b, Fidelity::Lossless);
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
  REQUIRE(e->vr_provenance() == VRProvenance::Dictionary);
  REQUIRE_FALSE(e->has_explicit_vr_in_source());
  REQUIRE(e->length_form() == fds::LengthForm::Short16);  // LO is short-form
  REQUIRE(e->value().as_string() == "ID1");
}

TEST_CASE("an undefined-length element parses as a Sequence", "[parser][implicit]") {
  FixtureBuilder item_content;
  item_content.element_implicit_ascii(0x0008, 0x0100, "12345");
  auto item = wrap_in_undefined_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x0008, 0x1140, fds::ValueLength::kUndefinedMarker);
  b.append(item);
  b.sequence_delimiter();

  auto result = parse_implicit(b, Fidelity::Lossless);
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* seq = result.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(seq != nullptr);
  REQUIRE(seq->is_sequence());
  REQUIRE(seq->sequence().has_undefined_length());
  const auto& items = seq->sequence().items();
  REQUIRE(items.size() == 1);
  REQUIRE(items[0].elements().size() == 1);
  REQUIRE(items[0].elements()[0].tag() == Tag(0x0008, 0x0100));
  REQUIRE(items[0].elements()[0].value().as_string() == "12345");
}

// A1.4 regression: before A1.4, this test (under its original name, "a
// defined-length nested sequence is NOT expanded -- documented
// limitation") asserted the opposite of what it asserts now -- that a
// defined-length standard Sequence could not be told apart from a large
// opaque value without a dictionary. A1.4 closes exactly this gap: this is
// the primary correctness target the increment exists for. Same fixture,
// corrected assertions.
TEST_CASE("a defined-length standard Sequence is recursively parsed (A1.4)",
          "[parser][implicit]") {
  FixtureBuilder item_content;
  item_content.element_implicit_ascii(0x0008, 0x0100, "12345");  // CodeValue, SH
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2");
  // (0008,1140) ReferencedImageSequence -- a standard, dictionary-SQ tag.
  b.element_implicit_header(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = parse_implicit(b, Fidelity::Lossless);
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0008, 0x1140));
  REQUIRE(e != nullptr);
  REQUIRE(e->is_sequence());
  REQUIRE(e->vr() == VR::SQ);
  REQUIRE(e->vr_provenance() == VRProvenance::Dictionary);
  REQUIRE_FALSE(e->sequence().has_undefined_length());
  const auto& items = e->sequence().items();
  REQUIRE(items.size() == 1);
  REQUIRE(items[0].elements().size() == 1);
  REQUIRE(items[0].elements()[0].tag() == Tag(0x0008, 0x0100));
  REQUIRE(items[0].elements()[0].vr() == VR::SH);
  REQUIRE(items[0].elements()[0].vr_provenance() == VRProvenance::Dictionary);
  REQUIRE(items[0].elements()[0].value().as_string() == "12345");

  // The nested attribute is addressable through the ordinary structural
  // API, not just reachable via sequence()/items() -- the whole point of
  // "recursively parsed," not merely "recognized."
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0008, 0x0100));
  REQUIRE(result.structure->find(path) != nullptr);
  REQUIRE(result.structure->find(path)->value().as_string() == "12345");
}

TEST_CASE("nested undefined-length sequences resolve through ElementPath", "[parser][implicit]") {
  FixtureBuilder inner_content;
  inner_content.element_implicit_ascii(0x300A, 0x0072, "6.0");
  auto inner_item = wrap_in_undefined_item(inner_content);

  FixtureBuilder outer_content;
  outer_content.element_implicit_header(0x300A, 0x00B6, fds::ValueLength::kUndefinedMarker);
  outer_content.append(inner_item);
  outer_content.sequence_delimiter();
  auto outer_item = wrap_in_undefined_item(outer_content);

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x300A, 0x00B0, fds::ValueLength::kUndefinedMarker);
  b.append(outer_item);
  b.sequence_delimiter();

  auto result = parse_implicit(b, Fidelity::Lossless);
  REQUIRE(result.status != ParseStatus::Failed);

  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x300A, 0x0072));
  const auto* e = result.structure->find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->value().as_string() == "6.0");
}

TEST_CASE("native Pixel Data under Implicit VR is referenced, not expanded as an element",
          "[parser][implicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_ascii(0x0010, 0x0020, "ID1");
  std::vector<std::byte> pixels(16, std::byte{0x2A});
  b.element_implicit(0x7FE0, 0x0010, pixels);

  auto result = parse_implicit(b, Fidelity::Lossless);
  REQUIRE(result.status != ParseStatus::Failed);
  REQUIRE_FALSE(result.structure->contains(Tag(0x7FE0, 0x0010)));
  const auto* pixel_data = result.structure->pixel_data();
  REQUIRE(pixel_data != nullptr);
  REQUIRE_FALSE(pixel_data->is_encapsulated());
  REQUIRE(pixel_data->vr() == VR::OB);
}

TEST_CASE("encapsulated Pixel Data under Implicit VR parses Basic Offset Table and fragments",
          "[parser][implicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x7FE0, 0x0010, fds::ValueLength::kUndefinedMarker);
  b.item_defined_header(0);  // empty Basic Offset Table
  std::vector<std::byte> fragment(10, std::byte{0x11});
  b.item_defined_header(static_cast<std::uint32_t>(fragment.size()));
  b.raw_bytes(fragment);
  b.sequence_delimiter();

  auto result = parse_implicit(b, Fidelity::Lossless);
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* pixel_data = result.structure->pixel_data();
  REQUIRE(pixel_data != nullptr);
  REQUIRE(pixel_data->is_encapsulated());
  REQUIRE(pixel_data->fragments().size() == 1);
}

TEST_CASE("a truncated Implicit VR element header is a recoverable error", "[parser][implicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_ascii(0x0010, 0x0020, "ID1");
  b.raw_u16(0x0008);  // half a tag, then nothing

  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  REQUIRE(result.structure->contains(Tag(0x0010, 0x0020)));
  bool found_recoverable = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == fds::DiagnosticSeverity::RecoverableError) found_recoverable = true;
  }
  REQUIRE(found_recoverable);
}

TEST_CASE("write() is Unsupported for an unmodified Implicit VR structure", "[parser][implicit]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_ascii(0x0010, 0x0020, "ID1");
  auto result = parse_implicit(b, Fidelity::Lossless);

  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Unsupported);
}

TEST_CASE("a mutated Implicit VR structure writes as valid Explicit VR output",
          "[parser][implicit][mutation]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_ascii(0x0010, 0x0020, "ID1 ");  // space-padded to even length
  b.element_implicit_ascii(0x0008, 0x0060, "CT");
  auto result = parse_implicit(b, Fidelity::Lossless);

  REQUIRE(result.structure->set_value(ElementPath(Tag(0x0010, 0x0020)),
                                       Value::from_string("ANON001 ")));
  REQUIRE(result.structure->erase(Tag(0x0008, 0x0060)));

  std::ostringstream out;
  auto write_result = result.structure->write(out);
  REQUIRE(write_result.status == WriteStatus::Success);

  std::string written = out.str();
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(written)));
  REQUIRE(reparsed.status != ParseStatus::Failed);
  // The writer always emits Explicit VR -- the output is a different
  // Transfer Syntax from the Implicit VR input, by design (see
  // docs/roundtrip-contract.md "Implicit VR Little Endian"). (0002,0010)
  // must be rewritten to say so, or the file would declare Implicit VR
  // while actually being encoded as Explicit VR.
  REQUIRE(reparsed.structure->transfer_syntax().explicit_vr());
  REQUIRE(reparsed.structure->transfer_syntax().uid() == "1.2.840.10008.1.2.1");
  REQUIRE(reparsed.structure->find(Tag(0x0010, 0x0020))->value().as_string() == "ANON001");
  REQUIRE_FALSE(reparsed.structure->contains(Tag(0x0008, 0x0060)));
}

TEST_CASE("exceeding max_sequence_depth stops cleanly under Implicit VR", "[parser][implicit]") {
  FixtureBuilder innermost;
  innermost.element_implicit_ascii(0x0008, 0x0100, "AB");

  FixtureBuilder level = innermost;
  for (int i = 0; i < 5; ++i) {
    FixtureBuilder wrapped;
    wrapped.element_implicit_header(0x300A, 0x00B6, fds::ValueLength::kUndefinedMarker);
    wrapped.append(wrap_in_undefined_item(level));
    wrapped.sequence_delimiter();
    level = wrapped;
  }

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.append(level);

  ParseOptions options;
  options.fidelity = Fidelity::Lossless;
  options.max_sequence_depth = 3;
  auto result = fds::parse_buffer(b.bytes(), options);
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  bool found_recoverable = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == fds::DiagnosticSeverity::RecoverableError) found_recoverable = true;
  }
  REQUIRE(found_recoverable);
}

// A truncated Pixel Data element must never be silently accepted as a clean
// Success with Pixel Data just absent -- every failure path in
// parse_pixel_data() must diagnose. Mirrors the Explicit VR coverage in
// tests/integration/test_pixel_data.cpp; see that file's comment for the
// external-review context.
namespace {
void require_truncated_pixel_data_is_recoverable(const FixtureBuilder& b) {
  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  bool found_recoverable = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == fds::DiagnosticSeverity::RecoverableError) found_recoverable = true;
  }
  REQUIRE(found_recoverable);
  REQUIRE(result.structure != nullptr);
  REQUIRE(result.structure->pixel_data() == nullptr);
  REQUIRE_FALSE(result.structure->contains(Tag(0x7FE0, 0x0010)));
}
}  // namespace

TEST_CASE("Implicit VR Pixel Data truncated right after its tag (missing length) is recoverable",
          "[parser][implicit][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.tag(0x7FE0, 0x0010);
  // nothing else
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Implicit VR Pixel Data with a BOT item tag but no length is recoverable",
          "[parser][implicit][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x7FE0, 0x0010, fds::ValueLength::kUndefinedMarker);
  b.tag(0xFFFE, 0xE000);  // Basic Offset Table item tag, then nothing
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Implicit VR Pixel Data whose BOT runs past the source is recoverable",
          "[parser][implicit][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x7FE0, 0x0010, fds::ValueLength::kUndefinedMarker);
  b.tag(0xFFFE, 0xE000);
  b.raw_u32(100);  // declares 100 bytes; none follow
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Implicit VR Pixel Data with only a partial fragment tag is recoverable",
          "[parser][implicit][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x7FE0, 0x0010, fds::ValueLength::kUndefinedMarker);
  b.item_defined_header(0);  // empty Basic Offset Table
  b.raw_u16(0xFFFE);         // half a tag: 2 bytes, not the full 4
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Implicit VR Pixel Data with a fragment tag but no length is recoverable",
          "[parser][implicit][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x7FE0, 0x0010, fds::ValueLength::kUndefinedMarker);
  b.item_defined_header(0);  // empty Basic Offset Table
  b.tag(0xFFFE, 0xE000);     // fragment item tag, then nothing
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("Encapsulated Implicit VR Pixel Data whose fragment runs past the source is recoverable",
          "[parser][implicit][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x7FE0, 0x0010, fds::ValueLength::kUndefinedMarker);
  b.item_defined_header(0);  // empty Basic Offset Table
  b.tag(0xFFFE, 0xE000);
  b.raw_u32(50);  // declares 50 bytes; none follow
  require_truncated_pixel_data_is_recoverable(b);
}

TEST_CASE("An Implicit VR element that overruns its containing Item's declared length is rejected",
          "[parser][implicit][malformed]") {
  // The Item declares only 4 bytes of content, but its one child element
  // actually needs 16 bytes (tag 4 + length 4 + value 8) to parse. A
  // defined-length outer element is opaque under Implicit VR (see the
  // parser's own documented heuristic), so the outer container here must
  // be an undefined-length Sequence to actually exercise parse_item's
  // defined-length boundary check.
  FixtureBuilder item;
  item.item_defined_header(4);
  item.element_implicit_ascii(0x0008, 0x0100, "ABCDEFGH");

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x0008, 0x1140, fds::ValueLength::kUndefinedMarker);
  b.append(item);
  b.sequence_delimiter();

  auto result = parse_implicit(b);
  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  bool found_recoverable = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == fds::DiagnosticSeverity::RecoverableError) found_recoverable = true;
  }
  REQUIRE(found_recoverable);
  REQUIRE_FALSE(result.structure->contains(Tag(0x0008, 0x1140)));
}

TEST_CASE("max_element_count bounds nested Implicit VR elements, not just the top level",
          "[parser][implicit][malformed]") {
  // One top-level undefined-length Sequence containing 10 items, each with
  // one element: 11 "real" elements total if nested elements are counted,
  // but only 1 (the Sequence itself) if only the top-level element list is
  // counted -- the bypass this guards against. The budget also continues
  // across the Explicit-VR-File-Meta-to-Implicit-VR-dataset handoff, which
  // this test exercises implicitly by using make_file_meta at all.
  FixtureBuilder content;
  for (int i = 0; i < 10; ++i) {
    FixtureBuilder item_content;
    item_content.element_implicit_ascii(0x0008, 0x0100, "AB");
    content.append(wrap_in_undefined_item(item_content));
  }
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_header(0x0008, 0x1140, fds::ValueLength::kUndefinedMarker);
  b.append(content);
  b.sequence_delimiter();

  ParseOptions options;
  options.max_element_count = 15;
  auto result = fds::parse_buffer(b.bytes(), options);

  REQUIRE(result.status == ParseStatus::SuccessWithWarnings);
  bool found_recoverable = false;
  for (const auto& d : result.diagnostics) {
    if (d.severity == fds::DiagnosticSeverity::RecoverableError) found_recoverable = true;
  }
  REQUIRE(found_recoverable);
  REQUIRE_FALSE(result.structure->contains(Tag(0x0008, 0x1140)));
}

// --- Bare datasets (A1.4: ParseOptions::bare_dataset_is_implicit_vr) ---

TEST_CASE("a bare dataset defaults to Explicit VR LE when the Implicit-VR hint is not set "
          "(unchanged default)",
          "[parser][implicit][bare_dataset]") {
  FixtureBuilder b;  // no preamble, no File Meta at all
  b.tag(0x0010, 0x0020);
  b.ascii("LO");
  b.raw_u16(4);
  b.ascii("ID1 ");

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  bool saw_explicit_default = false;
  for (const auto& d : result.diagnostics) {
    if (d.message.find("assuming Explicit VR Little Endian") != std::string::npos) {
      saw_explicit_default = true;
    }
  }
  REQUIRE(saw_explicit_default);
}

TEST_CASE("a bare dataset parses as Implicit VR LE when the hint is set", "[parser][implicit][bare_dataset]") {
  FixtureBuilder b;  // no preamble, no File Meta at all
  b.element_implicit_ascii(0x0010, 0x0020, "ID1");  // PatientID -- dictionary VR LO

  ParseOptions options;
  options.fidelity = Fidelity::Standard;
  options.bare_dataset_is_implicit_vr = true;
  auto result = fds::parse_buffer(b.bytes(), options);

  REQUIRE(result.status != ParseStatus::Failed);
  REQUIRE(result.structure->transfer_syntax().kind() == fds::TransferSyntaxKind::ImplicitVRLittleEndian);
  const auto* e = result.structure->find(Tag(0x0010, 0x0020));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::LO);
  REQUIRE(e->vr_provenance() == VRProvenance::Dictionary);
  REQUIRE(e->value().as_string() == "ID1");
}

TEST_CASE("the bare-dataset Implicit-VR hint never overrides an actual Transfer Syntax UID",
          "[parser][implicit][bare_dataset]") {
  // File Meta present and explicit about Explicit VR LE; the hint must be
  // irrelevant here -- it only ever applies when no (0002,0010) is found.
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0020, "LO", "ID1 ");

  ParseOptions options;
  options.bare_dataset_is_implicit_vr = true;
  auto result = fds::parse_buffer(b.bytes(), options);
  REQUIRE(result.status == ParseStatus::Success);
  REQUIRE(result.structure->transfer_syntax().kind() == fds::TransferSyntaxKind::ExplicitVRLittleEndian);
}

// --- Ambiguous-VR context resolution (A1.4) ---

TEST_CASE("a US-or-SS ambiguous tag resolves to US when Pixel Representation is 0",
          "[parser][implicit][ambiguous_vr]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit(0x0028, 0x0103, {std::byte{0}, std::byte{0}});  // PixelRepresentation = 0
  b.element_implicit(0x0028, 0x0106, {std::byte{0x2A}, std::byte{0x00}});  // SmallestImagePixelValue

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0028, 0x0106));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::US);
  REQUIRE(e->vr_provenance() == VRProvenance::ContextResolved);
  REQUIRE(e->length_form() == fds::LengthForm::Short16);
  REQUIRE(e->value().as_uint16() == 0x2A);
}

TEST_CASE("a US-or-SS ambiguous tag resolves to SS when Pixel Representation is 1",
          "[parser][implicit][ambiguous_vr]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit(0x0028, 0x0103, {std::byte{1}, std::byte{0}});  // PixelRepresentation = 1
  b.element_implicit(0x0028, 0x0106, {std::byte{0xFF}, std::byte{0xFF}});  // -1 as SS

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0028, 0x0106));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::SS);
  REQUIRE(e->vr_provenance() == VRProvenance::ContextResolved);
  REQUIRE(e->value().as_int16() == -1);
}

TEST_CASE("a US-or-SS ambiguous tag stays Unknown, never guessed, when Pixel Representation "
          "is absent",
          "[parser][implicit][ambiguous_vr]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit(0x0028, 0x0106, {std::byte{0x2A}, std::byte{0x00}});  // no Pixel Representation anywhere

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0028, 0x0106));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::Unknown);
  REQUIRE(e->vr_provenance() == VRProvenance::Unknown);
  // Raw bytes are still fully preserved even though the VR is unresolved.
  auto bytes = e->value().bytes();
  REQUIRE(bytes.size() == 2);
}

TEST_CASE("US-or-SS resolution does not depend on wire order: Pixel Representation appearing "
          "after the ambiguous element still resolves it",
          "[parser][implicit][ambiguous_vr]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit(0x0028, 0x0106, {std::byte{0x2A}, std::byte{0x00}});  // ambiguous, comes first
  b.element_implicit(0x0028, 0x0103, {std::byte{1}, std::byte{0}});        // Pixel Representation, comes after

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0028, 0x0106));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::SS);  // Pixel Representation = 1, resolved despite appearing later
  REQUIRE(e->vr_provenance() == VRProvenance::ContextResolved);
}

TEST_CASE("a nested US-or-SS ambiguous element resolves via the TOP-LEVEL Pixel "
          "Representation, not a local one",
          "[parser][implicit][ambiguous_vr]") {
  FixtureBuilder item_content;
  item_content.element_implicit(0x0028, 0x0106, {std::byte{0x10}, std::byte{0x00}});
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit(0x0028, 0x0103, {std::byte{1}, std::byte{0}});  // top-level Pixel Representation = 1
  b.element_implicit_header(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0028, 0x0106));
  const auto* e = result.structure->find(path);
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::SS);
  REQUIRE(e->vr_provenance() == VRProvenance::ContextResolved);
}

TEST_CASE("an out-of-spec Pixel Representation value leaves US-or-SS ambiguous tags "
          "unresolved rather than guessed",
          "[parser][implicit][ambiguous_vr]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit(0x0028, 0x0103, {std::byte{7}, std::byte{0}});  // PS3.5 only defines 0/1
  b.element_implicit(0x0028, 0x0106, {std::byte{0x2A}, std::byte{0x00}});

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x0028, 0x0106));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::Unknown);
  REQUIRE(e->vr_provenance() == VRProvenance::Unknown);
}

TEST_CASE("ambiguous forms other than US-or-SS are not resolved in V1 and stay Unknown",
          "[parser][implicit][ambiguous_vr]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit(0x0028, 0x0103, {std::byte{0}, std::byte{0}});  // Pixel Representation present
  // (5400,1010) WaveformData -- OB-or-OW ambiguous, deliberately not attempted in V1.
  b.element_implicit(0x5400, 0x1010, std::vector<std::byte>(4, std::byte{0x11}));

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x5400, 0x1010));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::Unknown);
  REQUIRE(e->vr_provenance() == VRProvenance::Unknown);
}

// --- A1.1's 17 excluded element-level wildcard patterns (section 10) ---

TEST_CASE("a tag matching one of A1.1's excluded element-level wildcard patterns parses as "
          "VR::Unknown, never fabricated",
          "[parser][implicit][a1_4]") {
  auto b = make_file_meta("1.2.840.10008.1.2");
  b.element_implicit_ascii(0x1010, 0x1234, "X");  // matches excluded pattern "1010,xxxx"

  auto result = fds::parse_buffer(b.bytes());
  REQUIRE(result.status != ParseStatus::Failed);
  const auto* e = result.structure->find(Tag(0x1010, 0x1234));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::Unknown);
  REQUIRE(e->vr_provenance() == VRProvenance::Unknown);
}
