#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <variant>

#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/value.hpp"
#include "fastdicomattrs/value_length.hpp"
#include "fastdicomattrs/vr.hpp"

namespace fds {

// Forward-declared to break the Element <-> Sequence <-> Item mutual
// recursion; only sequence.hpp needs the complete type. See
// docs/architecture.md section 3.
class Sequence;

// One DICOM data element: a Tag, a VR, a length representation, and either
// a Value (non-sequence) or a Sequence. Order within its containing
// DICOMStructure/Item is significant and preserved.
class Element {
 public:
  // Constructs a non-sequence element.
  Element(Tag tag, VR vr, VRProvenance provenance, LengthForm length_form,
          bool undefined_length, std::array<std::byte, 2> reserved, Value value);

  // Constructs a sequence element (vr is always VR::SQ). `provenance` must
  // be VRProvenance::Explicit, ::Dictionary, or ::Structural -- a Sequence
  // is never ContextResolved or Unknown (see vr.hpp).
  Element(Tag tag, VRProvenance provenance, LengthForm length_form, bool undefined_length,
          std::array<std::byte, 2> reserved, std::unique_ptr<Sequence> sequence);

  Element(Element&&) noexcept;
  Element& operator=(Element&&) noexcept;
  Element(const Element&) = delete;
  Element& operator=(const Element&) = delete;
  ~Element();

  Tag tag() const noexcept { return tag_; }
  VR vr() const noexcept { return vr_; }

  // True if the VR was literally present in (and recognized from) the
  // source header, or was supplied explicitly by a caller via the mutation
  // API -- i.e. VRProvenance::Explicit exactly. False for every
  // Implicit-VR-inferred provenance (Dictionary, Structural,
  // ContextResolved, Unknown) -- see vr_provenance() for which.
  bool has_explicit_vr_in_source() const noexcept { return provenance_ == VRProvenance::Explicit; }

  // How vr() was determined -- see VRProvenance (vr.hpp) for the full
  // distinction. Added in A1.4.
  VRProvenance vr_provenance() const noexcept { return provenance_; }

  LengthForm length_form() const noexcept { return length_form_; }
  bool has_undefined_length() const noexcept { return undefined_length_; }
  std::array<std::byte, 2> reserved_bytes() const noexcept { return reserved_; }

  bool is_sequence() const noexcept { return std::holds_alternative<std::unique_ptr<Sequence>>(content_); }

  // Precondition: !is_sequence().
  const Value& value() const;
  // Precondition: is_sequence().
  const Sequence& sequence() const;
  // Non-const overload, for path-based navigation during mutation (set/erase).
  Sequence& sequence();

  // Precondition: !is_sequence(). Replaces the value and marks the element
  // modified. Returns false (value left unchanged) if `new_value` cannot be
  // encoded as a conformant DICOM value: odd length (PS3.5 6.4 requires
  // every value to have even length, regardless of VR), or too large for
  // this element's LengthForm -- e.g. growing a Short16-form element's
  // value past ValueLength::kMaxShortFormLength, which DICOM cannot encode
  // for that header shape. See docs/architecture.md section 9 for mutation
  // rules.
  bool set_value(Value new_value);

  bool is_modified() const noexcept { return modified_; }

 private:
  Tag tag_;
  VR vr_;
  VRProvenance provenance_;
  LengthForm length_form_;
  bool undefined_length_;
  std::array<std::byte, 2> reserved_{};
  std::variant<Value, std::unique_ptr<Sequence>> content_;
  bool modified_ = false;
};

}  // namespace fds
