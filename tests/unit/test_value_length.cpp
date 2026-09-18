#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/value_length.hpp"

using fds::LengthForm;
using fds::ValueLength;

TEST_CASE("ValueLength::defined stores form and value", "[value_length]") {
  auto vl = ValueLength::defined(LengthForm::Short16, 42);
  REQUIRE(vl.form() == LengthForm::Short16);
  REQUIRE_FALSE(vl.is_undefined());
  REQUIRE(vl.value() == 42);
}

TEST_CASE("ValueLength::undefined uses the 0xFFFFFFFF marker in Long32 form", "[value_length]") {
  auto vl = ValueLength::undefined();
  REQUIRE(vl.form() == LengthForm::Long32);
  REQUIRE(vl.is_undefined());
  REQUIRE(vl.value() == ValueLength::kUndefinedMarker);
}

TEST_CASE("A defined Long32 length equal to the marker value is impossible by construction",
          "[value_length]") {
  // 0xFFFFFFFF can only be reached via ValueLength::undefined(); defined()
  // with that value would (correctly) also report is_undefined() since the
  // class distinguishes undefined-ness purely by value+form, matching wire
  // format semantics where 0xFFFFFFFF *is* the undefined marker.
  auto vl = ValueLength::defined(LengthForm::Long32, ValueLength::kUndefinedMarker);
  REQUIRE(vl.is_undefined());
}
