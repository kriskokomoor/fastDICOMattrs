#pragma once

#include <cstdint>

namespace fds {

// Which header shape a length was actually encoded with. This mirrors
// VR::is_long_form() but is stored independently on the parsed Element
// because LOSSLESS fidelity must preserve what was literally in the file,
// not what the VR table says should have been there (malformed/edge-case
// input can disagree). See docs/roundtrip-contract.md.
enum class LengthForm : std::uint8_t { Short16, Long32 };

// A DICOM value length as read from (or to be written to) an element,
// item, or sequence header: either a defined byte count, or the
// 0xFFFFFFFF "undefined length" marker (only legal in Long32 form).
class ValueLength {
 public:
  static constexpr std::uint32_t kUndefinedMarker = 0xFFFFFFFFu;

  // The largest value length a Short16-form header can encode. PS3.5 Table
  // 7.1-1 gives short-form VRs a 2-byte length field; 0xFFFF is excluded
  // (reserved), so 0xFFFE is the practical ceiling. A mutation that would
  // grow a short-form element's value past this must be rejected, not
  // truncated -- see Element::set_value.
  static constexpr std::uint32_t kMaxShortFormLength = 0xFFFEu;

  static constexpr ValueLength defined(LengthForm form, std::uint32_t value) noexcept {
    return ValueLength(form, value);
  }
  static constexpr ValueLength undefined() noexcept {
    return ValueLength(LengthForm::Long32, kUndefinedMarker);
  }

  constexpr LengthForm form() const noexcept { return form_; }
  constexpr bool is_undefined() const noexcept { return raw_ == kUndefinedMarker && form_ == LengthForm::Long32; }

  // Precondition: !is_undefined().
  constexpr std::uint32_t value() const noexcept { return raw_; }

 private:
  constexpr ValueLength(LengthForm form, std::uint32_t raw) noexcept : form_(form), raw_(raw) {}

  LengthForm form_;
  std::uint32_t raw_;
};

}  // namespace fds
