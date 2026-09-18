#include "fastdicomattrs/mutation.hpp"

#include "fastdicomattrs/value_length.hpp"
#include "fastdicomattrs/vr.hpp"
#include "internal/container_locate.hpp"
#include "internal/vr_inference.hpp"

namespace fds::mutation {
namespace {

InsertStatus to_insert_status(internal::ContainerLocateStatus status) {
  switch (status) {
    case internal::ContainerLocateStatus::Success: return InsertStatus::Success;
    case internal::ContainerLocateStatus::ContainerNotFound: return InsertStatus::ContainerNotFound;
    case internal::ContainerLocateStatus::NotASequence: return InsertStatus::NotASequence;
    case internal::ContainerLocateStatus::ItemIndexOutOfRange:
      return InsertStatus::ItemIndexOutOfRange;
  }
  return InsertStatus::ContainerNotFound;  // unreachable
}

// Shared by insert() and insert_inferred(): container resolution and
// duplicate check -- the one centralized implementation both use, per
// docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 4. Returns Success (loc.container non-null) iff `tag` is free to
// insert into the resolved container.
struct PreparedInsert {
  InsertStatus status = InsertStatus::ContainerNotFound;
  const std::vector<Element>* container = nullptr;
};

PreparedInsert prepare_insert(const DICOMStructure& structure, const ElementPath& parent,
                               Tag tag) {
  auto loc = internal::locate_container_const(structure.elements(), parent);
  if (loc.status != internal::ContainerLocateStatus::Success) {
    return {to_insert_status(loc.status), nullptr};
  }
  for (const auto& e : *loc.container) {
    if (e.tag() == tag) return {InsertStatus::AlreadyExists, nullptr};
  }
  return {InsertStatus::Success, loc.container};
}

// A1.7 section 19 (auto-padding audit): PS3.5 6.4's general padding rule
// -- VR::UI and every purely-binary VR pads with a trailing NUL; every
// other (text-natured) VR pads with a trailing SPACE. This mirrors
// charset::encode_text's own documented rule for its seven text VRs
// (charset.cpp: "the pad character is SPACE (0x20); NUL padding is
// specific to VR::UI") extended to the full VR set, so this convenience
// layer -- which always knows the final VR by the time it would need to
// pad, unlike the raw structural primitives below it -- never makes a
// caller manually pad an odd-length value the way DICOMStructure::insert()
// and set()/set_value() still require (that raw-primitive contract is
// unchanged; see their own doc comments).
std::byte pad_byte_for(VR vr) {
  switch (vr) {
    case VR::AE: case VR::AS: case VR::CS: case VR::DA: case VR::DS: case VR::DT:
    case VR::IS: case VR::LO: case VR::LT: case VR::PN: case VR::SH: case VR::ST:
    case VR::TM: case VR::UC: case VR::UR: case VR::UT:
      return std::byte{0x20};
    default:  // UI and every binary VR (AT/FL/FD/OB/OD/OF/OL/OV/OW/SL/SS/SV/UL/UN/US/UV)
      return std::byte{0x00};
  }
}

Value pad_if_odd(VR vr, Value value) {
  if (value.size() % 2 == 0) return value;
  std::vector<std::byte> bytes(value.bytes().begin(), value.bytes().end());
  bytes.push_back(pad_byte_for(vr));
  return Value::from_owned(std::move(bytes));
}

InsertStatus check_value_encodable(VR vr, const Value& value) {
  LengthForm form = is_long_form(vr) ? LengthForm::Long32 : LengthForm::Short16;
  if (form == LengthForm::Short16 && value.size() > ValueLength::kMaxShortFormLength) {
    return InsertStatus::ValueTooLong;
  }
  return InsertStatus::Success;
}

}  // namespace

InsertStatus insert(DICOMStructure& structure, const ElementPath& parent, Tag tag, VR vr,
                     Value value) {
  auto prepared = prepare_insert(structure, parent, tag);
  if (prepared.status != InsertStatus::Success) return prepared.status;

  if (vr == VR::SQ || vr == VR::Unknown) return InsertStatus::InvalidVR;
  value = pad_if_odd(vr, std::move(value));
  auto encodable = check_value_encodable(vr, value);
  if (encodable != InsertStatus::Success) return encodable;

  bool applied = structure.insert(parent, tag, vr, std::move(value));
  // Every precondition DICOMStructure::insert() itself checks has already
  // been verified above (container resolution, duplicate, VR, length), so
  // this is expected to always succeed; the fallback is defensive -- never
  // silently reports Success for a mutation that did not actually happen.
  return applied ? InsertStatus::Success : InsertStatus::ValueTooLong;
}

InsertStatus insert_inferred(DICOMStructure& structure, const ElementPath& parent, Tag tag,
                              Value value) {
  auto prepared = prepare_insert(structure, parent, tag);
  if (prepared.status != InsertStatus::Success) return prepared.status;

  auto inference = internal::infer_vr(tag);
  if (inference.outcome != internal::VRInferenceOutcome::Inferred) return InsertStatus::VRRequired;

  value = pad_if_odd(inference.vr, std::move(value));
  auto encodable = check_value_encodable(inference.vr, value);
  if (encodable != InsertStatus::Success) return encodable;

  bool applied = structure.insert(parent, tag, inference.vr, std::move(value));
  return applied ? InsertStatus::Success : InsertStatus::ValueTooLong;
}

}  // namespace fds::mutation
