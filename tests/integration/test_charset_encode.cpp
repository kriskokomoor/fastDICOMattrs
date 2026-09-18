#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "fastdicomattrs/charset.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// A1.6 -- charset-aware text encoding/mutation. See docs/architecture/
// A1_6_CHARACTER_SET_REENCODING_REPORT.md.
//
// Per-repertoire Unicode test characters mirror test_charset.cpp's (A1.5)
// choices exactly, so decode/encode symmetry is exercised against the same
// known-correct data points.

using namespace fds_test;
using fds::DICOMStructure;
using fds::Element;
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

// Parses `b`, calls `set_text` at `path`, and returns the reparsed
// structure after write() -- or nullopt if write failed (e.g. mutation
// itself failed and nothing changed, so there's nothing new to reparse).
struct MutateResult {
  cs::SetTextStatus status;
  std::unique_ptr<DICOMStructure> before;   // the mutated (or attempted) structure itself
  fds::ParseResult after;                   // write()+reparse of `before`, if write succeeded
  bool wrote = false;
};

MutateResult mutate_and_reparse(const FixtureBuilder& b, const Tag& target_tag,
                                 const std::vector<std::string>& values) {
  MutateResult result;
  auto parsed = parse(b);
  REQUIRE(parsed.status == ParseStatus::Success);
  result.before = std::move(parsed.structure);
  result.status = cs::set_text(*result.before, ElementPath(target_tag), values);
  if (result.status == cs::SetTextStatus::Success) {
    std::ostringstream out;
    if (result.before->write(out).status == WriteStatus::Success) {
      result.wrote = true;
      result.after = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
    }
  }
  return result;
}
}  // namespace

// --- Default repertoire ------------------------------------------------

TEST_CASE("default repertoire: ASCII Unicode input encodes successfully", "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0010, "PN", "OldName");
  auto r = mutate_and_reparse(b, Tag(0x0010, 0x0010), {"Smith^John"});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  REQUIRE(r.wrote);
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0010, 0x0010)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "Smith^John");
}

TEST_CASE("default repertoire: non-ASCII Unicode input fails explicitly, structure unchanged",
          "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0010, "PN", "OldName");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto raw_before = std::vector<std::byte>(structure.find(Tag(0x0010, 0x0010))->value().bytes().begin(),
                                            structure.find(Tag(0x0010, 0x0010))->value().bytes().end());

  auto status = cs::set_text(structure, ElementPath(Tag(0x0010, 0x0010)), {"Riesmeier^J\xC3\xB6rg"});
  REQUIRE(status == cs::SetTextStatus::UnrepresentableCharacter);
  REQUIRE_FALSE(structure.is_modified());
  auto raw_after = std::vector<std::byte>(structure.find(Tag(0x0010, 0x0010))->value().bytes().begin(),
                                           structure.find(Tag(0x0010, 0x0010))->value().bytes().end());
  REQUIRE(raw_before == raw_after);
}

// --- Ten single-byte repertoires: Unicode -> encode -> A1.5 decode == original ---

TEST_CASE("every supported single-byte repertoire round-trips a real non-ASCII character",
          "[charset_encode][single_byte]") {
  struct Case {
    const char* term;
    const char* text;  // UTF-8 input, matching test_charset.cpp's A1.5 fixtures
  };
  const Case cases[] = {
      {"ISO_IR 100", "A\xC3\xA9""B"},      // Latin1 'é'
      {"ISO_IR 101", "A\xC4\x8D""B"},      // Latin2 'č'
      {"ISO_IR 109", "A\xC3\xA0""B"},      // Latin3 'à'
      {"ISO_IR 110", "A\xC3\xA6""B"},      // Latin4 'æ'
      {"ISO_IR 144", "A\xD1\x80""B"},      // Cyrillic 'р'
      {"ISO_IR 127", "A\xD9\x80""B"},      // Arabic tatweel
      {"ISO_IR 126", "A\xCE\xB1""B"},      // Greek 'α'
      {"ISO_IR 138", "A\xD7\x90""B"},      // Hebrew 'א'
      {"ISO_IR 148", "A\xC4\xB1""B"},      // Turkish dotless i
      {"ISO_IR 166", "A\xE0\xB8\x81" "B"},  // Thai 'ก'
  };
  for (const auto& c : cases) {
    auto b = make_file_meta("1.2.840.10008.1.2.1");
    specific_character_set(b, {c.term});
    b.element_short(0x0010, 0x0010, "PN", "X");
    auto r = mutate_and_reparse(b, Tag(0x0010, 0x0010), {c.text});
    INFO("term: " << c.term);
    REQUIRE(r.status == cs::SetTextStatus::Success);
    REQUIRE(r.wrote);
    auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0010, 0x0010)));
    auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0010, 0x0010)), ctx);
    REQUIRE(decoded.status == cs::DecodeStatus::Success);
    REQUIRE(decoded.values[0] == c.text);
  }
}

TEST_CASE("a character unrepresentable in the declared single-byte repertoire fails, not "
          "transliterated",
          "[charset_encode][single_byte]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // Latin1 only
  b.element_short(0x0010, 0x0010, "PN", "X");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  // Cyrillic 'р' (U+0440) is not representable in Latin1.
  auto status = cs::set_text(structure, ElementPath(Tag(0x0010, 0x0010)), {"A\xD1\x80" "B"});
  REQUIRE(status == cs::SetTextStatus::UnrepresentableCharacter);
  REQUIRE_FALSE(structure.is_modified());
}

// --- UTF-8 ---------------------------------------------------------------

TEST_CASE("UTF-8: ASCII, 2-byte, 3-byte, and 4-byte scalars all encode and round-trip",
          "[charset_encode][utf8]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 192"});
  b.element_short(0x0008, 0x0080, "LO", "X");
  std::string text = "AB\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";  // A B é 中 😀
  auto r = mutate_and_reparse(b, Tag(0x0008, 0x0080), {text});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0008, 0x0080)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == text);
}

TEST_CASE("invalid UTF-8 input is rejected, never partially encoded", "[charset_encode][utf8]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 192"});
  b.element_short(0x0008, 0x0080, "LO", "X");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  std::string truncated = "AB\xE4\xB8";  // truncated 3-byte sequence
  auto status = cs::set_text(structure, ElementPath(Tag(0x0008, 0x0080)), {truncated});
  REQUIRE(status == cs::SetTextStatus::InvalidUnicodeInput);
  REQUIRE_FALSE(structure.is_modified());
}

// --- ISO 2022 switching, undeclared repertoire, determinism ---------------

TEST_CASE("ISO 2022: multi-repertoire text encodes with correct escape sequences and "
          "decodes back to the original",
          "[charset_encode][iso2022]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100", "ISO 2022 IR 144"});
  b.element_short(0x0010, 0x0010, "PN", "X");
  std::string text = "AB\xC3\xA9\xD1\x80";  // "AB" + Latin1 'é' + Cyrillic 'р'
  auto r = mutate_and_reparse(b, Tag(0x0010, 0x0010), {text});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0010, 0x0010)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == text);

  // Confirm the wire bytes actually contain the expected escape sequences
  // (not just that decode happens to recover the text some other way).
  const auto* e = r.after.structure->find(Tag(0x0010, 0x0010));
  auto raw = e->value().bytes();
  std::string raw_str(reinterpret_cast<const char*>(raw.data()), raw.size());
  REQUIRE(raw_str.find("\x1B-A") != std::string::npos);
  REQUIRE(raw_str.find("\x1B-L") != std::string::npos);
}

TEST_CASE("ISO 2022: a code point requiring an undeclared repertoire fails, no switch emitted",
          "[charset_encode][iso2022]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100"});  // only Latin1 declared
  b.element_short(0x0008, 0x0080, "LO", "X");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto status = cs::set_text(structure, ElementPath(Tag(0x0008, 0x0080)), {"A\xD1\x80" "B"});  // needs Cyrillic
  REQUIRE(status == cs::SetTextStatus::UnrepresentableCharacter);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("ISO 2022: repeated encoding of the same input/context is byte-identical",
          "[charset_encode][iso2022]") {
  auto context_terms = std::vector<std::string>{"", "ISO 2022 IR 100", "ISO 2022 IR 144"};
  cs::CharacterSetContext context;
  context.mode = cs::CharacterSetMode::MultiValue;
  for (auto& t : context_terms) {
    cs::RepertoireTerm term;
    term.defined_term = t;
    term.support = t.empty() ? cs::RepertoireSupport::DefaultRepertoire
                              : cs::RepertoireSupport::SingleByteSupported;
    context.terms.push_back(term);
  }
  std::string text = "AB\xC3\xA9\xD1\x80""CD\xC3\xA9";
  auto e1 = cs::encode_text({text}, VR::PN, context);
  auto e2 = cs::encode_text({text}, VR::PN, context);
  REQUIRE(e1.status == cs::EncodeStatus::Success);
  REQUIRE(e2.status == cs::EncodeStatus::Success);
  REQUIRE(*e1.bytes == *e2.bytes);
}

TEST_CASE("ISO 2022: deterministic selection prefers the currently-active repertoire, "
          "avoiding a gratuitous escape",
          "[charset_encode][iso2022]") {
  // 'à' (U+00E0) is representable in BOTH Latin1 and Latin3 (both declared,
  // Latin1 first). After switching to Latin3 for a Latin3-only character,
  // encoding 'à' next must stay in Latin3 (no escape back to Latin1),
  // because Latin3 already represents it and the policy prefers staying
  // active. Latin3-only test character: 0xA1 in ISO 8859-3 is 'Ħ' (U+0126),
  // not in Latin1.
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100", "ISO 2022 IR 109"});
  b.element_short(0x0008, 0x0080, "LO", "X");
  // Ħ (Latin3-only, U+0126, utf8 C4A6) then à (U+00E0, in both Latin1 and Latin3).
  std::string text = "\xC4\xA6\xC3\xA0";
  auto r = mutate_and_reparse(b, Tag(0x0008, 0x0080), {text});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  const auto* e = r.after.structure->find(Tag(0x0008, 0x0080));
  auto raw = e->value().bytes();
  std::string raw_str(reinterpret_cast<const char*>(raw.data()), raw.size());
  // Exactly one escape sequence (into Latin3) -- no second escape back to
  // Latin1 for 'à', since Latin3 (the now-active repertoire) already
  // represents it.
  std::size_t esc_count = 0;
  for (std::size_t i = 0; i + 2 < raw_str.size(); ++i) {
    if (raw_str[i] == '\x1B') ++esc_count;
  }
  REQUIRE(esc_count == 1);
  REQUIRE(raw_str.find("\x1B-C") != std::string::npos);  // Latin3's designator
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*e, ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == text);
}

// --- PN ---------------------------------------------------------------

TEST_CASE("PN: component and component-group delimiters round-trip through encode+decode",
          "[charset_encode][pn]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.element_short(0x0010, 0x0010, "PN", "X");
  std::string text = "Riesmeier^J\xC3\xB6rg";  // real name from the A1.5 real corpus
  auto r = mutate_and_reparse(b, Tag(0x0010, 0x0010), {text});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0010, 0x0010)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == text);
}

TEST_CASE("PN: ideographic/phonetic component groups separated by '=' round-trip",
          "[charset_encode][pn]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100"});
  b.element_short(0x0010, 0x0010, "PN", "X");
  std::string text = "Yamada^Tarou=\xC3\xA9";  // ascii group + escaped Latin1 group
  auto r = mutate_and_reparse(b, Tag(0x0010, 0x0010), {text});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0010, 0x0010)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == text);
}

TEST_CASE("PN: multiple values separated by backslash round-trip independently", "[charset_encode][pn]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x1001, "PN", "X");
  auto r = mutate_and_reparse(b, Tag(0x0010, 0x1001), {"Smith^John", "Doe^Jane"});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0010, 0x1001)));
  auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0010, 0x1001)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values.size() == 2);
  REQUIRE(decoded.values[0] == "Smith^John");
  REQUIRE(decoded.values[1] == "Doe^Jane");
}

// --- Multi-valued LO/SH/UC and ST/LT/UT literal-backslash -----------------

TEST_CASE("LO multi-value round-trips including an empty middle value", "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0080, "LO", "X");
  auto r = mutate_and_reparse(b, Tag(0x0008, 0x0080), {"AAA", "", "BBB"});
  REQUIRE(r.status == cs::SetTextStatus::Success);
  auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*r.after.structure->find(Tag(0x0008, 0x0080)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values.size() == 3);
  REQUIRE(decoded.values[0] == "AAA");
  REQUIRE(decoded.values[1].empty());
  REQUIRE(decoded.values[2] == "BBB");
}

TEST_CASE("ST/LT/UT: a literal backslash in the input is encoded as literal content, "
          "never split, mirroring the A1.5 decode fix",
          "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  std::string text = "C:\\Storage Card\\var\\patient\\file.xml";
  b.element_short(0x0008, 0x0081, "ST", "X");
  b.element_short(0x0008, 0x0082, "LT", "X");
  b.element_long(0x0008, 0x0083, "UT", std::vector<std::byte>{std::byte{'X'}, std::byte{'Y'}});
  for (auto tag : {Tag(0x0008, 0x0081), Tag(0x0008, 0x0082), Tag(0x0008, 0x0083)}) {
    auto r = mutate_and_reparse(b, tag, {text});
    INFO("tag");
    REQUIRE(r.status == cs::SetTextStatus::Success);
    auto ctx = cs::resolve_character_set_context(*r.after.structure, ElementPath(tag));
    auto decoded = cs::decode_text(*r.after.structure->find(tag), ctx);
    REQUIRE(decoded.status == cs::DecodeStatus::Success);
    REQUIRE(decoded.values.size() == 1);
    REQUIRE(decoded.values[0] == text);
  }
}

TEST_CASE("ST/LT/UT: passing more than one component is rejected as invalid input",
          "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0081, "ST", "X");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto status = cs::set_text(structure, ElementPath(Tag(0x0008, 0x0081)), {"a", "b"});
  REQUIRE(status == cs::SetTextStatus::InvalidUnicodeInput);
  REQUIRE_FALSE(structure.is_modified());
}

// --- Padding ---------------------------------------------------------------

TEST_CASE("an odd-length encoded value is padded with a trailing SPACE to even length",
          "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0080, "LO", "X");
  auto r = mutate_and_reparse(b, Tag(0x0008, 0x0080), {"ABC"});  // 3 bytes, odd
  REQUIRE(r.status == cs::SetTextStatus::Success);
  const auto* e = r.after.structure->find(Tag(0x0008, 0x0080));
  auto raw = e->value().bytes();
  REQUIRE(raw.size() == 4);
  REQUIRE(raw[3] == std::byte{0x20});
}

TEST_CASE("an odd-length single-byte-encoded value is padded correctly too", "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.element_short(0x0008, 0x0080, "LO", "X");
  auto r = mutate_and_reparse(b, Tag(0x0008, 0x0080), {"A\xC3\xA9"});  // 2 chars, 2 encoded bytes -- even already
  REQUIRE(r.status == cs::SetTextStatus::Success);
  const auto* e = r.after.structure->find(Tag(0x0008, 0x0080));
  REQUIRE(e->value().bytes().size() == 2);

  auto r2 = mutate_and_reparse(b, Tag(0x0008, 0x0080), {"A\xC3\xA9""B"});  // 3 encoded bytes -- odd
  REQUIRE(r2.status == cs::SetTextStatus::Success);
  const auto* e2 = r2.after.structure->find(Tag(0x0008, 0x0080));
  auto raw = e2->value().bytes();
  REQUIRE(raw.size() == 4);
  REQUIRE(raw[3] == std::byte{0x20});
}

// --- Atomicity, structural boundaries, Specific Character Set itself ------

TEST_CASE("set_text on a nonexistent path reports PathNotFound", "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto status = cs::set_text(structure, ElementPath(Tag(0x0010, 0x0010)), {"X"});
  REQUIRE(status == cs::SetTextStatus::PathNotFound);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("set_text on a Sequence element reports IsSequenceElement", "[charset_encode]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0100, "SH", "AB");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto status = cs::set_text(structure, ElementPath(Tag(0x0008, 0x1140)), {"X"});
  REQUIRE(status == cs::SetTextStatus::IsSequenceElement);
  REQUIRE_FALSE(structure.is_modified());
}

TEST_CASE("Specific Character Set itself cannot be set via set_text (its VR, CS, is not "
          "charset-governed)",
          "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto status = cs::set_text(structure, ElementPath(cs::kSpecificCharacterSetTag), {"ISO_IR 192"});
  REQUIRE(status == cs::SetTextStatus::NotATextVR);
  REQUIRE_FALSE(structure.is_modified());
}

// --- Nested inheritance, override, sibling isolation -----------------------

TEST_CASE("set_text on a nested element uses the inherited root charset", "[charset_encode][nested]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0010, 0x0010, "PN", "X");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0010, 0x0010));
  auto status = cs::set_text(structure, path, {"A\xC3\xA9" "B"});  // needs Latin1
  REQUIRE(status == cs::SetTextStatus::Success);

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  auto ctx = cs::resolve_character_set_context(*reparsed.structure, path);
  auto decoded = cs::decode_text(*reparsed.structure->find(path), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "A\xC3\xA9" "B");
}

TEST_CASE("set_text on a nested element with a local override uses the override, not the root",
          "[charset_encode][nested]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 144");  // override: Cyrillic
  item_content.element_short(0x0010, 0x0010, "PN", "X");
  auto item = wrap_in_item(item_content);
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0010, 0x0010));
  // 'р' (Cyrillic) is representable under the Item's own override, not
  // under the root's Latin1 -- proves the override, not the root, governs.
  auto status = cs::set_text(structure, path, {"A\xD1\x80" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);
}

TEST_CASE("sibling Items' charset overrides do not leak into each other during set_text",
          "[charset_encode][nested]") {
  FixtureBuilder item0_content;
  item0_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 100");  // Item 0: Latin1
  item0_content.element_short(0x0010, 0x0010, "PN", "X");
  auto item0 = wrap_in_item(item0_content);
  FixtureBuilder item1_content;
  item1_content.element_short(0x0010, 0x0010, "PN", "X");  // no override -- inherits root
  auto item1 = wrap_in_item(item1_content);
  FixtureBuilder items;
  items.append(item0).append(item1);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 144"});  // root: Cyrillic
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(items.size()));
  b.append(items);

  auto parsed = parse(b);
  auto& structure = *parsed.structure;

  ElementPath path1;
  path1.push(Tag(0x0008, 0x1140), 1).push(Tag(0x0010, 0x0010));
  // Item 1 has no local override -- inherits root's Cyrillic, so a
  // Latin1-only character must fail here even though Item 0 (its sibling)
  // declares Latin1.
  auto status = cs::set_text(structure, path1, {"A\xC3\xA9" "B"});  // needs Latin1
  REQUIRE(status == cs::SetTextStatus::UnrepresentableCharacter);
}

// --- Unsupported (Japanese) negative control -------------------------------

TEST_CASE("attempting set_text under an unsupported (deferred multi-byte) declaration fails "
          "safely with zero mutation",
          "[charset_encode][unsupported]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO 2022 IR 87"});  // Japanese Kanji, deferred
  b.element_short(0x0008, 0x0080, "LO", "hello");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto raw_before = std::vector<std::byte>(structure.find(Tag(0x0008, 0x0080))->value().bytes().begin(),
                                            structure.find(Tag(0x0008, 0x0080))->value().bytes().end());
  auto status = cs::set_text(structure, ElementPath(Tag(0x0008, 0x0080)), {"new value"});
  REQUIRE(status == cs::SetTextStatus::UnsupportedCharset);
  REQUIRE_FALSE(structure.is_modified());
  auto raw_after = std::vector<std::byte>(structure.find(Tag(0x0008, 0x0080))->value().bytes().begin(),
                                           structure.find(Tag(0x0008, 0x0080))->value().bytes().end());
  REQUIRE(raw_before == raw_after);
}

// --- Implicit-VR-origin mutation --------------------------------------------

TEST_CASE("charset-aware mutation works on an Implicit-VR-parsed element, semantic write "
          "contract applies",
          "[charset_encode][implicit_vr]") {
  auto b = make_file_meta("1.2.840.10008.1.2");  // Implicit VR LE
  // (0008,0005) itself must use Implicit VR wire format too -- no VR field
  // on the wire, matching the declared Transfer Syntax (element_implicit_*,
  // not the Explicit-VR-only specific_character_set() helper).
  b.element_implicit_ascii(0x0008, 0x0005, "ISO_IR 100");
  b.element_implicit_ascii(0x0010, 0x0010, "OldName");

  auto parsed = fds::parse_buffer(b.bytes());
  REQUIRE(parsed.status != ParseStatus::Failed);
  auto& structure = *parsed.structure;
  const auto* before = structure.find(Tag(0x0010, 0x0010));
  REQUIRE(before != nullptr);
  REQUIRE(before->vr() == VR::PN);  // dictionary-resolved, A1.4

  auto status = cs::set_text(structure, ElementPath(Tag(0x0010, 0x0010)), {"Riesmeier^J\xC3\xB6rg"});
  REQUIRE(status == cs::SetTextStatus::Success);
  REQUIRE(structure.is_modified());

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.status != ParseStatus::Failed);
  // Explicit VR output per the existing writer contract (A1.4's own note:
  // Implicit-VR-sourced structures are always re-encoded as Explicit VR
  // once modified).
  REQUIRE(reparsed.structure->transfer_syntax().explicit_vr());
  auto ctx = cs::resolve_character_set_context(*reparsed.structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*reparsed.structure->find(Tag(0x0010, 0x0010)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "Riesmeier^J\xC3\xB6rg");
}

// --- New top-level element insertion (A1.7: insert_text at root) -----------
//
// A1.6's original set_new_top_level_text() was removed during A1.7: an
// empty-`parent` call to the new, more general insert_text() below covers
// the identical top-level-only case exactly, so keeping both would be two
// long-term APIs for one capability (see
// docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 21). These two tests are its direct A1.7 successors.

TEST_CASE("insert_text inserts a brand-new root element under the top-level charset",
          "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  REQUIRE_FALSE(structure.contains(Tag(0x0010, 0x0010)));

  auto status = cs::insert_text(structure, ElementPath(), Tag(0x0010, 0x0010), VR::PN,
                                 {"A\xC3\xA9" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);
  const auto* e = structure.find(Tag(0x0010, 0x0010));
  REQUIRE(e != nullptr);
  REQUIRE(e->vr() == VR::PN);

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  auto ctx = cs::resolve_character_set_context(*reparsed.structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*reparsed.structure->find(Tag(0x0010, 0x0010)), ctx);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "A\xC3\xA9" "B");
}

TEST_CASE("insert_text with a non-text VR fails without inserting anything", "[charset_encode]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto status = cs::insert_text(structure, ElementPath(), Tag(0x0008, 0x0060), VR::CS, {"CT"});
  REQUIRE(status == cs::SetTextStatus::NotATextVR);
  REQUIRE_FALSE(structure.contains(Tag(0x0008, 0x0060)));
}

// --- Unrelated content and Pixel Data preservation --------------------------

TEST_CASE("charset-aware mutation of one element leaves Pixel Data and unrelated elements "
          "untouched",
          "[charset_encode]") {
  std::vector<std::byte> pixels(16, std::byte{0x42});
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.element_short(0x0008, 0x0060, "CS", "CT");
  b.element_short(0x0010, 0x0010, "PN", "OldName");
  b.pixel_data_native("OW", pixels);

  auto parsed = parse(b);
  auto& structure = *parsed.structure;
  auto status = cs::set_text(structure, ElementPath(Tag(0x0010, 0x0010)), {"A\xC3\xA9" "B"});
  REQUIRE(status == cs::SetTextStatus::Success);

  std::ostringstream out;
  REQUIRE(structure.write(out).status == WriteStatus::Success);
  auto reparsed = fds::parse_buffer(std::as_bytes(std::span<const char>(out.str())));
  REQUIRE(reparsed.structure->find(Tag(0x0008, 0x0060))->value().as_string() == "CT");
  REQUIRE(reparsed.structure->pixel_data() != nullptr);
  REQUIRE(reparsed.structure->pixel_data()->native_span().length() == 16);
}
