#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/charset.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fixture_builder.hpp"

// A1.5 -- Specific Character Set context resolution and text decoding. See
// docs/architecture/A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md.
//
// All per-repertoire byte->codepoint pairs below were derived from
// Python's own standard-library codecs -- the same source
// tools/generate_charset_tables.py uses to generate the tables under
// test, so these are internal-consistency fixtures (real, verifiable
// mappings, not guessed), not an independent oracle. The independent
// cross-implementation evidence (pydicom, DCMTK) is in the A1.5 freeze
// report, not here.

using namespace fds_test;
using fds::DICOMStructure;
using fds::Element;
using fds::ElementPath;
using fds::ParseResult;
using fds::ParseStatus;
using fds::Tag;
using fds::Value;
using fds::VR;
namespace cs = fds::charset;

namespace {
ParseResult parse(const FixtureBuilder& b) { return fds::parse_buffer(b.bytes()); }

std::vector<std::byte> to_bytes(const std::string& s) {
  std::vector<std::byte> out(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
  return out;
}

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

// --- Default repertoire ------------------------------------------------

TEST_CASE("no Specific Character Set declared: default repertoire decodes ASCII text",
          "[charset]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0010, "PN", "Smith^John");
  auto result = parse(b);
  REQUIRE(result.status == ParseStatus::Success);
  auto& structure = *result.structure;

  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
  REQUIRE(context.mode == cs::CharacterSetMode::Default);

  const auto* e = structure.find(Tag(0x0010, 0x0010));
  auto decoded = cs::decode_text(*e, context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values.size() == 1);
  REQUIRE(decoded.values[0] == "Smith^John");
}

TEST_CASE("default repertoire rejects a high-bit byte -- strictly 7-bit ASCII", "[charset]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short_bytes(0x0010, 0x0020, "LO",
                         {std::byte{'A'}, std::byte{0xE9}, std::byte{'B'}});
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0020)));
  REQUIRE(context.mode == cs::CharacterSetMode::Default);
  auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0020)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::InvalidEncodedBytes);
}

// --- UTF-8 (ISO_IR 192) --------------------------------------------------

TEST_CASE("ISO_IR 192: ASCII subset decodes correctly", "[charset][utf8]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 192"});
  b.element_short(0x0010, 0x0010, "PN", "Doe^Jane");
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
  REQUIRE(context.mode == cs::CharacterSetMode::SingleValue);
  auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0010)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "Doe^Jane");
}

TEST_CASE("ISO_IR 192: two-byte, three-byte, and four-byte UTF-8 sequences decode correctly",
          "[charset][utf8]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 192"});
  // U+00E9 (é, 2-byte), U+4E2D (中, 3-byte), U+1F600 (😀, 4-byte).
  std::vector<std::byte> value = {std::byte{0xC3}, std::byte{0xA9}, std::byte{0xE4},
                                   std::byte{0xB8}, std::byte{0xAD}, std::byte{0xF0},
                                   std::byte{0x9F}, std::byte{0x98}, std::byte{0x80}};
  b.element_short_bytes(0x0008, 0x0080, "LO", value);
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80");
}

TEST_CASE("ISO_IR 192: invalid, truncated, and overlong sequences are rejected, not replaced",
          "[charset][utf8]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 192"});
  // Truncated 3-byte sequence (0xE4 0xB8 missing final continuation byte).
  b.element_short_bytes(0x0008, 0x0080, "LO", {std::byte{0xE4}, std::byte{0xB8}});
  // Overlong encoding of U+002F ('/') as 2 bytes (0xC0 0xAF) -- illegal.
  b.element_short_bytes(0x0008, 0x0081, "LO", {std::byte{0xC0}, std::byte{0xAF}});
  // Lone continuation byte.
  b.element_short_bytes(0x0008, 0x0082, "LO", {std::byte{0x80}});
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  for (auto tag : {Tag(0x0008, 0x0080), Tag(0x0008, 0x0081), Tag(0x0008, 0x0082)}) {
    auto decoded = cs::decode_text(*structure.find(tag), context);
    REQUIRE(decoded.status == cs::DecodeStatus::InvalidEncodedBytes);
  }
}

// --- Single-byte repertoires (one discriminating non-ASCII char each) ---

TEST_CASE("each supported single-byte repertoire decodes a real, discriminating non-ASCII "
          "character correctly",
          "[charset][single_byte]") {
  struct Case {
    const char* term;
    std::byte byte_value;
    const char* expected_utf8;
  };
  const Case cases[] = {
      {"ISO_IR 100", std::byte{0xE9}, "\xC3\xA9"},          // Latin1 'é'
      {"ISO_IR 101", std::byte{0xE8}, "\xC4\x8D"},          // Latin2 'č'
      {"ISO_IR 109", std::byte{0xE0}, "\xC3\xA0"},          // Latin3 'à'
      {"ISO_IR 110", std::byte{0xE6}, "\xC3\xA6"},          // Latin4 'æ'
      {"ISO_IR 144", std::byte{0xE0}, "\xD1\x80"},          // Cyrillic 'р'
      {"ISO_IR 127", std::byte{0xE0}, "\xD9\x80"},          // Arabic tatweel 'ـ'
      {"ISO_IR 126", std::byte{0xE1}, "\xCE\xB1"},          // Greek 'α'
      {"ISO_IR 138", std::byte{0xE0}, "\xD7\x90"},          // Hebrew 'א'
      {"ISO_IR 148", std::byte{0xFD}, "\xC4\xB1"},          // Turkish dotless i 'ı'
      {"ISO_IR 166", std::byte{0xA1}, "\xE0\xB8\x81"},      // Thai 'ก'
  };
  for (const auto& c : cases) {
    auto b = make_file_meta("1.2.840.10008.1.2.1");
    specific_character_set(b, {c.term});
    b.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{'A'}, c.byte_value, std::byte{'B'}});
    auto result = parse(b);
    REQUIRE(result.status == ParseStatus::Success);
    auto& structure = *result.structure;
    auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
    REQUIRE(context.mode == cs::CharacterSetMode::SingleValue);
    auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0010)), context);
    INFO("term: " << c.term);
    REQUIRE(decoded.status == cs::DecodeStatus::Success);
    REQUIRE(decoded.values[0] == std::string("A") + c.expected_utf8 + "B");
  }
}

TEST_CASE("the same byte value decodes to a different character depending on the active "
          "repertoire",
          "[charset][single_byte]") {
  struct Case {
    const char* term;
    const char* expected_utf8;
  };
  const Case cases[] = {
      {"ISO_IR 100", "\xC3\xA0"},  // Latin1 0xE0 -> 'à'
      {"ISO_IR 144", "\xD1\x80"},  // Cyrillic 0xE0 -> 'р'
      {"ISO_IR 138", "\xD7\x90"},  // Hebrew 0xE0 -> 'א'
      {"ISO_IR 127", "\xD9\x80"},  // Arabic 0xE0 -> 'ـ'
  };
  for (const auto& c : cases) {
    auto b = make_file_meta("1.2.840.10008.1.2.1");
    specific_character_set(b, {c.term});
    b.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE0}});
    auto result = parse(b);
    auto& structure = *result.structure;
    auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
    auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0010)), context);
    INFO("term: " << c.term);
    REQUIRE(decoded.status == cs::DecodeStatus::Success);
    REQUIRE(decoded.values[0] == c.expected_utf8);
  }
}

// --- ISO 2022 multi-valued switching -------------------------------------

TEST_CASE("multi-valued Specific Character Set: ISO 2022 switching between Latin1 and "
          "Cyrillic mid-string",
          "[charset][iso2022]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100", "ISO 2022 IR 144"});
  std::vector<std::byte> value;
  auto append = [&](std::initializer_list<int> bytes) {
    for (int v : bytes) value.push_back(static_cast<std::byte>(v));
  };
  append({'A', 'B'});                          // ASCII (default, encodings[0])
  append({0x1B, '-', 'A'});                     // switch to Latin1
  append({0xE9});                               // 'é'
  append({0x1B, '-', 'L'});                     // switch to Cyrillic
  append({0xE0});                               // 'р'
  b.element_short_bytes(0x0010, 0x0010, "PN", value);

  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
  REQUIRE(context.mode == cs::CharacterSetMode::MultiValue);
  auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0010)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "AB\xC3\xA9\xD1\x80");
}

TEST_CASE("ISO 2022 state does not leak across VM-separated values", "[charset][iso2022]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100"});
  std::vector<std::byte> value;
  auto append = [&](std::initializer_list<int> bytes) {
    for (int v : bytes) value.push_back(static_cast<std::byte>(v));
  };
  append({0x1B, '-', 'A'});  // switch to Latin1 in the first value
  append({0xE9});            // 'é'
  append({'\\'});            // VM separator
  append({0xE9});             // in the *second* value: if (and only if) Latin1 state had
                              // incorrectly leaked across the '\' boundary, this would
                              // decode as 'é' too; per encodings[0] == strict ASCII (the
                              // correct, reset state), it is instead invalid.
  b.element_short_bytes(0x0008, 0x0080, "LO", value);

  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  // decode_text is all-or-nothing across VM components (see its contract);
  // the second value's byte being invalid under the correctly-reset state
  // is exactly the proof that no leakage occurred -- a leaked Latin1 state
  // would have wrongly made this whole call succeed.
  REQUIRE(decoded.status == cs::DecodeStatus::InvalidEncodedBytes);
}

TEST_CASE("a malformed escape sequence is rejected, not silently substituted", "[charset][iso2022]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100"});
  // ESC followed by a byte pair this library's table does not recognize.
  std::vector<std::byte> value = {std::byte{'A'}, std::byte{0x1B}, std::byte{'%'}, std::byte{'Z'}};
  b.element_short_bytes(0x0008, 0x0080, "LO", value);
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::InvalidEncodedBytes);
}

TEST_CASE("an escape sequence for a repertoire not in the declared terms is rejected",
          "[charset][iso2022]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  // Only Latin1 declared -- but the value tries to switch to Cyrillic.
  specific_character_set(b, {"", "ISO 2022 IR 100"});
  std::vector<std::byte> value = {std::byte{0x1B}, std::byte{'-'}, std::byte{'L'}, std::byte{0xE0}};
  b.element_short_bytes(0x0008, 0x0080, "LO", value);
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::InvalidEncodedBytes);
}

// --- Person Name delimiter handling ---------------------------------------

TEST_CASE("PN: alphabetic/ideographic/phonetic component groups separated by '=' decode "
          "independently and are rejoined literally",
          "[charset][pn]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100"});
  std::vector<std::byte> value;
  auto append_ascii = [&](const std::string& s) {
    for (char c : s) value.push_back(static_cast<std::byte>(c));
  };
  append_ascii("Yamada^Tarou");  // alphabetic group: plain ASCII, no escape needed
  value.push_back(std::byte{'='});
  // Ideographic group: starts completely fresh (encodings[0] == ASCII)
  // independent of the alphabetic group above, then escapes into Latin1
  // for one character -- proving groups are decoded independently, not
  // as one continuous byte stream.
  value.push_back(std::byte{0x1B});
  value.push_back(std::byte{'-'});
  value.push_back(std::byte{'A'});
  value.push_back(std::byte{0xE9});  // 'é'
  b.element_short_bytes(0x0010, 0x0010, "PN", value);

  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0010)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "Yamada^Tarou=\xC3\xA9");
}

TEST_CASE("PN: the '^' component delimiter resets state to the default within a group",
          "[charset][pn]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"", "ISO 2022 IR 100"});
  std::vector<std::byte> value = {std::byte{0x1B}, std::byte{'-'}, std::byte{'A'}, std::byte{0xE9},
                                   std::byte{'^'}, std::byte{0xE9}};
  b.element_short_bytes(0x0010, 0x0010, "PN", value);
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0010)), context);
  // After '^', state resets to default (ASCII, strict 7-bit) -- the second
  // 0xE9 is therefore invalid under the reset state, proving the reset
  // actually happened (it would decode as 'é' again if it hadn't).
  REQUIRE(decoded.status == cs::DecodeStatus::InvalidEncodedBytes);
}

TEST_CASE("PN: empty components are preserved", "[charset][pn]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x0010, "PN", "Smith^^Jane");  // empty middle component
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x0010)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "Smith^^Jane");
}

TEST_CASE("PN: multiple PN values separated by backslash decode independently", "[charset][pn]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0010, 0x1001, "PN", "Smith^John\\Doe^Jane");
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x1001)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0010, 0x1001)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values.size() == 2);
  REQUIRE(decoded.values[0] == "Smith^John");
  REQUIRE(decoded.values[1] == "Doe^Jane");
}

// --- Multi-valued text (non-PN) -------------------------------------------

TEST_CASE("multi-valued LO text preserves value boundaries and empty values", "[charset]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0080, "LO", "AAA\\\\BBB");  // empty middle value
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values.size() == 3);
  REQUIRE(decoded.values[0] == "AAA");
  REQUIRE(decoded.values[1].empty());
  REQUIRE(decoded.values[2] == "BBB");
}

TEST_CASE("ST/LT/UT are inherently single-valued: an embedded backslash is literal text, "
          "never a value delimiter",
          "[charset]") {
  // Real-world regression: a genuine DICOM file's LT value containing
  // embedded Windows-style file paths (literal backslashes) was silently
  // truncated at the first backslash before this was fixed -- found via
  // real-corpus qualification (see the A1.5 freeze report), not a
  // hypothetical.
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  std::string text = "C:\\Storage Card\\var\\patient\\file.xml";
  b.element_short(0x0008, 0x0081, "ST", text);  // ST is short-form
  b.element_short(0x0008, 0x0082, "LT", text);  // LT is short-form
  b.element_long(0x0008, 0x0083, "UT", to_bytes(text));  // UT is long-form
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0081)));
  for (auto tag : {Tag(0x0008, 0x0081), Tag(0x0008, 0x0082), Tag(0x0008, 0x0083)}) {
    auto decoded = cs::decode_text(*structure.find(tag), context);
    REQUIRE(decoded.status == cs::DecodeStatus::Success);
    REQUIRE(decoded.values.size() == 1);
    REQUIRE(decoded.values[0] == text);
  }
}

TEST_CASE("an empty text value decodes to an empty string, not an error", "[charset]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0080, "LO", "");
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values.size() == 1);
  REQUIRE(decoded.values[0].empty());
}

// --- Unsupported / malformed declarations ---------------------------------

TEST_CASE("a deferred (multi-byte, out-of-V1-scope) charset is explicit, never guessed",
          "[charset][unsupported]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO 2022 IR 87"});  // Japanese Kanji, explicitly deferred
  b.element_short(0x0008, 0x0080, "LO", "hello");
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  REQUIRE(context.mode == cs::CharacterSetMode::Unsupported);
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::UnsupportedCharset);
  // Raw bytes remain fully available regardless of decode status.
  REQUIRE(structure.find(Tag(0x0008, 0x0080))->value().as_string() == "hello");
}

TEST_CASE("a totally unrecognized Specific Character Set term is Unsupported, never guessed",
          "[charset][unsupported]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"NOT_A_REAL_TERM"});
  b.element_short(0x0008, 0x0080, "LO", "hello");
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  REQUIRE(context.mode == cs::CharacterSetMode::Unsupported);
}

TEST_CASE("combining a stand-alone term (UTF-8) with another term is Malformed",
          "[charset][malformed]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 192", "ISO 2022 IR 100"});
  b.element_short(0x0008, 0x0080, "LO", "hello");
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0080)));
  REQUIRE(context.mode == cs::CharacterSetMode::Malformed);
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0080)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::MalformedCharsetDeclaration);
}

// --- Text VR classification -----------------------------------------------

TEST_CASE("decode_text on a non-text VR returns NotATextVR without inspecting charset",
          "[charset]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0060, "CS", "CT");  // Modality -- CS is not charset-governed
  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0060)));
  auto decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0060)), context);
  REQUIRE(decoded.status == cs::DecodeStatus::NotATextVR);
}

TEST_CASE("every governed text VR is decodable; every non-governed VR reports NotATextVR",
          "[charset]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  b.element_short(0x0008, 0x0100, "SH", "AB");
  b.element_short(0x0008, 0x0080, "LO", "AB");
  b.element_short(0x0008, 0x0081, "ST", "AB");  // ST is short-form (PS3.5 Table 7.1-1)
  b.element_short(0x0008, 0x0082, "LT", "AB");  // LT is short-form too
  b.element_short(0x0010, 0x0010, "PN", "AB");
  b.element_long(0x0008, 0x0083, "UC", to_bytes("AB"));
  b.element_long(0x0008, 0x0084, "UT", to_bytes("AB"));
  // Non-governed, per PS3.5: always default repertoire regardless of (0008,0005).
  b.element_short(0x0008, 0x0060, "CS", "CT");

  auto result = parse(b);
  auto& structure = *result.structure;
  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x0100)));
  for (auto tag : {Tag(0x0008, 0x0100), Tag(0x0008, 0x0080), Tag(0x0008, 0x0081),
                    Tag(0x0008, 0x0082), Tag(0x0010, 0x0010), Tag(0x0008, 0x0083),
                    Tag(0x0008, 0x0084)}) {
    auto decoded = cs::decode_text(*structure.find(tag), context);
    INFO("tag governed-VR check");
    REQUIRE(decoded.status == cs::DecodeStatus::Success);
  }
  auto cs_decoded = cs::decode_text(*structure.find(Tag(0x0008, 0x0060)), context);
  REQUIRE(cs_decoded.status == cs::DecodeStatus::NotATextVR);
}

// --- Nested Item context: inheritance, override, sibling isolation --------

TEST_CASE("a root Specific Character Set declaration is inherited by a nested Item",
          "[charset][nested]") {
  FixtureBuilder item_content;
  item_content.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE9}});
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = parse(b);
  auto& structure = *result.structure;
  ElementPath path;
  path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0010, 0x0010));
  auto context = cs::resolve_character_set_context(structure, path);
  REQUIRE(context.mode == cs::CharacterSetMode::SingleValue);
  REQUIRE_FALSE(context.declared_locally);
  auto decoded = cs::decode_text(*structure.find(path), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values.back() == "\xC3\xA9");
}

TEST_CASE("a nested Item's own Specific Character Set overrides the root's", "[charset][nested]") {
  FixtureBuilder item_content;
  item_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 144");  // override: Cyrillic
  item_content.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE0}});
  auto item = wrap_in_item(item_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(item.size()));
  b.append(item);

  auto result = parse(b);
  auto& structure = *result.structure;

  // Root-level element still resolves to Latin1.
  auto root_context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0008, 0x1140)));
  REQUIRE(root_context.terms[0].defined_term == "ISO_IR 100");

  // Nested element resolves to the Item's own override, Cyrillic.
  ElementPath nested_path;
  nested_path.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0010, 0x0010));
  auto nested_context = cs::resolve_character_set_context(structure, nested_path);
  REQUIRE(nested_context.mode == cs::CharacterSetMode::SingleValue);
  REQUIRE(nested_context.declared_locally);
  REQUIRE(nested_context.terms[0].defined_term == "ISO_IR 144");
  auto decoded = cs::decode_text(*structure.find(nested_path), nested_context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "\xD1\x80");  // Cyrillic 0xE0 -> 'р', not Latin1's 'à'
}

TEST_CASE("sibling Items' Specific Character Set overrides do not leak into each other",
          "[charset][nested]") {
  FixtureBuilder item0_content;
  item0_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 100");  // Item 0: Latin1
  item0_content.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE0}});
  auto item0 = wrap_in_item(item0_content);

  FixtureBuilder item1_content;
  // No local override -- must NOT see Item 0's Latin1 override, only the root's.
  item1_content.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE0}});
  auto item1 = wrap_in_item(item1_content);

  FixtureBuilder items;
  items.append(item0).append(item1);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 144"});  // root: Cyrillic
  b.sequence_defined(0x0008, 0x1140, static_cast<std::uint32_t>(items.size()));
  b.append(items);

  auto result = parse(b);
  auto& structure = *result.structure;

  ElementPath path0;
  path0.push(Tag(0x0008, 0x1140), 0).push(Tag(0x0010, 0x0010));
  auto context0 = cs::resolve_character_set_context(structure, path0);
  REQUIRE(context0.terms[0].defined_term == "ISO_IR 100");
  auto decoded0 = cs::decode_text(*structure.find(path0), context0);
  REQUIRE(decoded0.values[0] == "\xC3\xA0");  // Latin1 'à'

  ElementPath path1;
  path1.push(Tag(0x0008, 0x1140), 1).push(Tag(0x0010, 0x0010));
  auto context1 = cs::resolve_character_set_context(structure, path1);
  REQUIRE_FALSE(context1.declared_locally);
  REQUIRE(context1.terms[0].defined_term == "ISO_IR 144");  // inherited from root, NOT item0
  auto decoded1 = cs::decode_text(*structure.find(path1), context1);
  REQUIRE(decoded1.values[0] == "\xD1\x80");  // Cyrillic 'р'
}

TEST_CASE("a nested child Item inherits from its immediate effective parent, not the root",
          "[charset][nested]") {
  FixtureBuilder inner_content;
  inner_content.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE0}});
  auto inner_item = wrap_in_item(inner_content);

  FixtureBuilder outer_content;
  outer_content.element_short(0x0008, 0x0005, "CS", "ISO_IR 138");  // Hebrew, overrides root
  outer_content.sequence_defined(0x300A, 0x00B6, static_cast<std::uint32_t>(inner_item.size()));
  outer_content.append(inner_item);
  auto outer_item = wrap_in_item(outer_content);

  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});  // root: Latin1
  b.sequence_defined(0x300A, 0x00B0, static_cast<std::uint32_t>(outer_item.size()));
  b.append(outer_item);

  auto result = parse(b);
  auto& structure = *result.structure;
  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 0).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x0010, 0x0010));
  auto context = cs::resolve_character_set_context(structure, path);
  REQUIRE(context.terms[0].defined_term == "ISO_IR 138");  // from the outer Item, not the root
  auto decoded = cs::decode_text(*structure.find(path), context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);
  REQUIRE(decoded.values[0] == "\xD7\x90");  // Hebrew 'א'
}

// --- Raw-byte preservation / no side effects --------------------------------

TEST_CASE("decoding text does not modify the structure, the element, or its raw bytes",
          "[charset][raw_preservation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE9}});
  auto result = parse(b);
  auto& structure = *result.structure;
  REQUIRE_FALSE(structure.is_modified());

  auto context = cs::resolve_character_set_context(structure, ElementPath(Tag(0x0010, 0x0010)));
  const auto* e = structure.find(Tag(0x0010, 0x0010));
  auto raw_before = std::vector<std::byte>(e->value().bytes().begin(), e->value().bytes().end());
  auto decoded = cs::decode_text(*e, context);
  REQUIRE(decoded.status == cs::DecodeStatus::Success);

  REQUIRE_FALSE(structure.is_modified());
  REQUIRE_FALSE(e->is_modified());
  auto raw_after = std::vector<std::byte>(e->value().bytes().begin(), e->value().bytes().end());
  REQUIRE(raw_before == raw_after);
  // element_short_bytes auto-pads the odd-length 1-byte value to 2 (a
  // trailing NUL pad byte, per PS3.5 6.4) -- bytes() includes that padding
  // verbatim, exactly as it does for every other element; decode_text
  // itself already strips it correctly (via Value::as_string_list()),
  // which is why the decode above succeeded.
  REQUIRE(raw_after.size() == 2);
  REQUIRE(raw_after[0] == std::byte{0xE9});
  REQUIRE(raw_after[1] == std::byte{0x00});
}

TEST_CASE("write() output is byte-identical whether or not decode_text was called first",
          "[charset][raw_preservation]") {
  auto b = make_file_meta("1.2.840.10008.1.2.1");
  specific_character_set(b, {"ISO_IR 100"});
  b.element_short_bytes(0x0010, 0x0010, "PN", {std::byte{0xE9}});

  fds::ParseOptions options;
  options.fidelity = fds::Fidelity::Lossless;

  auto result_a = fds::parse_buffer(b.bytes(), options);
  std::ostringstream out_a;
  REQUIRE(result_a.structure->write(out_a).status == fds::WriteStatus::Success);

  auto result_b = fds::parse_buffer(b.bytes(), options);
  auto context = cs::resolve_character_set_context(*result_b.structure, ElementPath(Tag(0x0010, 0x0010)));
  cs::decode_text(*result_b.structure->find(Tag(0x0010, 0x0010)), context);
  std::ostringstream out_b;
  REQUIRE(result_b.structure->write(out_b).status == fds::WriteStatus::Success);

  REQUIRE(out_a.str() == out_b.str());
  REQUIRE_FALSE(result_b.structure->is_modified());
}
