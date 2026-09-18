#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/element_path.hpp"

using fds::ElementPath;
using fds::Tag;

TEST_CASE("ElementPath single top-level tag formats without an index", "[element_path]") {
  ElementPath path(Tag(0x0010, 0x0020));
  REQUIRE(path.steps().size() == 1);
  REQUIRE_FALSE(path.steps()[0].item_index.has_value());
  REQUIRE(path.to_string() == "(0010,0020)");
}

TEST_CASE("ElementPath nested path formats with item indices", "[element_path]") {
  ElementPath path;
  path.push(Tag(0x300A, 0x00B0), 2).push(Tag(0x300A, 0x00B6), 0).push(Tag(0x300A, 0x0072));
  REQUIRE(path.steps().size() == 3);
  REQUIRE(path.to_string() == "(300A,00B0)[2]/(300A,00B6)[0]/(300A,0072)");
}
