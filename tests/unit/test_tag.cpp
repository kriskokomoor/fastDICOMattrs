#include <catch2/catch_test_macros.hpp>

#include "fastdicomattrs/tag.hpp"

using fds::Tag;

TEST_CASE("Tag equality and ordering", "[tag]") {
  Tag a{0x0010, 0x0020};
  Tag b{0x0010, 0x0020};
  Tag c{0x0010, 0x0021};
  REQUIRE(a == b);
  REQUIRE(a != c);
  REQUIRE(a < c);
}

TEST_CASE("Tag::is_private detects odd group numbers", "[tag]") {
  REQUIRE(Tag(0x0009, 0x0010).is_private());
  REQUIRE_FALSE(Tag(0x0010, 0x0010).is_private());
}

TEST_CASE("Tag::is_group_length detects element 0x0000", "[tag]") {
  REQUIRE(Tag(0x0008, 0x0000).is_group_length());
  REQUIRE_FALSE(Tag(0x0008, 0x0001).is_group_length());
}

TEST_CASE("Tag::is_item_or_delimiter detects group 0xFFFE", "[tag]") {
  REQUIRE(fds::kItemTag.is_item_or_delimiter());
  REQUIRE(fds::kItemDelimitationTag.is_item_or_delimiter());
  REQUIRE(fds::kSequenceDelimitationTag.is_item_or_delimiter());
  REQUIRE_FALSE(fds::kPixelDataTag.is_item_or_delimiter());
}

TEST_CASE("to_string(Tag) formats as (gggg,eeee)", "[tag]") {
  REQUIRE(fds::to_string(Tag(0x0010, 0x0020)) == "(0010,0020)");
  REQUIRE(fds::to_string(Tag(0x7FE0, 0x0010)) == "(7FE0,0010)");
}

TEST_CASE("Tag is hashable", "[tag]") {
  std::hash<Tag> hasher;
  REQUIRE(hasher(Tag(1, 2)) == hasher(Tag(1, 2)));
}
