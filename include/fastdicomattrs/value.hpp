#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fastdicomattrs/source.hpp"
#include "fastdicomattrs/source_span.hpp"

namespace fds {

// Thrown by Value's typed accessors on a size mismatch (e.g. as_uint16() on
// a 4-byte value). Never thrown across the C ABI -- see docs/abi-design.md.
class ValueTypeError : public std::runtime_error {
 public:
  explicit ValueTypeError(const std::string& what) : std::runtime_error(what) {}
};

// The bytes of one non-sequence Element. Either a zero-copy view into a
// Source (the common case for a freshly parsed, unmodified element) or an
// owned buffer (after a caller-initiated mutation, or when constructed
// directly from caller data with no backing Source). See
// docs/architecture.md section 4 for the lifetime contract of the
// source-backed case.
class Value {
 public:
  Value() = default;

  static Value from_source(const Source& source, SourceSpan span) { return Value(&source, span); }
  static Value from_owned(std::vector<std::byte> bytes) { return Value(std::move(bytes)); }
  static Value from_string(std::string_view text);

  bool is_source_backed() const noexcept { return source_ != nullptr; }
  std::size_t size() const noexcept { return is_source_backed() ? span_.length() : owned_.size(); }

  std::span<const std::byte> bytes() const;

  // Trims a single trailing space (0x20) or NUL (0x00) pad byte, per the
  // DICOM value-padding rule (PS3.5 6.4), then interprets the remainder as
  // text. Never mutates what bytes() returns.
  std::string as_string() const;
  std::vector<std::string> as_string_list() const;  // split on '\' (multi-valued VRs)

  std::uint16_t as_uint16() const;  // VR US
  std::int16_t as_int16() const;    // VR SS
  std::uint32_t as_uint32() const;  // VR UL
  std::int32_t as_int32() const;    // VR SL
  float as_float32() const;         // VR FL
  double as_float64() const;        // VR FD

 private:
  Value(const Source* source, SourceSpan span) : source_(source), span_(span) {}
  explicit Value(std::vector<std::byte> owned) : owned_(std::move(owned)) {}

  const Source* source_ = nullptr;
  SourceSpan span_{};
  std::vector<std::byte> owned_;
};

}  // namespace fds
