#include "internal/vr_inference.hpp"

#include "fastdicomattrs/dictionary.hpp"

namespace fds::internal {

VRInferenceResult infer_vr(Tag tag) {
  if (tag.is_private()) {
    if (tag.element >= 0x0010 && tag.element <= 0x00FF) {
      return {VRInferenceOutcome::Inferred, VR::LO};
    }
    return {VRInferenceOutcome::Required, VR::Unknown};
  }

  auto entry = dictionary::lookup(tag);
  if (!entry.has_value() || entry->ambiguity != dictionary::VRAmbiguity::None) {
    return {VRInferenceOutcome::Required, VR::Unknown};
  }
  return {VRInferenceOutcome::Inferred, entry->vr};
}

}  // namespace fds::internal
