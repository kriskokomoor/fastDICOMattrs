#include <catch2/catch_test_macros.hpp>

#include <span>
#include <sstream>

#include "fastdicomattrs/charset.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// A1.7 -- freeze-critical proof (see docs/architecture/
// A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md section 8) that
// charset::insert_text() resolves the Specific Character Set context for
// the CONTAINER a new element is about to be inserted into -- not merely
// "compiles by reusing resolve_character_set_context() unchanged."
//
// resolve_character_set_context() was built for an *element-locator* path
// (last step bare, naming an already-existing leaf) and its internal walk
// stops -- deliberately, correctly, for that use -- the moment it reaches
// the last step, without descending into it. A *container-locator* `parent`
// (every step a descent step, naming where a new element will live) needs
// exactly the opposite: the walk must descend through every step,
// including the last, to see that container's own local (0008,0005)
// override. A naive reuse of resolve_character_set_context(structure,
// parent) would silently stop one level too early and fall back to
// whatever ancestor scope was found first -- wrong, but not a compile
// error and not always wrong-looking (it only misbehaves when the target
// container itself declares a local override, which is exactly what test
// 3/4/5 below are built to detect: each uses a character representable
// ONLY under the correct (nearer) declaration, so a context resolved from
// the wrong scope fails outright (UnrepresentableCharacter) rather than
// merely producing different-but-plausible bytes.

using namespace fds_test;
using fds::DICOMStructure;
using fds::ElementPath;
using fds::ParseResult;
using fds::ParseStatus;
using fds::Tag;
using fds::VR;
using fds::WriteStatus;
namespace cs = fds::charset;

namespace {
ParseResult parse(const FixtureBuilder& b) { return fds::parse_buffer(b.bytes()); }

FixtureBuilder& specific_character_set(FixtureBuilder& b, const std::vector<std::string>& terms) {
  std::string joined;
  for (std::size_t i = 0; i < terms.size(); ++i) {
    if (i > 0) joined += '\\';
    joined += terms[i];
  }
  b.element_short(0x0008, 0x0005, "CS", joined);
  return b;
}
}  // namespace

TEST_CASE("insert_text: root declaration governs a root insertion", "[charset_encode][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  // 'é' (Latin1) is representable under the root's own declaration.
  auto status = cs::insert_text(structure, ElementPath(), Tag(0x0010, 0x0010), VR::PN,
                                 {"A\xC3\xA9" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  auto ctx = cs::resolve_character_set_context(*reparsed.structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*reparsed.structure->find(Tag(0x0010, 0x0010)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "A\xC3\xA9" "B");
}

TEST_CASE("insert_text: a nested Item with no local declaration inherits the root's",
          "[charset_encode][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0060, "CS", "CT");  // no (0008,0005) here
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  auto status = cs::insert_text(structure, parent, Tag(0x0010, 0x0010), VR::PN, {"A\xC3\xA9" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);
}

TEST_CASE("insert_text: a nested Item's own local declaration overrides the root",
          "[charset_encode][insert]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 144");  // local override: Cyrillic
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  // 'р' (Cyrillic, U+0440) is representable ONLY under this Item's own
  // override, never under the root's Latin1 -- a context resolved from
  // the wrong (root) scope would fail this with UnrepresentableCharacter.
  auto status = cs::insert_text(structure, parent, Tag(0x0010, 0x0010), VR::PN, {"A\xD1\x80" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0010, 0x0010));
  auto ctx = cs::resolve_character_set_context(*reparsed.structure, path);
  auto decoded = cs::decode_text(*reparsed.structure->find(path), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "A\xD1\x80" "B");
}

TEST_CASE("insert_text: a deeper child Item with no local declaration inherits the nearest "
          "enclosing declaration, not the root",
          "[charset_encode][insert]") {
  // root: Latin1
  //   SeqA Item 0: no local declaration
  //     SeqB Item 0: local declaration -- Cyrillic
  //       SeqC Item 0: no local declaration  <-- insertion target
  // The target must inherit Cyrillic (nearest enclosing, two levels up),
  // not Latin1 (the root, three levels up).
  FixtureBuilder seqC_item_content;
  seqC_item_content.element_short(0x0008, 0x0060, "CS", "CT");
  auto seqC_item = wrap_in_item(seqC_item_content);

  FixtureBuilder seqB_item_content;
  seqB_item_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 144");  // Cyrillic
  seqB_item_content.sequence_defined(0x300A, 0x00C7, static_cast<std::uint32_t>(seqC_item.size()));
  seqB_item_content.append(seqC_item);
  auto seqB_item = wrap_in_item(seqB_item_content);

  FixtureBuilder seqA_item_content;
  seqA_item_content.sequence_defined(0x300A, 0x00B6, static_cast<std::uint32_t>(seqB_item.size()));
  seqA_item_content.append(seqB_item);
  auto seqA_item = wrap_in_item(seqA_item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(seqA_item.size()));
  b.append(seqA_item);
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  ElementPath parent;
  parent.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x300A, 0x00C7), 0);
  auto status = cs::insert_text(structure, parent, Tag(0x0010, 0x0010), VR::PN, {"A\xD1\x80" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);
}

TEST_CASE("insert_text: sibling Items' local declarations do not leak into each other",
          "[charset_encode][insert]") {
  FixtureBuilder item0_content;
  item0_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 100");  // Item 0: Latin1
  auto item0 = wrap_in_item(item0_content);
  FixtureBuilder item1_content;
  item1_content.element_short(0x0008, 0x0060, "CS", "CT");  // Item 1: no override -- inherits root
  auto item1 = wrap_in_item(item1_content);
  FixtureBuilder items;
  items.append(item0).append(item1);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 144"});  // root: Cyrillic
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(items.size()));
  b.append(items);
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  // Item 0 declares Latin1 locally -- inserting a Latin1-only character
  // there must succeed.
  ElementPath parent0;
  parent0.push(Tag(0x0008, 0x1140), 0);
  REQUIRE(cs::insert_text(structure, parent0, Tag(0x0010, 0x0010), VR::PN, {"A\xC3\xA9" "B"}) ==
          cs::SetTextStatus::Success);

  // Item 1 has no local override -- inherits the root's Cyrillic, so the
  // very same Latin1-only character must fail here even though its
  // sibling (Item 0) just accepted it -- Item 0's declaration must not
  // leak into Item 1.
  ElementPath parent1;
  parent1.push(Tag(0x0008, 0x1140), 1);
  REQUIRE(cs::insert_text(structure, parent1, Tag(0x0010, 0x0010), VR::PN, {"A\xC3\xA9" "B"}) ==
          cs::SetTextStatus::UnrepresentableCharacter);
  // The failed attempt must not have inserted anything into Item 1.
  const auto* seq = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq->sequence().items()[1].elements().size() == 1);  // just (0008,0060)
}

TEST_CASE("insert_text: an unsupported nested charset declaration fails atomically",
          "[charset_encode][insert]") {
  FixtureBuilder item_content;
  // ISO 2022 IR 87 (JIS X 0208, Japanese) -- a real DICOM-defined term this
  // library recognizes but deliberately does not support decoding/encoding
  // (out of the V1 single-byte/UTF-8 envelope).
  item_content.element_short(0x0008, 0x0005, "CS", "ISO 2022 IR 87");
  item_content.element_short(0x0008, 0x0060, "CS", "CT");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);
  auto status = cs::insert_text(structure, parent, Tag(0x0010, 0x0010), VR::PN, {"AB"});
  REQUIRE(status == cs::SetTextStatus::UnsupportedCharset);

  // Nothing changed: no partial insertion, modified flag untouched.
  REQUIRE_FALSE(structure.is_modified());
  const auto* seq = structure.find(Tag(0x0008, 0x1140));
  REQUIRE(seq->sequence().items()[0].elements().size() == 2);  // (0008,0005) + (0008,0060) only
}

// --- Additional failure-class atomicity coverage (section 9) ---------------

TEST_CASE("insert_text rejects a duplicate tag atomically", "[charset_encode][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.element_short(0x0010, 0x0010, "PN", "X");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  auto status = cs::insert_text(structure, ElementPath(), Tag(0x0010, 0x0010), VR::PN, {"AB"});
  REQUIRE(status == cs::SetTextStatus::AlreadyExists);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_text rejects a nonexistent container atomically", "[charset_encode][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  ElementPath parent;
  parent.push(Tag(0x0008, 0x1140), 0);  // doesn't exist
  auto status = cs::insert_text(structure, parent, Tag(0x0010, 0x0010), VR::PN, {"AB"});
  REQUIRE(status == cs::SetTextStatus::ContainerNotFound);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_text rejects malformed UTF-8 input atomically", "[charset_encode][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  std::string invalid_utf8 = "A\xFF" "B";
  auto status = cs::insert_text(structure, ElementPath(), Tag(0x0010, 0x0010), VR::PN, {invalid_utf8});
  REQUIRE(status == cs::SetTextStatus::InvalidUnicodeInput);
  REQUIRE_FALSE(structure.contains(Tag(0x0010, 0x0010)));
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_text_inferred requires an explicit VR when inference is ambiguous",
          "[charset_encode][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  // (0028,0106) SmallestImagePixelValue -- "US or SS", ambiguous, and not a
  // text VR in any resolution anyway; VRRequired must win before any
  // charset work is attempted.
  auto status = cs::insert_text_inferred(structure, ElementPath(), Tag(0x0028, 0x0106), {"AB"});
  REQUIRE(status == cs::SetTextStatus::VRRequired);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("insert_text_inferred infers a real text VR and encodes under the resolved charset",
          "[charset_encode][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // Latin1
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  // (0010,0010) PatientName -- unambiguous PN.
  auto status =
      cs::insert_text_inferred(structure, ElementPath(), Tag(0x0010, 0x0010), {"A\xC3\xA9" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);
  const auto* e = structure.find(Tag(0x0010, 0x0010));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::PN);
}

TEST_CASE("insert_text_inferred returns NotATextVR when inference yields a non-text VR",
          "[charset_encode][insert]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  // (0008,0060) Modality -- unambiguous CS, not Specific-Character-Set-governed.
  auto status = cs::insert_text_inferred(structure, ElementPath(), Tag(0x0008, 0x0060), {"CT"});
  REQUIRE(status == cs::SetTextStatus::NotATextVR);
  REQUIRE_FALSE(structure.contains(Tag(0x0008, 0x0060)));
}
