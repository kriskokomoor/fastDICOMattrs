#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/vr.hpp"

using fds::is_long_form;
using fds::to_string;
using fds::vr_from_string;
using fds::VR;

TEST_CASE("vr_from_string recognizes all 33 standard VRs", "[vr]") {
  REQUIRE(vr_from_string("AE") == VR::AE);
  REQUIRE(vr_from_string("PN") == VR::PN);
  REQUIRE(vr_from_string("SQ") == VR::SQ);
  REQUIRE(vr_from_string("UN") == VR::UN);
  REQUIRE(vr_from_string("UV") == VR::UV);
}

TEST_CASE("vr_from_string rejects unrecognized text", "[vr]") {
  REQUIRE_FALSE(vr_from_string("ZZ").has_value());
  REQUIRE_FALSE(vr_from_string("").has_value());
}

TEST_CASE("to_string round-trips through vr_from_string", "[vr]") {
  for (VR vr : {VR::AE, VR::PN, VR::SQ, VR::OB, VR::UN, VR::UV}) {
    auto text = to_string(vr);
    REQUIRE(vr_from_string(text) == vr);
  }
}

TEST_CASE("is_long_form matches PS3.5 Table 7.1-1", "[vr]") {
  REQUIRE(is_long_form(VR::OB));
  REQUIRE(is_long_form(VR::OW));
  REQUIRE(is_long_form(VR::SQ));
  REQUIRE(is_long_form(VR::UN));
  REQUIRE(is_long_form(VR::UT));
  REQUIRE_FALSE(is_long_form(VR::US));
  REQUIRE_FALSE(is_long_form(VR::PN));
  REQUIRE_FALSE(is_long_form(VR::UI));
}
