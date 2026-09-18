#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace fds {

// A DICOM data element tag: (group, element). Deliberately not associated
// with any name/keyword/VR lookup -- that is dictionary knowledge this
// library does not own. See docs/architecture.md section 1.
struct Tag {
  std::uint16_t group = 0;
  std::uint16_t element = 0;

  constexpr Tag() noexcept = default;
  constexpr Tag(std::uint16_t g, std::uint16_t e) noexcept : group(g), element(e) {}

  constexpr std::uint32_t to_uint32() const noexcept {
    return (static_cast<std::uint32_t>(group) << 16) | element;
  }

  // Odd group numbers are reserved for private (manufacturer-specific) data.
  constexpr bool is_private() const noexcept { return (group & 0x1u) != 0; }

  // (gggg,0000) group length elements. Retired in the standard but still
  // appear in real files and must round-trip like any other element.
  constexpr bool is_group_length() const noexcept { return element == 0x0000; }

  // Item / delimiter pseudo-tags (group 0xFFFE) used to encode sequence and
  // item boundaries; never "real" data elements.
  constexpr bool is_item_or_delimiter() const noexcept { return group == 0xFFFE; }
};

constexpr bool operator==(Tag a, Tag b) noexcept {
  return a.group == b.group && a.element == b.element;
}
constexpr bool operator!=(Tag a, Tag b) noexcept { return !(a == b); }
constexpr bool operator<(Tag a, Tag b) noexcept { return a.to_uint32() < b.to_uint32(); }

std::string to_string(Tag tag);

// Well-known structural pseudo-tags (DICOM PS3.5 section 7.5).
inline constexpr Tag kItemTag{0xFFFE, 0xE000};
inline constexpr Tag kItemDelimitationTag{0xFFFE, 0xE00D};
inline constexpr Tag kSequenceDelimitationTag{0xFFFE, 0xE0DD};
inline constexpr Tag kPixelDataTag{0x7FE0, 0x0010};

}  // namespace fds

namespace std {
template <>
struct hash<fds::Tag> {
  std::size_t operator()(fds::Tag t) const noexcept {
    return std::hash<std::uint32_t>{}(t.to_uint32());
  }
};
}  // namespace std
