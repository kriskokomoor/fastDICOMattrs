// Tests for fds::dictionary::lookup(). See docs/architecture/
// A1_1_DICTIONARY_SUBSTRATE_REPORT.md for the full provenance and design
// this exercises. Nothing here touches the parser, writer, or mutation
// paths -- this dictionary is not wired into any of them yet (A1.4's job).

#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/dictionary.hpp"

using fds::Tag;
using fds::VR;
using fds::dictionary::lookup;
using fds::dictionary::VRAmbiguity;

TEST_CASE("exact lookup: ordinary current tags across several VR families", "[dictionary]") {
  // (0020,000D) Study Instance UID -- UI
  auto ui = lookup(Tag{0x0020, 0x000D});
  REQUIRE(ui.has_value());
  CHECK(ui->vr == VR::UI);
  CHECK(ui->ambiguity == VRAmbiguity::None);
  CHECK(ui->keyword == "StudyInstanceUID");
  CHECK_FALSE(ui->retired);

  // (0028,0010) Rows -- US
  auto rows = lookup(Tag{0x0028, 0x0010});
  REQUIRE(rows.has_value());
  CHECK(rows->vr == VR::US);
  CHECK(rows->keyword == "Rows");
  CHECK_FALSE(rows->retired);

  // (0010,0020) Patient ID -- LO
  auto pid = lookup(Tag{0x0010, 0x0020});
  REQUIRE(pid.has_value());
  CHECK(pid->vr == VR::LO);
  CHECK(pid->keyword == "PatientID");

  // (0008,0060) Modality -- CS
  auto modality = lookup(Tag{0x0008, 0x0060});
  REQUIRE(modality.has_value());
  CHECK(modality->vr == VR::CS);
  CHECK(modality->keyword == "Modality");
}

TEST_CASE("unknown, non-private tag returns no entry", "[dictionary]") {
  // (0008,FFFF): even (non-private) group, an element number no standard
  // edition has ever assigned in this group.
  auto result = lookup(Tag{0x0008, 0xFFFF});
  CHECK_FALSE(result.has_value());
}

TEST_CASE("private tag never spuriously resolves from the standard dictionary", "[dictionary]") {
  // (0009,0010): odd group -- private by construction, regardless of
  // whether some vendor happens to use this exact slot for a creator
  // element. This dictionary contains no private/vendor semantics.
  auto result = lookup(Tag{0x0009, 0x0010});
  CHECK_FALSE(result.has_value());

  // A private group's element numbers colliding numerically with a
  // *different* even group's standard element must not cause a false
  // match either -- (0009,0060) must not resolve just because (0008,0060)
  // (Modality) does.
  auto collide = lookup(Tag{0x0009, 0x0060});
  CHECK_FALSE(collide.has_value());
}

TEST_CASE("retired standard tag resolves to its historically correct VR", "[dictionary]") {
  // (0008,0010) Recognition Code -- SH, retired.
  auto result = lookup(Tag{0x0008, 0x0010});
  REQUIRE(result.has_value());
  CHECK(result->vr == VR::SH);
  CHECK(result->keyword == "RecognitionCode");
  CHECK(result->retired);
}

TEST_CASE("ambiguous VR entries are represented as ambiguous, never guessed", "[dictionary]") {
  // (0028,0106) Smallest Image Pixel Value -- "US or SS".
  auto us_or_ss = lookup(Tag{0x0028, 0x0106});
  REQUIRE(us_or_ss.has_value());
  CHECK(us_or_ss->ambiguity == VRAmbiguity::USorSS);
  CHECK(us_or_ss->vr == VR::Unknown);  // must not be prematurely resolved to US or SS
  CHECK(us_or_ss->keyword == "SmallestImagePixelValue");

  // (7FE0,0010) Pixel Data -- "OB or OW".
  auto ob_or_ow = lookup(Tag{0x7FE0, 0x0010});
  REQUIRE(ob_or_ow.has_value());
  CHECK(ob_or_ow->ambiguity == VRAmbiguity::OBorOW);
  CHECK(ob_or_ow->vr == VR::Unknown);

  // (0028,3006) LUT Data -- "US or OW".
  auto us_or_ow = lookup(Tag{0x0028, 0x3006});
  REQUIRE(us_or_ow.has_value());
  CHECK(us_or_ow->ambiguity == VRAmbiguity::USorOW);

  // (0028,1200) Gray Lookup Table Data -- "US or SS or OW", also retired.
  auto triple = lookup(Tag{0x0028, 0x1200});
  REQUIRE(triple.has_value());
  CHECK(triple->ambiguity == VRAmbiguity::USorSSorOW);
  CHECK(triple->retired);
}

TEST_CASE("repeating group 60xx (Overlay): boundaries", "[dictionary][repeating]") {
  // First valid group.
  auto first = lookup(Tag{0x6000, 0x0010});
  REQUIRE(first.has_value());
  CHECK(first->vr == VR::US);
  CHECK(first->keyword == "OverlayRows");

  // A middle valid group -- same element, same answer.
  auto middle = lookup(Tag{0x6010, 0x0010});
  REQUIRE(middle.has_value());
  CHECK(middle->vr == VR::US);
  CHECK(middle->keyword == "OverlayRows");

  // Last valid group (0x60FE -- see PROVENANCE.md for why this is the
  // adopted bound, not the narrower 0x601E PS3.5 SS7.6 cardinality text).
  auto last = lookup(Tag{0x60FE, 0x0010});
  REQUIRE(last.has_value());
  CHECK(last->vr == VR::US);

  // Immediately out-of-range: next even group past the last valid one.
  auto out_of_range = lookup(Tag{0x6100, 0x0010});
  CHECK_FALSE(out_of_range.has_value());

  // An odd group inside the numeric span must not match -- the pattern is
  // even-groups-only.
  auto odd_group = lookup(Tag{0x6001, 0x0010});
  CHECK_FALSE(odd_group.has_value());

  // A valid group with an element number the pattern does not define.
  auto unrelated_element = lookup(Tag{0x6010, 0x9999});
  CHECK_FALSE(unrelated_element.has_value());
}

TEST_CASE("repeating group 50xx (retired Curve Data): boundaries", "[dictionary][repeating]") {
  auto first = lookup(Tag{0x5000, 0x0005});
  REQUIRE(first.has_value());
  CHECK(first->vr == VR::US);
  CHECK(first->keyword == "CurveDimensions");
  CHECK(first->retired);

  auto last = lookup(Tag{0x50FE, 0x0005});
  REQUIRE(last.has_value());
  CHECK(last->retired);

  auto out_of_range = lookup(Tag{0x5100, 0x0005});
  CHECK_FALSE(out_of_range.has_value());
}

TEST_CASE("repeating group 7Fxx (retired Variable Pixel Data): boundaries", "[dictionary][repeating]") {
  auto first = lookup(Tag{0x7F00, 0x0010});
  REQUIRE(first.has_value());
  CHECK(first->ambiguity == VRAmbiguity::OBorOW);
  CHECK(first->keyword == "VariablePixelData");
  CHECK(first->retired);

  auto last = lookup(Tag{0x7FFE, 0x0010});
  REQUIRE(last.has_value());
  CHECK(last->ambiguity == VRAmbiguity::OBorOW);

  auto out_of_range = lookup(Tag{0x8000, 0x0010});
  CHECK_FALSE(out_of_range.has_value());
}

TEST_CASE("structural pseudo-tags are excluded from dictionary lookup", "[dictionary]") {
  // PS3.6 lists these with VR "See Note" -- pure encoding framing, not
  // data elements. Already handled structurally by Tag::
  // is_item_or_delimiter(); the dictionary must not offer a second,
  // competing answer for them.
  CHECK_FALSE(lookup(Tag{0xFFFE, 0xE000}).has_value());
  CHECK_FALSE(lookup(Tag{0xFFFE, 0xE00D}).has_value());
  CHECK_FALSE(lookup(Tag{0xFFFE, 0xE0DD}).has_value());
}

TEST_CASE("DICONDE/DICOS companion-standard entries resolve like ordinary current entries",
          "[dictionary]") {
  // (0014,0028) Component Manufacturer -- ST, a DICONDE-noted entry. Must
  // resolve normally (not retired, real VR) -- DICONDE/DICOS notes are not
  // a retirement marker.
  auto result = lookup(Tag{0x0014, 0x0028});
  REQUIRE(result.has_value());
  CHECK(result->vr == VR::ST);
  CHECK_FALSE(result->retired);
}

TEST_CASE("the 17 excluded element-level wildcard patterns never spuriously resolve",
          "[dictionary][a1_4]") {
  // A1.1 deliberately excludes 17 obscure, fully-retired element-level (not
  // group-level) wildcard patterns from the generated table -- see
  // A1_1_DICTIONARY_SUBSTRATE_REPORT.md section 6 and
  // tools/generate_dictionary.py's EXCLUDED_ELEMENT_PATTERNS. A1.4 must not
  // silently broaden that boundary: representative tags matching each
  // excluded pattern must still return no dictionary entry, exactly as
  // before A1.4 wired the dictionary into the parser.
  CHECK_FALSE(lookup(Tag{0x0020, 0x3105}).has_value());  // 0020,31xx
  CHECK_FALSE(lookup(Tag{0x0028, 0x04A0}).has_value());  // 0028,04x0
  CHECK_FALSE(lookup(Tag{0x0028, 0x08C2}).has_value());  // 0028,08x2
  CHECK_FALSE(lookup(Tag{0x1000, 0x1230}).has_value());  // 1000,xxx0
  CHECK_FALSE(lookup(Tag{0x1010, 0x1234}).has_value());  // 1010,xxxx
}
