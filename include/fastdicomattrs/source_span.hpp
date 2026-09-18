#pragma once

#include <cstdint>

namespace fds {

// Identifies a byte range within a Source by offset and length only -- no
// pointer. Resolved lazily against whichever Source owns it. See
// docs/architecture.md section 4 for the lifetime contract this implies.
class SourceSpan {
 public:
  constexpr SourceSpan() noexcept = default;
  constexpr SourceSpan(std::uint64_t offset, std::uint64_t length) noexcept
      : offset_(offset), length_(length) {}

  constexpr std::uint64_t offset() const noexcept { return offset_; }
  constexpr std::uint64_t length() const noexcept { return length_; }
  constexpr std::uint64_t end() const noexcept { return offset_ + length_; }
  constexpr bool empty() const noexcept { return length_ == 0; }

 private:
  std::uint64_t offset_ = 0;
  std::uint64_t length_ = 0;
};

constexpr bool operator==(SourceSpan a, SourceSpan b) noexcept {
  return a.offset() == b.offset() && a.length() == b.length();
}
constexpr bool operator!=(SourceSpan a, SourceSpan b) noexcept { return !(a == b); }

}  // namespace fds
