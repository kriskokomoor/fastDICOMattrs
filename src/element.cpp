#include "fastdicomattrs/element.hpp"

#include <stdexcept>

#include "fastdicomattrs/sequence.hpp"
#include "fastdicomattrs/value_length.hpp"

namespace fds {

Element::Element(Tag tag, VR vr, VRProvenance provenance, LengthForm length_form,
                  bool undefined_length, std::array<std::byte, 2> reserved, Value value)
    : tag_(tag),
      vr_(vr),
      provenance_(provenance),
      length_form_(length_form),
      undefined_length_(undefined_length),
      reserved_(reserved),
      content_(std::move(value)) {}

Element::Element(Tag tag, VRProvenance provenance, LengthForm length_form,
                  bool undefined_length, std::array<std::byte, 2> reserved,
                  std::unique_ptr<Sequence> sequence)
    : tag_(tag),
      vr_(VR::SQ),
      provenance_(provenance),
      length_form_(length_form),
      undefined_length_(undefined_length),
      reserved_(reserved),
      content_(std::move(sequence)) {}

Element::Element(Element&&) noexcept = default;
Element& Element::operator=(Element&&) noexcept = default;
Element::~Element() = default;

const Value& Element::value() const {
  if (auto* v = std::get_if<Value>(&content_)) return *v;
  throw std::logic_error("Element::value() called on a sequence element: " + to_string(tag_));
}

const Sequence& Element::sequence() const {
  if (auto* s = std::get_if<std::unique_ptr<Sequence>>(&content_)) return **s;
  throw std::logic_error("Element::sequence() called on a non-sequence element: " + to_string(tag_));
}

Sequence& Element::sequence() {
  if (auto* s = std::get_if<std::unique_ptr<Sequence>>(&content_)) return **s;
  throw std::logic_error("Element::sequence() called on a non-sequence element: " + to_string(tag_));
}

bool Element::set_value(Value new_value) {
  if (is_sequence()) {
    throw std::logic_error("Element::set_value() called on a sequence element: " + to_string(tag_));
  }
  // DICOM values must have even length (PS3.5 6.4) regardless of VR; an
  // odd-length value is not a rare-VR edge case, it is simply not
  // representable as a conformant element. Reject it rather than write a
  // structurally invalid length field.
  if (new_value.size() % 2 != 0) {
    return false;
  }
  if (length_form_ == LengthForm::Short16 && new_value.size() > ValueLength::kMaxShortFormLength) {
    return false;
  }
  content_ = std::move(new_value);
  modified_ = true;
  return true;
}

}  // namespace fds
